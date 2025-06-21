#include "encoder.h"
#include <iostream>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

static const char* encoder_name(EncoderType type) {
    switch (type) {
        case EncoderType::NVENC: return "h264_nvenc";
        case EncoderType::QSV:   return "h264_qsv";
        case EncoderType::AMF:   return "h264_amf";
        case EncoderType::SOFTWARE: return "libx264";
        default: return nullptr;
    }
}

static bool try_encoder(EncoderType type, const EncoderSettings& settings, EncoderContext& ctx) {
    const char* name = encoder_name(type);
    if (!name) return false;

    ctx.codec = avcodec_find_encoder_by_name(name);
    if (!ctx.codec) return false;

    ctx.codec_ctx = avcodec_alloc_context3(ctx.codec);
    if (!ctx.codec_ctx) return false;

    ctx.codec_ctx->width = settings.width;
    ctx.codec_ctx->height = settings.height;
    ctx.codec_ctx->time_base = {1, settings.fps};
    ctx.codec_ctx->framerate = {settings.fps, 1};
    ctx.codec_ctx->bit_rate = settings.bitrate;
    ctx.codec_ctx->gop_size = 10;
    ctx.codec_ctx->max_b_frames = 1;
    ctx.codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    // Encoder-specific tuning
    if (type == EncoderType::SOFTWARE) {
        av_opt_set(ctx.codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx.codec_ctx->priv_data, "tune", "zerolatency", 0);
    } else if (type == EncoderType::NVENC) {
        av_opt_set(ctx.codec_ctx->priv_data, "preset", "llhp", 0);  // low-latency high performance
    }

    if (avcodec_open2(ctx.codec_ctx, ctx.codec, nullptr) < 0) {
        avcodec_free_context(&ctx.codec_ctx);
        return false;
    }

    ctx.sws_ctx = sws_getContext(settings.width, settings.height, settings.input_format,
                                 settings.width, settings.height, AV_PIX_FMT_YUV420P,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!ctx.sws_ctx) {
        avcodec_free_context(&ctx.codec_ctx);
        return false;
    }

    ctx.frame = av_frame_alloc();
    ctx.frame->format = AV_PIX_FMT_YUV420P;
    ctx.frame->width = settings.width;
    ctx.frame->height = settings.height;
    av_frame_get_buffer(ctx.frame, 32);

    ctx.pkt = av_packet_alloc();
    ctx.input_format = settings.input_format;

    return true;
}

void print_available_encoders() {
    void* iter = nullptr;
    std::cout << "Available encoders at runtime:\n";
    while (const AVCodec* codec = av_codec_iterate(&iter)) {
        if (av_codec_is_encoder(codec)) {
            std::cout << " - " << codec->name << "\n";
        }
    }
}

bool init_encoder(const EncoderSettings& settings, EncoderContext& ctx) {
    // Find codec by preferred encoder type
    const char* codec_name = nullptr;
    switch (settings.preferred) {
        case EncoderType::NVENC: codec_name = "h264_nvenc"; break;
        case EncoderType::QSV: codec_name = "h264_qsv"; break;
        case EncoderType::AMF: codec_name = "h264_amf"; break;
        default: codec_name = nullptr; break;
    }

    if (codec_name)
        ctx.codec = avcodec_find_encoder_by_name(codec_name);
    else
        ctx.codec = avcodec_find_encoder(AV_CODEC_ID_H264);

    if (!ctx.codec) {
        std::cerr << "[Encoder] Failed to find encoder\n";
        return false;
    }

    ctx.codec_ctx = avcodec_alloc_context3(ctx.codec);
    if (!ctx.codec_ctx) {
        std::cerr << "[Encoder] Failed to allocate codec context\n";
        return false;
    }

    ctx.codec_ctx->width = settings.width;
    ctx.codec_ctx->height = settings.height;
    ctx.codec_ctx->time_base = AVRational{1, settings.fps};
    ctx.codec_ctx->framerate = AVRational{settings.fps, 1};
    ctx.codec_ctx->bit_rate = settings.bitrate;
    ctx.codec_ctx->gop_size = 10;
    ctx.codec_ctx->max_b_frames = 1;
    ctx.codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    // Set encoder options per codec_name, as before...

    if (avcodec_open2(ctx.codec_ctx, ctx.codec, nullptr) < 0) {
        std::cerr << "[Encoder] Failed to open codec\n";
        avcodec_free_context(&ctx.codec_ctx);
        return false;
    }

    // Initialize sws context here with input pixel format from settings
    ctx.input_format = settings.input_format;
    ctx.sws_ctx = sws_getContext(settings.width, settings.height, ctx.input_format,
                                 settings.width, settings.height, AV_PIX_FMT_YUV420P,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!ctx.sws_ctx) {
        std::cerr << "[Encoder] Failed to initialize sws context\n";
        avcodec_free_context(&ctx.codec_ctx);
        return false;
    }

    ctx.frame = av_frame_alloc();
    ctx.pkt = av_packet_alloc();
    if (!ctx.frame || !ctx.pkt) {
        std::cerr << "[Encoder] Failed to allocate frame or packet\n";
        destroy_encoder(ctx);
        return false;
    }

    ctx.frame->format = AV_PIX_FMT_YUV420P;
    ctx.frame->width = settings.width;
    ctx.frame->height = settings.height;
    if (av_frame_get_buffer(ctx.frame, 32) < 0) {
        std::cerr << "[Encoder] Failed to allocate frame buffer\n";
        destroy_encoder(ctx);
        return false;
    }

    return true;
}

void destroy_encoder(EncoderContext& ctx) {
    if (ctx.frame) {
        av_frame_free(&ctx.frame);
    }
    if (ctx.pkt) {
        av_packet_free(&ctx.pkt);
    }
    if (ctx.codec_ctx) {
        avcodec_free_context(&ctx.codec_ctx);
    }
    ctx.codec = nullptr;
}

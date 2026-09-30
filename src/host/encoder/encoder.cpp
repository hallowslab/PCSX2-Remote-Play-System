#include "encoder.h"
#include <iostream>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace rps {

static const char *encoder_name(EncoderType type) {
  switch (type) {
  case EncoderType::NVENC:
    return "h264_nvenc";
  case EncoderType::QSV:
    return "h264_qsv";
  case EncoderType::AMF:
    return "h264_amf";
  case EncoderType::SOFTWARE:
    return "libx264";
  default:
    return nullptr;
  }
}

static bool try_encoder(EncoderType type, const EncoderSettings &settings,
                        EncoderContext &ctx) {
  const char *name = encoder_name(type);
  if (!name)
    return false;

  ctx.codec = avcodec_find_encoder_by_name(name);
  if (!ctx.codec)
    return false;

  ctx.codec_ctx = avcodec_alloc_context3(ctx.codec);
  if (!ctx.codec_ctx)
    return false;

  ctx.codec_ctx->width = settings.width;
  ctx.codec_ctx->height = settings.height;
  ctx.codec_ctx->time_base = {1, settings.fps};
  ctx.codec_ctx->framerate = {settings.fps, 1};
  ctx.codec_ctx->bit_rate = settings.bitrate;
  ctx.codec_ctx->gop_size = 30;    // Keyframe every 0.5s for faster sync
  ctx.codec_ctx->max_b_frames = 0; // Zero B-frames for low latency

  // Use NV12 as the preferred hardware format
  AVPixelFormat target_fmt = AV_PIX_FMT_NV12;
  if (type == EncoderType::SOFTWARE) {
    target_fmt = AV_PIX_FMT_YUV420P;
  }

  ctx.codec_ctx->pix_fmt = target_fmt;

  // Encoder-specific tuning
  if (type == EncoderType::SOFTWARE) {
    av_opt_set(ctx.codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx.codec_ctx->priv_data, "tune", "zerolatency", 0);
  } else if (type == EncoderType::NVENC) {
    // High performance low latency settings
    // Using "p1" might be too new for some drivers, but "llhp" (low latency
    // high performance) is older/stable
    av_opt_set(ctx.codec_ctx->priv_data, "preset", "p1", 0);
    av_opt_set(ctx.codec_ctx->priv_data, "tune", "ull", 0);
    av_opt_set(ctx.codec_ctx->priv_data, "delay", "0", 0);
    av_opt_set(ctx.codec_ctx->priv_data, "zerolatency", "1", 0);
    av_opt_set(ctx.codec_ctx->priv_data, "rc", "cbr", 0);
    av_opt_set(ctx.codec_ctx->priv_data, "rc-lookahead", "0", 0);
  } else if (type == EncoderType::QSV) {
    av_opt_set(ctx.codec_ctx->priv_data, "preset", "veryfast", 0);
    av_opt_set(ctx.codec_ctx->priv_data, "async_depth", "1", 0);
  } else if (type == EncoderType::AMF) {
    // Fix for AMF usage
    av_opt_set(ctx.codec_ctx->priv_data, "usage", "transcoding", 0);
    av_opt_set(ctx.codec_ctx->priv_data, "quality", "speed", 0);
  }

  if (avcodec_open2(ctx.codec_ctx, ctx.codec, nullptr) < 0) {
    avcodec_free_context(&ctx.codec_ctx);
    ctx.codec_ctx = nullptr;
    return false;
  }

  ctx.sws_ctx =
      sws_getContext(settings.width, settings.height, settings.input_format,
                     settings.width, settings.height, target_fmt,
                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!ctx.sws_ctx) {
    avcodec_free_context(&ctx.codec_ctx);
    ctx.codec_ctx = nullptr;
    return false;
  }

  ctx.frame = av_frame_alloc();
  ctx.frame->format = target_fmt;
  ctx.frame->width = settings.width;
  ctx.frame->height = settings.height;
  if (av_frame_get_buffer(ctx.frame, 32) < 0) {
    destroy_encoder(ctx);
    return false;
  }

  ctx.pkt = av_packet_alloc();
  ctx.input_format = settings.input_format;
  ctx.frame_index = 0;

  return true;
}

bool init_encoder(const EncoderSettings &settings, EncoderContext &ctx) {
  std::vector<EncoderType> fallback_chain;

  fallback_chain.push_back(settings.preferred);

  auto add_if_not_present = [&](EncoderType type) {
    for (auto t : fallback_chain)
      if (t == type)
        return;
    fallback_chain.push_back(type);
  };

  add_if_not_present(EncoderType::NVENC);
  add_if_not_present(EncoderType::QSV);
  add_if_not_present(EncoderType::AMF);
  add_if_not_present(EncoderType::SOFTWARE);

  for (auto type : fallback_chain) {
    std::cout << "[Encoder] Attempting to initialize: " << encoder_name(type)
              << "..." << std::endl;
    if (try_encoder(type, settings, ctx)) {
      std::cout << "[Encoder] Successfully initialized: " << encoder_name(type)
                << std::endl;
      return true;
    }
  }

  std::cerr
      << "[Encoder] Failed to initialize any encoder in the fallback chain"
      << std::endl;
  return false;
}

void destroy_encoder(EncoderContext &ctx) {
  if (ctx.sws_ctx) {
    sws_freeContext(ctx.sws_ctx);
    ctx.sws_ctx = nullptr;
  }
  if (ctx.frame) {
    av_frame_free(&ctx.frame);
    ctx.frame = nullptr;
  }
  if (ctx.pkt) {
    av_packet_free(&ctx.pkt);
    ctx.pkt = nullptr;
  }
  if (ctx.codec_ctx) {
    avcodec_free_context(&ctx.codec_ctx);
    ctx.codec_ctx = nullptr;
  }
  ctx.codec = nullptr;
}

// --- Audio Encoding ---

bool init_audio_encoder(const AudioEncoderSettings &settings,
                        AudioEncoderContext &ctx) {
  const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
  if (!codec) {
    std::cerr << "[AudioEncoder] Opus encoder not found" << std::endl;
    return false;
  }

  ctx.codec_ctx = avcodec_alloc_context3(codec);
  if (!ctx.codec_ctx)
    return false;

  ctx.codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLT;
  ctx.codec_ctx->sample_rate = settings.sample_rate;
  ctx.codec_ctx->bit_rate = settings.bitrate;
  ctx.codec_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

  // Set Opus specific options for low latency
  av_opt_set(ctx.codec_ctx->priv_data, "application", "lowdelay", 0);
  av_opt_set(ctx.codec_ctx->priv_data, "vbr", "on", 0);
  av_opt_set_int(ctx.codec_ctx->priv_data, "compression_level", 10,
                 0); // Higher complexity for better quality at same bitrate

  // Set channel layout using newer API if available or use standard stereo
  av_channel_layout_default(&ctx.codec_ctx->ch_layout, settings.channels);

  if (avcodec_open2(ctx.codec_ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx.codec_ctx);
    return false;
  }

  ctx.frame = av_frame_alloc();
  ctx.frame->nb_samples = ctx.codec_ctx->frame_size;
  ctx.frame->format = ctx.codec_ctx->sample_fmt;
  ctx.frame->sample_rate = ctx.codec_ctx->sample_rate;
  av_channel_layout_copy(&ctx.frame->ch_layout, &ctx.codec_ctx->ch_layout);

  if (av_frame_get_buffer(ctx.frame, 0) < 0) {
    destroy_audio_encoder(ctx);
    return false;
  }

  ctx.pkt = av_packet_alloc();
  ctx.frame_index = 0;

  // Allocate FIFO with enough space for several frames to handle bursts
  ctx.fifo = av_audio_fifo_alloc(ctx.codec_ctx->sample_fmt,
                                 ctx.codec_ctx->ch_layout.nb_channels,
                                 ctx.codec_ctx->frame_size * 10);
  if (!ctx.fifo) {
    destroy_audio_encoder(ctx);
    return false;
  }

  return true;
}

void destroy_audio_encoder(AudioEncoderContext &ctx) {
  if (ctx.fifo) {
    av_audio_fifo_free(ctx.fifo);
    ctx.fifo = nullptr;
  }
  if (ctx.frame)
    av_frame_free(&ctx.frame);
  if (ctx.pkt)
    av_packet_free(&ctx.pkt);
  if (ctx.codec_ctx)
    avcodec_free_context(&ctx.codec_ctx);
}

} // namespace rps

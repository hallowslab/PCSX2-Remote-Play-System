#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

// Encoder backend types
enum class EncoderType {
    NVENC,
    QSV,
    AMF,
    SOFTWARE
};

struct EncoderSettings {
    int width = 1280;
    int height = 720;
    int fps = 30;
    int bitrate = 400000;
    EncoderType preferred = EncoderType::NVENC;
    AVPixelFormat input_format = AV_PIX_FMT_BGRA;
};

struct EncoderContext {
    const AVCodec* codec = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    AVPixelFormat input_format = AV_PIX_FMT_BGRA;
    int frame_index = 0;
};

// Initializes and returns an encoder context
bool init_encoder(const EncoderSettings& settings, EncoderContext& ctx);

// Frees encoder context
void destroy_encoder(EncoderContext& ctx);

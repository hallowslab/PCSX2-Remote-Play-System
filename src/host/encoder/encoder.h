#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace rps {

// Encoder backend types
enum class EncoderType { NVENC, QSV, AMF, SOFTWARE };

struct EncoderSettings {
  int width = 1280;
  int height = 720;
  int fps = 60;
  int bitrate = 8000000;
  EncoderType preferred = EncoderType::NVENC;
  AVPixelFormat input_format = AV_PIX_FMT_BGRA;
};

struct EncoderContext {
  const AVCodec *codec = nullptr;
  AVCodecContext *codec_ctx = nullptr;
  SwsContext *sws_ctx = nullptr;
  AVFrame *frame = nullptr;
  AVPacket *pkt = nullptr;
  AVPixelFormat input_format = AV_PIX_FMT_BGRA;
  int frame_index = 0;
};

bool init_encoder(const EncoderSettings &settings, EncoderContext &ctx);
void destroy_encoder(EncoderContext &ctx);

// --- Audio Encoding ---

struct AudioEncoderSettings {
  int sample_rate = 48000;
  int channels = 2;
  int bitrate = 128000;
};

struct AudioEncoderContext {
  AVCodecContext *codec_ctx = nullptr;
  AVAudioFifo *fifo = nullptr;
  AVPacket *pkt = nullptr;
  AVFrame *frame = nullptr;
  int frame_index = 0;
};

bool init_audio_encoder(const AudioEncoderSettings &settings,
                        AudioEncoderContext &ctx);
void destroy_audio_encoder(AudioEncoderContext &ctx);

} // namespace rps

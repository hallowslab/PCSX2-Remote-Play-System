#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rps {

struct AudioConfig {
  int sample_rate = 48000;
  int channels = 2;
  int bitrate = 128000;
};

struct AudioFrame {
  std::vector<float> samples;
  uint64_t timestamp_us;
};

class IAudioCapture {
public:
  virtual ~IAudioCapture() = default;
  virtual bool init(const AudioConfig &config) = 0;
  virtual bool start() = 0;
  virtual void stop() = 0;

  // Returns true if a frame was acquired, false otherwise
  virtual bool acquireFrame(AudioFrame &frame) = 0;
};

std::unique_ptr<IAudioCapture> createAudioCapture();

} // namespace rps

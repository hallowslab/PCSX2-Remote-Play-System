#include "audio_capture.h"

#ifdef _WIN32
#include <atomic>
#include <audioclient.h>
#include <initguid.h>
#include <iostream>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <mutex>
#include <queue>
#include <vector>
#include <windows.h>


extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace rps {

class WASAPICapture : public IAudioCapture {
public:
  WASAPICapture() : m_initialized(false), m_running(false) {}
  ~WASAPICapture() {
    stop();
    if (m_swr)
      swr_free(&m_swr);
  }

  bool init(const AudioConfig &config) override {
    m_config = config;

    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr))
      return false;

    IMMDeviceEnumerator *enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void **)&enumerator);
    if (FAILED(hr))
      return false;

    IMMDevice *device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr))
      return false;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
                          (void **)&m_audioClient);
    device->Release();
    if (FAILED(hr))
      return false;

    WAVEFORMATEX *pwfx = nullptr;
    hr = m_audioClient->GetMixFormat(&pwfx);
    if (FAILED(hr))
      return false;

    std::cout << "[AudioCapture] Mix Format: " << pwfx->nSamplesPerSec << "Hz, "
              << pwfx->nChannels << " channels, " << pwfx->wBitsPerSample
              << " bits\n";

    // Setup Resampler for 48kHz Stereo Interleaved Float
    AVChannelLayout in_ch_layout;
    av_channel_layout_default(&in_ch_layout, pwfx->nChannels);
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2);

    AVSampleFormat in_fmt = AV_SAMPLE_FMT_S16;
    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
      in_fmt = AV_SAMPLE_FMT_FLT;
    } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      WAVEFORMATEXTENSIBLE *ex = (WAVEFORMATEXTENSIBLE *)pwfx;
      if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
        in_fmt = AV_SAMPLE_FMT_FLT;
      } else if (ex->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
        if (pwfx->wBitsPerSample == 32)
          in_fmt = AV_SAMPLE_FMT_S32;
        else if (pwfx->wBitsPerSample == 24)
          in_fmt = AV_SAMPLE_FMT_S32; // Often padded
        else
          in_fmt = AV_SAMPLE_FMT_S16;
      }
    } else if (pwfx->wFormatTag == WAVE_FORMAT_PCM) {
      if (pwfx->wBitsPerSample == 32)
        in_fmt = AV_SAMPLE_FMT_S32;
      else
        in_fmt = AV_SAMPLE_FMT_S16;
    }

    std::cout << "[AudioCapture] Detected format: "
              << av_get_sample_fmt_name(in_fmt) << " (" << pwfx->wBitsPerSample
              << " bits)\n";

    int ret = swr_alloc_set_opts2(&m_swr, &out_ch_layout, AV_SAMPLE_FMT_FLT,
                                  48000, &in_ch_layout, in_fmt,
                                  pwfx->nSamplesPerSec, 0, nullptr);

    if (ret < 0 || swr_init(m_swr) < 0) {
      std::cerr << "[AudioCapture] Failed to initialize resampler\n";
      CoTaskMemFree(pwfx);
      return false;
    }

    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, pwfx,
                                   NULL);
    if (FAILED(hr)) {
      CoTaskMemFree(pwfx);
      return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient),
                                   (void **)&m_captureClient);
    CoTaskMemFree(pwfx);
    if (FAILED(hr))
      return false;

    m_initialized = true;
    return true;
  }

  bool start() override {
    if (!m_initialized)
      return false;
    HRESULT hr = m_audioClient->Start();
    if (SUCCEEDED(hr)) {
      m_running = true;
      return true;
    }
    return false;
  }

  void stop() override {
    if (m_running) {
      m_audioClient->Stop();
      m_running = false;
    }
  }

  bool acquireFrame(AudioFrame &frame) override {
    if (!m_running)
      return false;

    UINT32 packetSize = 0;
    HRESULT hr = m_captureClient->GetNextPacketSize(&packetSize);
    if (FAILED(hr) || packetSize == 0)
      return false;

    BYTE *data = nullptr;
    UINT32 numFrames = 0;
    DWORD flags = 0;

    hr = m_captureClient->GetBuffer(&data, &numFrames, &flags, NULL, NULL);
    if (FAILED(hr))
      return false;

    frame.samples.clear();
    if (numFrames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
      // Resample to 48kHz Stereo Interleaved Float
      int max_out_samples = swr_get_out_samples(m_swr, numFrames);
      std::vector<float> out_buffer(max_out_samples * 2);

      const uint8_t *in_data[1] = {data};
      uint8_t *out_data[1] = {(uint8_t *)out_buffer.data()};

      int out_samples =
          swr_convert(m_swr, out_data, max_out_samples, in_data, numFrames);
      if (out_samples > 0) {
        frame.samples.assign(out_buffer.begin(),
                             out_buffer.begin() + (out_samples * 2));

        static int audit_count = 0;
        static float max_peak = 0.0f;
        for (float s : frame.samples) {
          float abs_s = std::abs(s);
          if (abs_s > max_peak)
            max_peak = abs_s;
        }
        if (audit_count++ % 100 == 0) {
          std::cout << "[AudioCapture] Peak level (last 100 frames): "
                    << max_peak << "\n";
          max_peak = 0.0f;
        }
      }
    } else {
      frame.samples.resize(480 * 2, 0.0f); // Silence fallback
    }

    frame.timestamp_us = 0; // Will be set by caller or use performance counter

    m_captureClient->ReleaseBuffer(numFrames);
    return true;
  }

private:
  AudioConfig m_config;
  IAudioClient *m_audioClient = nullptr;
  IAudioCaptureClient *m_captureClient = nullptr;
  bool m_initialized;
  bool m_running;
  SwrContext *m_swr = nullptr;
};

std::unique_ptr<IAudioCapture> createAudioCapture() {
  return std::make_unique<WASAPICapture>();
}

} // namespace rps

#else
// Linux implementation stub
namespace rps {
std::unique_ptr<IAudioCapture> createAudioCapture() { return nullptr; }
} // namespace rps
#endif

#include "client.h"
#include "../shared/protocol/protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <enet/enet.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#include <enet/enet.h>

#include <SDL3/SDL.h>

namespace rps {

void start_client(const char *ip_addr, int port, bool &running) {
  if (enet_initialize() != 0) {
    std::cerr << "[Client] Failed to initialize ENet\n";
    return;
  }

  ENetHost *client = enet_host_create(nullptr, 1, 3, 0, 0);
  if (client == nullptr) {
    std::cerr << "[Client] Failed to create ENet client\n";
    enet_deinitialize();
    return;
  }

  ENetAddress address;
  enet_address_set_host(&address, ip_addr);
  address.port = static_cast<enet_uint16>(port);

  std::cout << "[Client] Connecting to " << ip_addr << ":" << port << "...\n";
  ENetPeer *peer = enet_host_connect(client, &address, 3, 0);
  if (peer == nullptr) {
    std::cerr << "[Client] No available peers for connection\n";
    enet_host_destroy(client);
    enet_deinitialize();
    return;
  }

  ENetEvent event;
  if (enet_host_service(client, &event, 5000) > 0 &&
      event.type == ENET_EVENT_TYPE_CONNECT) {
    std::cout << "[Client] Connected to host.\n";
  } else {
    std::cerr << "[Client] Connection failed\n";
    enet_peer_reset(peer);
    enet_host_destroy(client);
    enet_deinitialize();
    return;
  }

  // Initialize decoder
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    std::cerr << "[Client] H264 decoder not found\n";
    enet_host_destroy(client);
    return;
  }

  AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
  codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

  if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
    std::cerr << "[Client] Failed to open codec\n";
    avcodec_free_context(&codec_ctx);
    enet_host_destroy(client);
    return;
  }

  AVFrame *frame = av_frame_alloc();
  AVPacket *pkt = av_packet_alloc();

  // --- Audio State ---
  AVCodecContext *audio_ctx = nullptr;
  SDL_AudioStream *audio_stream = nullptr;
  AVFrame *audio_frame = nullptr;

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    std::cerr << "[Client] SDL_Init Error: " << SDL_GetError() << std::endl;
    return;
  }

  SDL_Window *win =
      SDL_CreateWindow("PCSX2 Remote Play", 1280, 720, SDL_WINDOW_RESIZABLE);
  SDL_Renderer *renderer = SDL_CreateRenderer(win, nullptr);
  SDL_SetRenderVSync(renderer, 0);

  SDL_Texture *texture = nullptr;
  int texture_width = 0;
  int texture_height = 0;

  std::cout << "[Client] Ready to receive stream\n";

  // --- Input capture (keyboard + gamepad) ---
  SDL_Gamepad *gamepad = nullptr;
  auto open_gamepad = [&]() {
    if (gamepad)
      return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids && count > 0)
      gamepad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
  };
  open_gamepad();

  auto send_input_packet = [&](uint8_t type, const void *payload,
                               size_t size) {
    if (!peer)
      return;
    FrameHeader header;
    header.type = type;
    header.flags = FLAG_NONE;
    header.sequence_num = 0;
    header.frame_id = 0;
    header.timestamp_us = get_timestamp_us();
    header.payload_size = (uint32_t)size;
    ENetPacket *packet = enet_packet_create(
        nullptr, sizeof(FrameHeader) + size, ENET_PACKET_FLAG_RELIABLE);
    memcpy(packet->data, &header, sizeof(FrameHeader));
    if (size)
      memcpy(packet->data + sizeof(FrameHeader), payload, size);
    enet_peer_send(peer, 0, packet);
  };

  auto build_input_packet = [&](SDL_Gamepad *g) {
    InputPacket ip = {};
    ip.timestamp_us = get_timestamp_us();
    auto set_btn = [&](SDL_GamepadButton b, uint16_t mask) {
      if (SDL_GetGamepadButton(g, b))
        ip.buttons |= mask;
    };
    set_btn(SDL_GAMEPAD_BUTTON_SOUTH, BTN_A);
    set_btn(SDL_GAMEPAD_BUTTON_EAST, BTN_B);
    set_btn(SDL_GAMEPAD_BUTTON_WEST, BTN_X);
    set_btn(SDL_GAMEPAD_BUTTON_NORTH, BTN_Y);
    set_btn(SDL_GAMEPAD_BUTTON_BACK, BTN_BACK);
    set_btn(SDL_GAMEPAD_BUTTON_START, BTN_START);
    set_btn(SDL_GAMEPAD_BUTTON_LEFT_STICK, BTN_LEFT_THUMB);
    set_btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK, BTN_RIGHT_THUMB);
    set_btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BTN_LEFT_SHOULDER);
    set_btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_RIGHT_SHOULDER);
    set_btn(SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_DPAD_UP);
    set_btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DPAD_DOWN);
    set_btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_DPAD_LEFT);
    set_btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_DPAD_RIGHT);
    ip.left_stick_x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
    ip.left_stick_y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
    ip.right_stick_x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
    ip.right_stick_y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY);
    ip.left_trigger =
        (uint8_t)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    ip.right_trigger =
        (uint8_t)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    return ip;
  };
  InputPacket last_ip = {};
  bool has_last_ip = false;
  auto last_input_time = std::chrono::steady_clock::now();

  // --- Audio Decoder Setup ---
  const AVCodec *audio_codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
  if (audio_codec) {
    audio_ctx = avcodec_alloc_context3(audio_codec);
    if (audio_ctx) {
      if (avcodec_open2(audio_ctx, audio_codec, nullptr) < 0) {
        std::cerr << "[Client] Failed to open audio codec\n";
      }
    }
  }

  SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};
  audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                           &spec, NULL, NULL);
  if (audio_stream) {
    SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
  }

  audio_frame = av_frame_alloc();

  // --- Audio Threading State ---
  std::queue<ENetPacket *> audio_queue;
  std::mutex audio_mtx;
  std::condition_variable audio_cv;
  std::atomic<bool> audio_running{true};

  std::thread audio_thread([&]() {
    AVPacket *a_pkt = av_packet_alloc();
    AVFrame *a_frame = av_frame_alloc();

    while (audio_running || !audio_queue.empty()) {
      ENetPacket *packet = nullptr;
      {
        std::unique_lock<std::mutex> lock(audio_mtx);
        audio_cv.wait(lock,
                      [&] { return !audio_queue.empty() || !audio_running; });
        if (audio_queue.empty() && !audio_running)
          break;
        packet = audio_queue.front();
        audio_queue.pop();
      }

      if (packet) {
        uint8_t *payload = packet->data + sizeof(FrameHeader);
        size_t payload_size = packet->dataLength - sizeof(FrameHeader);

        av_packet_unref(a_pkt);
        if (av_new_packet(a_pkt, static_cast<int>(payload_size)) >= 0) {
          memcpy(a_pkt->data, payload, payload_size);
          if (avcodec_send_packet(audio_ctx, a_pkt) >= 0) {
            while (avcodec_receive_frame(audio_ctx, a_frame) >= 0) {
              if (audio_stream) {
                // Determine layout and convert if necessary
                void *data_to_push = a_frame->data[0];
                size_t data_size = a_frame->nb_samples * 2 * sizeof(float);
                std::vector<float> interleaved;

                if (av_sample_fmt_is_planar((AVSampleFormat)a_frame->format)) {
                  interleaved.resize(a_frame->nb_samples * 2);
                  float *l = (float *)a_frame->data[0];
                  float *r = (float *)a_frame->data[1];
                  for (int i = 0; i < a_frame->nb_samples; ++i) {
                    interleaved[i * 2] = l[i];
                    interleaved[i * 2 + 1] = r[i];
                  }
                  data_to_push = interleaved.data();
                }

                SDL_PutAudioStreamData(audio_stream, data_to_push, data_size);

                // Jitter Buffer & Drift Management
                int queued_bytes = SDL_GetAudioStreamQueued(audio_stream);
                // 48kHz Stereo Float = 48000 * 2 * 4 bytes/sec = 384000
                // bytes/sec
                // 60ms = 384000 * 0.06 = 23040 bytes
                // 40ms = 384000 * 0.04 = 15360 bytes

                static int drift_counter = 0;
                if (drift_counter++ % 50 == 0) {
                  float ratio = 1.0f;
                  if (queued_bytes > 30000) { // > 78ms
                    ratio = 1.02f;
                  } else if (queued_bytes > 20000) { // > 52ms
                    ratio = 1.01f;
                  } else if (queued_bytes < 5000) { // < 13ms
                    ratio = 0.95f;
                  } else if (queued_bytes < 10000) { // < 26ms
                    ratio = 0.99f;
                  }
                  SDL_SetAudioStreamFrequencyRatio(audio_stream, ratio);

                  if (drift_counter % 500 == 0) {
                    std::cout << "[ClientAudio] Buffer: " << queued_bytes
                              << " bytes, Ratio: " << ratio << "\n";
                  }
                }
              }
            }
          }
        }
        enet_packet_destroy(packet);
      }
    }
    av_packet_free(&a_pkt);
    av_frame_free(&a_frame);
  });

  while (running) {
    // Handle SDL Events
    SDL_Event sdl_event;
    while (SDL_PollEvent(&sdl_event)) {
      if (sdl_event.type == SDL_EVENT_QUIT)
        running = false;

      if (sdl_event.type == SDL_EVENT_KEY_DOWN ||
          sdl_event.type == SDL_EVENT_KEY_UP) {
        // Client-local hotkeys (not forwarded): Esc quits, F fullscreen.
        if (sdl_event.type == SDL_EVENT_KEY_DOWN) {
          if (sdl_event.key.key == SDLK_ESCAPE)
            running = false;
          if (sdl_event.key.key == SDLK_F) {
            Uint32 flags = SDL_GetWindowFlags(win);
            SDL_SetWindowFullscreen(win, !(flags & SDL_WINDOW_FULLSCREEN));
          }
        }
        if (sdl_event.key.key != SDLK_ESCAPE && sdl_event.key.key != SDLK_F &&
            !sdl_event.key.repeat) {
          KeyPacket kp;
          kp.key = (uint32_t)sdl_event.key.key;
          kp.down = sdl_event.key.down ? 1 : 0;
          kp.reserved[0] = kp.reserved[1] = kp.reserved[2] = 0;
          send_input_packet((uint8_t)PacketType::KEYBOARD_EVENT, &kp,
                            sizeof(kp));
        }
      }

      if (sdl_event.type == SDL_EVENT_GAMEPAD_ADDED)
        open_gamepad();
      else if (sdl_event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        if (gamepad) {
          SDL_CloseGamepad(gamepad);
          gamepad = nullptr;
        }
      }
    }

    // Handle ENet Events
    while (enet_host_service(client, &event, 0) > 0) {
      if (event.type == ENET_EVENT_TYPE_RECEIVE) {
        static int total_pkts = 0;
        if (total_pkts++ % 500 == 0) {
          std::cout << "[Client] Total packets received: " << total_pkts
                    << " (Last channel: " << (int)event.channelID << ")\n";
        }

        if (event.channelID == 1) { // Video channel
          av_packet_unref(pkt);
          if (av_new_packet(pkt, static_cast<int>(event.packet->dataLength)) >=
              0) {
            memcpy(pkt->data, event.packet->data, event.packet->dataLength);

            if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
              while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                // Video rendering...
                Uint32 sdl_format = (frame->format == AV_PIX_FMT_NV12)
                                        ? SDL_PIXELFORMAT_NV12
                                        : SDL_PIXELFORMAT_IYUV;

                if (frame->width != texture_width ||
                    frame->height != texture_height || !texture) {
                  if (texture)
                    SDL_DestroyTexture(texture);
                  texture = SDL_CreateTexture(
                      renderer, static_cast<SDL_PixelFormat>(sdl_format),
                      SDL_TEXTUREACCESS_STREAMING, frame->width, frame->height);
                  texture_width = frame->width;
                  texture_height = frame->height;
                }

                if (frame->format == AV_PIX_FMT_NV12) {
                  SDL_UpdateNVTexture(texture, nullptr, frame->data[0],
                                      frame->linesize[0], frame->data[1],
                                      frame->linesize[1]);
                } else {
                  SDL_UpdateYUVTexture(texture, nullptr, frame->data[0],
                                       frame->linesize[0], frame->data[1],
                                       frame->linesize[1], frame->data[2],
                                       frame->linesize[2]);
                }

                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, texture, nullptr, nullptr);
                SDL_RenderPresent(renderer);
              }
            }
          }
        } else if (event.channelID == 2) {
          if (event.packet->dataLength < sizeof(FrameHeader)) {
            enet_packet_destroy(event.packet);
            continue;
          }

          {
            std::lock_guard<std::mutex> lock(audio_mtx);
            audio_queue.push(event.packet);
          }
          audio_cv.notify_one();
          continue; // Packet is now owned by audio thread
        }
        enet_packet_destroy(event.packet);
      } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
        std::cout << "[Client] Connection lost\n";
        running = false;
      }
    }

    // Gamepad state -> InputPacket at ~125 Hz (8 ms), send on change.
    auto now_input = std::chrono::steady_clock::now();
    if (gamepad &&
        now_input - last_input_time >= std::chrono::milliseconds(8)) {
      last_input_time = now_input;
      InputPacket ip = build_input_packet(gamepad);
      if (!has_last_ip || memcmp(&ip, &last_ip, sizeof(ip)) != 0) {
        last_ip = ip;
        has_last_ip = true;
        send_input_packet((uint8_t)PacketType::INPUT_EVENT, &ip, sizeof(ip));
      }
    }
  }

  std::cout << "[Client] Cleaning up...\n";
  audio_running = false;
  audio_cv.notify_all();
  if (audio_thread.joinable())
    audio_thread.join();
  if (texture)
    SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(win);
  if (gamepad)
    SDL_CloseGamepad(gamepad);
  SDL_Quit();

  av_frame_free(&frame);
  av_frame_free(&audio_frame);
  av_packet_free(&pkt);
  avcodec_free_context(&codec_ctx);
  if (audio_ctx)
    avcodec_free_context(&audio_ctx);
  if (audio_stream)
    SDL_DestroyAudioStream(audio_stream);

  enet_host_destroy(client);
  enet_deinitialize();
}

} // namespace rps
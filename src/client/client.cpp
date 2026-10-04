#include "client.h"
#include "../shared/protocol/protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
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

void start_client(const char *ip_addr, int port, bool &running,
                  const std::string &launch_name) {
  if (enet_initialize() != 0) {
    std::cerr << "[Client] Failed to initialize ENet\n";
    return;
  }

  ENetHost *client = enet_host_create(nullptr, 1, 4, 0, 0);
  if (client == nullptr) {
    std::cerr << "[Client] Failed to create ENet client\n";
    enet_deinitialize();
    return;
  }

  ENetAddress address;
  enet_address_set_host(&address, ip_addr);
  address.port = static_cast<enet_uint16>(port);

  std::cout << "[Client] Connecting to " << ip_addr << ":" << port << "...\n";
  ENetPeer *peer = enet_host_connect(client, &address, 4, 0);
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

  auto send_input_packet = [&](uint8_t type, const void *payload, size_t size,
                               int channel, enet_uint32 flags) {
    if (!peer)
      return;
    FrameHeader header;
    header.type = type;
    header.flags = FLAG_NONE;
    header.sequence_num = 0;
    header.frame_id = 0;
    header.timestamp_us = get_timestamp_us();
    header.payload_size = (uint32_t)size;
    ENetPacket *packet =
        enet_packet_create(nullptr, sizeof(FrameHeader) + size, flags);
    memcpy(packet->data, &header, sizeof(FrameHeader));
    if (size)
      memcpy(packet->data + sizeof(FrameHeader), payload, size);
    enet_peer_send(peer, channel, packet);
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
  auto last_input_time = std::chrono::steady_clock::now();

  // --- Game list / control plane state ---
  enum class OverlayPage { EMULATORS, GRID };
  struct GameItem {
    std::string name;
    uint32_t index;
    uint8_t emu; // 0 = pcsx2, 1 = rpcs3
  };
  std::vector<GameItem> emu_pcsx2, emu_rpcs3;
  std::vector<int> emu_present; // emu ids with games, in display order
  bool overlay_open = false;
  bool list_loaded = false;
  OverlayPage overlay_page = OverlayPage::EMULATORS;
  int emu_selected = 0; // index into emu_present
  size_t grid_selected = 0;
  int grid_scroll = 0;
  bool connected = true;
  uint8_t game_status = (uint8_t)GameStatus::GAME_IDLE;
  std::string game_status_name;
  std::string game_status_reason;
  std::string status_toast; // transient "Game closed: X" notice
  auto toast_since = std::chrono::steady_clock::now();

  auto current_games = [&]() -> std::vector<GameItem> & {
    return emu_present[emu_selected] == 0 ? emu_pcsx2 : emu_rpcs3;
  };
  auto emu_label = [](int emu) -> const char * {
    return emu == 0 ? "PCSX2" : "RPCS3";
  };

  // --- Streaming HUD state (FPS, round-trip, loss from ENet peer stats) ---
  uint32_t hud_frames = 0;
  float hud_fps = 0.0f;
  auto hud_last = std::chrono::steady_clock::now();

  auto send_control = [&](ControlType type, const void *payload, size_t size) {
    if (!peer)
      return;
    size_t total = sizeof(FrameHeader) + 1 + size;
    ENetPacket *packet =
        enet_packet_create(nullptr, total, ENET_PACKET_FLAG_RELIABLE);
    FrameHeader header;
    header.type = (uint8_t)PacketType::CONTROL;
    header.flags = FLAG_NONE;
    header.sequence_num = 0;
    header.frame_id = 0;
    header.timestamp_us = get_timestamp_us();
    header.payload_size = (uint32_t)(1 + size);
    memcpy(packet->data, &header, sizeof(FrameHeader));
    uint8_t ctype = (uint8_t)type;
    memcpy(packet->data + sizeof(FrameHeader), &ctype, 1);
    if (size)
      memcpy(packet->data + sizeof(FrameHeader) + 1, payload, size);
    enet_peer_send(peer, 0, packet);
  };

  // Request the game list from the host.
  send_control(ControlType::LIST_GAMES, nullptr, 0);

  auto game_status_text = [&]() -> std::string {
    if (!connected)
      return "Disconnected from host";
    switch ((GameStatus)game_status) {
    case GameStatus::GAME_LAUNCHING:
      return "Launching: " + game_status_name;
    case GameStatus::GAME_RUNNING:
      return "Running: " + game_status_name;
    case GameStatus::GAME_LAUNCH_FAILED:
      return "Launch failed: " + game_status_name + " - " +
             game_status_reason;
    case GameStatus::GAME_IDLE:
    default:
      if (!status_toast.empty() &&
          std::chrono::steady_clock::now() - toast_since <
              std::chrono::seconds(4))
        return status_toast;
      return "Idle";
    }
  };

  auto draw_status = [&](SDL_Renderer *r) {
    bool toast_active =
        !status_toast.empty() &&
        std::chrono::steady_clock::now() - toast_since <
            std::chrono::seconds(4);
    bool show = !connected || game_status != (uint8_t)GameStatus::GAME_IDLE ||
                overlay_open || toast_active;
    if (!show)
      return;
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(r, &w, &h);
    SDL_FRect bg = {0.0f, 0.0f, (float)w, 28.0f};
    SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderDebugText(r, 10.0f, 6.0f, game_status_text().c_str());
  };

  // Small top-right overlay: decoded FPS, round-trip time, packet loss.
  auto draw_hud = [&](SDL_Renderer *r) {
    if (!peer)
      return;
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(r, &w, &h);
    // ENet 1.3.18: packetLoss is a ratio scaled by
    // ENET_PEER_PACKET_LOSS_SCALE (65536); roundTripTime is uint32 ms.
    float loss = peer->packetLoss * 100.0f / (float)ENET_PEER_PACKET_LOSS_SCALE;
    enet_uint32 rtt = peer->roundTripTime;
    if (rtt == 0)
      rtt = peer->lastRoundTripTime;
    char line[128];
    std::snprintf(line, sizeof(line), "%.0f fps  %u ms  %.1f%% loss",
                  (double)hud_fps, rtt, (double)loss);
    int tw = (int)std::strlen(line) * 8 + 16;
    SDL_FRect bg = {(float)(w - tw), 4.0f, (float)tw, 22.0f};
    SDL_SetRenderDrawColor(r, 0, 0, 0, 150);
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    SDL_RenderDebugText(r, (float)(w - tw + 8), 6.0f, line);
  };

  // Game launcher overlay: emulator pages -> game grid with placeholder tiles.
  auto draw_overlay = [&](SDL_Renderer *r) {
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(r, &w, &h);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 200);
    SDL_RenderFillRect(r, nullptr);

    if (overlay_page == OverlayPage::EMULATORS) {
      SDL_SetRenderDrawColor(r, 220, 220, 220, 255);
      SDL_RenderDebugText(
          r, 24.0f, 20.0f,
          "Choose an emulator  (arrows: move, Enter: select, Esc: close)");
      if (!list_loaded) {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderDebugText(r, 24.0f, 64.0f, "Loading games...");
      } else if (emu_present.empty()) {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderDebugText(r, 24.0f, 64.0f, "No games found");
      } else {
        const int tile_w = 260, tile_h = 170, gap = 24;
        int total_w = (int)emu_present.size() * tile_w +
                      (int)(emu_present.size() - 1) * gap;
        int x0 = (w - total_w) / 2;
        int y0 = (h - tile_h) / 2 - 10;
        for (int i = 0; i < (int)emu_present.size(); ++i) {
          int x = x0 + i * (tile_w + gap);
          int emu = emu_present[i];
          bool sel = (i == emu_selected);
          SDL_FRect tile = {(float)x, (float)y0, (float)tile_w, (float)tile_h};
          SDL_SetRenderDrawColor(r, sel ? 80 : 45, sel ? 130 : 45,
                                 sel ? 220 : 55, 255);
          SDL_RenderFillRect(r, &tile);
          SDL_SetRenderDrawColor(r, sel ? 255 : 130, sel ? 255 : 130,
                                 sel ? 255 : 130, 255);
          SDL_RenderRect(r, &tile);
          const char *label = emu_label(emu);
          SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
          int tw = (int)std::strlen(label) * 8;
          SDL_RenderDebugText(r, (float)(x + (tile_w - tw) / 2),
                              (float)(y0 + 34), label);
          size_t cnt = emu == 0 ? emu_pcsx2.size() : emu_rpcs3.size();
          char cbuf[64];
          std::snprintf(cbuf, sizeof(cbuf), "%zu games", cnt);
          int cw = (int)std::strlen(cbuf) * 8;
          SDL_RenderDebugText(r, (float)(x + (tile_w - cw) / 2),
                              (float)(y0 + tile_h - 28), cbuf);
        }
      }
    } else { // GRID
      if (emu_present.empty()) {
        overlay_page = OverlayPage::EMULATORS;
        return;
      }
      const int tile_w = 180, tile_h = 132, gap = 14, margin = 30;
      int cols = (std::max)(1, (w - 2 * margin + gap) / (tile_w + gap));
      int rows_vis = (std::max)(1, (h - margin - 40) / (tile_h + gap));
      auto &games = current_games();
      size_t total = games.size();

      char hbuf[128];
      std::snprintf(hbuf, sizeof(hbuf),
                    "%s  (Enter: launch, Esc: back, Home: close)",
                    emu_label(emu_present[emu_selected]));
      SDL_SetRenderDrawColor(r, 220, 220, 220, 255);
      SDL_RenderDebugText(r, 24.0f, 18.0f, hbuf);
      if (total == 0) {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderDebugText(r, 24.0f, 60.0f, "No games found");
      }

      size_t start = (size_t)grid_scroll * cols;
      for (size_t i = start; i < total; ++i) {
        int col = (int)((i - start) % cols);
        int row = (int)((i - start) / cols);
        if (row >= rows_vis)
          break;
        int x = margin + col * (tile_w + gap);
        int y = margin + 14 + row * (tile_h + gap);
        bool sel = (i == grid_selected);
        bool run =
            (game_status == (uint8_t)GameStatus::GAME_RUNNING ||
             game_status == (uint8_t)GameStatus::GAME_LAUNCHING) &&
            games[i].name == game_status_name;

        // Placeholder art area — game image goes here later.
        SDL_FRect art = {(float)x, (float)y, (float)tile_w,
                         (float)(tile_h - 34)};
        if (sel)
          SDL_SetRenderDrawColor(r, 70, 110, 200, 255);
        else if (run)
          SDL_SetRenderDrawColor(r, 30, 120, 60, 255);
        else
          SDL_SetRenderDrawColor(r, 40, 40, 50, 255);
        SDL_RenderFillRect(r, &art);

        SDL_FRect strip = {(float)x, (float)(y + tile_h - 34), (float)tile_w,
                           34.0f};
        SDL_SetRenderDrawColor(r, 20, 20, 26, 255);
        SDL_RenderFillRect(r, &strip);

        std::string nm = games[i].name;
        int maxc = tile_w / 8;
        if ((int)nm.size() > maxc)
          nm = nm.substr(0, (size_t)maxc - 1) + "~";
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderDebugText(r, (float)(x + 6), (float)(y + tile_h - 30),
                            nm.c_str());
        if (run)
          SDL_RenderDebugText(r, (float)(x + tile_w - 36), (float)(y + 6),
                              "RUN");
      }

      int total_rows = (int)((total + cols - 1) / cols);
      if (grid_scroll > 0) {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderDebugText(r, (float)(w / 2 - 4), (float)(margin - 14), "^");
      }
      if (grid_scroll + rows_vis < total_rows) {
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderDebugText(r, (float)(w / 2 - 4), (float)(h - 22), "v");
      }
    }
  };

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

  auto last_render = std::chrono::steady_clock::now();

  while (running) {
    // Decay/refresh the decoded-FPS figure for the HUD.
    auto hud_now = std::chrono::steady_clock::now();
    auto hud_dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                      hud_now - hud_last)
                      .count();
    if (hud_dt >= 500) {
      hud_fps = hud_frames * 1000.0f / (float)hud_dt;
      hud_frames = 0;
      hud_last = hud_now;
    }
    // Handle SDL Events
    SDL_Event sdl_event;
    while (SDL_PollEvent(&sdl_event)) {
      if (sdl_event.type == SDL_EVENT_QUIT)
        running = false;

      if (sdl_event.type == SDL_EVENT_KEY_DOWN ||
          sdl_event.type == SDL_EVENT_KEY_UP) {
        bool is_down = (sdl_event.type == SDL_EVENT_KEY_DOWN);
        SDL_Keycode key = sdl_event.key.key;

        // Home toggles the game list (reserved, never forwarded).
        if (key == SDLK_HOME && is_down && !sdl_event.key.repeat) {
          overlay_open = !overlay_open;
          if (overlay_open) {
            overlay_page = OverlayPage::EMULATORS;
            send_control(ControlType::LIST_GAMES, nullptr, 0);
          }
          continue;
        }

        // While the list is open, consume all keys for navigation.
        if (overlay_open) {
          if (is_down && !sdl_event.key.repeat) {
            if (overlay_page == OverlayPage::EMULATORS) {
              if ((key == SDLK_LEFT || key == SDLK_UP) && emu_selected > 0)
                emu_selected--;
              else if ((key == SDLK_RIGHT || key == SDLK_DOWN) &&
                       emu_selected + 1 < (int)emu_present.size())
                emu_selected++;
              else if (key == SDLK_RETURN && !emu_present.empty()) {
                grid_selected = 0;
                grid_scroll = 0;
                overlay_page = OverlayPage::GRID;
              } else if (key == SDLK_ESCAPE)
                overlay_open = false;
            } else { // GRID
              auto &games = current_games();
              size_t total = games.size();
              int w = 0, h = 0;
              SDL_GetRenderOutputSize(renderer, &w, &h);
              const int tile_w = 180, tile_h = 132, gap = 14, margin = 30;
              int cols = (std::max)(1, (w - 2 * margin + gap) / (tile_w + gap));
              int rows_vis = (std::max)(1, (h - margin - 40) / (tile_h + gap));

              if (key == SDLK_LEFT && grid_selected > 0)
                grid_selected--;
              else if (key == SDLK_RIGHT && grid_selected + 1 < total)
                grid_selected++;
              else if (key == SDLK_UP && grid_selected >= (size_t)cols)
                grid_selected -= cols;
              else if (key == SDLK_DOWN) {
                size_t nxt = grid_selected + cols;
                grid_selected = (nxt < total) ? nxt
                                              : (total ? total - 1 : 0);
              } else if (key == SDLK_RETURN && grid_selected < total) {
                uint32_t idx = games[grid_selected].index;
                send_control(ControlType::LAUNCH_GAME, &idx, 4);
                overlay_open = false;
              } else if (key == SDLK_ESCAPE)
                overlay_page = OverlayPage::EMULATORS;

              if (grid_selected >= total)
                grid_selected = total ? total - 1 : 0;
              int row = (int)(grid_selected / cols);
              if (row < grid_scroll)
                grid_scroll = row;
              else if (row >= grid_scroll + rows_vis)
                grid_scroll = row - rows_vis + 1;
            }
          }
          continue;
        }

        // Otherwise forward every key (ignore repeats).
        if (!sdl_event.key.repeat) {
          KeyPacket kp;
          kp.key = (uint32_t)key;
          kp.down = is_down ? 1 : 0;
          kp.reserved[0] = kp.reserved[1] = kp.reserved[2] = 0;
          send_input_packet((uint8_t)PacketType::KEYBOARD_EVENT, &kp,
                            sizeof(kp), 3, ENET_PACKET_FLAG_RELIABLE);
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

        if (event.channelID == 0) {
          // Control messages (game list, etc.)
          if (event.packet->dataLength >= sizeof(FrameHeader) + 1) {
            const FrameHeader *hdr =
                reinterpret_cast<const FrameHeader *>(event.packet->data);
            if (hdr->type == (uint8_t)PacketType::CONTROL) {
              uint8_t ctype = event.packet->data[sizeof(FrameHeader)];
              if (ctype == (uint8_t)ControlType::GAME_LIST) {
                const char *data = reinterpret_cast<const char *>(
                    event.packet->data + sizeof(FrameHeader) + 1);
                size_t len = event.packet->dataLength - sizeof(FrameHeader) - 1;
                emu_pcsx2.clear();
                emu_rpcs3.clear();
                std::istringstream ss(std::string(data, len));
                std::string line;
                while (std::getline(ss, line)) {
                  if (line.empty())
                    continue;
                  size_t p1 = line.find('|');
                  if (p1 == std::string::npos)
                    continue;
                  size_t p2 = line.find('|', p1 + 1);
                  uint32_t idx = (uint32_t)std::stoul(line.substr(0, p1));
                  std::string emu =
                      (p2 != std::string::npos)
                          ? line.substr(p1 + 1, p2 - p1 - 1)
                          : "pcsx2";
                  std::string name = (p2 != std::string::npos)
                                         ? line.substr(p2 + 1)
                                         : line.substr(p1 + 1);
                  GameItem item{name, idx, (uint8_t)(emu == "rpcs3")};
                  if (item.emu == 0)
                    emu_pcsx2.push_back(item);
                  else
                    emu_rpcs3.push_back(item);
                }
                list_loaded = true;
                emu_present.clear();
                if (!emu_pcsx2.empty())
                  emu_present.push_back(0);
                if (!emu_rpcs3.empty())
                  emu_present.push_back(1);
                if (emu_selected >= (int)emu_present.size())
                  emu_selected = 0;
                grid_selected = 0;
                grid_scroll = 0;

                if (!launch_name.empty()) {
                  bool found = false;
                  for (const auto &it : emu_pcsx2)
                    if (it.name == launch_name) {
                      send_control(ControlType::LAUNCH_GAME, &it.index, 4);
                      found = true;
                      break;
                    }
                  if (!found)
                    for (const auto &it : emu_rpcs3)
                      if (it.name == launch_name) {
                        send_control(ControlType::LAUNCH_GAME, &it.index, 4);
                        break;
                      }
                  overlay_open = false;
                } else if (!emu_present.empty()) {
                  overlay_open = true;
                  overlay_page = OverlayPage::EMULATORS;
                }
              } else if (ctype == (uint8_t)ControlType::GAME_STATUS) {
                const uint8_t *d = reinterpret_cast<const uint8_t *>(
                    event.packet->data + sizeof(FrameHeader) + 1);
                size_t len = event.packet->dataLength - sizeof(FrameHeader) - 1;
                uint8_t prev_status = game_status;
                std::string prev_name = game_status_name;
                game_status_name.clear();
                game_status_reason.clear();
                if (len >= 2) {
                  game_status = d[0];
                  uint8_t name_len = d[1];
                  size_t off = 2;
                  if (off + name_len <= len) {
                    game_status_name.assign((const char *)d + off, name_len);
                    off += name_len;
                  }
                  if (off < len) {
                    uint8_t reason_len = d[off++];
                    if (off + reason_len <= len)
                      game_status_reason.assign((const char *)d + off,
                                                reason_len);
                  }
                }
                if (prev_status == (uint8_t)GameStatus::GAME_RUNNING &&
                    game_status == (uint8_t)GameStatus::GAME_IDLE) {
                  status_toast = "Game closed: " + prev_name;
                  toast_since = std::chrono::steady_clock::now();
                } else if (prev_status != (uint8_t)GameStatus::GAME_IDLE ||
                           game_status != (uint8_t)GameStatus::GAME_IDLE) {
                  status_toast.clear();
                }
              }
            }
          }
          enet_packet_destroy(event.packet);
          continue;
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

hud_frames++;
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
        connected = false;
        running = false;
      }
    }

    // Gamepad state -> InputPacket at ~125 Hz (8 ms). Sent every tick, not just
    // on change: unreliable packets can drop, and resending the full state
    // lets the host recover within one tick instead of missing an input.
    auto now_input = std::chrono::steady_clock::now();
    if (gamepad &&
        now_input - last_input_time >= std::chrono::milliseconds(8)) {
      last_input_time = now_input;
      InputPacket ip = build_input_packet(gamepad);
      send_input_packet((uint8_t)PacketType::INPUT_EVENT, &ip, sizeof(ip), 3, 0);
    }

    // Render once per tick while the launcher is open (menu/status respond
    // instantly even if video stalls); otherwise cap to ~60 fps and reuse the
    // last decoded texture.
    auto render_now = std::chrono::steady_clock::now();
    bool need_render =
        overlay_open || !texture ||
        std::chrono::duration_cast<std::chrono::milliseconds>(render_now -
                                                              last_render)
                .count() >= 16;
    if (need_render) {
      last_render = render_now;
      SDL_SetRenderDrawColor(renderer, 16, 16, 16, 255);
      SDL_RenderClear(renderer);
      if (texture)
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
      draw_status(renderer);
      if (overlay_open)
        draw_overlay(renderer);
      else if (texture)
        draw_hud(renderer);
      else {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDebugText(
            renderer, 20.0f, 20.0f,
            connected ? "Connecting to host..." : "Disconnected from host");
      }
      SDL_RenderPresent(renderer);
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
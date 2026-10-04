#include "host.h"
#include "../shared/protocol/protocol.h"
#include "../shared/keys.h"
#include "capture/audio_capture.h"
#include "capture/capture.h"
#include "capture/dxgi_capture.h"
#include "encoder/encoder.h"
#include "input/input_injector.h"
#include "input/input_mapper.h"
#include "input/hidmaestro_injector.h"
#include "process/game_config.h"
#include "process/process_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <enet/enet.h>

#include <cstdio>

namespace rps {

void start_host_server(int port, bool &running, bool debug_audio,
                       const std::string &config_path) {
  if (enet_initialize() != 0) {
    std::cerr << "[Host] Failed to initialize ENet\n";
    return;
  }

  FILE *audio_dump = nullptr;
  if (debug_audio) {
    audio_dump = fopen("host_capture_debug.raw", "wb");
    std::cout
        << "[Host] Debug audio recording enabled: host_capture_debug.raw\n";
  }

  ENetAddress address;
  address.host = ENET_HOST_ANY;
  address.port = static_cast<enet_uint16>(port);

  // Create host with 1 client connection, 4 channels (0: reliable control, 1:
  // video, 2: audio, 3: input).
  ENetHost *server = enet_host_create(&address, 1, 4, 0, 0);
  if (server == nullptr) {
    std::cerr << "[Host] Failed to create ENet server\n";
    enet_deinitialize();
    return;
  }

  std::cout << "[Host] Waiting for client on UDP port " << port << "...\n";

  auto capture = createCapture();
  CaptureConfig capture_config;
  if (!capture->init(capture_config)) {
    std::cerr << "[Host] Failed to initialize capture\n";
    enet_host_destroy(server);
    enet_deinitialize();
    return;
  }

  int width, height;
  capture->getDimensions(width, height);
  std::cout << "[Host] Capture dimensions: " << width << "x" << height << "\n";

  EncoderSettings enc_settings;
  enc_settings.width = width;
  enc_settings.height = height;
  enc_settings.fps = 60;
  enc_settings.bitrate = 8000000;
  enc_settings.preferred = EncoderType::NVENC;
  enc_settings.input_format = AV_PIX_FMT_BGR0; // DXGI standard
  enc_settings.fps = 60;                       // Ensure FPS is explicitly 60

  EncoderContext enc_ctx;
  if (!init_encoder(enc_settings, enc_ctx)) {
    std::cerr << "[Host] Failed to initialize video encoder\n";
    enet_host_destroy(server);
    enet_deinitialize();
    return;
  }

  // --- Audio Setup ---
  auto audio_capture = createAudioCapture();
  AudioEncoderContext audio_enc_ctx;
  AudioEncoderSettings audio_settings;
  audio_settings.bitrate = 64000; // 64kbps is plenty for Opus and safer for UDP
  bool audio_enabled = false;

  if (audio_capture && audio_capture->init(AudioConfig())) {
    if (init_audio_encoder(audio_settings, audio_enc_ctx)) {
      std::cout
          << "[Host] Audio capture and encoder initialized (Bitrate: 64kbps)\n";
      audio_enabled = true;
      audio_capture->start();
    }
  }

  if (!capture->start()) {
    std::cerr << "[Host] Failed to start video capture\n";
    destroy_encoder(enc_ctx);
    if (audio_enabled)
      destroy_audio_encoder(audio_enc_ctx);
    enet_host_destroy(server);
    enet_deinitialize();
    return;
  }

  std::atomic<ENetPeer *> client_peer{nullptr};
  bool streaming = false;

  // Encoded audio packets (header + payload) produced by the audio thread and
  // drained by the main loop, so ENet stays single-threaded.
  std::queue<std::vector<uint8_t>> audio_send_queue;
  std::mutex audio_send_mtx;

  // --- Input injection ---
  auto injector = createInputInjector();
  InputMapper mapper;
  if (!injector || !injector->init()) {
    std::cerr << "[Host] Failed to initialize input injector\n";
  }

  // --- Game control plane ---
  GameConfig game_config;
  bool control_plane = false;
  if (!config_path.empty() && load_game_config(config_path, game_config)) {
    control_plane = true;
    std::cout << "[Host] Loaded " << game_config.games.size()
              << " games from " << config_path << "\n";
  } else if (!config_path.empty()) {
    std::cerr << "[Host] Control plane disabled (config load failed)\n";
  }
  ProcessManager process_manager;

  // Optional analog gamepad via HIDMaestro bridge (opt-in: [input] analog=true).
  // Initialized synchronously BEFORE the serve loop: the bridge's UAC prompt
  // and virtual-device PnP setup steal focus, so it must finish before the
  // client can connect or launch a game. The client retries its connection
  // until the host starts serving, so this does not break connectivity.
  std::unique_ptr<HidMaestroInjector> analog;
  if (game_config.analog_input) {
    analog = std::make_unique<HidMaestroInjector>();
    if (analog->init()) {
      std::cout << "[Host] Analog controller injection enabled (HIDMaestro)\n";
    } else {
      analog.reset();
      std::cout << "[Host] Analog injection unavailable; using digital "
                   "key-mapping\n";
    }
  }

  // Keyboard events and digital gamepad mapping both inject global SendInput
  // state. Release it on every lifecycle boundary so dropped key-up/focus
  // events cannot leave modifiers or buttons latched in the host desktop.
  auto release_input_state = [&]() {
    if (injector)
      injector->releaseAll();
    mapper.reset();
    if (analog) {
      InputPacket neutral = {};
      neutral.timestamp_us = get_timestamp_us();
      analog->sendGamepadState(neutral);
    }
  };

  // Send a control message (FrameHeader{CONTROL} + control_type + payload)
  auto send_control = [&](ENetPeer *peer, ControlType type, const void *payload,
                          size_t size) {
    size_t total = sizeof(FrameHeader) + 1 + size;
    ENetPacket *packet = enet_packet_create(nullptr, total,
                                            ENET_PACKET_FLAG_RELIABLE);
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

  // --- Game lifecycle status (GAME_STATUS) ---
  uint8_t game_status = (uint8_t)GameStatus::GAME_IDLE;
  std::string current_game;
  std::string game_status_reason;
  auto send_game_status = [&](ENetPeer *peer) {
    if (!peer)
      return;
    std::vector<uint8_t> payload;
    payload.push_back(game_status);
    uint8_t name_len = (uint8_t)std::min<size_t>(current_game.size(), 255);
    payload.push_back(name_len);
    payload.insert(payload.end(), current_game.begin(),
                   current_game.begin() + name_len);
    uint8_t reason_len =
        (uint8_t)std::min<size_t>(game_status_reason.size(), 255);
    payload.push_back(reason_len);
    payload.insert(payload.end(), game_status_reason.begin(),
                   game_status_reason.begin() + reason_len);
    send_control(peer, ControlType::GAME_STATUS, payload.data(), payload.size());
  };

  auto frame_duration = std::chrono::microseconds(1000000 / enc_settings.fps);
  auto last_frame_time = std::chrono::steady_clock::now();
  ULONGLONG last_focus_tick = 0;
  bool focus_done = false;

  // Dedicated audio thread: capture + encode run independently of the video
  // path so video encode spikes cannot starve audio capture (was causing
  // stutter).
  std::thread audio_worker;
  if (audio_enabled) {
    audio_worker = std::thread([&]() {
      while (running) {
        ENetPeer *peer = client_peer.load();
        if (!peer) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }

        AudioFrame audio_frame;
        if (!audio_capture->acquireFrame(audio_frame)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        if (audio_frame.samples.empty())
          continue;

        uint8_t *data_ptr = (uint8_t *)audio_frame.samples.data();
        int nb_samples = (int)audio_frame.samples.size() / 2;

        if (audio_dump) {
          fwrite(audio_frame.samples.data(), sizeof(float),
                 audio_frame.samples.size(), audio_dump);
        }

        static auto last_audit = std::chrono::steady_clock::now();
        static int audit_samples = 0;
        audit_samples += (int)audio_frame.samples.size() / 2;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_audit)
                .count() >= 1) {
          std::cout << "[Host] Actual Audio Capture Rate: " << audit_samples
                    << " Hz\n";
          audit_samples = 0;
          last_audit = now;
        }

        av_audio_fifo_write(audio_enc_ctx.fifo, (void **)&data_ptr,
                            nb_samples);

        while (av_audio_fifo_size(audio_enc_ctx.fifo) >=
               audio_enc_ctx.codec_ctx->frame_size) {
          av_audio_fifo_read(audio_enc_ctx.fifo,
                             (void **)audio_enc_ctx.frame->data,
                             audio_enc_ctx.codec_ctx->frame_size);

          audio_enc_ctx.frame->pts = audio_enc_ctx.frame_index;
          audio_enc_ctx.frame_index += audio_enc_ctx.frame->nb_samples;

          if (avcodec_send_frame(audio_enc_ctx.codec_ctx,
                                 audio_enc_ctx.frame) >= 0) {
            while (avcodec_receive_packet(audio_enc_ctx.codec_ctx,
                                          audio_enc_ctx.pkt) >= 0) {
              static int audio_pkt_count = 0;
              static uint16_t audio_seq_num = 0;

              if (audio_pkt_count++ % 100 == 0) {
                std::cout << "[Host] Sent 100 audio packets\n";
              }

              FrameHeader header;
              header.type = (uint8_t)PacketType::AUDIO_FRAME;
              header.flags = FLAG_NONE;
              header.sequence_num = audio_seq_num++;
              header.frame_id = audio_enc_ctx.frame_index;
              header.timestamp_us = get_timestamp_us();
              header.payload_size = (uint32_t)audio_enc_ctx.pkt->size;

              std::vector<uint8_t> buf(sizeof(FrameHeader) +
                                       audio_enc_ctx.pkt->size);
              memcpy(buf.data(), &header, sizeof(FrameHeader));
              memcpy(buf.data() + sizeof(FrameHeader), audio_enc_ctx.pkt->data,
                     audio_enc_ctx.pkt->size);

              {
                std::lock_guard<std::mutex> lock(audio_send_mtx);
                audio_send_queue.push(std::move(buf));
              }

              av_packet_unref(audio_enc_ctx.pkt);
            }
          }
        }
      }
    });
  }

  while (running) {
    ENetEvent event;
    while (enet_host_service(server, &event, 0) > 0) {
      switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT:
        std::cout << "[Host] Client connected from "
                  << (event.peer->address.host & 0xFF) << "."
                  << ((event.peer->address.host >> 8) & 0xFF) << "."
                  << ((event.peer->address.host >> 16) & 0xFF) << "."
                  << ((event.peer->address.host >> 24) & 0xFF) << ":"
                  << event.peer->address.port << "\n";
        client_peer.store(event.peer);
        streaming = true;
        send_game_status(event.peer);
        break;

      case ENET_EVENT_TYPE_RECEIVE: {
        if (event.packet->dataLength < sizeof(FrameHeader)) {
          enet_packet_destroy(event.packet);
          break;
        }
        const FrameHeader *hdr =
            reinterpret_cast<const FrameHeader *>(event.packet->data);
        // Gamepad frames on channel 3 (unreliable, latest state wins). Keyboard is
        // reliable on channel 0: reliable + unreliable must not share a channel,
        // or a dropped gamepad frame stalls key delivery.
        if (event.channelID == 3) {
          if (hdr->type == (uint8_t)PacketType::INPUT_EVENT &&
              event.packet->dataLength >=
                  sizeof(FrameHeader) + sizeof(InputPacket)) {
            InputPacket ip;
            memcpy(&ip, event.packet->data + sizeof(FrameHeader),
                   sizeof(InputPacket));
            // Diagnostic: confirm gamepad frames reach the host and which path
            // handles them (analog bridge vs digital key-map).
            static uint32_t input_diag = 0;
            if (++input_diag % 125 == 0) {
              std::cout << "[Host] input buttons=0x" << std::hex << ip.buttons
                        << std::dec << " analog=" << (analog ? 1 : 0) << " lx="
                        << ip.left_stick_x << " rx=" << ip.right_stick_x
                        << "\n";
            }
            if (analog && analog->sendGamepadState(ip)) {
              // analog injection handled
            } else if (injector) {
              mapper.processGamepad(ip, [&](uint32_t vk, bool down) {
                injector->sendKey(vk, down);
              });
            }
          }
        }
        else if (event.channelID == 0) {
          if (hdr->type == (uint8_t)PacketType::KEYBOARD_EVENT &&
              event.packet->dataLength >=
                  sizeof(FrameHeader) + sizeof(KeyPacket)) {
            KeyPacket kp;
            memcpy(&kp, event.packet->data + sizeof(FrameHeader),
                   sizeof(KeyPacket));
            uint32_t vk = sdlk_to_vk((SDL_Keycode)kp.key);
            if (injector && vk != 0)
              injector->sendKey(vk, kp.down != 0);
          } else if (hdr->type == (uint8_t)PacketType::CONTROL &&
                     event.packet->dataLength >= sizeof(FrameHeader) + 1) {
            uint8_t ctype = event.packet->data[sizeof(FrameHeader)];
            const uint8_t *payload =
                event.packet->data + sizeof(FrameHeader) + 1;
            size_t payload_size =
                event.packet->dataLength - sizeof(FrameHeader) - 1;

            switch ((ControlType)ctype) {
            case ControlType::LIST_GAMES: {
              if (!control_plane)
                break;
              std::string list;
              for (size_t i = 0; i < game_config.games.size(); ++i) {
                const GameEntry &g = game_config.games[i];
                list += std::to_string(i) + "|" +
                        (g.emulator == EmulatorType::RPCS3 ? "rpcs3" : "pcsx2") +
                        "|" + g.name + "\n";
              }
              send_control(event.peer, ControlType::GAME_LIST, list.data(),
                           list.size());
              break;
            }
            case ControlType::LAUNCH_GAME: {
              if (!control_plane || payload_size < 4)
                break;
              uint32_t index = 0;
              memcpy(&index, payload, 4);
              if (index >= game_config.games.size()) {
                game_status = (uint8_t)GameStatus::GAME_LAUNCH_FAILED;
                current_game.clear();
                game_status_reason = "invalid game index";
                send_game_status(event.peer);
                break;
              }
              current_game = game_config.games[index].name;
              game_status_reason.clear();
              game_status = (uint8_t)GameStatus::GAME_LAUNCHING;
              send_game_status(event.peer);
              if (process_manager.launch(game_config, index)) {
                game_status = (uint8_t)GameStatus::GAME_RUNNING;
                focus_done = false;
              } else {
                game_status = (uint8_t)GameStatus::GAME_LAUNCH_FAILED;
                game_status_reason = "launch failed";
              }
              send_game_status(event.peer);
              break;
            }
            default:
              break;
            }
          }
          }
        enet_packet_destroy(event.packet);
        break;
      }

      case ENET_EVENT_TYPE_DISCONNECT:
        std::cout << "[Host] Client disconnected\n";
        client_peer.store(nullptr);
        streaming = false;
        release_input_state();
        // Free the single peer slot right away so a reconnecting client isn't
        // rejected while this peer lingers in the disconnected state.
        enet_peer_reset(event.peer);
        break;

      default:
        break;
      }
    }

    // Detect emulator exit (PCSX2 --batch quits when the game closes).
    if (game_status == (uint8_t)GameStatus::GAME_RUNNING &&
        !process_manager.isRunning()) {
      std::cout << "[Host] Game closed: " << current_game << "\n";
      game_status = (uint8_t)GameStatus::GAME_IDLE;
      current_game.clear();
      game_status_reason.clear();
      release_input_state();
      if (ENetPeer *p = client_peer.load())
        send_game_status(p);
    }

    // Focus the game window once after launch (bounded retry until it takes
    // foreground), then stop. Re-asserting focus continuously interferes with
    // the game's own keyboard handling (Esc / Alt+Enter fullscreen toggle).
    if (game_status == (uint8_t)GameStatus::GAME_RUNNING && !focus_done &&
        GetTickCount64() - last_focus_tick >= 500) {
      last_focus_tick = GetTickCount64();
      process_manager.ensureForeground();
      if (process_manager.isForeground())
        focus_done = true;
    }

    ENetPeer *peer = client_peer.load();
    if (streaming && peer) {
      // --- Audio send: drain queue produced by audio thread ---
      {
        std::lock_guard<std::mutex> lock(audio_send_mtx);
        while (!audio_send_queue.empty()) {
          std::vector<uint8_t> &buf = audio_send_queue.front();
          ENetPacket *packet = enet_packet_create(
              buf.data(), buf.size(), ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
          enet_peer_send(peer, 2, packet);
          audio_send_queue.pop();
        }
      }

      // --- Video Processing ---
      auto now = std::chrono::steady_clock::now();
      if (now - last_frame_time >= frame_duration) {
        last_frame_time = now;

        CapturedFrame frame;
        if (capture->acquireFrame(frame, 0)) {
          // Check for resolution change
          if (frame.width != enc_settings.width ||
              frame.height != enc_settings.height) {
            std::cout << "[Host] Resolution changed to " << frame.width << "x"
                      << frame.height << "\n";
            destroy_encoder(enc_ctx);
            enc_settings.width = frame.width;
            enc_settings.height = frame.height;
            if (!init_encoder(enc_settings, enc_ctx)) {
              std::cerr << "[Host] Failed to reinit encoder\n";
              running = false;
              break;
            }
          }

          // Convert and encode
          const uint8_t *src_slices[] = {frame.data};
          int src_stride[] = {frame.pitch};
          sws_scale(enc_ctx.sws_ctx, src_slices, src_stride, 0, frame.height,
                    enc_ctx.frame->data, enc_ctx.frame->linesize);

          enc_ctx.frame->pts = enc_ctx.frame_index++;

          if (avcodec_send_frame(enc_ctx.codec_ctx, enc_ctx.frame) >= 0) {
            while (avcodec_receive_packet(enc_ctx.codec_ctx, enc_ctx.pkt) >=
                   0) {
              // Send packet via ENet
              // Channel 1 for video (unreliable)
              ENetPacket *packet =
                  enet_packet_create(enc_ctx.pkt->data, enc_ctx.pkt->size,
                                     ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
              enet_peer_send(peer, 1, packet);

              av_packet_unref(enc_ctx.pkt);
            }
          }
          capture->releaseFrame();
        }
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    enet_host_flush(server);
  }

  std::cout << "[Host] Streaming stopped\n";
  release_input_state();
  if (audio_worker.joinable())
    audio_worker.join();
  if (capture)
    capture->stop();
  if (audio_enabled) {
    audio_capture->stop();
    destroy_audio_encoder(audio_enc_ctx);
  }
  destroy_encoder(enc_ctx);
  if (audio_dump)
    fclose(audio_dump);
  enet_host_destroy(server);
  enet_deinitialize();
}

} // namespace rps

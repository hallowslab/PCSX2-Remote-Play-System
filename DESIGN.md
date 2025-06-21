# 🎮 PCSX2 Remote Play System - Technical Design Document

## 📌 Objective

To build a **local remote play system** for **PCSX2 (PlayStation 2 emulator)** that functions similarly to **Steam Remote Play**, allowing gameplay to be streamed from a host PC to a client device over a local network with low latency.

---

## 🧠 System Overview

### ✅ Functionality Summary

- **Stream** video and audio from host PC (running PCSX2) to a client device.
- **Capture and forward inputs** (controller/keyboard/mouse) from client to host.
- Maintain **low-latency**, synchronized playback and responsiveness.

---

## 🔧 System Components

### 1. Host (Server)
- Runs PCSX2 emulator.
- Captures video and audio output.
- Encodes and streams to client.
- Receives and injects input from client.

### 2. Client
- Receives video/audio stream and displays it.
- Captures user input (keyboard, controller).
- Sends input events to host.

---

## 🏗️ Software Architecture

### 🧱 Modular Component Design
```
/remote-play-system
├── host/
│ ├── capture/ # Video/audio capture modules
│ ├── encoder/ # Encoding using FFmpeg/NVENC
│ ├── input-injection/ # ViGEm / SendInput
│ └── server-main.*
├── client/
│ ├── decoder/ # Decode video/audio streams
│ ├── input-capture/ # SDL2 / controller input
│ ├── video-renderer/ # Render on-screen
│ └── client-main.*
├── shared/
│ ├── protocol/ # Networking protocol definitions
│ └── networking/ # TCP/UDP/WebRTC communication
```
---

## 🧰 Technology Stack

| Area              | Tools / APIs                                 |
|-------------------|----------------------------------------------|
| Screen Capture    | Desktop Duplication API (Windows), GDI       |
| Audio Capture     | WASAPI loopback, VoiceMeeter, VB-Audio       |
| Encoding          | FFmpeg, NVENC, Media Foundation              |
| Transport         | TCP (simple), QUIC/UDP (low latency), WebRTC |
| Input Capture     | SDL2, XInput, evdev                          |
| Input Injection   | ViGEmBus (virtual controllers), SendInput    |
| Decoder (Client)  | FFmpeg, SDL2, OpenGL/Vulkan/DirectX          |
| GUI Framework     | ImGui, Qt, SDL2                              |

---

## 🔌 Data Flow

### 1. Host Side

- Capture screen using DirectX/Desktop Duplication.
- Capture audio using WASAPI loopback.
- Encode audio/video using FFmpeg or NVENC.
- Stream to client over TCP/WebRTC.
- Receive input events.
- Inject inputs via ViGEmBus or Windows APIs.

### 2. Client Side

- Decode stream using FFmpeg.
- Display using OpenGL/Vulkan.
- Capture input from keyboard/controller.
- Send input events to host.

---

## 🔁 Synchronization

- Timestamp video/audio frames.
- Use buffering and time synchronization to handle network jitter.
- Optionally implement adaptive bitrate streaming.

---

## 📦 Networking Protocol (Simplified)

### Commands

| Command         | Description                            |
|-----------------|----------------------------------------|
| `STREAM_INIT`   | Handshake and configuration            |
| `VIDEO_FRAME`   | Encoded video frame                    |
| `AUDIO_FRAME`   | Encoded audio frame                    |
| `INPUT_EVENT`   | Key/controller state                   |
| `PING/PONG`     | Keep-alive + latency measurement       |

Use a binary protocol over TCP for simplicity. Upgrade to UDP/WebRTC if needed later.

---

## 🚀 Future Enhancements

- Encrypted transport (DTLS/WebRTC).
- Automatic resolution negotiation.
- Multi-client support.
- Touch input support (mobile client).
- UI overlay (virtual controls, status, etc.).

---

## 🧪 Development Milestones

### Phase 1: MVP
- [ ] Basic video stream (host to client)
- [ ] Input capture and feedback (client to host)
- [ ] Controller passthrough (ViGEm)
- [ ] Basic TCP-based protocol

### Phase 2: Usability
- [ ] Audio streaming
- [ ] Config UI
- [ ] Reconnection handling
- [ ] Performance tuning

### Phase 3: Advanced Features
- [ ] WebRTC transport
- [ ] Touch UI for mobile
- [ ] Stream quality adjustment

---

## 📚 References

- [PCSX2 GitHub](https://github.com/PCSX2/pcsx2)
- [ViGEmBus](https://github.com/ViGEm/ViGEmBus)
- [FFmpeg Documentation](https://ffmpeg.org/documentation.html)
- [SDL2 Documentation](https://wiki.libsdl.org/)
- [WebRTC](https://webrtc.org/)

---

## 🧩 Notes

- Consider embedding the server into PCSX2 eventually via a plugin or IPC.
- Modular architecture allows plugging in different streaming protocols or encoding pipelines.
- Focus on **low-latency**, **robust input**, and **simple pairing** for best experience.

---

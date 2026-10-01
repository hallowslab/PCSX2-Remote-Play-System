#pragma once

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#else
#include <arpa/inet.h>
#endif // _WIN32

namespace rps {

// Packet types
enum class PacketType : uint8_t {
  VIDEO_FRAME = 0,
  AUDIO_FRAME = 1,
  INPUT_EVENT = 2,
  CONTROL = 3,
  KEYBOARD_EVENT = 4
};

// Packet flags
enum PacketFlags : uint8_t {
  FLAG_NONE = 0x00,
  FLAG_KEYFRAME = 0x01,
  FLAG_END_OF_FRAME = 0x02
};

// Control message types
enum class ControlType : uint8_t {
  STREAM_INIT = 0,
  STREAM_METADATA = 1,
  RESOLUTION_CHANGE = 2,
  REQUEST_KEYFRAME = 3,
  PING = 4,
  PONG = 5,
  STREAM_PAUSED = 6,
  STREAM_RESUMED = 7,
  LIST_GAMES = 8,  // client->host: request game list
  GAME_LIST = 9,   // host->client: "index|emulator|name\n" lines
  LAUNCH_GAME = 10, // client->host: uint32 game index
  CLOSE_GAME = 11   // client->host: no payload
};

#pragma pack(push, 1)

// Header for all packets
struct FrameHeader {
  uint8_t type;  // PacketType
  uint8_t flags; // PacketFlags
  uint16_t sequence_num;
  uint32_t frame_id;
  uint64_t timestamp_us;
  uint32_t payload_size;
};

// Stream metadata sent during initialization
struct StreamMetadata {
  uint32_t width;
  uint32_t height;
  uint32_t fps;
  uint32_t bitrate;
  uint8_t codec_id;       // 0 = H264
  uint8_t audio_codec_id; // 0 = Opus, 1 = PCM
  uint16_t audio_sample_rate;
  uint8_t audio_channels;
  uint8_t reserved[3];
};

// Input packet from client
struct InputPacket {
  uint64_t timestamp_us;
  uint16_t buttons; // Button bitmask
  int16_t left_stick_x;
  int16_t left_stick_y;
  int16_t right_stick_x;
  int16_t right_stick_y;
  uint8_t left_trigger;
  uint8_t right_trigger;
};

// Keyboard event from client. `key` is an SDL_Keycode (platform-neutral); the
// host converts it to its native key code before injection.
struct KeyPacket {
  uint32_t key;
  uint8_t down; // 1 = press, 0 = release
  uint8_t reserved[3];
};

// Button bitmask values (Xbox layout)
enum ButtonMask : uint16_t {
  BTN_DPAD_UP = 0x0001,
  BTN_DPAD_DOWN = 0x0002,
  BTN_DPAD_LEFT = 0x0004,
  BTN_DPAD_RIGHT = 0x0008,
  BTN_START = 0x0010,
  BTN_BACK = 0x0020,
  BTN_LEFT_THUMB = 0x0040,
  BTN_RIGHT_THUMB = 0x0080,
  BTN_LEFT_SHOULDER = 0x0100,
  BTN_RIGHT_SHOULDER = 0x0200,
  BTN_A = 0x1000,
  BTN_B = 0x2000,
  BTN_X = 0x4000,
  BTN_Y = 0x8000
};

#pragma pack(pop)

// Utility to get current timestamp in microseconds
uint64_t get_timestamp_us();

// Network byte order helpers
uint32_t to_network_u32(uint32_t value);
uint32_t from_network_u32(uint32_t value);
uint64_t to_network_u64(uint64_t value);
uint64_t from_network_u64(uint64_t value);

} // namespace rps

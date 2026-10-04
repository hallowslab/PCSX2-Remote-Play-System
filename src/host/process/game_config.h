#pragma once

#include <string>
#include <vector>

namespace rps {

enum class EmulatorType { PCSX2, RPCS3 };

struct GameEntry {
  std::string name;
  EmulatorType emulator = EmulatorType::PCSX2;
  std::string boot_path;
  std::string args; // optional, passed verbatim to the emulator
};

struct GameConfig {
  std::string pcsx2_path;
  std::string rpcs3_path;
  std::vector<GameEntry> games;
  bool analog_input = false; // [input] analog = true -> HIDMaestro analog gamepad
};

// Parse config.ini. Returns false on failure (file missing/unreadable).
bool load_game_config(const std::string &path, GameConfig &out);

} // namespace rps

#pragma once

#include "game_config.h"
#include <cstddef>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rps {

// Spawns and kills a single emulator process at a time.
class ProcessManager {
public:
  ~ProcessManager();
  ProcessManager() = default;
  ProcessManager(const ProcessManager &) = delete;
  ProcessManager &operator=(const ProcessManager &) = delete;

  // Launch the game at game_index, closing any currently running process first.
  bool launch(const GameConfig &config, size_t game_index);

  // Terminate the running emulator process, if any.
  void close();

  // Find the emulator's game window (once) and keep it foreground. Called
  // periodically while a game runs so the host terminal / other windows don't
  // steal focus from the streamed game.
  void ensureForeground();

  // True when the emulator's game window currently owns the foreground.
  bool isForeground() const;

  bool isRunning() const;

private:
#ifdef _WIN32
  HANDLE m_process = nullptr;
  HWND m_hwnd = nullptr;
#endif
};

} // namespace rps

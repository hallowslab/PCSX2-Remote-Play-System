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

  bool isRunning() const;

private:
#ifdef _WIN32
  HANDLE m_process = nullptr;
#endif
};

} // namespace rps

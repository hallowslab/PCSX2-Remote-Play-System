#include "process_manager.h"

#include <iostream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rps {

ProcessManager::~ProcessManager() { close(); }

bool ProcessManager::launch(const GameConfig &config, size_t game_index) {
  if (game_index >= config.games.size()) {
    std::cerr << "[Process] Invalid game index " << game_index << "\n";
    return false;
  }
  const GameEntry &g = config.games[game_index];

  std::string exe = (g.emulator == EmulatorType::PCSX2) ? config.pcsx2_path
                                                        : config.rpcs3_path;
  if (exe.empty()) {
    std::cerr << "[Process] Emulator path not configured for game '" << g.name
              << "'\n";
    return false;
  }

  std::ostringstream cmdline;
  cmdline << "\"" << exe << "\" ";
  if (g.emulator == EmulatorType::PCSX2) {
    cmdline << "-fullscreen -- \"" << g.boot_path << "\"";
  } else {
    cmdline << "--no-gui --fullscreen \"" << g.boot_path << "\"";
  }
  if (!g.args.empty())
    cmdline << " " << g.args;

  // Close any existing process first.
  close();

  std::string cmd = cmdline.str();
  std::cout << "[Process] Launching: " << cmd << "\n";

#ifdef _WIN32
  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};

  std::vector<char> buf(cmd.begin(), cmd.end());
  buf.push_back('\0');

  if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      nullptr, &si, &pi)) {
    std::cerr << "[Process] CreateProcess failed (error " << GetLastError()
              << ")\n";
    return false;
  }
  CloseHandle(pi.hThread);
  m_process = pi.hProcess;
  return true;
#else
  return false;
#endif
}

void ProcessManager::close() {
#ifdef _WIN32
  if (m_process) {
    TerminateProcess(m_process, 0);
    WaitForSingleObject(m_process, 1000);
    CloseHandle(m_process);
    m_process = nullptr;
    std::cout << "[Process] Emulator terminated\n";
  }
#endif
}

bool ProcessManager::isRunning() const {
#ifdef _WIN32
  return m_process != nullptr;
#else
  return false;
#endif
}

} // namespace rps

#include "process_manager.h"

#include <iostream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rps {

namespace {
struct EnumCtx {
  DWORD pid;
  HWND hwnd;
};

// Find the emulator's visible top-level window so we can ask it to close
// gracefully (WM_CLOSE) instead of hard-killing the process.
BOOL CALLBACK find_emu_window(HWND hwnd, LPARAM lparam) {
  auto *ctx = reinterpret_cast<EnumCtx *>(lparam);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == ctx->pid && IsWindowVisible(hwnd)) {
    ctx->hwnd = hwnd;
    return FALSE;
  }
  return TRUE;
}
} // namespace

// Deliberately no close() in the destructor: the host must never tear down a
// running emulator when it exits (would risk save-data corruption). The OS
// cleans up the process handle; the emulator keeps running.
ProcessManager::~ProcessManager() = default;

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
    // -batch: run headless and exit the emulator when the game closes.
    cmdline << "-batch -fullscreen -- \"" << g.boot_path << "\"";
  } else {
    // RPCS3 --no-gui hides the main window and shows only the game view. Its
    // Vulkan/D3D12 renderer must use borderless fullscreen (user setting) so
    // Desktop Duplication / GDI capture sees the game instead of black.
    cmdline << "--no-gui \"" << g.boot_path << "\"";
    std::cout
        << "[Process] RPCS3: for a stable stream, set Configuration > Advanced "
           "> Exclusive Fullscreen Mode > Prefer borderless fullscreen.\n";
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
  m_hwnd = nullptr;
  return true;
#else
  return false;
#endif
}

void ProcessManager::ensureForeground() {
#ifdef _WIN32
  if (!m_process)
    return;
  if (m_hwnd && !IsWindow(m_hwnd))
    m_hwnd = nullptr;
  if (!m_hwnd) {
    EnumCtx ctx{GetProcessId(m_process), nullptr};
    EnumWindows(find_emu_window, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.hwnd)
      return; // game window not up yet
    m_hwnd = ctx.hwnd;
  }
  if (GetForegroundWindow() == m_hwnd)
    return;
  // Plain SetForegroundWindow: we just spawned this process, so Windows grants
  // it the right to become foreground. Do NOT AttachThreadInput — attaching our
  // input queue to the game's breaks its Esc / Alt+Enter handling.
  SetForegroundWindow(m_hwnd);
#endif
}

bool ProcessManager::isForeground() const {
#ifdef _WIN32
  return m_hwnd && GetForegroundWindow() == m_hwnd;
#else
  return false;
#endif
}

void ProcessManager::close() {
#ifdef _WIN32
  if (!m_process)
    return;
  // Graceful close: post WM_CLOSE so the emulator can shut down and flush
  // saves. Only hard-kill if it does not exit in time.
  EnumCtx ctx{GetProcessId(m_process), nullptr};
  EnumWindows(find_emu_window, reinterpret_cast<LPARAM>(&ctx));
  if (ctx.hwnd)
    PostMessage(ctx.hwnd, WM_CLOSE, 0, 0);

  DWORD wait = WaitForSingleObject(m_process, 5000);
  if (wait == WAIT_OBJECT_0) {
    CloseHandle(m_process);
    m_process = nullptr;
    m_hwnd = nullptr;
    std::cout << "[Process] Emulator closed gracefully\n";
    return;
  }

  TerminateProcess(m_process, 0);
  WaitForSingleObject(m_process, 1000);
  CloseHandle(m_process);
  m_process = nullptr;
  m_hwnd = nullptr;
  std::cout << "[Process] Emulator terminated\n";
#else
  (void)0;
#endif
}

bool ProcessManager::isRunning() const {
#ifdef _WIN32
  if (!m_process)
    return false;
  // Signaled handle = process exited (with --batch, PCSX2 quits when the
  // game closes). WAIT_TIMEOUT means it is still alive.
  return WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
#else
  return false;
#endif
}

} // namespace rps

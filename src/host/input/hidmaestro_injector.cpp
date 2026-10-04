#include "hidmaestro_injector.h"

#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>

namespace rps {

namespace {

// Locate HidMaestroBridge.exe next to the host executable.
std::string bridge_exe_path() {
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return "HidMaestroBridge.exe";
  std::string s(path, n);
  size_t slash = s.find_last_of("\\/");
  if (slash != std::string::npos)
    s = s.substr(0, slash + 1);
  return s + "HidMaestroBridge.exe";
}

bool process_is_elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return false;
  TOKEN_ELEVATION elev = {};
  DWORD size = 0;
  bool elevated = false;
  if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size))
    elevated = (elev.TokenIsElevated != 0);
  CloseHandle(token);
  return elevated;
}

std::wstring to_wide(const std::string &s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

} // namespace

HidMaestroInjector::~HidMaestroInjector() { shutdown(); }

bool HidMaestroInjector::init() {
  const char *pipeName = "\\\\.\\pipe\\pcsx2rps-hidmaestro";

  m_pipe = CreateNamedPipeA(pipeName, PIPE_ACCESS_OUTBOUND,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                            512, 512, 0, nullptr);
  if (m_pipe == INVALID_HANDLE_VALUE) {
    std::cerr << "[HidMaestro] CreateNamedPipe failed (" << GetLastError()
              << ")\n";
    return false;
  }

  // Spawn the bridge. Its manifest requires elevation, and CreateProcess
  // cannot elevate: from a non-elevated host it fails with
  // ERROR_ELEVATION_REQUIRED (740). When the host is not already elevated we
  // launch via ShellExecuteEx("runas") to raise the UAC prompt. The bridge is
  // a WinExe with no window of its own, so once approved nothing of its own
  // steals focus; the bridge restores focus to our console (--focus) after it
  // finishes device setup, since PnP/UMDF device start steals foreground.
  // The UAC consent dialog itself is the only focus grab before that.
  std::string bridge = bridge_exe_path();

  // Pass our console window to the bridge so it can restore foreground after
  // it finishes device setup (PnP/UMDF device start steals focus).
  DWORD_PTR consoleHwnd = (DWORD_PTR)GetConsoleWindow();
  std::string focusArg = std::to_string(consoleHwnd);
  std::string bridgeCmd = bridge + " --focus " + focusArg;

  if (!process_is_elevated()) {
    std::wstring wideExe = to_wide(bridge);
    std::wstring wideArgs = L"--focus " + std::to_wstring(consoleHwnd);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = wideExe.c_str();
    sei.lpParameters = wideArgs.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
      DWORD err = GetLastError();
      std::cerr << "[HidMaestro] Failed to launch bridge " << bridge
                << " (error " << err << ")\n";
      CloseHandle(m_pipe);
      m_pipe = INVALID_HANDLE_VALUE;
      return false;
    }
    m_bridge = sei.hProcess;
  } else {
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<char> cmd(bridgeCmd.begin(), bridgeCmd.end());
    cmd.push_back('\0');

    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      DWORD err = GetLastError();
      std::cerr << "[HidMaestro] Failed to launch bridge " << bridge
                << " (error " << err << ")\n";
      CloseHandle(m_pipe);
      m_pipe = INVALID_HANDLE_VALUE;
      return false;
    }
    CloseHandle(pi.hThread);
    m_bridge = pi.hProcess;
  }

  // Wait for the bridge to connect (up to 30s: UAC + driver install).
  OVERLAPPED ov = {};
  ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  BOOL connected = ConnectNamedPipe(m_pipe, &ov);
  if (!connected) {
    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
      m_ready = true;
    } else if (err == ERROR_IO_PENDING) {
      m_ready = (WaitForSingleObject(ov.hEvent, 30000) == WAIT_OBJECT_0);
    }
  } else {
    m_ready = true;
  }
  CloseHandle(ov.hEvent);

  if (!m_ready) {
    std::cerr << "[HidMaestro] Bridge did not connect in time\n";
    shutdown();
    return false;
  }

  // Best-effort foreground reclaim. The elevated bridge already restored
  // focus to us; this covers any residual grab between connect and here.
  HWND console = GetConsoleWindow();
  if (console)
    SetForegroundWindow(console);

  std::cout << "[HidMaestro] Analog gamepad bridge connected\n";
  return true;
}

void HidMaestroInjector::shutdown() {
  if (m_pipe != INVALID_HANDLE_VALUE) {
    DisconnectNamedPipe(m_pipe);
    CloseHandle(m_pipe);
    m_pipe = INVALID_HANDLE_VALUE;
  }
  if (m_bridge) {
    TerminateProcess(m_bridge, 0);
    CloseHandle(m_bridge);
    m_bridge = nullptr;
  }
  m_ready = false;
}

void HidMaestroInjector::sendKey(uint32_t vk, bool down) {
  (void)vk;
  (void)down; // keyboard handled by the SendInput injector
}

bool HidMaestroInjector::sendGamepadState(const InputPacket &state) {
  if (!m_ready || m_pipe == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  BOOL ok = WriteFile(m_pipe, &state, sizeof(InputPacket), &written, nullptr);
  if (!ok || written != sizeof(InputPacket)) {
    static DWORD fail_count = 0;
    if (++fail_count % 125 == 1)
      std::cerr << "[HidMaestro] analog write failed (err " << GetLastError()
                << ")\n";
    return false;
  }
  return true;
}

} // namespace rps
#endif // _WIN32

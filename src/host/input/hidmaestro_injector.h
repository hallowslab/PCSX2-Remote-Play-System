#pragma once

#include "input_injector.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace rps {

// Injects gamepad input as a real analog controller via the HIDMaestro bridge.
//
// The host (non-elevated) creates a named pipe and spawns the elevated bridge
// (UAC prompt). sendGamepadState writes InputPacket frames down the pipe; the
// bridge maps them to a virtual Xbox 360 pad the emulator reads natively.
//
// sendKey is a no-op: keyboard injection stays on the SendInput injector.
class HidMaestroInjector : public IInputInjector {
public:
  ~HidMaestroInjector() override;

  bool init() override;   // create pipe, spawn bridge, wait for connect
  void shutdown() override;
  void sendKey(uint32_t vk, bool down) override;
  bool sendGamepadState(const InputPacket &state) override;

private:
#ifdef _WIN32
  HANDLE m_pipe = INVALID_HANDLE_VALUE;
  HANDLE m_bridge = nullptr;
#endif
  bool m_ready = false;
};

} // namespace rps

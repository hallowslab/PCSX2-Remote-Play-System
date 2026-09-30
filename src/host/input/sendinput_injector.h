#pragma once

#include "input_injector.h"

namespace rps {

// Keyboard-only injection via Win32 SendInput. No analog gamepad support.
class SendInputInjector : public IInputInjector {
public:
  bool init() override;
  void shutdown() override;
  void sendKey(uint32_t vk, bool down) override;
  bool sendGamepadState(const InputPacket &state) override;

private:
  bool m_initialized = false;
};

} // namespace rps

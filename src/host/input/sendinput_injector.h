#pragma once

#include "input_injector.h"

#include <array>

namespace rps {

// Keyboard-only injection via Win32 SendInput. No analog gamepad support.
class SendInputInjector : public IInputInjector {
public:
  bool init() override;
  void shutdown() override;
  void sendKey(uint32_t vk, bool down) override;
  void releaseAll() override;
  bool sendGamepadState(const InputPacket &state) override;

private:
  bool m_initialized = false;
  std::array<bool, 256> m_pressed{};
};

} // namespace rps

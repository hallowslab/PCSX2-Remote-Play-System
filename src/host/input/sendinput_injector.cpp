#include "sendinput_injector.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace rps {

bool SendInputInjector::init() {
  m_initialized = true;
  return true;
}

void SendInputInjector::shutdown() {
  releaseAll();
  m_initialized = false;
}

void SendInputInjector::sendKey(uint32_t vk, bool down) {
  if (!m_initialized || vk == 0)
    return;

  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = static_cast<WORD>(vk);
  input.ki.wScan = 0;
  input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
  if (SendInput(1, &input, sizeof(INPUT)) == 1 && vk < m_pressed.size())
    m_pressed[vk] = down;
}

void SendInputInjector::releaseAll() {
  if (!m_initialized)
    return;
  for (uint32_t vk = 1; vk < m_pressed.size(); ++vk) {
    if (!m_pressed[vk])
      continue;
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
    m_pressed[vk] = false;
  }
}

bool SendInputInjector::sendGamepadState(const InputPacket &state) {
  (void)state;
  return false; // no analog support; caller maps gamepad to keys
}

std::unique_ptr<IInputInjector> createInputInjector() {
  return std::make_unique<SendInputInjector>();
}

} // namespace rps
#else
namespace rps {
std::unique_ptr<IInputInjector> createInputInjector() { return nullptr; }
} // namespace rps
#endif

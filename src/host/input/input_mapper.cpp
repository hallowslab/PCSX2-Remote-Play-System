#include "input_mapper.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace rps {

namespace {

// Default gamepad-button -> keyboard key (Windows VK) mapping. These keys MUST
// match the PCSX2 Pad 1 keyboard bindings on the host. Letter/digit literals
// equal their VK codes (VK_A == 'A'). Edit here or make configurable later.
uint32_t button_vk(uint16_t button) {
  switch (button) {
  case BTN_A:
    return 'X'; // Cross
  case BTN_B:
    return 'C'; // Circle
  case BTN_X:
    return 'S'; // Square
  case BTN_Y:
    return 'V'; // Triangle
  case BTN_DPAD_UP:
    return VK_UP;
  case BTN_DPAD_DOWN:
    return VK_DOWN;
  case BTN_DPAD_LEFT:
    return VK_LEFT;
  case BTN_DPAD_RIGHT:
    return VK_RIGHT;
  case BTN_START:
    return VK_RETURN;
  case BTN_BACK:
    return VK_BACK;
  case BTN_LEFT_SHOULDER:
    return 'Q'; // L1
  case BTN_RIGHT_SHOULDER:
    return 'E'; // R1
  case BTN_LEFT_THUMB:
    return 'Z'; // L3
  case BTN_RIGHT_THUMB:
    return 'O'; // R3
  default:
    return 0;
  }
}

} // namespace

void InputMapper::processGamepad(const InputPacket &state,
                                 const KeySink &sink) {
  static const uint16_t button_bits[] = {
      BTN_A,           BTN_B,            BTN_X,
      BTN_Y,           BTN_DPAD_UP,      BTN_DPAD_DOWN,
      BTN_DPAD_LEFT,   BTN_DPAD_RIGHT,   BTN_START,
      BTN_BACK,        BTN_LEFT_SHOULDER, BTN_RIGHT_SHOULDER,
      BTN_LEFT_THUMB,  BTN_RIGHT_THUMB,
  };
  for (uint16_t bit : button_bits) {
    updateButton(bit, state.buttons, button_vk(bit), sink);
  }

  // Analog sticks -> digital directions (threshold). neg = negative axis, pos =
  // positive axis. SDL left_y: negative = up.
  constexpr int16_t threshold = 16000;
  updateAxis(state.left_stick_x, threshold, 'A', 'D', m_left_x, sink);
  updateAxis(state.left_stick_y, threshold, 'W', 'S', m_left_y, sink);
  updateAxis(state.right_stick_x, threshold, 'J', 'L', m_right_x, sink);
  updateAxis(state.right_stick_y, threshold, 'I', 'K', m_right_y, sink);
  updateAxis(static_cast<int16_t>(state.left_trigger) << 7, threshold, 0, '1',
             m_left_trig, sink);
  updateAxis(static_cast<int16_t>(state.right_trigger) << 7, threshold, 0, '3',
             m_right_trig, sink);
}

void InputMapper::reset() {
  m_prev_buttons = 0;
  m_left_x = m_left_y = m_right_x = m_right_y = 0;
  m_left_trig = m_right_trig = 0;
}

void InputMapper::updateButton(uint16_t bit, uint16_t buttons, uint32_t vk,
                               const KeySink &sink) {
  if (vk == 0)
    return;
  bool was = (m_prev_buttons & bit) != 0;
  bool now = (buttons & bit) != 0;
  if (now != was)
    sink(vk, now);
  if (now)
    m_prev_buttons |= bit;
  else
    m_prev_buttons &= ~bit;
}

void InputMapper::updateAxis(int16_t value, int16_t threshold, uint32_t neg_vk,
                             uint32_t pos_vk, uint8_t &prev,
                             const KeySink &sink) {
  uint8_t dir = 0;
  if (value > threshold)
    dir = 1; // positive
  else if (value < -threshold)
    dir = 2; // negative

  if (dir == prev)
    return;

  if (prev == 1 && pos_vk)
    sink(pos_vk, false);
  else if (prev == 2 && neg_vk)
    sink(neg_vk, false);

  if (dir == 1 && pos_vk)
    sink(pos_vk, true);
  else if (dir == 2 && neg_vk)
    sink(neg_vk, true);

  prev = dir;
}

} // namespace rps

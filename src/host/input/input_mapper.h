#pragma once

#include "../../shared/protocol/protocol.h"
#include <cstdint>
#include <functional>

namespace rps {

// Maps gamepad InputPacket state to keyboard key events (digital). Stateful so
// it emits down/up edges for SendInput. Used by hosts whose injector cannot do
// analog (e.g. SendInput). Analog-capable backends skip this mapper.
class InputMapper {
public:
  using KeySink = std::function<void(uint32_t vk, bool down)>;

  // Process a gamepad state snapshot and emit key down/up events for changed
  // controls. vk values are Windows VK codes.
  void processGamepad(const InputPacket &state, const KeySink &sink);

  // Reset edge state (e.g. on client disconnect) to avoid stuck keys.
  void reset();

private:
  void updateButton(uint16_t bit, uint16_t buttons, uint32_t vk,
                    const KeySink &sink);
  void updateAxis(int16_t value, int16_t threshold, uint32_t neg_vk,
                  uint32_t pos_vk, uint8_t &prev, const KeySink &sink);

  uint16_t m_prev_buttons = 0;
  uint8_t m_left_x = 0; // 1 = pos, 2 = neg, 0 = center
  uint8_t m_left_y = 0;
  uint8_t m_right_x = 0;
  uint8_t m_right_y = 0;
  uint8_t m_left_trig = 0;
  uint8_t m_right_trig = 0;
};

} // namespace rps

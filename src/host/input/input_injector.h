#pragma once

#include "../../shared/protocol/protocol.h"
#include <cstdint>
#include <memory>

namespace rps {

// Abstraction over how host-side input reaches the OS/PCSX2.
//
// sendKey is universal (keyboard injection). sendGamepadState is optional and
// only supported by analog-capable backends (e.g. a future ViGEm virtual-pad
// backend). When sendGamepadState returns false the caller falls back to
// keyboard mapping of the gamepad state.
class IInputInjector {
public:
  virtual ~IInputInjector() = default;

  virtual bool init() = 0;
  virtual void shutdown() = 0;

  // Inject a keyboard key. vk is a Windows VK code; down = press/release.
  virtual void sendKey(uint32_t vk, bool down) = 0;

  // Release any keys the backend currently holds. Backends without persistent
  // key state may keep default no-op implementation.
  virtual void releaseAll() {}

  // Inject a full gamepad state (analog-capable backends). Returns false when
  // the backend cannot handle analog.
  virtual bool sendGamepadState(const InputPacket &state) = 0;
};

// Factory: creates the platform input injector.
std::unique_ptr<IInputInjector> createInputInjector();

} // namespace rps

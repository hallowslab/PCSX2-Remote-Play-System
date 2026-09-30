#pragma once

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace rps {

// Convert an SDL virtual keycode to a Windows VK code. Returns 0 if no mapping
// exists. The client sends SDL_Keycode on the wire (platform-neutral); the host
// converts to VK for SendInput. Letters normalize to uppercase VK.
inline uint32_t sdlk_to_vk(SDL_Keycode key) {
  if (key >= SDLK_A && key <= SDLK_Z)
    return 'A' + (key - SDLK_A); // VK_A..VK_Z == 'A'..'Z'
  if (key >= SDLK_0 && key <= SDLK_9)
    return '0' + (key - SDLK_0); // VK_0..VK_9 == '0'..'9'
  if (key >= SDLK_F1 && key <= SDLK_F12)
    return VK_F1 + (key - SDLK_F1);
  switch (key) {
  case SDLK_RETURN:
  case SDLK_RETURN2:
    return VK_RETURN;
  case SDLK_ESCAPE:
    return VK_ESCAPE;
  case SDLK_BACKSPACE:
    return VK_BACK;
  case SDLK_TAB:
    return VK_TAB;
  case SDLK_SPACE:
    return VK_SPACE;
  case SDLK_UP:
    return VK_UP;
  case SDLK_DOWN:
    return VK_DOWN;
  case SDLK_LEFT:
    return VK_LEFT;
  case SDLK_RIGHT:
    return VK_RIGHT;
  case SDLK_LSHIFT:
  case SDLK_RSHIFT:
    return VK_SHIFT;
  case SDLK_LCTRL:
  case SDLK_RCTRL:
    return VK_CONTROL;
  case SDLK_LALT:
  case SDLK_RALT:
    return VK_MENU;
  default:
    return 0;
  }
}

} // namespace rps
#endif // _WIN32

#pragma once
// The platform through SDL3: the Linux build's (the Steam Deck), and a Mac
// build that tests it with the Vulkan renderer (DCUSA_PLATFORM=sdl).
//
//  - The game window: an SDL window with a Vulkan surface (gpu::set_layer
//    takes the SDL_Window*). Full screen with DCUSA_FULLSCREEN=1 or on a
//    Steam Deck; F11 or Alt+Return toggles it; closing it or Ctrl+Q ends the run.
//  - The edition picker: the title images on the carousel (platform/
//    edition_choice), in a window of its own that closes before the game's
//    opens (an SDL renderer and the game's Vulkan surface never share one).
//  - The controller: the first SDL gamepad (a Steam Deck's own controls
//    included), mapped with runtime/pad_mapping.
//  - The sound: the default playback device, float stereo.
#include <memory>

#include "platform/macos/game_window.hpp"
#include "runtime/host_platform.hpp"

namespace dcusa {

std::unique_ptr<GameWindow> make_sdl_game_window(const char* title, unsigned width, unsigned height);
std::unique_ptr<PadSource> make_sdl_pad();
std::unique_ptr<AudioOutput> make_sdl_audio_output();

}  // namespace dcusa

#pragma once
// The D3D11 device the game creates at startup, and the DXGI objects it asks
// that device for. Method slots live in a guest address range below the import
// thunks, so a virtual call lands here instead of failing as a missing function.
#include <cstdint>

#include "audio/ds_mixer.hpp"
#include "recomp/cpu.hpp"
#include "runtime/host_platform.hpp"

namespace dcusa::recomp {

constexpr std::uint32_t kComCall = 0xFFE00000u;

void D3D11CreateDevice(Cpu& c);
void D3DCompile(Cpu& c);
void D3DReflect(Cpu& c);
void DirectInput8Create(Cpu& c);
void DirectSoundCreate8(Cpu& c);
void call_com(Cpu& c, std::uint32_t target);

// The image Present last copied out of the bound render target. The pointer
// stays valid until the next Present. Returns false before the first one.
bool presented_frame(std::uint32_t* width, std::uint32_t* height, const std::uint8_t** rgba);

// The window Present shows the frame in (its present() runs once per frame,
// after the frame is drawn); nullptr runs offscreen.
void set_presenter(dcusa::Presenter* presenter);

// The display the game is told it has: DCUSA_RESOLUTION=WxH, 1920x1080 by
// default. It sizes the back buffer only; the game renders its scene targets
// at its own resolution (GameData/config.ini DYNAMIC_RESOLUTION_MAX, 1920).
void display_size(std::uint32_t* width, std::uint32_t* height);

// The DirectSound buffers the game plays, mixed for an AudioOutput to pull
// (audio::DsMixer::pull with this as `user`).
audio::DsMixer& sound_mixer();

}

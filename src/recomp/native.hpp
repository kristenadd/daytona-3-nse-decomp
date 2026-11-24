#pragma once
// Native replacements for guest functions the translator would otherwise emit.
// Each one is a cdecl callee: arguments are dwords at [esp+4], [esp+8], ...,
// the return value is eax, and esp moves past the return address only.
// ebx, ebp, esi and edi are left alone.
#include "recomp/cpu.hpp"

namespace dcusa::recomp {

void native_memmove(Cpu& c);  // void* (dst, src, n) -> dst, overlap is defined
void native_memset(Cpu& c);   // void* (dst, fill, n) -> dst
void native_strchr(Cpu& c);   // char* (s, char) -> match or 0
void native_strrchr(Cpu& c);  // char* (s, char) -> last match or 0
void native_strstr(Cpu& c);   // char* (haystack, needle) -> match or 0; empty needle returns haystack
// The CRT unary dispatcher at 0xC44FD0 jumps through a descriptor whose name is the
// function ("exp", "log", ...). The handlers it selects are not translated. The argument
// is st(0); the result replaces it. The guest return address stays where it is.
void native_crt_unary(Cpu& c);
// Frame skip (d3d11.cpp): whether this tick leaves out the scene render (the
// call to 0x4DC120 in the frame routine 0x4DD160), since the frame is late.
bool frame_skip_render();
// The boot's "Data Loading" percentage (0x5BFD15 pushes it for "%d%%"), for the
// boot screen (d3d11.cpp).
void boot_progress(std::uint32_t percent);
// The scene walk's fast path (SITE_OVERRIDES 0x4423BA): whether 0x459580
// would find nothing to do for the object `o`. It updates the parts whose
// dirty flags are set: [o+0x2E0], [o+0x94] with [o+0xA8], [o+0xC], [o+0x2E1],
// and the sub-objects [o+0x60] (flag +0x7C) and [o+0x5C] (flag +0x84).
inline bool scene_object_clean(const std::uint8_t* M, std::uint32_t o) {
    auto u8 = [M](std::uint32_t a) { return M[a]; };
    auto u32 = [M](std::uint32_t a) { std::uint32_t v; __builtin_memcpy(&v, M + a, 4); return v; };
    if (u8(o + 0x2E0u) | u8(o + 0xCu) | u8(o + 0x2E1u)) return false;
    if (u32(o + 0x94u) != 0 && u8(o + 0xA8u) != 0) return false;
    const std::uint32_t a = u32(o + 0x60u), b = u32(o + 0x5Cu);
    if (a != 0 && u8(a + 0x7Cu) != 0) return false;
    if (b != 0 && u8(b + 0x84u) != 0) return false;
    return true;
}

}

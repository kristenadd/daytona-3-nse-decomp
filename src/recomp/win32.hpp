#pragma once
// Handlers for imported stdcall functions. A handler is a callee: arguments are
// the dwords at [esp+4], [esp+8], ..., the result is eax, and esp moves past
// the return address and the arguments. ebx, ebp, esi and edi are left alone.
#include <string_view>

#include "recomp/cpu.hpp"
#include "recomp/dispatch.hpp"
#include "runtime/host_platform.hpp"

namespace dcusa::recomp {

// One bump heap for the process. The handle is the guest address of its header.
constexpr std::uint32_t kProcessHeap = 0x24000000u;
// The process heap stays 64 MB. The game's memory manager VirtualAllocs 100 MB
// out of the arena that begins at the end of this heap, so the heap cannot grow
// into it. Requests that do not fit spill into kHeap2.
constexpr std::uint32_t kHeapBytes = 64u * 1024u * 1024u;
constexpr std::uint32_t kHeap2 = 0x37000000u;
constexpr std::uint32_t kHeap2Bytes = 64u * 1024u * 1024u;

// The handler for `name` ("KERNEL32.dll!GetStartupInfoW"), or nullptr when
// that import still fails closed.
GuestFn import_handler(std::string_view name);

// The pad XInputGetState reports. A disconnected snapshot returns
// ERROR_DEVICE_NOT_CONNECTED, which is what startup counts as "no pad".
void set_pad_snapshot(const dcusa::PadSnapshot& pad);
const dcusa::PadSnapshot& current_pad();

// True when `address` is inside memory the allocators have committed (the
// image is not included). For debug readers that must not fault.
bool guest_heap_committed(std::uint32_t address);

// Posted into the queue PeekMessage reads. WM_QUIT is 0x12.
void post_guest_message(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wparam, std::uint32_t lparam);

}

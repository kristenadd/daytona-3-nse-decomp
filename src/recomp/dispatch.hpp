#pragma once
// Calls whose target is only known at run time: virtual calls, function
// pointers, imports through the IAT. Every guest entry point the translator
// produced is in one sorted table; anything else fails closed.
#include <cstdint>
#include <initializer_list>
#include <span>

#include "recomp/cpu.hpp"

namespace dcusa::recomp {

using GuestFn = void (*)(Cpu&);

struct GuestEntry {
    std::uint32_t address;
    GuestFn fn;
};

// IAT slots are filled with addresses in this range at load; slot k holds ImportBase + 16 * k.
constexpr std::uint32_t ImportBase = 0xFFF00000u;

// One imported function. `handler` is a native replacement; a null handler fails
// closed and names `name` (for example "KERNEL32.dll!GetCommandLineA"). `iat` is
// the guest address of the slot, so the report points at the executable.
struct ImportSlot {
    std::uint32_t iat = 0;
    const char* name = nullptr;
    GuestFn handler = nullptr;
};

// Installs the translated entry table (sorted by address) and the import slots.
// Both spans must outlive every guest call.
void dispatch_install(std::span<const GuestEntry> entries, std::span<const ImportSlot> imports);
// Calls the guest function at `target` (the return address is already pushed).
void call_indirect(Cpu& c, std::uint32_t target);
// Looks up a translated function; nullptr when there is none.
GuestFn find_entry(std::uint32_t target);
// Calls a guest stdcall function from a handler, as Windows calls an
// application callback: `args` are pushed right to left below the handler's
// stack, then a return address. Returns eax; esp is as it was before.
std::uint32_t call_guest(Cpu& c, std::uint32_t target, std::initializer_list<std::uint32_t> args);

}

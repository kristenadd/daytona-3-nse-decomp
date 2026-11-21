#pragma once
// Calls whose target is only known at run time: virtual calls, function
// pointers, imports through the IAT. Every guest entry point the translator
// produced is in one sorted table; anything else fails closed.
#include <cstdint>
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

// Installs the translated entry table (sorted by address) and the import handlers.
void dispatch_install(std::span<const GuestEntry> entries, std::span<const GuestFn> imports);
// Calls the guest function at `target` (the return address is already pushed).
void call_indirect(Cpu& c, std::uint32_t target);
// Looks up a translated function; nullptr when there is none.
GuestFn find_entry(std::uint32_t target);

}

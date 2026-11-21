#include "recomp/guest.hpp"

#include <chrono>
#include <cstdio>

#include "runtime/fail_closed.hpp"

namespace dcusa::recomp {

void trap(Cpu&, std::uint32_t va, const char* what) {
    char text[96];
    std::snprintf(text, sizeof text, "guest fault: %s", what);
    fail_closed(Unported::Instruction, va, text);
}

void guest_unported(Cpu&, std::uint32_t va, const char* why) { fail_closed(Unported::Function, va, why); }

// The CPU the translated code can actually run: x87, MMX, SSE, SSE2, CMOV, CX8,
// TSC; nothing newer, since nothing newer is translated.
void guest_cpuid(Cpu& c) {
    std::uint32_t leaf = c.r[Cpu::EAX];
    std::uint32_t a = 0, b = 0, cx = 0, d = 0;
    if (leaf == 0) {
        a = 1;
        std::memcpy(&b, "Genu", 4);
        std::memcpy(&d, "ineI", 4);
        std::memcpy(&cx, "ntel", 4);
    } else if (leaf == 1) {
        a = 0x000106A5;  // family 6, model 26 (a Core i7 of the era)
        d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
    }
    c.r[Cpu::EAX] = a;
    c.r[Cpu::EBX] = b;
    c.r[Cpu::ECX] = cx;
    c.r[Cpu::EDX] = d;
}

std::uint64_t guest_rdtsc() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

}

#include "recomp/guest.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_set>

#include "recomp/rt_lock.hpp"
#include "recomp/threads.hpp"
#include "runtime/fail_closed.hpp"

namespace dcusa::recomp {

void trap(Cpu&, std::uint32_t va, const char* what) {
    char text[96];
    std::snprintf(text, sizeof text, "guest fault: %s", what);
    fail_closed(Unported::Instruction, va, text);
}

void guest_unported(Cpu&, std::uint32_t va, const char* why) { fail_closed(Unported::Function, va, why); }

// The CPU the translated code can actually run: x87, MMX, SSE, SSE2, CMOV, CX8,
// TSC. Leaf 1 ECX stays 0, so the CRT and the game's feature table never select
// the SSE4 or AVX paths. Those instructions stay on the trap allow-list.
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

void guest_pause(Cpu& c) { sched_yield(c); }

std::atomic<bool> g_preempt{false};
std::uint64_t g_probe[8] = {};

// The first time: the x87 stack, the registers (as last written back; a
// translated function keeps them in locals between calls), and 32 bytes at esi
// and edi, for the functions that walk their input through those.
void nan_trap_report(Cpu& c, std::uint32_t site) {
    static std::mutex m;
    static std::unordered_set<std::uint32_t> seen;
    static std::uint64_t order = 0;
    std::lock_guard<std::mutex> g(m);
    if (!seen.insert(site).second) return;
    std::fprintf(stderr, "nan trap #%llu: x87 store at %08X wrote NaN\n", static_cast<unsigned long long>(++order), site);
    if (order > 12) return;
    std::fprintf(stderr, "  st:");
    for (unsigned i = 0; i < 8; ++i) std::fprintf(stderr, " %g", c.fpu.at(i));
    std::fprintf(stderr, "  top %u tag %04x cw %04x\n", c.fpu.top, c.fpu.tag, c.fpu.control);
    static const char* names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    std::fprintf(stderr, "  regs:");
    for (unsigned i = 0; i < 8; ++i) std::fprintf(stderr, " %s %08x", names[i], c.r[i]);
    std::fprintf(stderr, "\n");
    for (unsigned r : {6u, 7u}) {
        const std::uint32_t a = c.r[r];
        if (a < 0x10000 || a > 0xFFFF0000u) continue;
        std::fprintf(stderr, "  [%s]:", names[r]);
        for (unsigned i = 0; i < 32; ++i) std::fprintf(stderr, " %02x", c.mem[a + i]);
        std::fprintf(stderr, "\n");
    }
}

void debug_probe(Cpu& c, std::uint32_t va, std::uint32_t a, std::uint32_t b, std::uint32_t d, std::uint32_t e) {
    std::fprintf(stderr, "probe %08X thread %u: %08X %08X %08X %08X\n", va, c.mem ? *reinterpret_cast<const std::uint32_t*>(c.mem + c.fs_base + 0x24) : 0u, a, b, d, e);
}

void guest_preempt(Cpu& c) {
    // A parallel guest thread (rt_lock.hpp) has no turn to give; it leaves the
    // flag to the threads that take turns.
    if (sched_on_free_thread()) return;
    g_preempt.store(false, std::memory_order_relaxed);
    RtGuard locked;
    const auto t0 = std::chrono::steady_clock::now();
    sched_yield(c);
    g_preempt_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}
std::atomic<std::uint64_t> g_preempt_ns{0};

std::uint64_t guest_rdtsc() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

}

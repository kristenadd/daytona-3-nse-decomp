#include "recomp/dispatch.hpp"
#include "recomp/game_addresses.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "recomp/d3d11.hpp"
#include "recomp/rt_lock.hpp"
#include "recomp/x86ops.hpp"
#include "runtime/fail_closed.hpp"

namespace dcusa::recomp {

namespace {

std::span<const GuestEntry> g_entries;
std::span<const ImportSlot> g_imports;

// Virtual calls go to a few hundred targets over and over; the binary search
// over ~100,000 entries was 15% of the main thread in a race. A direct-mapped
// cache in front of it. An entry is one 64-bit word (the guest address, and the
// index of its entry plus one), so a guest thread running in parallel
// (rt_lock.hpp) reads either a whole old entry or a whole new one.
constexpr std::uint32_t kEntryCacheSize = 1u << 14;
std::atomic<std::uint64_t> g_entry_cache[kEntryCacheSize];


}

void dispatch_install(std::span<const GuestEntry> entries, std::span<const ImportSlot> imports) {
    g_entries = entries;
    g_imports = imports;
    for (auto& slot : g_entry_cache) slot.store(0, std::memory_order_relaxed);
}

GuestFn find_entry(std::uint32_t target) {
    auto it = std::lower_bound(g_entries.begin(), g_entries.end(), target,
                               [](const GuestEntry& e, std::uint32_t a) { return e.address < a; });
    return it != g_entries.end() && it->address == target ? it->fn : nullptr;
}

namespace {

GuestFn find_entry_slow(std::uint32_t target);

// Inlined into call_indirect: a hit is a compare and a load.
inline GuestFn find_entry_cached(std::uint32_t target) {
    const std::uint64_t e = g_entry_cache[(target ^ (target >> 14)) & (kEntryCacheSize - 1)].load(std::memory_order_relaxed);
    const std::uint32_t index = static_cast<std::uint32_t>(e);
    if (static_cast<std::uint32_t>(e >> 32) == target && index != 0) return g_entries[index - 1].fn;
    return find_entry_slow(target);
}

[[gnu::noinline]] GuestFn find_entry_slow(std::uint32_t target) {
    auto it = std::lower_bound(g_entries.begin(), g_entries.end(), target,
                               [](const GuestEntry& e, std::uint32_t a) { return e.address < a; });
    if (it == g_entries.end() || it->address != target) return nullptr;
    const std::uint64_t index = static_cast<std::uint64_t>(it - g_entries.begin()) + 1;
    g_entry_cache[(target ^ (target >> 14)) & (kEntryCacheSize - 1)].store(std::uint64_t(target) << 32 | index,
                                                                            std::memory_order_relaxed);
    return it->fn;
}

// The rare paths, out of line: call_indirect runs for every virtual call and
// import, and with these inside it every call paid for their stack frame.
[[gnu::noinline, gnu::cold]] void import_unbound(Cpu& c, std::uint32_t target) {
    const std::uint32_t k = (target - ImportBase) / 16;
    if ((target - ImportBase) % 16 == 0 && k < g_imports.size()) {
        const ImportSlot& slot = g_imports[k];
        // The return address the guest pushed, and the thread, locate the caller.
        char what[160];
        std::snprintf(what, sizeof what, "%s (called from %08X, thread %u)", slot.name ? slot.name : "import",
                      ld32(c.mem, c.r[Cpu::ESP]), c.fs_base ? ld32(c.mem, c.fs_base + 0x24) : 0u);
        if (std::getenv("DCUSA_STATS") != nullptr) {
            // Code addresses on the guest stack: an approximate call chain.
            std::fprintf(stderr, "guest stack:");
            int shown = 0;
            const std::uint32_t top = c.fs_base ? ld32(c.mem, c.fs_base + 4) : c.r[Cpu::ESP];
            for (std::uint32_t p = c.r[Cpu::ESP]; p + 4 <= top && p < c.r[Cpu::ESP] + 0x4000u && shown < 24; p += 4) {
                const std::uint32_t v = ld32(c.mem, p);
                if (v >= 0x401000u && v < 0xD00000u) {
                    std::fprintf(stderr, " %08X", v);
                    ++shown;
                }
            }
            std::fprintf(stderr, "\n");
        }
        fail_closed(Unported::Import, slot.iat, what);
    }
    fail_closed(Unported::Import, target, "import slot without a handler");
}

// The fpatan tail of atan2 (2019 0xC5030C, GameAddresses::crt_fpatan_tail). The binary dispatcher jumps there,
// and that address is not a translated function entry. st(0) is x, st(1) is y.
[[gnu::noinline]] void fpatan_tail(Cpu& c) {
    const double x = c.fpu.at(0);
    const double y = c.fpu.at(1);
    c.fpu.pop();
    c.fpu.at(0) = std::atan2(y, x);
}

}  // namespace

void call_indirect(Cpu& c, std::uint32_t target) {
    if (target >= ImportBase) {
        const std::uint32_t k = (target - ImportBase) / 16;
        if ((target - ImportBase) % 16 == 0 && k < g_imports.size() && g_imports[k].handler) {
            // With parallel guest threads, the runtime is entered under its lock.
            if (rt_parallel()) {
                RtGuard locked;
                return g_imports[k].handler(c);
            }
            return g_imports[k].handler(c);
        }
        return import_unbound(c, target);
    }
    if (target >= kComCall) {
        if (rt_parallel()) {
            RtGuard locked;
            return call_com(c, target);
        }
        return call_com(c, target);
    }
    if (target == game_addresses().crt_fpatan_tail) return fpatan_tail(c);
    if (GuestFn fn = find_entry_cached(target)) return fn(c);
    fail_closed(Unported::Function, target, "indirect call to an address with no translated function");
}

std::uint32_t call_guest(Cpu& c, std::uint32_t target, std::initializer_list<std::uint32_t> args) {
    const std::uint32_t saved = c.r[Cpu::ESP];
    std::uint32_t esp = saved;
    for (auto it = args.end(); it != args.begin();) {
        esp -= 4;
        st32(c.mem, esp, *--it);
    }
    esp -= 4;
    st32(c.mem, esp, 0);  // return address: translated code returns natively
    c.r[Cpu::ESP] = esp;
    call_indirect(c, target);
    const std::uint32_t eax = c.r[Cpu::EAX];
    c.r[Cpu::ESP] = saved;
    return eax;
}

}

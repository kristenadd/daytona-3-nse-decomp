#include "recomp/dispatch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "recomp/d3d11.hpp"
#include "recomp/x86ops.hpp"
#include "runtime/fail_closed.hpp"

namespace dcusa::recomp {

namespace {

std::span<const GuestEntry> g_entries;
std::span<const ImportSlot> g_imports;

}

void dispatch_install(std::span<const GuestEntry> entries, std::span<const ImportSlot> imports) {
    g_entries = entries;
    g_imports = imports;
}

GuestFn find_entry(std::uint32_t target) {
    auto it = std::lower_bound(g_entries.begin(), g_entries.end(), target,
                               [](const GuestEntry& e, std::uint32_t a) { return e.address < a; });
    return it != g_entries.end() && it->address == target ? it->fn : nullptr;
}

void call_indirect(Cpu& c, std::uint32_t target) {
    if (target >= ImportBase) {
        std::uint32_t k = (target - ImportBase) / 16;
        if ((target - ImportBase) % 16 == 0 && k < g_imports.size()) {
            const ImportSlot& slot = g_imports[k];
            if (slot.handler) return slot.handler(c);
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
    if (target >= kComCall && target < ImportBase) return call_com(c, target);
    // 0xC5030C is the fpatan tail of atan2. The binary dispatcher jumps there,
    // and that address is not a translated function entry. st(0) is x, st(1) is y.
    if (target == 0x00C5030Cu) {
        const double x = c.fpu.at(0);
        const double y = c.fpu.at(1);
        c.fpu.pop();
        c.fpu.at(0) = std::atan2(y, x);
        return;
    }
    if (GuestFn fn = find_entry(target)) return fn(c);
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

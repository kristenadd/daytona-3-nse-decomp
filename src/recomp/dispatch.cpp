#include "recomp/dispatch.hpp"

#include <algorithm>

#include "runtime/fail_closed.hpp"

namespace dcusa::recomp {

namespace {

std::span<const GuestEntry> g_entries;
std::span<const GuestFn> g_imports;

}

void dispatch_install(std::span<const GuestEntry> entries, std::span<const GuestFn> imports) {
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
        if ((target - ImportBase) % 16 == 0 && k < g_imports.size() && g_imports[k]) return g_imports[k](c);
        fail_closed(Unported::Import, target, "import slot without a handler");
    }
    if (GuestFn fn = find_entry(target)) return fn(c);
    fail_closed(Unported::Function, target, "indirect call to an address with no translated function");
}

}

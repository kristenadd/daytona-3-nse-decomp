#include "runtime/fail_closed.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>

namespace dcusa {
namespace {

std::atomic<std::uint64_t> g_count{0};
std::mutex g_mutex;
std::optional<FailClosedReport> g_first;

}

std::string_view unported_name(Unported kind) {
    switch (kind) {
    case Unported::Function: return "function";
    case Unported::Instruction: return "instruction";
    case Unported::Import: return "import";
    case Unported::Service: return "service";
    case Unported::Data: return "data";
    }
    return "?";
}

std::string FailClosedReport::describe() const {
    std::string s = "not ported (";
    s += unported_name(kind);
    s += ")";
    if (guest_address) {
        char buf[16];
        std::snprintf(buf, sizeof buf, " %08X", guest_address);
        s += buf;
    }
    s += ": ";
    s += what;
    return s;
}

FailClosed::FailClosed(FailClosedReport report) : std::runtime_error(report.describe()), report_(std::move(report)) {}

void fail_closed(Unported kind, std::uint32_t guest_address, std::string_view what) {
    FailClosedReport report{kind, guest_address, std::string(what)};
    ++g_count;
    {
        std::lock_guard lock(g_mutex);
        if (!g_first) g_first = report;
    }
    throw FailClosed(std::move(report));
}

std::uint64_t fail_closed_count() { return g_count; }

const FailClosedReport* fail_closed_first() {
    std::lock_guard lock(g_mutex);
    return g_first ? &*g_first : nullptr;
}

void fail_closed_reset() {
    std::lock_guard lock(g_mutex);
    g_first.reset();
    g_count = 0;
}

}

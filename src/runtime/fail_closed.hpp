#pragma once
// Anything not ported stops the game with a report instead of being guessed:
// an untranslated function, an unsupported instruction, a Win32 import or
// arcade service without a native replacement, unexpected data.
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dcusa {

enum class Unported : std::uint8_t {
    Function,     // guest function with neither a translation nor a native override
    Instruction,  // x86 instruction the translator does not support
    Import,       // Win32 / DLL import without a native replacement
    Service,      // arcade service (Shell message, I/O, network, camera) not provided
    Data,         // guest data in a shape the port does not handle
};

std::string_view unported_name(Unported kind);

struct FailClosedReport {
    Unported kind{};
    std::uint32_t guest_address = 0;  // 0 when there is none
    std::string what;
    std::string describe() const;
};

// Thrown by fail_closed; the runner catches it at the top, reports and stops.
class FailClosed : public std::runtime_error {
public:
    explicit FailClosed(FailClosedReport report);
    const FailClosedReport& report() const { return report_; }

private:
    FailClosedReport report_;
};

// Records the report (the first one is kept for the exit summary) and throws FailClosed.
[[noreturn]] void fail_closed(Unported kind, std::uint32_t guest_address, std::string_view what);

std::uint64_t fail_closed_count();
// The first report since start or the last reset, if any.
const FailClosedReport* fail_closed_first();
void fail_closed_reset();

}

// A guest entry point that is known but not ported yet.
#define DCUSA_UNPORTED_FUNCTION(address, name) ::dcusa::fail_closed(::dcusa::Unported::Function, address, name)
#define DCUSA_UNPORTED_IMPORT(dll, name) ::dcusa::fail_closed(::dcusa::Unported::Import, 0, dll "!" name)

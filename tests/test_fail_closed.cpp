#include <string>

#include "check.hpp"
#include "runtime/fail_closed.hpp"

using namespace dcusa;

static void unported_leaf() { DCUSA_UNPORTED_FUNCTION(0x005DB060u, "m2e bus read"); }

int main() {
    fail_closed_reset();
    CHECK(fail_closed_count() == 0 && !fail_closed_first());

    bool caught = false;
    try {
        unported_leaf();
    } catch (const FailClosed& e) {
        caught = true;
        CHECK(e.report().kind == Unported::Function);
        CHECK(e.report().guest_address == 0x005DB060u);
        CHECK(std::string(e.what()) == "not ported (function) 005DB060: m2e bus read");
    }
    CHECK(caught);

    try {
        DCUSA_UNPORTED_IMPORT("AVICAP32.dll", "capCreateCaptureWindowA");
    } catch (const FailClosed& e) {
        CHECK(e.report().describe() == "not ported (import): AVICAP32.dll!capCreateCaptureWindowA");
    }

    // The first report is kept for the exit summary.
    CHECK(fail_closed_count() == 2);
    REQUIRE(fail_closed_first());
    CHECK(fail_closed_first()->guest_address == 0x005DB060u);
    fail_closed_reset();
    CHECK(fail_closed_count() == 0 && !fail_closed_first());
    return check_result();
}

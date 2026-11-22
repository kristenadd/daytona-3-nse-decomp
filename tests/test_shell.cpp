// The shell's answer to the game's security challenge passes the game's check:
// the check (ported from 0x584600) of shell_answer(c) gives c back.
#include <array>
#include <cstdint>

#include "check.hpp"
#include "recomp/shell.hpp"

using namespace dcusa::recomp;

int main() {
    std::uint32_t seed = 0x2468ACE1u;
    for (int n = 0; n < 20000; ++n) {
        seed = seed * 1664525u + 1013904223u;
        const std::array<std::uint8_t, 4> challenge = {static_cast<std::uint8_t>(seed), static_cast<std::uint8_t>(seed >> 8),
                                                       static_cast<std::uint8_t>(seed >> 16),
                                                       static_cast<std::uint8_t>(seed >> 24)};
        const std::array<std::uint8_t, 4> answer = shell_answer(challenge);
        CHECK(shell_check_transform(answer) == challenge);
    }
    // Distinct challenges have distinct answers (the transform is a bijection).
    CHECK(shell_answer({0, 0, 0, 0}) != shell_answer({1, 0, 0, 0}));
    return check_result();
}

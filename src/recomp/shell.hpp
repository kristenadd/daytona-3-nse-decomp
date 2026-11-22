#pragma once
// The cabinet shell's side of the game's link protocol (DYT packets over the
// shell socket), answered natively since Shell.exe does not run.
//
// Every 10 s the game sends message 0x2A with a 4-byte challenge and counts the
// challenges left unanswered; after five it asks for a reboot (0x584B20 ->
// 0x5BE410) and the boot ends in ExitProcess. The shell's answer is message
// 0x2E whose 4-byte payload, run through the game's check (0x584600), gives
// the challenge back.
#include <array>
#include <cstdint>

namespace dcusa::recomp {

// The game's check at 0x584600, applied to `response`: XOR 0xB1, twelve rounds
// of a 32-bit shift register with feedback from fixed bits, a bit reversal of
// byte 0, XOR 0xC0, XOR 0x77. A response is accepted when this equals the
// challenge.
std::array<std::uint8_t, 4> shell_check_transform(std::array<std::uint8_t, 4> response);

// The response the check accepts for `challenge`. The transform is affine over
// GF(2) and invertible, so this solves it exactly.
std::array<std::uint8_t, 4> shell_answer(const std::array<std::uint8_t, 4>& challenge);

}  // namespace dcusa::recomp

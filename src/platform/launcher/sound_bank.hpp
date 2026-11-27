#pragma once
// Reading the game's CRI sound bank (SE.csb: @UTF tables holding ADX) and
// decoding the launcher's sounds and music (ported from daytona-shim).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dcusa::launcher {

struct Pcm {
    std::vector<std::int16_t> samples;  // interleaved
    int channels = 0, rate = 0;
    // The loop the ADX header gives, in frames (end exclusive); 0 / 0 when none.
    std::uint32_t loop_start = 0, loop_end = 0;
};

// A standalone .adx file (the music), with its loop points.
bool decode_adx_file(const std::uint8_t* data, std::size_t size, Pcm& out);

// In the order of Sound: the name panel sound, decision, cancel, the engine.
constexpr int kBankSounds = 4;
// Fills `out` (kBankSounds entries); false when the bank cannot be read.
bool decode_bank(const std::uint8_t* bank, std::size_t size, Pcm out[kBankSounds]);

}  // namespace dcusa::launcher

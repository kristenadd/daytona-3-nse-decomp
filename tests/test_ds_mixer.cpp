// The DirectSound mixer: cursors, looping and one-shot buffers, volume and
// pan, the 5.1 fold-down and the frequency change.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "audio/ds_mixer.hpp"
#include "check.hpp"

using dcusa::audio::DsMixer;
using dcusa::audio::WaveFormat;

namespace {

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

WaveFormat pcm16(std::uint16_t channels, std::uint32_t rate) {
    WaveFormat f;
    f.channels = channels;
    f.rate = rate;
    f.bits = 16;
    f.block = static_cast<std::uint16_t>(2 * channels);
    return f;
}

std::vector<std::uint8_t> constant(std::uint16_t channels, std::uint32_t frames, std::int16_t value) {
    std::vector<std::uint8_t> b(static_cast<std::size_t>(frames) * channels * 2);
    for (std::size_t i = 0; i < b.size(); i += 2) std::memcpy(&b[i], &value, 2);
    return b;
}

void cursors_and_looping() {
    DsMixer m;
    auto data = constant(2, 4800, 16384);  // 0.5 full scale, 100 ms at 48 kHz
    m.create(1, pcm16(2, 48000), data.data(), static_cast<std::uint32_t>(data.size()));
    std::uint32_t play = 1, write = 1;
    m.position(1, &play, &write);
    CHECK(play == 0 && write == 0);  // stopped: both at the start
    CHECK(m.status(1) == 0);

    m.play(1, true);
    CHECK(m.status(1) == (DsMixer::kPlaying | DsMixer::kLooping));
    std::vector<float> out(2 * 1200);
    m.mix(out.data(), 1200);
    CHECK(near(out[0], 0.5f) && near(out[1], 0.5f));
    m.position(1, &play, &write);
    CHECK(play == 1200 * 4);
    CHECK(write == (1200 + 480) * 4);  // 10 ms ahead
    // Past the end of a looping buffer the cursor wraps.
    std::vector<float> more(2 * 4000);
    m.mix(more.data(), 4000);
    m.position(1, &play, &write);
    CHECK(play == (5200 - 4800) * 4);
    CHECK(m.status(1) != 0);
}

void one_shot_stops() {
    DsMixer m;
    auto data = constant(1, 100, 32767);
    m.create(2, pcm16(1, 48000), data.data(), static_cast<std::uint32_t>(data.size()));
    m.play(2, false);
    std::vector<float> out(2 * 300);
    m.mix(out.data(), 300);
    CHECK(near(out[0], 1.0f) && near(out[1], 1.0f));  // mono to both sides
    CHECK(out[2 * 150] == 0.0f);                       // silent after its end
    CHECK(m.status(2) == 0);
    std::uint32_t play = 1;
    m.position(2, &play, nullptr);
    CHECK(play == 0);
}

void volume_and_pan() {
    DsMixer m;
    auto data = constant(1, 480, 32767);
    m.create(3, pcm16(1, 48000), data.data(), static_cast<std::uint32_t>(data.size()));
    m.play(3, true);
    m.set_volume(3, -600);  // -6 dB
    std::vector<float> out(2 * 16);
    m.mix(out.data(), 16);
    CHECK(near(out[0], std::pow(10.0f, -0.3f)));
    m.set_volume(3, 0);
    m.set_pan(3, 10000);  // hard right: left at -100 dB
    m.mix(out.data(), 16);
    CHECK(out[0] < 1e-4f && near(out[1], 1.0f));
    m.set_volume(3, -10000);
    m.mix(out.data(), 16);
    CHECK(out[0] == 0.0f && out[1] == 0.0f);
    CHECK(m.volume(3) == -10000 && m.pan(3) == 10000);
}

void fold_down_5_1() {
    DsMixer m;
    // FL FR FC LFE BL BR = 0.25 0 0.25 0.9 0 0.25
    const std::int16_t frame[6] = {8192, 0, 8192, 29491, 0, 8192};
    std::vector<std::uint8_t> data(12 * 64);
    for (std::size_t i = 0; i < 64; ++i) std::memcpy(&data[12 * i], frame, 12);
    m.create(4, pcm16(6, 48000), data.data(), static_cast<std::uint32_t>(data.size()));
    m.play(4, true);
    std::vector<float> out(2 * 8);
    m.mix(out.data(), 8);
    const float front = 1.0f / (1.0f + 2.0f * 0.70710678f), side = 0.70710678f * front;
    CHECK(near(out[0], front * 0.25f + side * 0.25f));  // FL + C, no LFE
    CHECK(near(out[1], side * 0.25f + side * 0.25f));   // C + BR
}

void frequency_change() {
    DsMixer m;
    auto data = constant(2, 44100, 1000);
    m.create(5, pcm16(2, 48000), data.data(), static_cast<std::uint32_t>(data.size()));
    m.set_frequency(5, 44100);
    CHECK(m.frequency(5) == 44100);
    m.play(5, true);
    std::vector<float> out(2 * 48000);
    m.mix(out.data(), 48000);  // one second of output takes one second of 44.1 kHz data
    std::uint32_t play = 0;
    m.position(5, &play, nullptr);
    CHECK(play / 4 <= 1 || play / 4 >= 44099);  // wrapped back to (about) the start
    m.set_frequency(5, 0);
    CHECK(m.frequency(5) == 48000);
}

}  // namespace

int main() {
    cursors_and_looping();
    one_shot_stops();
    volume_and_pan();
    fold_down_5_1();
    frequency_change();
    return check_result();
}

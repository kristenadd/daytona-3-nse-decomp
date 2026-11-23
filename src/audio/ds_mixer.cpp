#include "audio/ds_mixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dcusa::audio {
namespace {

// DirectSound volume and pan are attenuations in hundredths of a decibel.
float attenuation(std::int32_t hundredths_db) {
    if (hundredths_db <= -10000) return 0.0f;
    return std::pow(10.0f, static_cast<float>(hundredths_db) / 2000.0f);
}

std::int16_t sample16(const std::uint8_t* p) {
    std::int16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

}  // namespace

DsMixer::Voice* DsMixer::find(std::uint32_t id) {
    auto it = voices_.find(id);
    return it == voices_.end() ? nullptr : &it->second;
}

void DsMixer::create(std::uint32_t id, const WaveFormat& format, const std::uint8_t* data, std::uint32_t bytes) {
    std::lock_guard<std::mutex> g(lock_);
    Voice v;
    v.format = format;
    if (v.format.block == 0) v.format.block = static_cast<std::uint16_t>(std::max(1, format.channels * format.bits / 8));
    v.data = data;
    v.bytes = bytes;
    v.frames = bytes / v.format.block;
    voices_[id] = v;
}

void DsMixer::remove(std::uint32_t id) {
    std::lock_guard<std::mutex> g(lock_);
    voices_.erase(id);
}

void DsMixer::play(std::uint32_t id, bool loop) {
    std::lock_guard<std::mutex> g(lock_);
    if (Voice* v = find(id)) {
        v->playing = v->frames != 0;
        v->loop = loop;
    }
}

void DsMixer::stop(std::uint32_t id) {
    std::lock_guard<std::mutex> g(lock_);
    if (Voice* v = find(id)) v->playing = false;
}

std::uint32_t DsMixer::status(std::uint32_t id) {
    std::lock_guard<std::mutex> g(lock_);
    const Voice* v = find(id);
    if (v == nullptr || !v->playing) return 0;
    return kPlaying | (v->loop ? kLooping : 0u);
}

void DsMixer::position(std::uint32_t id, std::uint32_t* play, std::uint32_t* write) {
    std::lock_guard<std::mutex> g(lock_);
    const Voice* v = find(id);
    std::uint32_t p = 0, w = 0;
    if (v != nullptr && v->frames != 0) {
        const std::uint32_t frame = static_cast<std::uint32_t>(v->pos) % v->frames;
        p = frame * v->format.block;
        // DirectSound's write cursor leads the play cursor by the data already
        // committed to the device: about 10 ms here (the output pulls in
        // blocks of that order).
        const std::uint32_t lead = v->playing ? std::max<std::uint32_t>(1, v->format.rate / 100) : 0;
        w = ((frame + lead) % v->frames) * v->format.block;
    }
    if (play) *play = p;
    if (write) *write = w;
}

void DsMixer::set_position(std::uint32_t id, std::uint32_t bytes) {
    std::lock_guard<std::mutex> g(lock_);
    if (Voice* v = find(id); v != nullptr && v->frames != 0) v->pos = (bytes / v->format.block) % v->frames;
}

void DsMixer::set_volume(std::uint32_t id, std::int32_t hundredths_db) {
    std::lock_guard<std::mutex> g(lock_);
    if (Voice* v = find(id)) v->volume = std::clamp(hundredths_db, -10000, 0);
}

void DsMixer::set_pan(std::uint32_t id, std::int32_t pan) {
    std::lock_guard<std::mutex> g(lock_);
    if (Voice* v = find(id)) v->pan = std::clamp(pan, -10000, 10000);
}

void DsMixer::set_frequency(std::uint32_t id, std::uint32_t hz) {
    std::lock_guard<std::mutex> g(lock_);
    if (Voice* v = find(id)) v->frequency = hz;
}

std::int32_t DsMixer::volume(std::uint32_t id) {
    std::lock_guard<std::mutex> g(lock_);
    const Voice* v = find(id);
    return v ? v->volume : 0;
}

std::int32_t DsMixer::pan(std::uint32_t id) {
    std::lock_guard<std::mutex> g(lock_);
    const Voice* v = find(id);
    return v ? v->pan : 0;
}

std::uint32_t DsMixer::frequency(std::uint32_t id) {
    std::lock_guard<std::mutex> g(lock_);
    const Voice* v = find(id);
    if (v == nullptr) return kOutputRate;
    return v->frequency != 0 ? v->frequency : v->format.rate;
}

// One frame as stereo. 5.1 (FL FR FC LFE BL BR) folds down with the centre and
// surrounds at -3 dB relative to the front, normalised so the three weights add
// up to 1 and a full-scale mix cannot clip (as Windows does when it folds a
// 5.1 stream for stereo speakers). The LFE channel is left out.
void DsMixer::read_frame(const Voice& v, std::uint32_t frame, float* l, float* r) {
    const std::uint8_t* p = v.data + static_cast<std::size_t>(frame) * v.format.block;
    const unsigned ch = v.format.channels;
    auto s = [&](unsigned i) -> float {
        if (v.format.bits == 8) return (static_cast<float>(p[i]) - 128.0f) / 128.0f;
        return static_cast<float>(sample16(p + 2 * i)) / 32768.0f;
    };
    if (ch == 1) {
        *l = *r = s(0);
    } else if (ch >= 6) {
        constexpr float front = 1.0f / (1.0f + 2.0f * 0.70710678f);
        constexpr float side = 0.70710678f * front;
        *l = front * s(0) + side * s(2) + side * s(4);
        *r = front * s(1) + side * s(2) + side * s(5);
    } else {
        *l = s(0);
        *r = s(1);
    }
}

void DsMixer::mix(float* out, std::size_t frames) {
    std::fill(out, out + 2 * frames, 0.0f);
    std::lock_guard<std::mutex> g(lock_);
    static const bool stats = std::getenv("DCUSA_AUDIO_STATS") != nullptr;
    if (stats && (frames_mixed_ += frames) >= 10 * kOutputRate) {
        frames_mixed_ = 0;
        report();
    }
    for (auto& [id, v] : voices_) {
        if (!v.playing || v.frames == 0 || v.data == nullptr) continue;
        const float gain = attenuation(v.volume);
        const float gl = gain * (v.pan > 0 ? attenuation(-v.pan) : 1.0f);
        const float gr = gain * (v.pan < 0 ? attenuation(v.pan) : 1.0f);
        const double step = static_cast<double>(v.frequency != 0 ? v.frequency : v.format.rate) / kOutputRate;
        for (std::size_t i = 0; i < frames; ++i) {
            const std::uint32_t f0 = static_cast<std::uint32_t>(v.pos);
            const float t = static_cast<float>(v.pos - f0);
            std::uint32_t f1 = f0 + 1;
            if (f1 >= v.frames) f1 = v.loop ? 0 : f0;
            float l0, r0, l1, r1;
            read_frame(v, f0, &l0, &r0);
            read_frame(v, f1, &l1, &r1);
            if (stats) {
                const float m = std::max(std::fabs(l0), std::fabs(r0));
                v.sum_sq += l0 * l0 + r0 * r0;
                v.peak = std::max(v.peak, m);
                v.full_scale += m >= 0.9999f;
                ++v.mixed;
            }
            out[2 * i] += gl * (l0 + (l1 - l0) * t);
            out[2 * i + 1] += gr * (r0 + (r1 - r0) * t);
            v.pos += step;
            if (v.pos >= v.frames) {
                if (v.loop) {
                    v.pos -= v.frames;
                } else {
                    // A one-shot buffer stops at its end, with the cursor back at the start.
                    v.playing = false;
                    v.pos = 0;
                    break;
                }
            }
        }
    }
    for (std::size_t i = 0; i < 2 * frames; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

// One line per voice: format, state, volume, and the source's level (before
// volume) since the last report; "full" counts frames at full scale.
void DsMixer::report() {
    for (auto& [id, v] : voices_) {
        const double rms = v.mixed ? std::sqrt(v.sum_sq / (2.0 * static_cast<double>(v.mixed))) : 0.0;
        std::fprintf(stderr, "audio voice %08x %uch %uHz %s%s vol %d pan %d freq %u: rms %.3f peak %.3f full %llu of %llu\n",
                     id, v.format.channels, v.format.rate, v.playing ? "playing" : "stopped", v.loop ? " loop" : "",
                     v.volume, v.pan, v.frequency, rms, v.peak, static_cast<unsigned long long>(v.full_scale),
                     static_cast<unsigned long long>(v.mixed));
        // DCUSA_AUDIO_DUMP=<dir>: each voice's buffer memory as it is now.
        if (const char* dir = std::getenv("DCUSA_AUDIO_DUMP"); dir != nullptr && v.data != nullptr) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/voice_%08x_%uch.raw", dir, id, v.format.channels);
            if (std::FILE* f = std::fopen(path, "wb")) {
                std::fwrite(v.data, 1, v.bytes, f);
                std::fclose(f);
            }
        }
        v.sum_sq = 0;
        v.peak = 0;
        v.mixed = v.full_scale = 0;
    }
}

void DsMixer::pull(void* user, std::span<float> interleaved_stereo) {
    static_cast<DsMixer*>(user)->mix(interleaved_stereo.data(), interleaved_stereo.size() / 2);
}

}  // namespace dcusa::audio

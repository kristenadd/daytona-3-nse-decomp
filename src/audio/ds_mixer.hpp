#pragma once
// DirectSound secondary buffers, mixed to stereo float.
//
// The game's own audio engine mixes everything (5.1, 48 kHz, 16-bit) into one
// streaming DirectSound buffer and keeps it fed from GetCurrentPosition; CRI
// keeps two buffers of its own. So emulating DirectSound faithfully is all the
// game needs: buffer memory (in guest memory, so Lock can hand it out), a play
// cursor that advances as the output consumes samples, looping, and volume,
// pan and frequency. The output (Core Audio, or a paced null device) pulls
// mix() from its own thread; the guest's calls take the same lock.
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>

namespace dcusa::audio {

struct WaveFormat {
    std::uint16_t tag = 1;        // WAVE_FORMAT_PCM, or 0xFFFE (EXTENSIBLE, PCM inside)
    std::uint16_t channels = 2;
    std::uint32_t rate = 44100;
    std::uint16_t block = 4;      // bytes per frame
    std::uint16_t bits = 16;      // 8 (unsigned) or 16 (signed)
};

class DsMixer {
public:
    static constexpr std::uint32_t kOutputRate = 48000;
    // DSBSTATUS_PLAYING and DSBSTATUS_LOOPING.
    static constexpr std::uint32_t kPlaying = 1, kLooping = 4;

    // `data` is the buffer's memory (host pointer to its guest bytes).
    void create(std::uint32_t id, const WaveFormat& format, const std::uint8_t* data, std::uint32_t bytes);
    void remove(std::uint32_t id);
    void play(std::uint32_t id, bool loop);
    void stop(std::uint32_t id);
    std::uint32_t status(std::uint32_t id);
    // Byte offsets. The write cursor is where data can safely go: a little
    // ahead of what the output has already taken.
    void position(std::uint32_t id, std::uint32_t* play, std::uint32_t* write);
    void set_position(std::uint32_t id, std::uint32_t bytes);
    void set_volume(std::uint32_t id, std::int32_t hundredths_db);   // -10000 .. 0
    void set_pan(std::uint32_t id, std::int32_t pan);                // -10000 left .. 10000 right
    void set_frequency(std::uint32_t id, std::uint32_t hz);          // 0: the format's rate
    std::int32_t volume(std::uint32_t id);
    std::int32_t pan(std::uint32_t id);
    std::uint32_t frequency(std::uint32_t id);

    // Adds every playing buffer into `stereo` (interleaved L R, `frames` frames
    // at kOutputRate), after clearing it.
    void mix(float* stereo, std::size_t frames);
    // AudioOutput::Pull adapter; `user` is the DsMixer.
    static void pull(void* user, std::span<float> interleaved_stereo);

private:
    struct Voice {
        WaveFormat format;
        const std::uint8_t* data = nullptr;
        std::uint32_t bytes = 0, frames = 0;
        double pos = 0;  // frames
        bool playing = false, loop = false;
        std::int32_t volume = 0, pan = 0;
        std::uint32_t frequency = 0;
        // DCUSA_AUDIO_STATS: what this voice contributed since the last report.
        double sum_sq = 0;
        float peak = 0;
        std::uint64_t mixed = 0, full_scale = 0;
    };
    void report();
    std::uint64_t frames_mixed_ = 0;
    Voice* find(std::uint32_t id);
    static void read_frame(const Voice& v, std::uint32_t frame, float* l, float* r);

    std::mutex lock_;
    std::unordered_map<std::uint32_t, Voice> voices_;
};

}  // namespace dcusa::audio

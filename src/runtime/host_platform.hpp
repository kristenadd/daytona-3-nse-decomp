#pragma once
// What each platform (macOS, iOS / iPadOS, visionOS, the headless runner)
// provides, and nothing more: the cabinet controls, a sound output and a
// way to show frames. Everything else is shared.
#include <cstdint>
#include <span>

namespace dcusa {

// The cabinet's controls as the game sees them, sampled once per frame.
struct PadSnapshot {
    float steering = 0.0f;  // -1 full left .. +1 full right
    float accelerator = 0.0f, brake = 0.0f;  // 0 .. 1
    enum Button : std::uint32_t {
        View1 = 1u << 0, View2 = 1u << 1, View3 = 1u << 2, View4 = 1u << 3,  // VR buttons
        ShiftUp = 1u << 4, ShiftDown = 1u << 5,
        Start = 1u << 6,
        Test = 1u << 7, Service = 1u << 8, Coin = 1u << 9,
        Gear1 = 1u << 10, Gear2 = 1u << 11, Gear3 = 1u << 12, Gear4 = 1u << 13,  // direct gear selection
    };
    std::uint32_t buttons = 0;
    std::int8_t gear = 0;  // H-pattern lever: 0 neutral or not fitted, 1..4
    bool connected = false;
};

class PadSource {
public:
    virtual ~PadSource() = default;
    virtual PadSnapshot sample() = 0;
    // Force feedback the game asked for, -1 .. +1 (platforms map it to haptics or ignore it).
    virtual void force_feedback(float /*torque*/) {}
};

// Interleaved float stereo at a fixed rate; the platform pulls frames from the mixer.
class AudioOutput {
public:
    virtual ~AudioOutput() = default;
    using Pull = void (*)(void* user, std::span<float> interleaved_stereo);
    virtual bool start(std::uint32_t sample_rate, Pull pull, void* user) = 0;
    virtual void stop() = 0;
};

// Shows finished frames. The graphics device (Phase 3) renders into a target
// the presenter owns; the headless runner's presenter keeps CPU copies.
class Presenter {
public:
    virtual ~Presenter() = default;
    virtual void drawable_size(std::uint32_t& width, std::uint32_t& height) = 0;
    virtual void present() = 0;
};

}

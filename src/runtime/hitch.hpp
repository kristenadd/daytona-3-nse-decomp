#pragma once
// DCUSA_HITCH=<ms>: where a slow frame's time went. Host code that can stall a
// frame (file reads, guest waits, resource and shader creation, the render
// thread) adds its time to a slot; Present prints the slots for every frame
// longer than <ms> (d3d11.cpp) and clears them each frame.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace dcusa::hitch {

enum Slot { Io, Wait, Texture, Buffer, Shader, Handover, Pipeline, Count };

inline std::atomic<std::uint64_t> g_ns[Count];
inline std::atomic<std::uint32_t> g_calls[Count];

inline bool enabled() {
    static const bool on = std::getenv("DCUSA_HITCH") != nullptr;
    return on;
}

// Adds the time from construction to destruction to `slot` (nothing when off).
class Timer {
public:
    explicit Timer(Slot slot, bool when = true) : slot_(slot), on_(when && enabled()) {
        if (on_) t0_ = std::chrono::steady_clock::now();
    }
    ~Timer() {
        if (!on_) return;
        g_ns[slot_] += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0_).count());
        ++g_calls[slot_];
    }
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    Slot slot_;
    bool on_;
    std::chrono::steady_clock::time_point t0_;
};

}  // namespace dcusa::hitch

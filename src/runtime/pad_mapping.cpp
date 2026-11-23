#include "runtime/pad_mapping.hpp"

#include <algorithm>
#include <cmath>

namespace dcusa {

float apply_dead_zone(float value, float dead_zone) {
    if (!(value == value)) return 0.0f;  // NaN
    const float v = std::clamp(value, -1.0f, 1.0f);
    const float mag = std::fabs(v);
    if (mag <= dead_zone) return 0.0f;
    return std::copysign((mag - dead_zone) / (1.0f - dead_zone), v);
}

PadSnapshot map_gamepad(const GamepadState& pad) {
    PadSnapshot s;
    s.connected = pad.connected;
    if (!pad.connected) return s;
    s.steering = apply_dead_zone(pad.left_x, kStickDeadZone);
    s.accelerator = std::clamp(pad.right_trigger, 0.0f, 1.0f);
    s.brake = std::clamp(pad.left_trigger, 0.0f, 1.0f);
    const auto set = [&](bool on, PadSnapshot::Button b) {
        if (on) s.buttons |= b;
    };
    set(pad.back, PadSnapshot::Coin);
    set(pad.start, PadSnapshot::Start);
    set(pad.right_shoulder, PadSnapshot::ShiftUp);
    set(pad.left_shoulder, PadSnapshot::ShiftDown);
    set(pad.y, PadSnapshot::View1);
    set(pad.x, PadSnapshot::View2);
    set(pad.b, PadSnapshot::View3);
    set(pad.a, PadSnapshot::View4);
    return s;
}

}  // namespace dcusa

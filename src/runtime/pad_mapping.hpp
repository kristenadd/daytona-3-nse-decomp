#pragma once
// The default gamepad layout: a controller's state, by position, to the
// cabinet controls in a PadSnapshot. Positional names follow the Xbox layout;
// on a PlayStation pad Y is Triangle, X is Square, B is Circle, A is Cross,
// Back is Share / Create and Start is Options.
//
//   Coin            Back            View 1  Y          Steering  left stick X
//   Start           Start           View 2  X          Gas       right trigger
//   Shift up        right shoulder  View 3  B          Brake     left trigger
//   Shift down      left shoulder   View 4  A          Gear 1-4  unassigned
//
// Shifting, gear limits and the AT / MT choice stay the game's: the snapshot
// holds the buttons down and the game counts each press once.
#include "runtime/host_platform.hpp"

namespace dcusa {

struct GamepadState {
    bool connected = false;
    float left_x = 0.0f;                                   // -1 left .. +1 right
    float left_trigger = 0.0f, right_trigger = 0.0f;       // 0 .. 1
    bool a = false, b = false, x = false, y = false;
    bool left_shoulder = false, right_shoulder = false;
    bool back = false, start = false;
};

// Stick travel inside the dead zone reads as centre; the rest is rescaled so
// full deflection is still full lock.
constexpr float kStickDeadZone = 0.10f;
float apply_dead_zone(float value, float dead_zone);

PadSnapshot map_gamepad(const GamepadState& pad);

}  // namespace dcusa

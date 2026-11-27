#pragma once
// A controller's state, by position, to the cabinet controls in a
// PadSnapshot. Positional names follow the Xbox layout; on a PlayStation pad
// Y is Triangle, X is Square, B is Circle, A is Cross, Back is Share / Create
// and Start is Options. The default layout:
//
//   Coin            Back            View 1  Y          Steering  left stick X
//   Start           Start           View 2  X          Gas       right trigger
//   Shift up        right shoulder  View 3  B          Brake     left trigger
//   Shift down      left shoulder   View 4  A          Gear 1-4  unassigned
//
// The launcher's CONTROLS screen changes it per edition (controls.ini in the
// edition's container: PadBindings). Shifting, gear limits and the AT / MT
// choice stay the game's: the snapshot holds the buttons down and the game
// counts each press once.
#include <cstdint>
#include <filesystem>
#include <string>

#include "runtime/host_platform.hpp"

namespace dcusa {

struct GamepadState {
    bool connected = false;
    float left_x = 0.0f, left_y = 0.0f;                    // -1 left / up .. +1 right / down
    float right_x = 0.0f, right_y = 0.0f;
    float left_trigger = 0.0f, right_trigger = 0.0f;       // 0 .. 1
    bool a = false, b = false, x = false, y = false;
    bool left_shoulder = false, right_shoulder = false;
    bool back = false, start = false;
    bool left_stick = false, right_stick = false;          // stick presses
    bool dpad_up = false, dpad_down = false, dpad_left = false, dpad_right = false;
    bool misc = false;                                     // Share / Capture / Mic
    bool paddle[4] = {};
    bool touchpad = false;
    // What the bottom face button is called, for the launcher's prompts.
    enum class Labels : std::uint8_t { Xbox, PlayStation, Nintendo } labels = Labels::Xbox;
};

// Stick travel inside the dead zone reads as centre; the rest is rescaled so
// full deflection is still full lock.
constexpr float kStickDeadZone = 0.10f;
float apply_dead_zone(float value, float dead_zone);

// ---- the bindings ----

// The pad's buttons and axes by name (SDL's mapping names; positions).
enum class PadButton : std::uint8_t {
    A, B, X, Y, Back, Start, LeftShoulder, RightShoulder, LeftStick, RightStick,
    DpadUp, DpadDown, DpadLeft, DpadRight, Misc, Paddle1, Paddle2, Paddle3, Paddle4, Touchpad, Count
};
enum class PadAxis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

struct PadBinding {
    enum Kind : std::uint8_t { None, Button, Axis } kind = None;
    std::uint8_t index = 0;  // PadButton or PadAxis
    bool operator==(const PadBinding&) const = default;
};

const char* pad_button_name(PadButton b);    // "a", "rightshoulder", ...
const char* pad_button_label(PadButton b);   // "A / Cross (bottom)", ...
const char* pad_axis_name(PadAxis a);        // "leftx", "righttrigger", ...
const char* pad_axis_label(PadAxis a);       // "Left stick X", ...
PadBinding parse_pad_binding(const std::string& name);  // None for "none" or unknown
std::string pad_binding_name(const PadBinding& b);
std::string pad_binding_label(const PadBinding& b);
bool pad_button_down(const GamepadState& s, PadButton b);
float pad_axis_value(const GamepadState& s, PadAxis a);

// The game's actions, in controls.ini's order: three analog ones, then the
// buttons.
enum class PadAction : std::uint8_t {
    Steering, Gas, Brake, Start, Coin, ShiftUp, ShiftDown, View1, View2, View3, View4, Gear1, Gear2, Gear3, Gear4, Count
};
constexpr int kPadActions = static_cast<int>(PadAction::Count);
const char* pad_action_key(PadAction a);    // controls.ini key: "Steering", "ShiftUp", ...
const char* pad_action_label(PadAction a);  // "Steering", "Shift up", ...

struct PadBindings {
    PadBinding action[kPadActions];
    static PadBindings defaults();  // the layout above
    // controls.ini [Pad] (missing keys keep the defaults); save writes all.
    static PadBindings load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file) const;
};

// The bindings map_gamepad uses (the defaults until set).
void set_pad_bindings(const PadBindings& b);
PadSnapshot map_gamepad(const GamepadState& pad);
PadSnapshot map_gamepad(const GamepadState& pad, const PadBindings& b);

// A controller read as a GamepadState; the launcher reads it raw, the game
// through map_gamepad.
class GamepadSource : public PadSource {
public:
    virtual GamepadState read() = 0;
    PadSnapshot sample() override { return map_gamepad(read()); }
};

}  // namespace dcusa

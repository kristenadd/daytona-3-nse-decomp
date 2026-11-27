#include "runtime/pad_mapping.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace dcusa {

float apply_dead_zone(float value, float dead_zone) {
    if (!(value == value)) return 0.0f;  // NaN
    const float v = std::clamp(value, -1.0f, 1.0f);
    const float mag = std::fabs(v);
    if (mag <= dead_zone) return 0.0f;
    return std::copysign((mag - dead_zone) / (1.0f - dead_zone), v);
}

namespace {

struct ButtonName {
    const char* name;
    const char* label;
};
constexpr ButtonName kButtons[] = {
    {"a", "A / Cross (bottom)"},   {"b", "B / Circle (right)"},   {"x", "X / Square (left)"},
    {"y", "Y / Triangle (top)"},   {"back", "Back / Share / -"},  {"start", "Start / Options / +"},
    {"leftshoulder", "Left bumper"}, {"rightshoulder", "Right bumper"}, {"leftstick", "Left stick press"},
    {"rightstick", "Right stick press"}, {"dpup", "D-pad up"}, {"dpdown", "D-pad down"},
    {"dpleft", "D-pad left"},      {"dpright", "D-pad right"},    {"misc1", "Misc / Capture / Mic"},
    {"paddle1", "Back paddle 1"},  {"paddle2", "Back paddle 2"},  {"paddle3", "Back paddle 3"},
    {"paddle4", "Back paddle 4"},  {"touchpad", "Touchpad press"},
};
constexpr ButtonName kAxes[] = {
    {"leftx", "Left stick X"},  {"lefty", "Left stick Y"},     {"rightx", "Right stick X"},
    {"righty", "Right stick Y"}, {"lefttrigger", "Left trigger"}, {"righttrigger", "Right trigger"},
};
constexpr ButtonName kActions[] = {
    {"Steering", "Steering"}, {"Gas", "Gas"},       {"Brake", "Brake"},     {"Start", "Start"},
    {"Coin", "Coin"},         {"ShiftUp", "Shift up"}, {"ShiftDown", "Shift down"}, {"View1", "View 1"},
    {"View2", "View 2"},      {"View3", "View 3"},   {"View4", "View 4"},   {"Gear1", "Gear 1"},
    {"Gear2", "Gear 2"},      {"Gear3", "Gear 3"},   {"Gear4", "Gear 4"},
};
static_assert(std::size(kButtons) == static_cast<std::size_t>(PadButton::Count));
static_assert(std::size(kAxes) == static_cast<std::size_t>(PadAxis::Count));
static_assert(std::size(kActions) == static_cast<std::size_t>(kPadActions));

// The PadSnapshot bit of each button action (Start .. Gear4).
constexpr std::uint32_t kActionBits[] = {
    0, 0, 0, PadSnapshot::Start, PadSnapshot::Coin, PadSnapshot::ShiftUp, PadSnapshot::ShiftDown,
    PadSnapshot::View1, PadSnapshot::View2, PadSnapshot::View3, PadSnapshot::View4,
    PadSnapshot::Gear1, PadSnapshot::Gear2, PadSnapshot::Gear3, PadSnapshot::Gear4,
};

PadBinding button(PadButton b) { return {PadBinding::Button, static_cast<std::uint8_t>(b)}; }
PadBinding axis(PadAxis a) { return {PadBinding::Axis, static_cast<std::uint8_t>(a)}; }

std::mutex bindings_lock;
PadBindings& current() {
    static PadBindings b = PadBindings::defaults();
    return b;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

}  // namespace

const char* pad_button_name(PadButton b) { return kButtons[static_cast<int>(b)].name; }
const char* pad_button_label(PadButton b) { return kButtons[static_cast<int>(b)].label; }
const char* pad_axis_name(PadAxis a) { return kAxes[static_cast<int>(a)].name; }
const char* pad_axis_label(PadAxis a) { return kAxes[static_cast<int>(a)].label; }
const char* pad_action_key(PadAction a) { return kActions[static_cast<int>(a)].name; }
const char* pad_action_label(PadAction a) { return kActions[static_cast<int>(a)].label; }

PadBinding parse_pad_binding(const std::string& name) {
    for (int i = 0; i < static_cast<int>(PadButton::Count); ++i)
        if (name == kButtons[i].name) return button(static_cast<PadButton>(i));
    for (int i = 0; i < static_cast<int>(PadAxis::Count); ++i)
        if (name == kAxes[i].name) return axis(static_cast<PadAxis>(i));
    return {};
}

std::string pad_binding_name(const PadBinding& b) {
    if (b.kind == PadBinding::Button) return kButtons[b.index].name;
    if (b.kind == PadBinding::Axis) return kAxes[b.index].name;
    return "none";
}

std::string pad_binding_label(const PadBinding& b) {
    if (b.kind == PadBinding::Button) return kButtons[b.index].label;
    if (b.kind == PadBinding::Axis) return kAxes[b.index].label;
    return "None";
}

bool pad_button_down(const GamepadState& s, PadButton b) {
    switch (b) {
        case PadButton::A: return s.a;
        case PadButton::B: return s.b;
        case PadButton::X: return s.x;
        case PadButton::Y: return s.y;
        case PadButton::Back: return s.back;
        case PadButton::Start: return s.start;
        case PadButton::LeftShoulder: return s.left_shoulder;
        case PadButton::RightShoulder: return s.right_shoulder;
        case PadButton::LeftStick: return s.left_stick;
        case PadButton::RightStick: return s.right_stick;
        case PadButton::DpadUp: return s.dpad_up;
        case PadButton::DpadDown: return s.dpad_down;
        case PadButton::DpadLeft: return s.dpad_left;
        case PadButton::DpadRight: return s.dpad_right;
        case PadButton::Misc: return s.misc;
        case PadButton::Paddle1: return s.paddle[0];
        case PadButton::Paddle2: return s.paddle[1];
        case PadButton::Paddle3: return s.paddle[2];
        case PadButton::Paddle4: return s.paddle[3];
        case PadButton::Touchpad: return s.touchpad;
        default: return false;
    }
}

float pad_axis_value(const GamepadState& s, PadAxis a) {
    switch (a) {
        case PadAxis::LeftX: return s.left_x;
        case PadAxis::LeftY: return s.left_y;
        case PadAxis::RightX: return s.right_x;
        case PadAxis::RightY: return s.right_y;
        case PadAxis::LeftTrigger: return s.left_trigger;
        case PadAxis::RightTrigger: return s.right_trigger;
        default: return 0.0f;
    }
}

PadBindings PadBindings::defaults() {
    PadBindings b;
    const auto set = [&](PadAction a, PadBinding v) { b.action[static_cast<int>(a)] = v; };
    set(PadAction::Steering, axis(PadAxis::LeftX));
    set(PadAction::Gas, axis(PadAxis::RightTrigger));
    set(PadAction::Brake, axis(PadAxis::LeftTrigger));
    set(PadAction::Start, button(PadButton::Start));
    set(PadAction::Coin, button(PadButton::Back));
    set(PadAction::ShiftUp, button(PadButton::RightShoulder));
    set(PadAction::ShiftDown, button(PadButton::LeftShoulder));
    set(PadAction::View1, button(PadButton::Y));
    set(PadAction::View2, button(PadButton::X));
    set(PadAction::View3, button(PadButton::B));
    set(PadAction::View4, button(PadButton::A));
    return b;
}

PadBindings PadBindings::load(const std::filesystem::path& file) {
    PadBindings b = defaults();
    std::ifstream in(file);
    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[') {
            section = trim(line.substr(1, line.find(']') - 1));
            continue;
        }
        const auto eq = line.find('=');
        if (section != "Pad" || eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        for (int i = 0; i < kPadActions; ++i)
            if (key == kActions[i].name) b.action[i] = parse_pad_binding(value);
    }
    return b;
}

bool PadBindings::save(const std::filesystem::path& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    out << "; The controller layout for this edition (the launcher's CONTROLS).\n"
           "; Pad buttons and axes by position: a (bottom), b (right), x (left), y (top),\n"
           "; back, start, leftshoulder, rightshoulder, leftstick, rightstick, dpup,\n"
           "; dpdown, dpleft, dpright, misc1, paddle1-4, touchpad; axes leftx, lefty,\n"
           "; rightx, righty, lefttrigger, righttrigger; none.\n[Pad]\n";
    for (int i = 0; i < kPadActions; ++i) out << kActions[i].name << '=' << pad_binding_name(action[i]) << '\n';
    return static_cast<bool>(out);
}

void set_pad_bindings(const PadBindings& b) {
    std::lock_guard<std::mutex> g(bindings_lock);
    current() = b;
}

PadSnapshot map_gamepad(const GamepadState& pad) {
    PadBindings b;
    {
        std::lock_guard<std::mutex> g(bindings_lock);
        b = current();
    }
    return map_gamepad(pad, b);
}

PadSnapshot map_gamepad(const GamepadState& pad, const PadBindings& b) {
    PadSnapshot s;
    s.connected = pad.connected;
    if (!pad.connected) return s;
    const auto value = [&](const PadBinding& v) -> float {
        if (v.kind == PadBinding::Axis) return pad_axis_value(pad, static_cast<PadAxis>(v.index));
        if (v.kind == PadBinding::Button) return pad_button_down(pad, static_cast<PadButton>(v.index)) ? 1.0f : 0.0f;
        return 0.0f;
    };
    const PadBinding& steer = b.action[static_cast<int>(PadAction::Steering)];
    s.steering = steer.kind == PadBinding::Axis ? apply_dead_zone(value(steer), kStickDeadZone) : 0.0f;
    s.accelerator = std::clamp(value(b.action[static_cast<int>(PadAction::Gas)]), 0.0f, 1.0f);
    s.brake = std::clamp(value(b.action[static_cast<int>(PadAction::Brake)]), 0.0f, 1.0f);
    for (int i = static_cast<int>(PadAction::Start); i < kPadActions; ++i)
        if (value(b.action[i]) > 0.5f) s.buttons |= kActionBits[i];
    return s;
}

}  // namespace dcusa

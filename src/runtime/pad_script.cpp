#include "runtime/pad_script.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <string>

namespace dcusa {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool parse_uint(std::string_view s, unsigned* out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), *out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

}  // namespace

bool ScriptPad::parse(std::string_view text, std::string* error) {
    steps_.clear();
    frame_ = 0;
    unsigned line_no = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++line_no;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        const auto fail = [&](const char* why) {
            if (error) *error = "pad script line " + std::to_string(line_no) + ": " + why;
            return false;
        };
        const std::size_t space = line.find_first_of(" \t");
        if (space == std::string_view::npos) return fail("expected '<frames> <control>'");
        const std::string_view frames = line.substr(0, space);
        std::string_view control = trim(line.substr(space));
        Step step;
        if (const std::size_t dash = frames.find('-'); dash != std::string_view::npos) {
            if (!parse_uint(frames.substr(0, dash), &step.first) || !parse_uint(frames.substr(dash + 1), &step.last))
                return fail("bad frame range");
        } else {
            if (!parse_uint(frames, &step.first)) return fail("bad frame number");
            step.last = step.first;
        }
        if (step.first == 0 || step.last < step.first) return fail("frames start at 1 and run forward");
        std::string_view value_text;
        if (const std::size_t eq = control.find('='); eq != std::string_view::npos) {
            value_text = trim(control.substr(eq + 1));
            control = trim(control.substr(0, eq));
        }
        static const struct {
            std::string_view name;
            std::uint32_t bit;
        } buttons[] = {
            {"start", PadSnapshot::Start},     {"coin", PadSnapshot::Coin},         {"view1", PadSnapshot::View1},
            {"view2", PadSnapshot::View2},     {"view3", PadSnapshot::View3},       {"view4", PadSnapshot::View4},
            {"shiftup", PadSnapshot::ShiftUp}, {"shiftdown", PadSnapshot::ShiftDown}, {"gear1", PadSnapshot::Gear1},
            {"gear2", PadSnapshot::Gear2},     {"gear3", PadSnapshot::Gear3},       {"gear4", PadSnapshot::Gear4},
            {"test", PadSnapshot::Test},       {"service", PadSnapshot::Service},
        };
        if (control == "steer" || control == "accel" || control == "brake") {
            step.kind = control == "steer" ? Step::Steer : control == "accel" ? Step::Accel : Step::Brake;
            if (value_text.empty()) return fail("an axis needs '=<value>'");
            const std::string v(value_text);
            char* end = nullptr;
            step.value = std::strtof(v.c_str(), &end);
            if (end != v.c_str() + v.size()) return fail("bad axis value");
            const float lo = step.kind == Step::Steer ? -1.0f : 0.0f;
            if (step.value < lo || step.value > 1.0f) return fail("axis value out of range");
        } else {
            const auto it = std::find_if(std::begin(buttons), std::end(buttons),
                                         [&](const auto& b) { return b.name == control; });
            if (it == std::end(buttons)) return fail("unknown control");
            if (!value_text.empty()) return fail("a button takes no value");
            step.button = it->bit;
        }
        steps_.push_back(step);
    }
    return true;
}

PadSnapshot ScriptPad::at(unsigned frame) const {
    PadSnapshot s;
    s.connected = true;
    for (const Step& step : steps_) {
        if (frame < step.first || frame > step.last) continue;
        switch (step.kind) {
        case Step::Steer: s.steering = step.value; break;
        case Step::Accel: s.accelerator = step.value; break;
        case Step::Brake: s.brake = step.value; break;
        case Step::Button: s.buttons |= step.button; break;
        }
    }
    return s;
}

PadSnapshot ScriptPad::sample() { return at(++frame_); }

}  // namespace dcusa

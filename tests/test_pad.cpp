// The default gamepad layout and the pad script.
#include <cmath>
#include <string>

#include "check.hpp"
#include "runtime/pad_mapping.hpp"
#include "runtime/pad_script.hpp"

using namespace dcusa;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

void layout() {
    GamepadState g;
    CHECK(!map_gamepad(g).connected);
    g.connected = true;
    // Each control alone, so a swapped pair shows up.
    const struct {
        bool GamepadState::*field;
        std::uint32_t expect;
    } buttons[] = {
        {&GamepadState::back, PadSnapshot::Coin},
        {&GamepadState::start, PadSnapshot::Start},
        {&GamepadState::right_shoulder, PadSnapshot::ShiftUp},
        {&GamepadState::left_shoulder, PadSnapshot::ShiftDown},
        {&GamepadState::y, PadSnapshot::View1},
        {&GamepadState::x, PadSnapshot::View2},
        {&GamepadState::b, PadSnapshot::View3},
        {&GamepadState::a, PadSnapshot::View4},
    };
    for (const auto& b : buttons) {
        GamepadState one = g;
        one.*(b.field) = true;
        const PadSnapshot s = map_gamepad(one);
        CHECK(s.connected);
        CHECK(s.buttons == b.expect);
    }
    // Gears 1-4 stay unassigned: nothing on the pad sets them.
    GamepadState all = g;
    all.a = all.b = all.x = all.y = all.left_shoulder = all.right_shoulder = all.back = all.start = true;
    CHECK((map_gamepad(all).buttons &
           (PadSnapshot::Gear1 | PadSnapshot::Gear2 | PadSnapshot::Gear3 | PadSnapshot::Gear4)) == 0);

    // Triggers are analog, gas on the right, brake on the left.
    GamepadState t = g;
    t.right_trigger = 0.37f;
    t.left_trigger = 0.81f;
    CHECK(near(map_gamepad(t).accelerator, 0.37f));
    CHECK(near(map_gamepad(t).brake, 0.81f));
    CHECK(map_gamepad(t).buttons == 0);

    // Steering: centre in the dead zone, proportional outside it, full lock at full travel.
    GamepadState st = g;
    st.left_x = 0.05f;
    CHECK(near(map_gamepad(st).steering, 0.0f));
    st.left_x = -kStickDeadZone;
    CHECK(near(map_gamepad(st).steering, 0.0f));
    st.left_x = 1.0f;
    CHECK(near(map_gamepad(st).steering, 1.0f));
    st.left_x = -1.0f;
    CHECK(near(map_gamepad(st).steering, -1.0f));
    st.left_x = 0.55f;
    CHECK(near(map_gamepad(st).steering, 0.5f));
    st.left_x = -0.55f;
    CHECK(near(map_gamepad(st).steering, -0.5f));
    CHECK(near(apply_dead_zone(NAN, kStickDeadZone), 0.0f));
    CHECK(near(apply_dead_zone(3.0f, kStickDeadZone), 1.0f));
}

void script() {
    ScriptPad pad;
    std::string why;
    CHECK(pad.parse("# attract, then a race\n"
                    "10 coin\n"
                    "20-22 start   # three frames\n"
                    "30-40 accel=0.75\n"
                    "35 steer=-0.5\n"
                    "50 shiftup\n",
                    &why));
    CHECK(pad.at(9).buttons == 0);
    CHECK(pad.at(10).buttons == PadSnapshot::Coin);
    CHECK(pad.at(21).buttons == PadSnapshot::Start);
    CHECK(pad.at(23).buttons == 0);
    CHECK(near(pad.at(30).accelerator, 0.75f));
    CHECK(near(pad.at(41).accelerator, 0.0f));
    CHECK(near(pad.at(35).steering, -0.5f));
    CHECK(pad.at(50).buttons == PadSnapshot::ShiftUp);
    CHECK(pad.at(1).connected);
    // sample() advances one frame per call, starting at 1.
    for (int i = 1; i < 10; ++i) CHECK(pad.sample().buttons == 0);
    CHECK(pad.sample().buttons == PadSnapshot::Coin);

    ScriptPad bad;
    CHECK(!bad.parse("5 jump\n", &why));
    CHECK(why.find("line 1") != std::string::npos);
    CHECK(!bad.parse("5 accel\n", &why));
    CHECK(!bad.parse("5 accel=1.5\n", &why));
    CHECK(!bad.parse("9-3 start\n", &why));
    CHECK(!bad.parse("0 start\n", &why));
    CHECK(!bad.parse("5 start=1\n", &why));
}

}  // namespace

int main() {
    layout();
    script();
    return check_result();
}

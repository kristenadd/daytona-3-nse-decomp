#pragma once
// A PadSource that plays a script, one sample per presented frame: for
// autoplay and for checking the controls without a controller.
//
// One step per line, `#` starts a comment:
//   <frame>[-<last frame>] <control>[=<value>]
// Controls: steer (-1..1), accel, brake (0..1), start, coin, view1..view4,
// shiftup, shiftdown, gear1..gear4, test, service. A button is held for the
// frames given; an axis keeps the value for those frames and is 0 otherwise.
// Frame 1 is the first sample.
#include <string>
#include <string_view>
#include <vector>

#include "runtime/host_platform.hpp"

namespace dcusa {

class ScriptPad : public PadSource {
public:
    // False with `error` set when a line does not parse.
    bool parse(std::string_view text, std::string* error);
    PadSnapshot sample() override;
    // The snapshot for `frame` without advancing.
    PadSnapshot at(unsigned frame) const;

private:
    struct Step {
        unsigned first = 0, last = 0;
        enum Kind { Steer, Accel, Brake, Button } kind = Button;
        float value = 0.0f;
        std::uint32_t button = 0;
    };
    std::vector<Step> steps_;
    unsigned frame_ = 0;
};

}  // namespace dcusa

#pragma once
// The first connected controller with an extended gamepad profile (Xbox,
// PlayStation, MFi), read through GameController.framework and mapped with
// the default layout in runtime/pad_mapping.hpp.
#include <memory>

#include "runtime/host_platform.hpp"

namespace dcusa {

// Starts watching for controllers. Samples are taken on the caller's thread;
// when that is the main thread, the main run loop is serviced so connection
// and input events are delivered while the game holds the thread.
std::unique_ptr<PadSource> make_gamecontroller_pad();

}  // namespace dcusa

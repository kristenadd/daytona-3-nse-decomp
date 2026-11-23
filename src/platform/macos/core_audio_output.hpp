#pragma once
// The default output device through Core Audio (an output AudioUnit), as
// 32-bit float interleaved stereo at the requested rate.
#include <memory>

#include "runtime/host_platform.hpp"

namespace dcusa {

std::unique_ptr<AudioOutput> make_core_audio_output();

}  // namespace dcusa

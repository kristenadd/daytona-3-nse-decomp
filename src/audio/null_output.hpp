#pragma once
// An AudioOutput with no device: a thread pulls the mix at the real-time rate,
// so DirectSound cursors move as they would with speakers, and optionally
// writes what it pulled to a WAV file (16-bit stereo). For headless and
// scripted runs, and for checking the sound without playing it.
#include <memory>
#include <string>

#include "runtime/host_platform.hpp"

namespace dcusa::audio {

// `wav_path` empty: discard the samples.
std::unique_ptr<AudioOutput> make_null_output(const std::string& wav_path);

}  // namespace dcusa::audio

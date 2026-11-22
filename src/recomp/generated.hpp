#pragma once
// Defined by the translator's entries.cpp, so only a build that compiled the
// generated game code can link a caller of this.
#include "recomp/dispatch.hpp"

namespace dcusa::recomp {

std::span<const GuestEntry> translated_entries();

}

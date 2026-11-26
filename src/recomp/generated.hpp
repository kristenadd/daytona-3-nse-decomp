#pragma once
// The translated game code, one entry table per edition built in
// (DCUSA_PREPARED_CACHE lists the editions). Defined in generated_select.cpp,
// part of dcusa_generated, so only a build that compiled game code can link a
// caller of these.
#include <span>
#include <vector>

#include "recomp/dispatch.hpp"
#include "system/edition.hpp"

namespace dcusa::recomp {

// The editions this binary can run, 2019 first.
std::vector<dcusa::Edition> built_editions();
bool edition_built(dcusa::Edition edition);
// That edition's translated functions; empty when it is not built in.
std::span<const GuestEntry> translated_entries(dcusa::Edition edition);

}

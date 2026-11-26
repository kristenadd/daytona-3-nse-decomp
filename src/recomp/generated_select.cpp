// Picks an edition's translated code. Compiled into dcusa_generated with
// DCUSA_HAS_EDITION_<year> for each edition built in; each edition's
// entries.cpp defines translated_entries_<year>().
#include "recomp/generated.hpp"

namespace dcusa::recomp {

#if DCUSA_HAS_EDITION_2017
std::span<const GuestEntry> translated_entries_2017();
#endif
#if DCUSA_HAS_EDITION_2019
std::span<const GuestEntry> translated_entries_2019();
#endif

std::span<const GuestEntry> translated_entries(dcusa::Edition edition) {
    switch (edition) {
    case dcusa::Edition::Dcu2017:
#if DCUSA_HAS_EDITION_2017
        return translated_entries_2017();
#else
        return {};
#endif
    case dcusa::Edition::Nse2019:
#if DCUSA_HAS_EDITION_2019
        return translated_entries_2019();
#else
        return {};
#endif
    }
    return {};
}

bool edition_built(dcusa::Edition edition) { return !translated_entries(edition).empty(); }

std::vector<dcusa::Edition> built_editions() {
    std::vector<dcusa::Edition> out;
    for (dcusa::Edition e : {dcusa::Edition::Nse2019, dcusa::Edition::Dcu2017})
        if (edition_built(e)) out.push_back(e);
    return out;
}

}

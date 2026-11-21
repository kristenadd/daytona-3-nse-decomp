#include "system/exe_manifest.hpp"

namespace dcusa {
namespace {

constexpr ExeRange Manifest2017[] = {
#include "system/exe_manifest_2017.inc"
};
constexpr ExeRange Manifest2019[] = {
#include "system/exe_manifest_2019.inc"
};
constexpr RomFile Roms[] = {
#include "system/rom_manifest.inc"
};

}

std::span<const ExeRange> exe_manifest(Edition edition) {
    return edition == Edition::Dcu2017 ? std::span<const ExeRange>(Manifest2017) : std::span<const ExeRange>(Manifest2019);
}

Sha256Digest exe_manifest_id(Edition edition) {
    Sha256 h;
    for (const ExeRange& r : exe_manifest(edition)) {
        const std::uint8_t head[8] = {std::uint8_t(r.va), std::uint8_t(r.va >> 8), std::uint8_t(r.va >> 16), std::uint8_t(r.va >> 24),
                                      std::uint8_t(r.size), std::uint8_t(r.size >> 8), std::uint8_t(r.size >> 16), std::uint8_t(r.size >> 24)};
        h.update(head);
        h.update(r.sha256);
    }
    return h.finish();
}

std::span<const RomFile> rom_manifest() { return Roms; }

}

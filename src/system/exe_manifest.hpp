#pragma once
#include <cstdint>
#include <span>
#include <string_view>

#include "system/edition.hpp"
#include "system/sha256.hpp"

namespace dcusa {

// A guest address range of the mapped EXE and the SHA-256 it has in the
// pristine build (generated: tools/make_manifests.py).
struct ExeRange {
    std::uint32_t va, size;
    Sha256Digest sha256;
};

std::span<const ExeRange> exe_manifest(Edition edition);
// Digest of the manifest table itself: a cache prepared against another table is re-verified.
Sha256Digest exe_manifest_id(Edition edition);

struct RomFile {
    std::string_view path;  // relative to the game folder (the one holding Daytona.exe)
    std::uint32_t size;
    Sha256Digest sha256;
};
std::span<const RomFile> rom_manifest();

}

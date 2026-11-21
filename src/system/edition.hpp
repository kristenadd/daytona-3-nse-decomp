#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "system/sha256.hpp"

namespace dcusa {

// The two releases this port runs. The player picks one at start-up; both
// can be installed side by side.
enum class Edition : std::uint8_t { Dcu2017, Nse2019 };

struct EditionInfo {
    Edition edition;
    std::string_view id;     // "2017" / "2019": command lines, file names
    std::string_view title;
    // The pristine Daytona.exe this edition is built against.
    std::uint64_t exe_size;
    Sha256Digest exe_sha256;
    std::uint32_t image_base, image_size, entry_rva;
    std::string_view cache_name;  // prepared image file in the port's cache folder
};

std::span<const EditionInfo> editions();
const EditionInfo& edition_info(Edition edition);
std::optional<Edition> edition_from_id(std::string_view id);

// A Daytona.exe the port recognises but refuses, with the reason shown to the player.
struct RefusedExe {
    std::uint64_t size;
    Sha256Digest sha256;
    Edition edition;
    std::string_view reason;
};
std::span<const RefusedExe> refused_exes();

}

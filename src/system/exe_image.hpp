#pragma once
// The player's own Daytona.exe, mapped at its guest addresses. Nothing of the
// original executable is compiled into the port: the prepare step reads the
// player's file, checks every range against the manifest of its edition, and
// keeps the mapped image in a cache in the port's own folder (never in the
// game folder).
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "system/edition.hpp"
#include "system/exe_manifest.hpp"

namespace dcusa {

struct ExeImage {
    Edition edition{};
    std::uint32_t base = 0;
    std::vector<std::uint8_t> bytes;  // guest [base, base + bytes.size())

    bool contains(std::uint32_t va, std::uint32_t size) const;
    // Throws std::out_of_range outside the image.
    const std::uint8_t* at(std::uint32_t va, std::uint32_t size) const;
    std::uint32_t u32(std::uint32_t va) const;
};

using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;

// Index of the first range whose bytes differ (or fall outside the image); nothing when all match.
std::optional<std::size_t> exe_first_mismatch(std::span<const std::uint8_t> image, std::uint32_t base,
                                              std::span<const ExeRange> ranges, const Progress& progress = {});

enum class ExeIdentity {
    Pristine,     // a supported edition's unmodified Daytona.exe
    Refused,      // recognised, but not usable (see message)
    OtherBuild,   // a PE32 executable that matches neither edition
    NotExecutable,
    Unreadable,
};

struct ExeFileCheck {
    ExeIdentity identity = ExeIdentity::Unreadable;
    std::optional<Edition> edition;
    std::string message;  // for the player
};

// Reads one file and, when it is a pristine executable of `want` (any
// edition when empty), maps and verifies it into `out`.
bool exe_image_from_file(const std::filesystem::path& path, std::optional<Edition> want, ExeImage& out,
                         ExeFileCheck& check, const Progress& progress = {});

struct ExeCandidate {
    std::filesystem::path path;
    ExeFileCheck check;
};

// Looks in a game folder (the one holding Daytona.exe) for a usable
// executable: Daytona.exe, Daytona.exe.original, then any other large *.exe.
// `seen` lists every file looked at with its verdict, for error messages.
bool exe_image_find(const std::filesystem::path& game_dir, std::optional<Edition> want, ExeImage& out,
                    std::filesystem::path* used, std::vector<ExeCandidate>* seen, std::string* error,
                    const Progress& progress = {});

// Prepared image cache: a 64-byte header, then the zlib-compressed image.
// Loading verifies every range again, so a damaged or stale cache is rejected.
bool exe_image_write_cache(const std::filesystem::path& path, const ExeImage& image, std::string* error);
bool exe_image_load_cache(const std::filesystem::path& path, Edition want, ExeImage& out, std::string* error,
                          const Progress& progress = {});

}

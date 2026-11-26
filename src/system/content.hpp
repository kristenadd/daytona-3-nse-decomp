#pragma once
// Finding the player's installations. The player may point at the game
// folder itself (the one holding Daytona.exe), the edition's top folder
// (2017: <top>/Daytona, 2019: <top>/Sega/Daytona) or a folder holding both
// editions. The port only ever reads these folders.
//
// A slim install (tools/overlay/make_overlay.py) holds only the files of one
// edition that differ from another edition's install, with the folder tree
// and dcusa-overlay.txt in its root: "base <edition id>" and then the
// relative paths (forward slashes) it shares with that base. The scan links
// it to the base install found in the same search; the game then reads a
// shared file from the base (recomp/host_files: set_guest_base).
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "system/edition.hpp"
#include "system/exe_image.hpp"

namespace dcusa {

struct GameInstall {
    Edition edition{};
    std::filesystem::path game_dir;      // Daytona.exe, rom/, nrt/
    std::filesystem::path install_root;  // parent of game_dir: GameData/, ShellData/
    std::filesystem::path exe;           // the verified executable (may be Daytona.exe.original)
    // A slim install: the base install's root and the paths shared with it
    // (relative to install_root, forward slashes). Empty for a full install.
    std::filesystem::path base_root;
    std::vector<std::string> shared;
    // A file of the install by relative path: in the install, else the base's copy.
    std::filesystem::path resolve(const std::filesystem::path& relative) const;
};

// The file in a slim install's root that names its base and shared files.
inline constexpr const char* kOverlayManifest = "dcusa-overlay.txt";

struct InstallScan {
    std::vector<GameInstall> installs;   // at most one per edition, the first found
    std::vector<std::string> problems;   // game folders found but not usable, with the reason
};

// Searches `chosen` and up to four folder levels below it.
InstallScan find_installs(const std::filesystem::path& chosen);
const GameInstall* pick_install(const InstallScan& scan, Edition edition);

// Folders the game reads, relative to the game folder; all must exist.
std::span<const char* const> required_content_folders();
// Checks the required folders and every Model 2 ROM image by SHA-256.
bool verify_content(const GameInstall& install, std::string* error, const Progress& progress = {});

}

#pragma once
// Finding the player's installations. The player may point at the game
// folder itself (the one holding Daytona.exe), the edition's top folder
// (2017: <top>/Daytona, 2019: <top>/Sega/Daytona) or a folder holding both
// editions. The port only ever reads these folders.
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
};

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

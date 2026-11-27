#pragma once
// The operator settings the cabinet's shell (Shell.exe) would edit:
// ShellData.ini (credits / free play, volumes, calibration) and
// GameSettings.ini (difficulty, race length, car and track select; the game
// reads it as ../ShellData/GameSettings.ini). Edits go to the edition's
// container copy, made from the install's the first time; the game reads the
// copy in place of the install's. Section GameSettings is GameSettings.ini,
// every other section ShellData.ini. Keys match with or without spaces
// around '=' ("Freeplay = 1" in the 2017 file).
#include <filesystem>
#include <optional>
#include <string>

namespace dcusa {

// The value, from the container's copy when there is one, else the install's.
std::optional<std::string> shell_setting(const std::filesystem::path& install_root,
                                         const std::filesystem::path& container, const std::string& section,
                                         const std::string& key);

// Sets it in the container's copy (replaced in its section, or added).
bool set_shell_setting(const std::filesystem::path& install_root, const std::filesystem::path& container,
                       const std::string& section, const std::string& key, const std::string& value);

}  // namespace dcusa

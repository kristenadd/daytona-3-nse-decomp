#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dcusa {

// Whole file into bytes. False with *error set on failure.
bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out, std::string* error);
// Writes through a temporary file in the same folder and renames it over path,
// so a reader never sees a partial file. Creates missing parent folders.
bool write_file_atomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string* error);

}

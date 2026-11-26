#pragma once
// Guest Win32 paths over the player's install. The install is only read.
// Writes land in a separate directory with the same relative layout.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dcusa::recomp {

// `readable` is the folder that contains the game directory and GameData.
// `cwd_relative` is the starting directory inside it ("Daytona").
// `writable` receives every create, write, and delete.
void set_guest_files(std::filesystem::path readable, std::string cwd_relative, std::filesystem::path writable);
// A slim install (system/content.hpp): the files in `shared` (paths relative
// to `readable`, forward slashes, any case) are read from `base` instead,
// another edition's install root. Other base files stay out of sight. Call
// after set_guest_files, which clears it.
void set_guest_base(std::filesystem::path base, const std::vector<std::string>& shared);
void clear_guest_files();
bool guest_files_ready();
// "D:\\Daytona\\<exe>": the executable inside the starting directory. Just
// `exe` when no install is mounted.
std::string guest_module_path(std::string_view exe);

// Windows-style path ("D:\\Daytona\\rom\\x", "rom\\x", "..\\GameData").
struct GuestFileInfo {
    bool exists = false;
    bool is_dir = false;
    std::uint64_t size = 0;
    std::uint32_t attributes = 0;  // FILE_ATTRIBUTE_*
};

// ERROR_* when the path cannot be used. 0 and a filled `info` otherwise.
std::uint32_t guest_stat(std::string_view windows_path, GuestFileInfo* info);

// Absolute form using the D: drive. Empty when `windows_path` is not a path.
std::string guest_full_path(std::string_view windows_path);

std::string guest_cwd();
std::uint32_t guest_set_cwd(std::string_view windows_path);
std::uint32_t guest_mkdir(std::string_view windows_path);

// CreateFile disposition and access, as the guest passed them.
constexpr std::uint32_t kGenericRead = 0x80000000u;
constexpr std::uint32_t kGenericWrite = 0x40000000u;
constexpr std::uint32_t kFileWriteData = 0x2u;
constexpr std::uint32_t kFileAppendData = 0x4u;

std::uint32_t guest_create_file(std::string_view windows_path, std::uint32_t access, std::uint32_t disposition,
                                std::uint32_t flags, std::uint32_t* handle, std::uint32_t* error);
bool guest_is_file(std::uint32_t handle);
std::uint32_t guest_close_file(std::uint32_t handle);
std::uint32_t guest_read_file(std::uint32_t handle, std::uint8_t* dst, std::uint32_t n, std::uint32_t* got);
std::uint32_t guest_write_file(std::uint32_t handle, const std::uint8_t* src, std::uint32_t n, std::uint32_t* put);
std::uint32_t guest_seek_file(std::uint32_t handle, std::int64_t distance, std::uint32_t method, std::int64_t* now);
std::uint32_t guest_file_size(std::uint32_t handle, std::uint64_t* size);
std::uint32_t guest_truncate_file(std::uint32_t handle);
std::uint32_t guest_flush_file(std::uint32_t handle);
std::uint32_t guest_file_attributes(std::uint32_t handle, std::uint32_t* attributes);
std::uint32_t guest_delete_file(std::string_view windows_path);

struct GuestDirEntry {
    std::string name;
    bool is_dir = false;
    std::uint64_t size = 0;
    std::uint32_t attributes = 0;
};

// `pattern` may end in a wildcard. The first match is returned; the rest wait
// on guest_find_next. `handle` is 0 when nothing matched.
std::uint32_t guest_find_first(std::string_view pattern, GuestDirEntry* first, std::uint32_t* handle);
std::uint32_t guest_find_next(std::uint32_t handle, GuestDirEntry* next);
std::uint32_t guest_find_close(std::uint32_t handle);

// The value from an ini file, when the section and key are present.
std::optional<std::string> guest_ini_get(std::string_view windows_path, std::string_view section, std::string_view key);
std::uint32_t guest_ini_set(std::string_view windows_path, std::string_view section, std::string_view key,
                            const std::optional<std::string>& value);

}

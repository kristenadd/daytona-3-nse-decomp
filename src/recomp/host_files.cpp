#include "recomp/host_files.hpp"

#include <TargetConditionals.h>
#include "runtime/hitch.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <unordered_map>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <vector>

namespace dcusa::recomp {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kErrorFileNotFound = 2;
constexpr std::uint32_t kErrorPathNotFound = 3;
constexpr std::uint32_t kErrorTooMany = 4;
constexpr std::uint32_t kErrorAccessDenied = 5;
constexpr std::uint32_t kErrorInvalidHandle = 6;
constexpr std::uint32_t kErrorInvalidParameter = 87;
constexpr std::uint32_t kErrorInvalidName = 123;
constexpr std::uint32_t kErrorAlreadyExists = 183;

constexpr std::uint32_t kAttrDir = 0x10;
constexpr std::uint32_t kAttrArchive = 0x20;
constexpr std::uint64_t kCopyLimit = 32ull << 20;

constexpr int kCreateNew = 1;
constexpr int kCreateAlways = 2;
constexpr int kOpenExisting = 3;
constexpr int kOpenAlways = 4;
constexpr int kTruncateExisting = 5;

// File and find handles have ranges of their own, clear of the kernel objects
// at 0x50000 (threads.hpp). The boot keeps several hundred files open at once
// (NRT containers stay open while their entries stream); Windows allows far
// more, and a full table made every later open fail with ERROR_TOO_MANY_OPEN_FILES.
constexpr std::uint32_t kFileHandleBase = 0x100000;
constexpr std::uint32_t kFileHandleCap = 0x10000;
constexpr std::uint32_t kFindHandleBase = 0x200000;
constexpr std::uint32_t kBackupSemantics = 0x02000000u;

fs::path g_readable;
fs::path g_writable;
std::vector<std::string> g_cwd;
std::vector<std::string> g_start_dir;

struct OpenFile {
    bool live = false;
    int fd = -1;
    bool is_dir = false;
    bool writable = false;
    std::uint32_t attributes = 0;
};

struct Find {
    bool live = false;
    std::vector<GuestDirEntry> rest;
};

std::vector<OpenFile> g_files;
std::vector<Find> g_finds;

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

std::string lower_copy(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

bool wild(std::string_view text) {
    return text.find('*') != std::string_view::npos || text.find('?') != std::string_view::npos;
}

bool match_wild(std::string_view text, std::string_view pat) {
    if (ieq(pat, "*.*")) pat = "*";
    std::size_t ti = 0, pi = 0, star = std::string_view::npos, mark = 0;
    while (ti < text.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || std::tolower(static_cast<unsigned char>(pat[pi])) ==
                                                       std::tolower(static_cast<unsigned char>(text[ti])))) {
            ++ti;
            ++pi;
        } else if (pi < pat.size() && pat[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

struct Parsed {
    bool ok = false;
    std::uint32_t error = kErrorPathNotFound;
    std::vector<std::string> parts;
    std::string spec;  // last component, which may be a wildcard
    bool has_spec = false;
};

Parsed parse_path(std::string_view in, bool keep_spec) {
    Parsed p;
    std::string s(in);
    for (char& c : s) {
        if (c == '/') c = '\\';
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) {
        // A trailing dot or space is not a distinct name, except ".." and ".".
        if (s.size() >= 2 && s[s.size() - 2] == '.' && (s.size() == 2 || s[s.size() - 3] == '\\')) break;
        if (s.back() == '\\') break;
        s.pop_back();
    }
    std::size_t i = 0;
    bool absolute = false;
    if (s.size() >= 2 && s[1] == ':') {
        const char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
        if (drive != 'd') {
            p.error = kErrorPathNotFound;
            return p;
        }
        absolute = true;
        i = 2;
    }
    if (i < s.size() && s[i] == '\\') {
        absolute = true;
        ++i;
    }
    std::vector<std::string> parts = absolute ? std::vector<std::string>{} : g_cwd;
    std::string cur;
    auto push = [&](std::string piece) {
        if (piece.empty() || piece == ".") return true;
        if (piece == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
            return true;
        }
        parts.push_back(std::move(piece));
        return true;
    };
    for (; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\\') {
            if (!cur.empty() || (i < s.size())) {
                if (i == s.size() && keep_spec && wild(cur)) {
                    p.has_spec = true;
                    p.spec = cur;
                } else if (!push(cur)) {
                    p.error = kErrorPathNotFound;
                    return p;
                }
            }
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
    p.ok = true;
    p.error = 0;
    p.parts = std::move(parts);
    return p;
}

// The install is read-only, so a lookup there is the same for the whole run.
// Without this memo every guest open listed each directory on its path; after
// boot the game opens files often enough that this took most of every frame.
std::unordered_map<std::string, std::optional<fs::path>> g_install_lookups;

std::optional<fs::path> descend_uncached(const fs::path& root, const std::vector<std::string>& parts);

std::optional<fs::path> descend(const fs::path& root, const std::vector<std::string>& parts) {
    if (root.empty() || root != g_readable) return descend_uncached(root, parts);
    std::string key;
    for (const std::string& part : parts) {
        for (char ch : part) key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        key.push_back('/');
    }
    if (const auto it = g_install_lookups.find(key); it != g_install_lookups.end()) return it->second;
    auto found = descend_uncached(root, parts);
    g_install_lookups.emplace(std::move(key), found);
    return found;
}

std::optional<fs::path> descend_uncached(const fs::path& root, const std::vector<std::string>& parts) {
    if (root.empty()) return std::nullopt;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    fs::path cur = root;
    for (const std::string& part : parts) {
        bool found = false;
        fs::path next;
        for (fs::directory_iterator it(cur, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (ieq(it->path().filename().string(), part)) {
                next = it->path();
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
        cur = next;
    }
    return cur;
}

fs::path overlay_path(const std::vector<std::string>& parts) {
    fs::path p = g_writable;
    for (const std::string& part : parts) p /= part;
    return p;
}

struct Located {
    bool content = false;
    bool overlay = false;
    bool is_dir = false;
    std::uint64_t size = 0;
    fs::path content_path;
    fs::path overlay_file;
};

// The player's GameData/.shaders/shader_cache.bin holds DXBC from Microsoft's
// compiler. That bytecode cannot run here, and with the cache present the game
// creates shaders from it instead of compiling its HLSL (the source the port
// translates). Only a cache the game writes into the container is visible.
bool hidden_content(const std::vector<std::string>& parts) {
    const std::size_t n = parts.size();
    return n >= 3 && ieq(parts[n - 1], "shader_cache.bin") && ieq(parts[n - 2], ".shaders") && ieq(parts[n - 3], "GameData");
}

Located locate(const std::vector<std::string>& parts) {
    Located loc;
    loc.overlay_file = overlay_path(parts);
    std::error_code ec;
    if (!g_writable.empty() && fs::exists(loc.overlay_file, ec)) {
        loc.overlay = true;
        loc.is_dir = fs::is_directory(loc.overlay_file, ec);
        if (!loc.is_dir) loc.size = fs::file_size(loc.overlay_file, ec);
    }
    if (auto hit = hidden_content(parts) ? std::nullopt : descend(g_readable, parts)) {
        loc.content = true;
        loc.content_path = *hit;
        if (!loc.overlay) {
            loc.is_dir = fs::is_directory(*hit, ec);
            if (!loc.is_dir) loc.size = fs::file_size(*hit, ec);
        }
    }
    return loc;
}

bool ensure_overlay_parent(const std::vector<std::string>& parts) {
    if (g_writable.empty() || parts.empty()) return false;
    std::error_code ec;
    fs::create_directories(g_writable, ec);
    fs::path cur = g_writable;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        bool found = false;
        if (fs::is_directory(cur, ec)) {
            for (fs::directory_iterator it(cur, ec), end; !ec && it != end; it.increment(ec)) {
                if (ieq(it->path().filename().string(), parts[i])) {
                    cur = it->path();
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            cur /= parts[i];
            if (!fs::create_directory(cur, ec) && !fs::is_directory(cur, ec)) return false;
        }
    }
    return true;
}

bool copy_to_overlay(const Located& loc, const std::vector<std::string>& parts) {
    if (!loc.content || loc.is_dir || loc.size > kCopyLimit) return false;
    if (!ensure_overlay_parent(parts)) return false;
    std::error_code ec;
    fs::copy_file(loc.content_path, loc.overlay_file, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

std::uint32_t alloc_file(OpenFile file, std::uint32_t* handle) {
    for (std::size_t i = 0; i < g_files.size(); ++i) {
        if (!g_files[i].live) {
            g_files[i] = std::move(file);
            g_files[i].live = true;
            *handle = kFileHandleBase + static_cast<std::uint32_t>(i);
            return 0;
        }
    }
    if (g_files.size() >= kFileHandleCap) return kErrorTooMany;
    g_files.push_back(std::move(file));
    g_files.back().live = true;
    *handle = kFileHandleBase + static_cast<std::uint32_t>(g_files.size() - 1);
    return 0;
}

OpenFile* file_at(std::uint32_t handle) {
    if (handle < kFileHandleBase) return nullptr;
    const std::uint32_t i = handle - kFileHandleBase;
    if (i >= g_files.size() || !g_files[i].live) return nullptr;
    return &g_files[i];
}

void fill_info(const Located& loc, GuestFileInfo* info) {
    info->exists = loc.content || loc.overlay;
    info->is_dir = info->exists && loc.is_dir;
    info->size = info->exists && !loc.is_dir ? loc.size : 0;
    info->attributes = !info->exists ? 0 : loc.is_dir ? kAttrDir : kAttrArchive;
}

void list_dir(const fs::path& dir, std::vector<GuestDirEntry>* out) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        GuestDirEntry e;
        e.name = it->path().filename().string();
        e.is_dir = it->is_directory(ec);
        e.attributes = e.is_dir ? kAttrDir : kAttrArchive;
        if (!e.is_dir) e.size = it->file_size(ec);
        bool replaced = false;
        for (GuestDirEntry& have : *out) {
            if (ieq(have.name, e.name)) {
                have = e;
                replaced = true;
                break;
            }
        }
        if (!replaced) out->push_back(std::move(e));
    }
}

std::string read_whole(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

void set_guest_files(fs::path readable, std::string cwd_relative, fs::path writable) {
    g_readable = std::move(readable);
    g_install_lookups.clear();
    g_writable = std::move(writable);
    g_cwd.clear();
    Parsed cwd = parse_path(cwd_relative, false);
    if (cwd.ok) g_cwd = std::move(cwd.parts);
    g_start_dir = g_cwd;
    for (OpenFile& f : g_files) {
        if (f.live && f.fd >= 0) ::close(f.fd);
        f = {};
    }
    g_finds.clear();
}

void clear_guest_files() { set_guest_files({}, {}, {}); }

bool guest_files_ready() { return !g_readable.empty(); }

std::string guest_module_path(std::string_view exe) {
    if (!guest_files_ready()) return std::string(exe);
    std::string out = "D:\\";
    for (const std::string& part : g_start_dir) {
        out += part;
        out.push_back('\\');
    }
    out += exe;
    return out;
}

std::uint32_t guest_stat(std::string_view windows_path, GuestFileInfo* info) {
    *info = {};
    if (!guest_files_ready()) return kErrorPathNotFound;
    Parsed p = parse_path(windows_path, false);
    if (!p.ok) return p.error;
    if (p.has_spec) return kErrorInvalidName;
    fill_info(locate(p.parts), info);
    return 0;
}

std::string guest_full_path(std::string_view windows_path) {
    Parsed p = parse_path(windows_path, true);
    if (!p.ok) return {};
    std::string out = "D:\\";
    for (std::size_t i = 0; i < p.parts.size(); ++i) {
        if (i != 0) out.push_back('\\');
        out += p.parts[i];
    }
    if (p.has_spec) {
        if (!p.parts.empty()) out.push_back('\\');
        out += p.spec;
    }
    if (out.size() == 2) out.push_back('\\');
    return out;
}

std::string guest_cwd() {
    std::string out = "D:\\";
    for (std::size_t i = 0; i < g_cwd.size(); ++i) {
        if (i != 0) out.push_back('\\');
        out += g_cwd[i];
    }
    return out;
}

std::uint32_t guest_set_cwd(std::string_view windows_path) {
    Parsed p = parse_path(windows_path, false);
    if (!p.ok) return p.error;
    if (guest_files_ready()) {
        GuestFileInfo info;
        fill_info(locate(p.parts), &info);
        if (!info.is_dir) return kErrorPathNotFound;
    }
    g_cwd = std::move(p.parts);
    return 0;
}

std::uint32_t guest_mkdir(std::string_view windows_path) {
    if (!guest_files_ready()) return 0;
    Parsed p = parse_path(windows_path, false);
    if (!p.ok) return p.error;
    if (p.parts.empty()) return kErrorAlreadyExists;
    Located loc = locate(p.parts);
    if (loc.content || loc.overlay) return loc.is_dir ? kErrorAlreadyExists : kErrorAlreadyExists;
    if (!ensure_overlay_parent(p.parts)) return kErrorPathNotFound;
    std::error_code ec;
    fs::create_directory(loc.overlay_file, ec);
    return ec ? kErrorAccessDenied : 0u;
}

std::uint32_t guest_create_file(std::string_view windows_path, std::uint32_t access, std::uint32_t disposition,
                                std::uint32_t flags, std::uint32_t* handle, std::uint32_t* error) {
    *handle = 0xFFFFFFFFu;
    if (!guest_files_ready()) {
        *error = kErrorFileNotFound;
        return *error;
    }
    Parsed p = parse_path(windows_path, false);
    if (!p.ok) {
        *error = p.error;
        return *error;
    }
    if (p.has_spec || wild(windows_path)) {
        *error = kErrorInvalidName;
        return *error;
    }
    const bool want_write = (access & (kGenericWrite | kFileWriteData | kFileAppendData)) != 0;
    Located loc = locate(p.parts);
    const bool exists = loc.content || loc.overlay;
    if (exists && loc.is_dir) {
        if ((flags & kBackupSemantics) == 0 && disposition != static_cast<std::uint32_t>(kOpenExisting)) {
            *error = kErrorAccessDenied;
            return *error;
        }
        OpenFile file;
        file.is_dir = true;
        file.attributes = kAttrDir;
        *error = alloc_file(std::move(file), handle);
        return *error;
    }
    if (disposition == kCreateNew && exists) {
        *error = kErrorAlreadyExists;
        return *error;
    }
    if ((disposition == kOpenExisting || disposition == kTruncateExisting) && !exists) {
        *error = p.parts.empty() ? kErrorPathNotFound : kErrorFileNotFound;
        return *error;
    }
    if (disposition != kCreateNew && disposition != kCreateAlways && disposition != kOpenExisting &&
        disposition != kOpenAlways && disposition != kTruncateExisting) {
        *error = kErrorInvalidParameter;
        return *error;
    }

    fs::path opened;
    bool created_overlay = false;
    if (!want_write && disposition != kCreateAlways && disposition != kCreateNew && disposition != kTruncateExisting) {
        if (loc.overlay && !loc.is_dir) opened = loc.overlay_file;
        else if (loc.content && !loc.is_dir) opened = loc.content_path;
        else if (!exists && disposition == kOpenAlways) {
            if (!ensure_overlay_parent(p.parts)) {
                *error = kErrorPathNotFound;
                return *error;
            }
            opened = loc.overlay_file;
            created_overlay = true;
        } else {
            *error = kErrorFileNotFound;
            return *error;
        }
    } else {
        if (g_writable.empty()) {
            *error = kErrorAccessDenied;
            return *error;
        }
        if (!loc.overlay && loc.content && disposition != kCreateAlways && disposition != kCreateNew) {
            if (!copy_to_overlay(loc, p.parts)) {
                *error = kErrorAccessDenied;
                return *error;
            }
        } else if (!ensure_overlay_parent(p.parts)) {
            *error = kErrorPathNotFound;
            return *error;
        }
        opened = loc.overlay_file;
        created_overlay = true;
    }

    int oflag = want_write || created_overlay ? O_RDWR : O_RDONLY;
    if (disposition == kCreateAlways || disposition == kCreateNew) oflag |= O_CREAT | O_TRUNC;
    else if (disposition == kTruncateExisting) oflag |= O_TRUNC;
    else if (created_overlay && !exists) oflag |= O_CREAT;
    int fd = ::open(opened.c_str(), oflag, 0644);
    if (fd < 0) {
        *error = kErrorAccessDenied;
        return *error;
    }
    OpenFile file;
    file.fd = fd;
    file.writable = want_write || created_overlay;
    file.attributes = kAttrArchive;
    *error = alloc_file(std::move(file), handle);
    if (*error != 0) ::close(fd);
    return *error;
}

bool guest_is_file(std::uint32_t handle) { return file_at(handle) != nullptr; }

std::uint32_t guest_close_file(std::uint32_t handle) {
    OpenFile* f = file_at(handle);
    if (!f) return kErrorInvalidHandle;
    if (f->fd >= 0) ::close(f->fd);
    *f = {};
    return 0;
}

std::uint32_t guest_read_file(std::uint32_t handle, std::uint8_t* dst, std::uint32_t n, std::uint32_t* got) {
    hitch::Timer timer(hitch::Io);
    *got = 0;
    OpenFile* f = file_at(handle);
    if (!f || f->fd < 0) return kErrorInvalidHandle;
    std::uint32_t done = 0;
    while (done < n) {
        const ssize_t r = ::read(f->fd, dst + done, n - done);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            return kErrorAccessDenied;
        }
        done += static_cast<std::uint32_t>(r);
    }
    *got = done;
    return 0;
}

std::uint32_t guest_write_file(std::uint32_t handle, const std::uint8_t* src, std::uint32_t n, std::uint32_t* put) {
    *put = 0;
    OpenFile* f = file_at(handle);
    if (!f || f->fd < 0) return kErrorInvalidHandle;
    if (!f->writable) return kErrorAccessDenied;
    std::uint32_t done = 0;
    while (done < n) {
        const ssize_t w = ::write(f->fd, src + done, n - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            return kErrorAccessDenied;
        }
        done += static_cast<std::uint32_t>(w);
    }
    *put = done;
    return 0;
}

std::uint32_t guest_seek_file(std::uint32_t handle, std::int64_t distance, std::uint32_t method, std::int64_t* now) {
    OpenFile* f = file_at(handle);
    if (!f || f->fd < 0) return kErrorInvalidHandle;
    int whence = SEEK_SET;
    if (method == 1) whence = SEEK_CUR;
    else if (method == 2) whence = SEEK_END;
    else if (method != 0) return kErrorInvalidParameter;
    const off_t at = ::lseek(f->fd, static_cast<off_t>(distance), whence);
    if (at < 0) return kErrorInvalidParameter;
    *now = static_cast<std::int64_t>(at);
    return 0;
}

std::uint32_t guest_file_size(std::uint32_t handle, std::uint64_t* size) {
    OpenFile* f = file_at(handle);
    if (!f || f->fd < 0) return kErrorInvalidHandle;
    struct stat st {};
    if (::fstat(f->fd, &st) != 0) return kErrorInvalidHandle;
    *size = static_cast<std::uint64_t>(st.st_size);
    return 0;
}

std::uint32_t guest_truncate_file(std::uint32_t handle) {
    OpenFile* f = file_at(handle);
    if (!f || f->fd < 0) return kErrorInvalidHandle;
    const off_t at = ::lseek(f->fd, 0, SEEK_CUR);
    if (at < 0 || ::ftruncate(f->fd, at) != 0) return kErrorAccessDenied;
    return 0;
}

std::uint32_t guest_flush_file(std::uint32_t handle) {
    OpenFile* f = file_at(handle);
    if (!f || f->fd < 0) return kErrorInvalidHandle;
    if (::fsync(f->fd) != 0) return kErrorAccessDenied;
    return 0;
}

std::uint32_t guest_file_attributes(std::uint32_t handle, std::uint32_t* attributes) {
    OpenFile* f = file_at(handle);
    if (!f) return kErrorInvalidHandle;
    *attributes = f->attributes;
    return 0;
}

std::uint32_t guest_delete_file(std::string_view windows_path) {
    if (!guest_files_ready()) return kErrorFileNotFound;
    Parsed p = parse_path(windows_path, false);
    if (!p.ok) return p.error;
    Located loc = locate(p.parts);
    if (loc.overlay && !loc.is_dir) {
        std::error_code ec;
        fs::remove(loc.overlay_file, ec);
        return ec ? kErrorAccessDenied : 0u;
    }
    if (loc.content && !loc.is_dir) return kErrorAccessDenied;
    if (loc.is_dir) return kErrorAccessDenied;
    return kErrorFileNotFound;
}

std::uint32_t guest_find_first(std::string_view pattern, GuestDirEntry* first, std::uint32_t* handle) {
    *handle = 0xFFFFFFFFu;
    if (!guest_files_ready()) return kErrorFileNotFound;
    Parsed p = parse_path(pattern, true);
    if (!p.ok) return p.error;
    std::vector<GuestDirEntry> matches;
    if (!p.has_spec) {
        Located loc = locate(p.parts);
        if (!loc.content && !loc.overlay) return kErrorFileNotFound;
        GuestDirEntry e;
        e.name = p.parts.empty() ? std::string("Daytona") : p.parts.back();
        e.is_dir = loc.is_dir;
        e.size = loc.is_dir ? 0 : loc.size;
        e.attributes = loc.is_dir ? kAttrDir : kAttrArchive;
        matches.push_back(std::move(e));
    } else {
        std::vector<GuestDirEntry> entries;
        entries.push_back(GuestDirEntry{".", true, 0, kAttrDir});
        entries.push_back(GuestDirEntry{"..", true, 0, kAttrDir});
        if (auto dir = descend(g_readable, p.parts)) list_dir(*dir, &entries);
        if (!g_writable.empty()) {
            if (auto dir = descend(g_writable, p.parts)) list_dir(*dir, &entries);
        }
        for (GuestDirEntry& e : entries) {
            if (match_wild(e.name, p.spec)) matches.push_back(std::move(e));
        }
        if (matches.empty()) return kErrorFileNotFound;
    }
    *first = matches.front();
    Find find;
    find.live = true;
    for (std::size_t i = 1; i < matches.size(); ++i) find.rest.push_back(std::move(matches[i]));
    for (std::size_t i = 0; i < g_finds.size(); ++i) {
        if (!g_finds[i].live) {
            g_finds[i] = std::move(find);
            *handle = kFindHandleBase + static_cast<std::uint32_t>(i);
            return 0;
        }
    }
    if (g_finds.size() >= 64) return kErrorTooMany;
    g_finds.push_back(std::move(find));
    *handle = kFindHandleBase + static_cast<std::uint32_t>(g_finds.size() - 1);
    return 0;
}

std::uint32_t guest_find_next(std::uint32_t handle, GuestDirEntry* next) {
    if (handle < kFindHandleBase) return kErrorInvalidHandle;
    const std::uint32_t i = handle - kFindHandleBase;
    if (i >= g_finds.size() || !g_finds[i].live) return kErrorInvalidHandle;
    if (g_finds[i].rest.empty()) return kErrorFileNotFound;
    *next = std::move(g_finds[i].rest.front());
    g_finds[i].rest.erase(g_finds[i].rest.begin());
    return 0;
}

std::uint32_t guest_find_close(std::uint32_t handle) {
    if (handle < kFindHandleBase) return kErrorInvalidHandle;
    const std::uint32_t i = handle - kFindHandleBase;
    if (i >= g_finds.size() || !g_finds[i].live) return kErrorInvalidHandle;
    g_finds[i] = {};
    return 0;
}

// Settings of GameData/config.ini the host overrides: DCUSA_INI="Section.Key=Value;..."
// (section and key in any case), on top of the platform's defaults (iOS: a
// phone's GPU budget, see kPhoneIni).
// Empty: the width was pinned at 960 while width changes drew the stands
// black; the cause was texture uploads racing a mip carry-over (metal.mm,
// Texture::carried), so the dynamic resolution is free again.
const char* const kPhoneIni = "";

std::optional<std::string> ini_override(std::string_view windows_path, std::string_view section, std::string_view key) {
    std::string file = lower_copy(windows_path);
    if (file.size() < 10 || file.compare(file.size() - 10, 10, "config.ini") != 0) return std::nullopt;
    const std::string want = lower_copy(section) + "." + lower_copy(key);
    std::optional<std::string> found;
    auto scan = [&](const char* list) {
        if (list == nullptr) return;
        std::string_view rest(list);
        while (!rest.empty()) {
            const std::size_t semi = rest.find(';');
            std::string_view item = rest.substr(0, semi);
            rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
            const std::size_t eq = item.find('=');
            if (eq != std::string_view::npos && lower_copy(item.substr(0, eq)) == want) found = std::string(item.substr(eq + 1));
        }
    };
    if (TARGET_OS_IPHONE) scan(kPhoneIni);
    scan(std::getenv("DCUSA_INI"));  // last: wins over the defaults
    return found;
}

std::optional<std::string> guest_ini_get(std::string_view windows_path, std::string_view section, std::string_view key) {
    if (!guest_files_ready() || section.empty() || key.empty()) return std::nullopt;
    if (auto forced = ini_override(windows_path, section, key)) return forced;
    Parsed p = parse_path(windows_path, false);
    if (!p.ok) return std::nullopt;
    Located loc = locate(p.parts);
    fs::path path;
    if (loc.overlay && !loc.is_dir) path = loc.overlay_file;
    else if (loc.content && !loc.is_dir) path = loc.content_path;
    else return std::nullopt;
    const std::string text = read_whole(path);
    const std::string want_sec = lower_copy(section);
    const std::string want_key = lower_copy(key);
    bool in = false;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t end = text.find('\n', i);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + i, end - i);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        i = end + (end < text.size() ? 1 : 0);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close == std::string_view::npos) {
                in = false;
                continue;
            }
            in = ieq(line.substr(1, close - 1), want_sec);
            continue;
        }
        if (!in) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view name = line.substr(0, eq);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
        if (!ieq(name, want_key)) continue;
        std::string_view value = line.substr(eq + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        return std::string(value);
    }
    return std::nullopt;
}

std::uint32_t guest_ini_set(std::string_view windows_path, std::string_view section, std::string_view key,
                            const std::optional<std::string>& value) {
    if (!guest_files_ready() || g_writable.empty()) return kErrorAccessDenied;
    Parsed p = parse_path(windows_path, false);
    if (!p.ok) return p.error;
    if (section.empty()) return kErrorInvalidParameter;
    Located loc = locate(p.parts);
    std::string text;
    if (loc.overlay && !loc.is_dir) text = read_whole(loc.overlay_file);
    else if (loc.content && !loc.is_dir) {
        if (!copy_to_overlay(loc, p.parts)) return kErrorAccessDenied;
        text = read_whole(loc.overlay_file);
    } else if (!ensure_overlay_parent(p.parts)) {
        return kErrorPathNotFound;
    }
    const std::string want_sec = lower_copy(section);
    const std::string want_key = lower_copy(key);
    std::string out;
    bool in = false;
    bool wrote = false;
    bool section_seen = false;
    std::size_t i = 0;
    auto flush_line = [&](std::string_view line) { out.append(line); out.push_back('\n'); };
    while (i < text.size()) {
        std::size_t end = text.find('\n', i);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + i, end - i);
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.remove_suffix(1);
        i = end + (end < text.size() ? 1 : 0);
        std::string_view trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.remove_prefix(1);
        if (!trimmed.empty() && trimmed.front() == '[') {
            if (in && value && !key.empty() && !wrote) {
                out.append(std::string(key));
                out.push_back('=');
                out.append(*value);
                out.push_back('\n');
                wrote = true;
            }
            const auto close = trimmed.find(']');
            in = close != std::string_view::npos && ieq(trimmed.substr(1, close - 1), want_sec);
            if (in) section_seen = true;
            flush_line(line);
            continue;
        }
        if (in && !key.empty()) {
            const auto eq = trimmed.find('=');
            if (eq != std::string_view::npos) {
                std::string_view name = trimmed.substr(0, eq);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
                if (ieq(name, want_key)) {
                    wrote = true;
                    if (value) {
                        out.append(std::string(key));
                        out.push_back('=');
                        out.append(*value);
                        out.push_back('\n');
                    }
                    continue;
                }
            }
        }
        flush_line(line);
    }
    if (value && !key.empty() && !wrote) {
        if (!section_seen) {
            if (!out.empty() && out.back() != '\n') out.push_back('\n');
            out.push_back('[');
            out.append(section);
            out.append("]\n");
        }
        out.append(std::string(key));
        out.push_back('=');
        out.append(*value);
        out.push_back('\n');
    }
    std::ofstream file(loc.overlay_file, std::ios::binary | std::ios::trunc);
    if (!file) return kErrorAccessDenied;
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return file ? 0u : kErrorAccessDenied;
}

}  // namespace dcusa::recomp

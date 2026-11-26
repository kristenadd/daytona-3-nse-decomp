#include "system/content.hpp"

#include <algorithm>
#include <fstream>

#include "system/files.hpp"

namespace dcusa {

namespace fs = std::filesystem;

namespace {

constexpr int MaxDepth = 4;
constexpr const char* RequiredFolders[] = {
    "rom/daytona_rom", "rom/fv_rom", "rom/auth2d", "rom/message", "rom/movie",
    "rom/shader", "rom/sound", "rom/sprite", "nrt/dx11",
};

bool looks_like_game_dir(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir / "rom", ec) && fs::is_directory(dir / "nrt", ec) &&
           (fs::is_regular_file(dir / "Daytona.exe", ec) || fs::is_regular_file(dir / "Daytona.exe.original", ec));
}

void scan(const fs::path& dir, int depth, InstallScan& out) {
    std::error_code ec;
    if (looks_like_game_dir(dir)) {
        ExeImage image;
        fs::path used;
        std::vector<ExeCandidate> seen;
        std::string error;
        if (exe_image_find(dir, std::nullopt, image, &used, &seen, &error)) {
            if (!pick_install(out, image.edition)) {
                GameInstall install{image.edition, dir, dir.parent_path(), used, {}, {}};
                // A slim install: its base is linked once the whole search is done.
                std::ifstream manifest(dir.parent_path() / kOverlayManifest);
                std::string line;
                while (manifest && std::getline(manifest, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty() || line[0] == '#') continue;
                    if (line.rfind("base ", 0) == 0) install.base_root = line.substr(5);  // the edition id, for now
                    else install.shared.push_back(line);
                }
                out.installs.push_back(std::move(install));
            }
        } else {
            out.problems.push_back(error);
        }
        return;  // a game folder's own subfolders hold no other installation
    }
    if (depth >= MaxDepth) return;
    std::vector<fs::path> children;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        if (entry.is_directory(ec) && !entry.is_symlink(ec)) children.push_back(entry.path());
    std::sort(children.begin(), children.end());
    for (const fs::path& child : children) scan(child, depth + 1, out);
}

}

InstallScan find_installs(const fs::path& chosen) {
    InstallScan out;
    std::error_code ec;
    if (!fs::is_directory(chosen, ec)) {
        out.problems.push_back("not a folder: " + chosen.string());
        return out;
    }
    scan(chosen, 0, out);
    // Slim installs: base_root holds the base edition's id until linked here.
    for (auto it = out.installs.begin(); it != out.installs.end();) {
        if (it->base_root.empty()) {
            ++it;
            continue;
        }
        const std::string id = it->base_root.string();
        const std::optional<Edition> base = edition_from_id(id);
        const GameInstall* found = nullptr;
        if (base)
            for (const GameInstall& g : out.installs)
                if (g.edition == *base && g.base_root.empty()) found = &g;
        if (found == nullptr) {
            out.problems.push_back(it->install_root.string() + " holds only the files that differ from the " + id +
                                   " edition, and no " + id + " game folder was found beside it");
            it = out.installs.erase(it);
            continue;
        }
        it->base_root = found->install_root;
        ++it;
    }
    if (out.installs.empty() && out.problems.empty())
        out.problems.push_back("no Daytona Championship USA game folder (Daytona.exe beside rom/ and nrt/) under " + chosen.string());
    return out;
}

const GameInstall* pick_install(const InstallScan& scan_result, Edition edition) {
    for (const GameInstall& g : scan_result.installs)
        if (g.edition == edition) return &g;
    return nullptr;
}

std::span<const char* const> required_content_folders() { return RequiredFolders; }

fs::path GameInstall::resolve(const fs::path& relative) const {
    std::error_code ec;
    const fs::path own = install_root / relative;
    if (base_root.empty() || fs::exists(own, ec)) return own;
    return base_root / relative;
}

bool verify_content(const GameInstall& install, std::string* error, const Progress& progress) {
    std::error_code ec;
    for (const char* folder : RequiredFolders) {
        if (!fs::is_directory(install.game_dir / folder, ec)) {
            if (error) *error = std::string("missing folder ") + folder + " in " + install.game_dir.string();
            return false;
        }
    }
    std::uint64_t total = 0, done = 0;
    for (const RomFile& rom : rom_manifest()) total += rom.size;
    std::vector<std::uint8_t> bytes;
    for (const RomFile& rom : rom_manifest()) {
        const fs::path path = install.resolve(install.game_dir.filename() / fs::path(std::string(rom.path)));
        if (!read_file(path, bytes, error)) return false;
        if (bytes.size() != rom.size || sha256(bytes) != rom.sha256) {
            if (error) *error = "ROM image " + std::string(rom.path) + " differs from the release (damaged or modified)";
            return false;
        }
        done += rom.size;
        if (progress) progress(done, total);
    }
    return true;
}

}

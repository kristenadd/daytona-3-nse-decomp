// dcprepare: verifies the player's Daytona Championship USA installation(s)
// and writes the prepared EXE image cache for each edition found. The game
// folders are only read; caches go to the port's own folder.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "system/content.hpp"
#include "system/exe_image.hpp"

namespace fs = std::filesystem;
using namespace dcusa;

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: dcprepare <folder> [--edition 2017|2019|all] [--cache-dir <dir>]\n"
                 "  <folder>     the game folder, an edition's top folder, or a folder holding both\n"
                 "  --edition    which edition(s) to prepare (default: all found)\n"
                 "  --cache-dir  where prepared images go (default: ~/Library/Application Support/DaytonaUSA/Cache)\n");
}

fs::path default_cache_dir() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / "Library" / "Application Support" / "DaytonaUSA" / "Cache";
}

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

bool is_inside(const fs::path& inner, const fs::path& outer) {
    std::error_code ec;
    fs::path a = fs::weakly_canonical(inner, ec), b = fs::weakly_canonical(outer, ec);
    auto [end, _] = std::mismatch(b.begin(), b.end(), a.begin(), a.end());
    return end == b.end();
}

bool prepare(const GameInstall& install, const fs::path& cache_dir) {
    const EditionInfo& info = edition_info(install.edition);
    std::printf("%s\n  game folder: %s\n", std::string(info.title).c_str(), install.game_dir.string().c_str());
    auto t0 = std::chrono::steady_clock::now();
    std::string error;
    if (!verify_content(install, &error)) {
        std::printf("  content: FAILED: %s\n", error.c_str());
        return false;
    }
    std::printf("  content: required folders present, %zu Model 2 ROM images verified (%.1f s)\n", rom_manifest().size(), seconds_since(t0));

    t0 = std::chrono::steady_clock::now();
    ExeImage image;
    fs::path used;
    std::vector<ExeCandidate> seen;
    if (!exe_image_find(install.game_dir, install.edition, image, &used, &seen, &error)) {
        std::printf("  executable: FAILED: %s\n", error.c_str());
        return false;
    }
    for (const ExeCandidate& c : seen)
        if (c.path != used) std::printf("  skipped %s: %s\n", c.path.filename().string().c_str(), c.check.message.c_str());
    std::printf("  executable: %s verified, %zu ranges (%.1f s)\n", used.filename().string().c_str(), exe_manifest(install.edition).size(), seconds_since(t0));

    t0 = std::chrono::steady_clock::now();
    const fs::path cache = cache_dir / std::string(info.cache_name);
    ExeImage reloaded;
    if (!exe_image_write_cache(cache, image, &error) || !exe_image_load_cache(cache, install.edition, reloaded, &error)) {
        std::printf("  cache: FAILED: %s\n", error.c_str());
        return false;
    }
    std::printf("  cache: %s (%.1f MB, re-verified, %.1f s)\n", cache.string().c_str(),
                double(fs::file_size(cache)) / 1048576.0, seconds_since(t0));
    return true;
}

}

int main(int argc, char** argv) {
    std::optional<fs::path> folder;
    std::optional<Edition> only;
    fs::path cache_dir = default_cache_dir();
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--edition" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v != "all") {
                only = edition_from_id(v);
                if (!only) { usage(); return 2; }
            }
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (!arg.empty() && arg[0] != '-' && !folder) {
            folder = arg;
        } else {
            usage();
            return 2;
        }
    }
    if (!folder) { usage(); return 2; }
    if (is_inside(cache_dir, *folder)) {
        std::fprintf(stderr, "dcprepare: the cache folder must not be inside the game folder (it is only read)\n");
        return 2;
    }

    InstallScan scan = find_installs(*folder);
    for (const std::string& p : scan.problems) std::printf("problem: %s\n", p.c_str());
    int wanted = 0, prepared = 0;
    for (const EditionInfo& e : editions()) {
        if (only && *only != e.edition) continue;
        const GameInstall* install = pick_install(scan, e.edition);
        if (!install) {
            if (only) std::printf("%s: not found under %s\n", std::string(e.title).c_str(), folder->string().c_str());
            continue;
        }
        ++wanted;
        prepared += prepare(*install, cache_dir) ? 1 : 0;
    }
    if (wanted == 0) {
        std::printf("nothing prepared: no usable installation found\n");
        return 1;
    }
    std::printf("%d of %d edition(s) prepared\n", prepared, wanted);
    return prepared == wanted ? 0 : 1;
}

// Installation discovery on made-up folder trees (no game files needed).
#include <algorithm>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <fstream>

#include "check.hpp"
#include "recomp/host_files.hpp"
#include "system/content.hpp"
#include "system/files.hpp"

using namespace dcusa;
namespace fs = std::filesystem;

static void make_game_dir(const fs::path& dir) {
    for (const char* folder : required_content_folders()) fs::create_directories(dir / folder);
    std::string error;
    REQUIRE(write_file_atomic(dir / "Daytona.exe", std::vector<std::uint8_t>(64, 0), &error));
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("dcusa_test_content_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "empty");

    InstallScan scan = find_installs(root / "empty");
    CHECK(scan.installs.empty());
    REQUIRE(scan.problems.size() == 1);
    CHECK(scan.problems[0].find("no Daytona Championship USA game folder") != std::string::npos);

    scan = find_installs(root / "does-not-exist");
    CHECK(scan.installs.empty() && scan.problems.size() == 1);

    // Both release layouts under one library folder, each with an unusable EXE:
    // found as game folders, reported as problems, never as installs.
    make_game_dir(root / "lib" / "Daytona Championship USA (2017)" / "Daytona");
    make_game_dir(root / "lib" / "New Season Edition (2019)" / "Sega" / "Daytona");
    fs::create_directories(root / "lib" / "unrelated" / "rom");
    scan = find_installs(root / "lib");
    CHECK(scan.installs.empty());
    CHECK(scan.problems.size() == 2);
    for (const std::string& p : scan.problems) CHECK(p.find("Daytona.exe") != std::string::npos);
    CHECK(!pick_install(scan, Edition::Dcu2017));

    // Pointing at the game folder itself works the same.
    scan = find_installs(root / "lib" / "New Season Edition (2019)" / "Sega" / "Daytona");
    CHECK(scan.problems.size() == 1);

    // Too deep: not searched.
    make_game_dir(root / "deep" / "a" / "b" / "c" / "d" / "e");
    scan = find_installs(root / "deep");
    CHECK(scan.problems.size() == 1 && scan.problems[0].find("no Daytona") != std::string::npos);

    // verify_content reports the first missing folder.
    GameInstall fake{Edition::Nse2019, root / "lib" / "unrelated", root / "lib", {}};
    std::string error;
    CHECK(!verify_content(fake, &error));
    CHECK(error.find("missing folder rom/daytona_rom") != std::string::npos);

    // A slim install: its shared files come from the base install, the base's
    // other files stay out of sight, and folder listings show both.
    {
        auto write = [](const fs::path& p, const char* text) {
            fs::create_directories(p.parent_path());
            std::ofstream(p, std::ios::binary) << text;
        };
        const fs::path base = root / "slim" / "base", slim = root / "slim" / "own", out = root / "slim" / "out";
        write(base / "Daytona" / "rom" / "Shared.bin", "shared");
        write(base / "Daytona" / "rom" / "base_only.bin", "base");
        write(slim / "Daytona" / "rom" / "own.bin", "own");
        write(slim / "Daytona" / "rom" / "both.bin", "slim's");
        write(base / "Daytona" / "rom" / "both.bin", "base's");
        using namespace dcusa::recomp;
        set_guest_files(slim, "Daytona", out);
        GuestFileInfo info;
        CHECK(guest_stat("rom\\shared.bin", &info) == 0 && !info.exists);  // no base yet
        set_guest_base(base, {"Daytona/rom/shared.bin", "Daytona/rom/both.bin"});
        CHECK(guest_stat("ROM\\SHARED.BIN", &info) == 0 && info.exists && info.size == 6);
        CHECK(guest_stat("rom\\base_only.bin", &info) == 0 && !info.exists);
        CHECK(guest_stat("rom\\both.bin", &info) == 0 && info.exists && info.size == 6);  // its own copy wins
        GuestDirEntry e;
        std::uint32_t h = 0;
        std::vector<std::string> names;
        if (guest_find_first("rom\\*.bin", &e, &h) == 0) {
            names.push_back(e.name);
            while (guest_find_next(h, &e) == 0) names.push_back(e.name);
            guest_find_close(h);
        }
        std::sort(names.begin(), names.end());
        CHECK((names == std::vector<std::string>{"Shared.bin", "both.bin", "own.bin"}));
        GameInstall slim_install{Edition::Dcu2017, slim / "Daytona", slim, {}, base, {"Daytona/rom/Shared.bin"}};
        CHECK(slim_install.resolve("Daytona/rom/own.bin") == slim / "Daytona/rom/own.bin");
        CHECK(slim_install.resolve("Daytona/rom/Shared.bin") == base / "Daytona/rom/Shared.bin");
        clear_guest_files();
    }

    fs::remove_all(root);
    return check_result();
}

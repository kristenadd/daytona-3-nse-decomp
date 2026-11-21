// Installation discovery on made-up folder trees (no game files needed).
#include <filesystem>
#include <string>
#include <unistd.h>

#include "check.hpp"
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

    fs::remove_all(root);
    return check_result();
}

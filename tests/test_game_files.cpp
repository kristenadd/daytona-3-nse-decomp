// Against the player's real installation(s): discovery, the refused patched
// 2017 executable, content and ROM checks, image verification and the cache
// round trip. Skips when the files are absent. The game folders are only
// read; caches are written under argv[2] (in the build tree).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "check.hpp"
#include "system/content.hpp"
#include "system/files.hpp"
#include "system/pe32.hpp"

using namespace dcusa;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 3 || !fs::is_directory(argv[1])) {
        std::printf("skipped: no game library (set DCUSA_GAME_LIBRARY)\n");
        return SkipTest;
    }
    const fs::path library = argv[1], cache_dir = argv[2];
    InstallScan scan = find_installs(library);
    for (const std::string& p : scan.problems) std::printf("problem: %s\n", p.c_str());
    if (scan.installs.empty()) {
        std::printf("skipped: no usable installation under %s\n", library.string().c_str());
        return SkipTest;
    }

    for (const GameInstall& install : scan.installs) {
        const EditionInfo& info = edition_info(install.edition);
        std::printf("%s: %s\n", std::string(info.id).c_str(), install.exe.string().c_str());
        std::string error;
        CHECK(verify_content(install, &error));

        ExeImage image;
        fs::path used;
        std::vector<ExeCandidate> seen;
        REQUIRE(exe_image_find(install.game_dir, install.edition, image, &used, &seen, &error));
        CHECK(image.edition == install.edition && image.base == info.image_base && image.bytes.size() == info.image_size);
        CHECK(image.at(image.base, 2)[0] == 'M');

        // The patched 2017 file is recognised and refused; the pristine copy beside it is used.
        if (install.edition == Edition::Dcu2017 && fs::exists(install.game_dir / "Daytona.exe.original")) {
            CHECK(used.filename() == "Daytona.exe.original");
            bool refused = false;
            for (const ExeCandidate& c : seen)
                refused |= c.path.filename() == "Daytona.exe" && c.check.identity == ExeIdentity::Refused;
            CHECK(refused);
        }

        // The mapped image agrees with the PE headers.
        std::vector<std::uint8_t> file;
        REQUIRE(read_file(used, file, &error));
        Pe32Header header;
        REQUIRE(pe32_parse(file, header, &error));
        CHECK(header.entry_rva == info.entry_rva);
        CHECK(header.directories[std::size_t(Pe32Directory::Relocation)].size == 0);
        for (const Pe32Section& s : header.sections)
            if (s.file_backed() >= 16) CHECK(std::memcmp(image.at(image.base + s.rva, 16), file.data() + s.raw_offset, 16) == 0);

        // Cache round trip, then a damaged copy is rejected.
        const fs::path cache = cache_dir / std::string(info.cache_name);
        REQUIRE(exe_image_write_cache(cache, image, &error));
        ExeImage reloaded;
        REQUIRE(exe_image_load_cache(cache, install.edition, reloaded, &error));
        CHECK(reloaded.bytes == image.bytes);
        const Edition other = install.edition == Edition::Dcu2017 ? Edition::Nse2019 : Edition::Dcu2017;
        CHECK(!exe_image_load_cache(cache, other, reloaded, &error));

        std::vector<std::uint8_t> bytes;
        REQUIRE(read_file(cache, bytes, &error));
        bytes[bytes.size() / 2] ^= 0xff;
        const fs::path damaged = cache_dir / ("damaged-" + std::string(info.cache_name));
        REQUIRE(write_file_atomic(damaged, bytes, &error));
        CHECK(!exe_image_load_cache(damaged, install.edition, reloaded, &error));
        std::printf("  damaged cache rejected: %s\n", error.c_str());
        fs::remove(damaged);
    }
    return check_result();
}

// EXE image checks that need no game files: range verification, bounds,
// identification of files that are not a supported build, manifest sanity.
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "check.hpp"
#include "synthetic_pe.hpp"
#include "system/exe_image.hpp"
#include "system/files.hpp"

using namespace dcusa;
namespace fs = std::filesystem;

static fs::path temp_dir() {
    fs::path d = fs::temp_directory_path() / ("dcusa_test_exe_image_" + std::to_string(::getpid()));
    fs::create_directories(d);
    return d;
}

int main() {
    // Range verification over a made-up image.
    std::vector<std::uint8_t> image(0x3000);
    for (std::size_t i = 0; i < image.size(); ++i) image[i] = std::uint8_t(i ^ (i >> 8));
    const std::uint32_t base = 0x400000;
    std::vector<ExeRange> ranges;
    for (std::uint32_t at = 0; at < 0x3000; at += 0x1000)
        ranges.push_back({base + at, 0x1000, sha256(std::span<const std::uint8_t>(image).subspan(at, 0x1000))});
    std::uint64_t last_done = 0, last_total = 0;
    CHECK(!exe_first_mismatch(image, base, ranges, [&](std::uint64_t d, std::uint64_t t) { last_done = d; last_total = t; }));
    CHECK(last_done == 0x3000 && last_total == 0x3000);
    image[0x1abc] ^= 1;
    CHECK(exe_first_mismatch(image, base, ranges) == std::optional<std::size_t>(1));
    image[0x1abc] ^= 1;
    ranges.push_back({base + 0x2800, 0x1000, {}});  // runs past the image
    CHECK(exe_first_mismatch(image, base, ranges) == std::optional<std::size_t>(3));
    CHECK(exe_first_mismatch(image, base + 0x1000, ranges) == std::optional<std::size_t>(0));

    // Bounds.
    ExeImage img{Edition::Nse2019, base, image};
    CHECK(img.contains(base, 0x3000) && !img.contains(base, 0x3001) && !img.contains(base - 1, 1));
    CHECK(img.u32(base + 4) == (std::uint32_t(image[4]) | std::uint32_t(image[5]) << 8 | std::uint32_t(image[6]) << 16 | std::uint32_t(image[7]) << 24));
    bool threw = false;
    try { img.at(base + 0x2ffe, 4); } catch (const std::out_of_range&) { threw = true; }
    CHECK(threw);

    // Files that are not a supported build.
    const fs::path dir = temp_dir();
    ExeImage out;
    ExeFileCheck check;
    CHECK(!exe_image_from_file(dir / "missing.exe", std::nullopt, out, check));
    CHECK(check.identity == ExeIdentity::Unreadable);

    std::string error;
    REQUIRE(write_file_atomic(dir / "junk.exe", std::vector<std::uint8_t>(5000, 0x33), &error));
    CHECK(!exe_image_from_file(dir / "junk.exe", std::nullopt, out, check));
    CHECK(check.identity == ExeIdentity::NotExecutable);

    auto pe = make_pe32(0x400000, 0x8000, {{".text", 0x1000, 0x1000, 0x1000, 0x60000020, 1}});
    REQUIRE(write_file_atomic(dir / "Daytona.exe", pe, &error));
    CHECK(!exe_image_from_file(dir / "Daytona.exe", std::nullopt, out, check));
    CHECK(check.identity == ExeIdentity::OtherBuild);
    CHECK(check.message.find("not a supported") != std::string::npos);

    std::vector<ExeCandidate> seen;
    fs::path used;
    CHECK(!exe_image_find(dir, std::nullopt, out, &used, &seen, &error));
    CHECK(seen.size() == 1 && error.find("Daytona.exe") != std::string::npos);

    // A cache file that isn't one.
    CHECK(!exe_image_load_cache(dir / "junk.exe", Edition::Nse2019, out, &error));
    CHECK(error.find("not a prepared image") != std::string::npos);
    CHECK(!exe_image_load_cache(dir / "none.cache", Edition::Nse2019, out, &error));

    // Generated manifests: ordered, non-overlapping, inside each edition's image, covering .text.
    for (const EditionInfo& e : editions()) {
        auto m = exe_manifest(e.edition);
        REQUIRE(m.size() > 200);
        CHECK(m.front().va == e.image_base);
        bool covers_text = false;
        for (std::size_t i = 0; i < m.size(); ++i) {
            CHECK(m[i].size > 0 && m[i].size <= 0x10000);
            CHECK(m[i].va >= e.image_base && std::uint64_t(m[i].va) + m[i].size <= std::uint64_t(e.image_base) + e.image_size);
            if (i) CHECK(m[i].va >= m[i - 1].va + m[i - 1].size);
            covers_text |= m[i].va == e.image_base + 0x1000;
        }
        CHECK(covers_text);
    }
    CHECK(exe_manifest_id(Edition::Dcu2017) != exe_manifest_id(Edition::Nse2019));
    CHECK(rom_manifest().size() == 15);

    fs::remove_all(dir);
    return check_result();
}

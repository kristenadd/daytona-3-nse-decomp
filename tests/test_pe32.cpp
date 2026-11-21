#include <string>

#include "check.hpp"
#include "synthetic_pe.hpp"
#include "system/pe32.hpp"

using namespace dcusa;

int main() {
    const std::vector<SyntheticSection> layout = {
        {".text", 0x1000, 0x1800, 0x1800, 0x60000020, 0x10},
        {".data", 0x3000, 0x4000, 0x0200, 0xc0000040, 0x80},  // mostly zero-filled
        {".rsrc", 0x7000, 0x0100, 0x0200, 0x40000040, 0x40},  // raw larger than virtual
    };
    const auto file = make_pe32(0x400000, 0x8000, layout);

    Pe32Header h;
    std::string error;
    REQUIRE(pe32_parse(file, h, &error));
    CHECK(h.image_base == 0x400000 && h.image_size == 0x8000 && h.headers_size == 0x400);
    CHECK(h.entry_rva == 0x1000 && h.linker_major == 11);
    REQUIRE(h.sections.size() == 3);
    CHECK(h.sections[0].name == ".text" && h.sections[0].executable());
    CHECK(!h.sections[1].executable());
    CHECK(h.sections[1].file_backed() == 0x200);
    CHECK(h.sections[2].file_backed() == 0x100);

    std::vector<std::uint8_t> image;
    REQUIRE(pe32_map(file, h, image, &error));
    REQUIRE(image.size() == 0x8000);
    CHECK(image[0] == 'M' && image[1] == 'Z');
    CHECK(image[0x1000] == 0x10 && image[0x1001] == 0x11);
    CHECK(image[0x3000] == 0x80 && image[0x31ff] == std::uint8_t(0x80 + 0x1ff));
    CHECK(image[0x3200] == 0);                 // past .data's file-backed bytes
    CHECK(image[0x70ff] == std::uint8_t(0x40 + 0xff));
    CHECK(image[0x7100] == 0);                 // raw padding past virtual size is not mapped

    // Rejections.
    CHECK(!pe32_parse(make_pe32(0x400000, 0x8000, layout, 0x8664), h, &error));
    CHECK(error.find("32-bit") != std::string::npos);
    CHECK(!pe32_parse(make_pe32(0x400000, 0x8000, layout, 0x014c, 0x020b), h, &error));
    CHECK(!pe32_parse(make_pe32(0x400000, 0x4000, layout), h, &error));  // section outside image
    auto truncated = file;
    truncated.resize(file.size() - 0x180);  // .rsrc maps its first 0x100 of 0x200 raw bytes
    CHECK(!pe32_parse(truncated, h, &error));
    CHECK(error.find("past the end") != std::string::npos);
    std::vector<std::uint8_t> junk(4096, 0x5a);
    CHECK(!pe32_parse(junk, h, &error));
    return check_result();
}

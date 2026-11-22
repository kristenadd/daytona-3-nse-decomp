// A hand-built mapped image: the loader must copy it, point the import slot at
// the fail-closed sentinel, seed the security cookie, and build a TEB.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "recomp/dispatch.hpp"
#include "recomp/loader.hpp"
#include "recomp/win32.hpp"
#include "recomp/x86ops.hpp"
#include "system/exe_image.hpp"

using namespace dcusa;
using namespace dcusa::recomp;

namespace {

void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = std::uint8_t(v);
    b[at + 1] = std::uint8_t(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    b[at] = std::uint8_t(v);
    b[at + 1] = std::uint8_t(v >> 8);
    b[at + 2] = std::uint8_t(v >> 16);
    b[at + 3] = std::uint8_t(v >> 24);
}
void put_str(std::vector<std::uint8_t>& b, std::size_t at, const char* s) {
    std::memcpy(b.data() + at, s, std::strlen(s) + 1);
}

ExeImage mapped_image() {
    constexpr std::uint32_t ImageSize = 0x3000;
    constexpr std::uint32_t Base = 0x400000;
    std::vector<std::uint8_t> bytes(ImageSize, 0);
    bytes[0] = 'M';
    bytes[1] = 'Z';
    put32(bytes, 0x3c, 0x80);
    std::memcpy(bytes.data() + 0x80, "PE\0\0", 4);
    put16(bytes, 0x84, 0x14c);
    put16(bytes, 0x84 + 2, 2);
    put16(bytes, 0x84 + 16, 224);
    const std::size_t opt = 0x98;
    put16(bytes, opt, 0x10b);
    put32(bytes, opt + 16, 0x1000);
    put32(bytes, opt + 28, Base);
    put32(bytes, opt + 56, ImageSize);
    put32(bytes, opt + 60, 0x400);
    put32(bytes, opt + 92, 16);
    put32(bytes, opt + 96 + 8, 0x2000);  // import directory
    put32(bytes, opt + 100 + 8, 40);
    put32(bytes, opt + 96 + 80, 0x2100);  // load config
    put32(bytes, opt + 100 + 80, 64);

    const std::size_t table = opt + 224;
    std::memcpy(bytes.data() + table, ".rdata", 6);
    put32(bytes, table + 8, 0x1000);
    put32(bytes, table + 12, 0x1000);
    put32(bytes, table + 36, 0x40000000u);
    std::memcpy(bytes.data() + table + 40, ".data", 5);
    put32(bytes, table + 40 + 8, 0x1000);
    put32(bytes, table + 40 + 12, 0x2000);
    put32(bytes, table + 40 + 36, 0xC0000000u);

    bytes[0x1000] = 0xAB;
    put32(bytes, 0x2000, 0x2200);        // OriginalFirstThunk
    put32(bytes, 0x2000 + 12, 0x2300);   // library name
    put32(bytes, 0x2000 + 16, 0x2400);   // FirstThunk
    put32(bytes, 0x2200, 0x2310);        // hint/name
    put_str(bytes, 0x2300, "KERNEL32.dll");
    put_str(bytes, 0x2312, "GetCommandLineA");
    put32(bytes, 0x2100, 64);            // load-config size
    put32(bytes, 0x2100 + 60, Base + 0x2500);
    put32(bytes, 0x2500, 0xBB40E64Eu);   // default security cookie

    ExeImage image;
    image.edition = Edition::Nse2019;
    image.base = Base;
    image.bytes = std::move(bytes);
    return image;
}

}

int main() {
    std::string error;
    GuestProcess process;
    REQUIRE(process.load(mapped_image(), &error));
    CHECK(error.empty());

    const std::uint8_t* M = process.cpu().mem;
    CHECK(M[0x401000] == 0xAB);
    CHECK(ld32(M, 0x402400) == ImportBase);
    REQUIRE(process.imports().size() == 1);
    CHECK(process.imports()[0].iat == 0x402400u);
    CHECK(std::string(process.imports()[0].name) == "KERNEL32.dll!GetCommandLineA");
    CHECK(process.imports()[0].handler == import_handler("KERNEL32.dll!GetCommandLineA"));
    CHECK(ld32(M, 0x402500) == 0x0C0FFEE1u);

    CHECK(process.cpu().fs_base == 0x10000u);
    CHECK(process.cpu().r[Cpu::ESP] == 0x2FFFFFF0u);
    CHECK(process.entry() == 0x401000u);
    CHECK(ld32(M, 0x10000) == 0xFFFFFFFFu);
    CHECK(ld32(M, 0x10004) == 0x30000000u);
    CHECK(ld32(M, 0x10018) == 0x10000u);
    CHECK(ld32(M, 0x10030) == 0x20000u);
    CHECK(ld32(M, 0x20008) == 0x400000u);

    st8(process.cpu().mem, 0x402600, 0x42);
    CHECK(process.cpu().mem[0x402600] == 0x42);

    ExeImage broken = mapped_image();
    broken.bytes.resize(0x100);
    GuestProcess rejected;
    CHECK(!rejected.load(broken, &error));
    CHECK(!error.empty());
    return check_result();
}

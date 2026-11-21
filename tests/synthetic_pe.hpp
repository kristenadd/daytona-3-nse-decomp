#pragma once
// A small, made-up PE32 file for tests that must not depend on the game.
#include <cstdint>
#include <cstring>
#include <vector>

struct SyntheticSection {
    const char* name;
    std::uint32_t rva, virtual_size, raw_size, characteristics;
    std::uint8_t fill;
};

inline void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = std::uint8_t(v);
    b[at + 1] = std::uint8_t(v >> 8);
}
inline void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + std::size_t(i)] = std::uint8_t(v >> (8 * i));
}

// Headers in the first 0x400 bytes, then each section's raw bytes (filled
// with `fill + offset`) back to back.
inline std::vector<std::uint8_t> make_pe32(std::uint32_t image_base, std::uint32_t image_size,
                                           const std::vector<SyntheticSection>& sections,
                                           std::uint16_t machine = 0x014c, std::uint16_t magic = 0x010b) {
    constexpr std::size_t pe = 0x80, opt = pe + 24, optional_size = 224, headers = 0x400;
    std::size_t total = headers;
    for (const SyntheticSection& s : sections) total += s.raw_size;
    std::vector<std::uint8_t> b(total, 0);
    b[0] = 'M'; b[1] = 'Z';
    put32(b, 0x3c, pe);
    std::memcpy(b.data() + pe, "PE\0\0", 4);
    put16(b, pe + 4, machine);
    put16(b, pe + 6, std::uint16_t(sections.size()));
    put32(b, pe + 8, 0x5a000000u);
    put16(b, pe + 20, optional_size);
    put16(b, pe + 22, 0x0123);
    put16(b, opt, magic);
    b[opt + 2] = 11;
    put32(b, opt + 16, sections.empty() ? 0 : sections[0].rva);
    put32(b, opt + 28, image_base);
    put32(b, opt + 56, image_size);
    put32(b, opt + 60, headers);
    put16(b, opt + 70, 0x8100);
    put32(b, opt + 92, 16);
    std::size_t raw = headers;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const SyntheticSection& s = sections[i];
        std::size_t at = opt + optional_size + 40 * i;
        std::memcpy(b.data() + at, s.name, std::strlen(s.name));
        put32(b, at + 8, s.virtual_size);
        put32(b, at + 12, s.rva);
        put32(b, at + 16, s.raw_size);
        put32(b, at + 20, std::uint32_t(raw));
        put32(b, at + 36, s.characteristics);
        for (std::uint32_t k = 0; k < s.raw_size; ++k) b[raw + k] = std::uint8_t(s.fill + k);
        raw += s.raw_size;
    }
    return b;
}

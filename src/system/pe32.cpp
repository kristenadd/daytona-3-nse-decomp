#include "system/pe32.hpp"

#include <cstring>

namespace dcusa {
namespace {

constexpr std::uint16_t MachineI386 = 0x014c;
constexpr std::uint16_t MagicPe32 = 0x010b;
// Largest image a 32-bit large-address-aware process can map.
constexpr std::uint64_t MaxImage = 0x80000000ull;

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t at) { return std::uint16_t(b[at] | b[at + 1] << 8); }
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    return std::uint32_t(b[at]) | std::uint32_t(b[at + 1]) << 8 | std::uint32_t(b[at + 2]) << 16 | std::uint32_t(b[at + 3]) << 24;
}

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

}

bool pe32_parse(std::span<const std::uint8_t> file, Pe32Header& h, std::string* error) {
    h = {};
    if (file.size() < 0x40 || file[0] != 'M' || file[1] != 'Z') return fail(error, "not a Windows executable (no MZ header)");
    std::uint32_t pe = u32(file, 0x3c);
    if (std::uint64_t(pe) + 24 > file.size() || std::memcmp(file.data() + pe, "PE\0\0", 4) != 0)
        return fail(error, "not a Windows executable (no PE header)");
    if (u16(file, pe + 4) != MachineI386) return fail(error, "not a 32-bit x86 executable");
    std::uint16_t section_count = u16(file, pe + 6);
    h.timestamp = u32(file, pe + 8);
    std::uint16_t optional_size = u16(file, pe + 20);
    h.characteristics = u16(file, pe + 22);
    std::size_t opt = pe + 24;
    if (optional_size < 96 + 16 * 8 || opt + optional_size > file.size()) return fail(error, "optional header truncated");
    if (u16(file, opt) != MagicPe32) return fail(error, "not a PE32 executable");
    h.linker_major = file[opt + 2];
    h.linker_minor = file[opt + 3];
    h.entry_rva = u32(file, opt + 16);
    h.image_base = u32(file, opt + 28);
    h.image_size = u32(file, opt + 56);
    h.headers_size = u32(file, opt + 60);
    h.dll_characteristics = u16(file, opt + 70);
    std::uint32_t directory_count = u32(file, opt + 92);
    for (std::uint32_t i = 0; i < directory_count && i < 16; ++i)
        h.directories[i] = {u32(file, opt + 96 + 8 * i), u32(file, opt + 100 + 8 * i)};
    if (h.image_size == 0 || std::uint64_t(h.image_base) + h.image_size > 0x100000000ull || h.image_size > MaxImage)
        return fail(error, "image size out of range");
    if (h.headers_size > h.image_size || h.headers_size > file.size()) return fail(error, "headers larger than the image");

    std::size_t table = opt + optional_size;
    if (table + std::size_t(section_count) * 40 > file.size()) return fail(error, "section table truncated");
    for (std::uint16_t i = 0; i < section_count; ++i) {
        std::size_t s = table + std::size_t(i) * 40;
        Pe32Section sec;
        const char* name = reinterpret_cast<const char*>(file.data() + s);
        sec.name.assign(name, strnlen(name, 8));
        sec.virtual_size = u32(file, s + 8);
        sec.rva = u32(file, s + 12);
        sec.raw_size = u32(file, s + 16);
        sec.raw_offset = u32(file, s + 20);
        sec.characteristics = u32(file, s + 36);
        if (std::uint64_t(sec.rva) + sec.virtual_size > h.image_size)
            return fail(error, "section " + sec.name + " lies outside the image");
        if (std::uint64_t(sec.raw_offset) + sec.file_backed() > file.size())
            return fail(error, "section " + sec.name + " runs past the end of the file");
        h.sections.push_back(std::move(sec));
    }
    return true;
}

bool pe32_map(std::span<const std::uint8_t> file, const Pe32Header& h, std::vector<std::uint8_t>& image, std::string* error) {
    if (h.image_size == 0) return fail(error, "headers not parsed");
    image.assign(h.image_size, 0);
    std::memcpy(image.data(), file.data(), h.headers_size);
    for (const Pe32Section& sec : h.sections) {
        if (std::uint64_t(sec.raw_offset) + sec.file_backed() > file.size()) return fail(error, "section " + sec.name + " truncated");
        std::memcpy(image.data() + sec.rva, file.data() + sec.raw_offset, sec.file_backed());
    }
    return true;
}

}

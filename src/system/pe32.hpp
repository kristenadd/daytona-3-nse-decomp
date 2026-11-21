#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dcusa {

// A 32-bit x86 Windows executable (PE32, machine i386), as both game EXEs are.
struct Pe32Section {
    std::string name;
    std::uint32_t rva = 0, virtual_size = 0;
    std::uint32_t raw_offset = 0, raw_size = 0;
    std::uint32_t characteristics = 0;
    // Bytes of the section that come from the file (the rest of virtual_size is zero).
    std::uint32_t file_backed() const { return raw_size < virtual_size ? raw_size : virtual_size; }
    bool executable() const { return (characteristics & 0x20000000u) != 0; }
};

struct Pe32DataDirectory {
    std::uint32_t rva = 0, size = 0;
};

struct Pe32Header {
    std::uint16_t characteristics = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t image_base = 0, image_size = 0, headers_size = 0;
    std::uint32_t entry_rva = 0;
    std::uint16_t dll_characteristics = 0;
    std::uint8_t linker_major = 0, linker_minor = 0;
    std::array<Pe32DataDirectory, 16> directories{};
    std::vector<Pe32Section> sections;
};

enum class Pe32Directory : std::size_t { Export = 0, Import = 1, Resource = 2, Relocation = 5, Debug = 6, Tls = 9, LoadConfig = 10, Iat = 12 };

// Parses and checks the headers. False with *error set for anything that is not
// a well-formed PE32 i386 image whose sections fit the file and the image.
bool pe32_parse(std::span<const std::uint8_t> file, Pe32Header& header, std::string* error);

// Lays the file out at its virtual addresses: image[rva] is the byte the
// Windows loader would place at image_base + rva (headers, then each
// section's file-backed bytes; everything else zero). No relocation is
// applied; both game EXEs are fixed-base without a .reloc section.
bool pe32_map(std::span<const std::uint8_t> file, const Pe32Header& header, std::vector<std::uint8_t>& image, std::string* error);

}

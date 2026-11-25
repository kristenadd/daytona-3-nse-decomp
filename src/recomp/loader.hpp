#pragma once
// Loads a prepared Daytona.exe into guest memory and builds the machine the
// translated entry point runs on: sections with their protections, the import
// address table pointing at fail-closed handlers, a stack, and a TEB.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "recomp/cpu.hpp"
#include "recomp/dispatch.hpp"
#include "recomp/guest_memory.hpp"
#include "system/exe_image.hpp"

namespace dcusa::recomp {

class GuestProcess {
public:
    // Copies `image` into a fresh guest address space. False with *error on a
    // malformed image or a memory failure; do not call start() after that.
    bool load(const dcusa::ExeImage& image, std::string* error);

    Cpu& cpu() { return cpu_; }
    std::uint32_t entry() const { return entry_; }
    std::span<const ImportSlot> imports() const { return imports_; }

    // Installs `entries` (the translator's table) and calls the guest entry point.
    // Returns when the entry point returns; an unported import or instruction throws FailClosed.
    void start(std::span<const GuestEntry> entries);

private:
    GuestMemory memory_;
    Cpu cpu_{};
    std::vector<std::string> import_names_;
    std::vector<ImportSlot> imports_;
    std::uint32_t entry_ = 0;
};

// The loaded game image's guest address range (0, 0 before one is loaded).
inline std::uint32_t g_image_lo = 0, g_image_hi = 0;

}

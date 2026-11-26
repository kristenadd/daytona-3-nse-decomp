#include "recomp/loader.hpp"
#include "recomp/game_addresses.hpp"

#include <cstring>
#include <span>

#include "recomp/threads.hpp"
#include "recomp/win32.hpp"
#include "recomp/x86ops.hpp"
#include "runtime/fail_closed.hpp"

namespace dcusa::recomp {
namespace {

constexpr std::uint32_t MemWrite = 0x80000000u;
constexpr std::uint32_t OrdinalFlag = 0x80000000u;
// The cookie MSVC emits. __security_init_cookie replaces it, using several
// kernel imports, unless it is already a value whose high half is nonzero.
// A fixed value keeps runs reproducible; the complement is written by the CRT.
constexpr std::uint32_t DefaultCookie = 0xBB40E64Eu;
constexpr std::uint32_t Cookie = 0x0C0FFEE1u;

constexpr std::uint32_t StackTop = 0x30000000u;
constexpr std::uint32_t StackSize = 1u << 20;
constexpr std::uint32_t TebAddress = 0x10000u;
constexpr std::uint32_t PebAddress = 0x20000u;
constexpr std::uint32_t TlsAddress = 0x21000u;

bool fail(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t at) { return std::uint16_t(b[at] | b[at + 1] << 8); }
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    return std::uint32_t(b[at]) | std::uint32_t(b[at + 1]) << 8 | std::uint32_t(b[at + 2]) << 16 | std::uint32_t(b[at + 3]) << 24;
}

struct Section {
    std::uint32_t rva = 0, virtual_size = 0, characteristics = 0;
};

struct View {
    std::uint32_t image_base = 0, image_size = 0, headers_size = 0, entry_rva = 0;
    std::uint32_t import_rva = 0, import_size = 0;
    std::uint32_t load_config_rva = 0, load_config_size = 0;
    std::vector<Section> sections;
};

bool parse_view(std::span<const std::uint8_t> bytes, View& v, std::string* error) {
    if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z') return fail(error, "prepared image has no MZ header");
    std::uint32_t pe = u32(bytes, 0x3c);
    if (std::uint64_t(pe) + 24 > bytes.size() || std::memcmp(bytes.data() + pe, "PE\0\0", 4) != 0)
        return fail(error, "prepared image has no PE header");
    std::uint16_t section_count = u16(bytes, pe + 6);
    std::uint16_t optional_size = u16(bytes, pe + 20);
    std::size_t opt = pe + 24;
    if (optional_size < 224 || opt + optional_size > bytes.size()) return fail(error, "optional header truncated");
    v.entry_rva = u32(bytes, opt + 16);
    v.image_base = u32(bytes, opt + 28);
    v.image_size = u32(bytes, opt + 56);
    v.headers_size = u32(bytes, opt + 60);
    if (v.image_size == 0 || v.image_size > bytes.size()) return fail(error, "image size does not match the prepared image");
    std::uint32_t directories = u32(bytes, opt + 92);
    auto directory = [&](std::uint32_t index, std::uint32_t& rva, std::uint32_t& size) {
        if (index >= directories) return;
        rva = u32(bytes, opt + 96 + 8 * index);
        size = u32(bytes, opt + 100 + 8 * index);
    };
    directory(1, v.import_rva, v.import_size);
    directory(10, v.load_config_rva, v.load_config_size);
    std::size_t table = opt + optional_size;
    if (table + std::size_t(section_count) * 40 > bytes.size()) return fail(error, "section table truncated");
    for (std::uint16_t i = 0; i < section_count; ++i) {
        std::size_t s = table + std::size_t(i) * 40;
        Section sec;
        sec.virtual_size = u32(bytes, s + 8);
        sec.rva = u32(bytes, s + 12);
        sec.characteristics = u32(bytes, s + 36);
        if (std::uint64_t(sec.rva) + sec.virtual_size > v.image_size) return fail(error, "section lies outside the image");
        v.sections.push_back(sec);
    }
    return true;
}

bool in_image(const View& v, std::uint32_t rva, std::uint32_t size) {
    return std::uint64_t(rva) + size <= v.image_size;
}

std::string bounded_string(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::size_t n = 0;
    while (at + n < bytes.size() && bytes[at + n] != 0 && n < 512) ++n;
    return std::string(reinterpret_cast<const char*>(bytes.data() + at), n);
}

std::string import_name(std::span<const std::uint8_t> bytes, const View& v, std::uint32_t thunk, std::string* error) {
    if (thunk & OrdinalFlag) return "#" + std::to_string(thunk & 0xFFFFu);
    if (!in_image(v, thunk + 2, 1)) {
        if (error) *error = "import name outside the image";
        return {};
    }
    return bounded_string(bytes, thunk + 2);
}

}

bool GuestProcess::load(const dcusa::ExeImage& image, std::string* error) {
    std::span<const std::uint8_t> bytes(image.bytes.data(), image.bytes.size());
    View view;
    if (!parse_view(bytes, view, error)) return false;
    if (image.base != view.image_base) return fail(error, "prepared image base does not match its header");
    if (!memory_.reserve(error)) return false;
    if (!memory_.commit(view.image_base, view.image_size, error)) return false;
    std::memcpy(memory_.host(view.image_base), image.bytes.data(), view.image_size);
    g_image_lo = view.image_base;
    g_image_hi = view.image_base + view.image_size;
    set_game_edition(image.edition);

    std::uint8_t* M = memory_.base();
    std::vector<std::uint32_t> slots;
    if (view.import_rva) {
        for (std::uint32_t at = view.import_rva; in_image(view, at, 20); at += 20) {
            std::uint32_t lookup = u32(bytes, at);
            std::uint32_t name_rva = u32(bytes, at + 12);
            std::uint32_t iat_rva = u32(bytes, at + 16);
            if (!name_rva) break;
            if (!in_image(view, name_rva, 1)) return fail(error, "import library name outside the image");
            std::string dll = bounded_string(bytes, name_rva);
            std::uint32_t thunks = lookup ? lookup : iat_rva;
            for (std::uint32_t i = 0;; ++i) {
                if (!in_image(view, thunks + 4 * i, 4) || !in_image(view, iat_rva + 4 * i, 4))
                    return fail(error, "import thunk outside the image");
                std::uint32_t thunk = u32(bytes, thunks + 4 * i);
                if (!thunk) break;
                std::string function = import_name(image.bytes, view, thunk, error);
                if (function.empty() && error && !error->empty()) return false;
                slots.push_back(view.image_base + iat_rva + 4 * i);
                import_names_.push_back(dll + "!" + function);
            }
        }
    }
    imports_.resize(import_names_.size());
    for (std::size_t i = 0; i < imports_.size(); ++i) {
        imports_[i] = {slots[i], import_names_[i].c_str(), import_handler(import_names_[i])};
        st32(M, slots[i], ImportBase + 16 * std::uint32_t(i));
    }

    if (view.load_config_rva && in_image(view, view.load_config_rva, 64)) {
        std::uint32_t described = u32(bytes, view.load_config_rva);
        if (described >= 64) {
            std::uint32_t cookie = u32(bytes, view.load_config_rva + 60);
            if (cookie >= view.image_base && std::uint64_t(cookie) + 4 <= std::uint64_t(view.image_base) + view.image_size) {
                std::uint32_t value = ld32(M, cookie);
                if (value == DefaultCookie || (value & 0xFFFF0000u) == 0) st32(M, cookie, Cookie);
            }
        }
    }

    for (const Section& sec : view.sections) {
        if (sec.characteristics & MemWrite) continue;
        if (!memory_.protect_read_only(view.image_base + sec.rva, sec.virtual_size, error)) return false;
    }

    if (!memory_.commit(StackTop - StackSize, StackSize, error)) return false;
    if (!memory_.commit(kProcessHeap, kHeapBytes, error)) return false;
    if (!memory_.commit(kHeap2, kHeap2Bytes, error)) return false;
    if (!memory_.commit(TebAddress, 0x1000, error)) return false;
    if (!memory_.commit(PebAddress, 0x2000, error)) return false;
    st32(M, TebAddress + 0x00, 0xFFFFFFFFu);  // ExceptionList: end of the chain
    st32(M, TebAddress + 0x04, StackTop);
    st32(M, TebAddress + 0x08, StackTop - StackSize);
    st32(M, TebAddress + 0x18, TebAddress);  // Self
    st32(M, TebAddress + 0x20, 1);           // ClientId.UniqueProcess
    st32(M, TebAddress + 0x24, 1);           // ClientId.UniqueThread
    st32(M, TebAddress + 0x2C, TlsAddress);
    st32(M, TebAddress + 0x30, PebAddress);
    st32(M, PebAddress + 0x08, view.image_base);
    st32(M, PebAddress + 0x18, 6);  // OSMajorVersion
    st32(M, PebAddress + 0x1C, 1);  // OSMinorVersion

    cpu_.mem = M;
    cpu_.fs_base = TebAddress;
    cpu_.r[Cpu::ESP] = StackTop - 16;
    entry_ = view.image_base + view.entry_rva;
    return true;
}

void GuestProcess::start(std::span<const GuestEntry> entries) {
    dispatch_install(entries, imports_);
    sched_reset(cpu_);
    GuestFn fn = find_entry(entry_);
    if (!fn) fail_closed(Unported::Function, entry_, "no translation for the entry point");
    fn(cpu_);
}

}

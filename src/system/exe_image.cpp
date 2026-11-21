#include "system/exe_image.hpp"

#include <zlib.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "system/files.hpp"
#include "system/pe32.hpp"

namespace dcusa {

namespace fs = std::filesystem;

namespace {

constexpr char CacheMagic[8] = {'D', 'C', 'U', 'S', 'A', 'I', 'M', 'G'};
constexpr std::uint32_t CacheVersion = 1;
constexpr std::size_t CacheHeaderSize = 64;
// Smallest file worth hashing as a candidate executable (both EXEs are ~18 MB).
constexpr std::uintmax_t MinCandidateSize = 8u << 20;

void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (8 * i));
}
std::uint32_t get32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

std::uint32_t edition_number(Edition e) { return e == Edition::Dcu2017 ? 2017u : 2019u; }

std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

// Maps a PE32 file and checks it against one edition's manifest.
bool map_and_verify(std::span<const std::uint8_t> file, Edition edition, ExeImage& out, std::string* why,
                    const Progress& progress) {
    const EditionInfo& info = edition_info(edition);
    Pe32Header header;
    std::vector<std::uint8_t> image;
    if (!pe32_parse(file, header, why) || !pe32_map(file, header, image, why)) return false;
    if (header.image_base != info.image_base || header.image_size != info.image_size)
        return fail(why, "image layout differs from the " + std::string(info.id) + " build");
    if (auto bad = exe_first_mismatch(image, header.image_base, exe_manifest(edition), progress)) {
        const ExeRange& r = exe_manifest(edition)[*bad];
        return fail(why, "differs from the " + std::string(info.id) + " build at " + hex32(r.va) + ".." + hex32(r.va + r.size));
    }
    out.edition = edition;
    out.base = header.image_base;
    out.bytes = std::move(image);
    return true;
}

}

bool ExeImage::contains(std::uint32_t va, std::uint32_t size) const {
    return va >= base && std::uint64_t(va - base) + size <= bytes.size();
}

const std::uint8_t* ExeImage::at(std::uint32_t va, std::uint32_t size) const {
    if (!contains(va, size)) throw std::out_of_range("guest range " + hex32(va) + "+" + std::to_string(size) + " outside the EXE image");
    return bytes.data() + (va - base);
}

std::uint32_t ExeImage::u32(std::uint32_t va) const { return get32(at(va, 4)); }

std::optional<std::size_t> exe_first_mismatch(std::span<const std::uint8_t> image, std::uint32_t base,
                                              std::span<const ExeRange> ranges, const Progress& progress) {
    std::uint64_t total = 0, done = 0;
    for (const ExeRange& r : ranges) total += r.size;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const ExeRange& r = ranges[i];
        if (r.va < base || std::uint64_t(r.va - base) + r.size > image.size()) return i;
        if (sha256(image.subspan(r.va - base, r.size)) != r.sha256) return i;
        done += r.size;
        if (progress) progress(done, total);
    }
    return std::nullopt;
}

bool exe_image_from_file(const fs::path& path, std::optional<Edition> want, ExeImage& out, ExeFileCheck& check,
                         const Progress& progress) {
    check = {};
    std::vector<std::uint8_t> file;
    std::string error;
    if (!read_file(path, file, &error)) {
        check.message = error;
        return false;
    }
    const Sha256Digest digest = sha256(file);

    for (const RefusedExe& r : refused_exes()) {
        if (r.size == file.size() && r.sha256 == digest) {
            check = {ExeIdentity::Refused, r.edition, std::string(r.reason)};
            return false;
        }
    }
    for (const EditionInfo& e : editions()) {
        if (e.exe_size != file.size() || e.exe_sha256 != digest) continue;
        check.edition = e.edition;
        if (want && *want != e.edition) {
            check = {ExeIdentity::Pristine, e.edition, "this is the " + std::string(e.id) + " executable, not the one asked for"};
            return false;
        }
        if (!map_and_verify(file, e.edition, out, &error, progress)) {
            // The file hash matched, so this is a manifest / mapper defect, not the player's file.
            check = {ExeIdentity::Pristine, e.edition, "internal check failed: " + error};
            return false;
        }
        check = {ExeIdentity::Pristine, e.edition, std::string(e.title)};
        return true;
    }

    Pe32Header header;
    if (!pe32_parse(file, header, &error)) {
        check = {ExeIdentity::NotExecutable, std::nullopt, error};
        return false;
    }
    // Unknown file: say how close it is to each edition, for the player and for bug reports.
    std::string closest;
    for (const EditionInfo& e : editions()) {
        if (want && *want != e.edition) continue;
        ExeImage probe;
        std::string why;
        if (map_and_verify(file, e.edition, probe, &why, {})) {
            // Every checked byte matches; only bytes outside the ranges differ.
            out = std::move(probe);
            check = {ExeIdentity::Pristine, e.edition, std::string(e.title) + " (file differs outside the checked ranges)"};
            return true;
        }
        if (!closest.empty()) closest += "; ";
        closest += why;
    }
    check = {ExeIdentity::OtherBuild, std::nullopt, "not a supported Daytona.exe build (" + closest + ")"};
    return false;
}

bool exe_image_find(const fs::path& game_dir, std::optional<Edition> want, ExeImage& out, fs::path* used,
                    std::vector<ExeCandidate>* seen, std::string* error, const Progress& progress) {
    std::vector<fs::path> order;
    for (const char* name : {"Daytona.exe", "Daytona.exe.original"})
        if (fs::is_regular_file(game_dir / name)) order.push_back(game_dir / name);
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(game_dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.file_size(ec) < MinCandidateSize) continue;
        std::string ext = entry.path().extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".exe") continue;
        bool known = false;
        for (const fs::path& p : order) known |= p == entry.path();
        if (!known) order.push_back(entry.path());
    }
    if (order.empty()) return fail(error, "no Daytona.exe in " + game_dir.string());

    for (const fs::path& path : order) {
        ExeFileCheck check;
        bool ok = exe_image_from_file(path, want, out, check, progress);
        if (seen) seen->push_back({path, check});
        if (ok) {
            if (used) *used = path;
            return true;
        }
    }
    std::string message = "no usable executable in " + game_dir.string() + ":";
    if (seen)
        for (const ExeCandidate& c : *seen) message += "\n  " + c.path.filename().string() + ": " + c.check.message;
    return fail(error, message);
}

bool exe_image_write_cache(const fs::path& path, const ExeImage& image, std::string* error) {
    uLongf packed = compressBound(uLong(image.bytes.size()));
    std::vector<std::uint8_t> out(CacheHeaderSize + packed);
    if (compress2(out.data() + CacheHeaderSize, &packed, image.bytes.data(), uLong(image.bytes.size()), 6) != Z_OK)
        return fail(error, "cannot compress the image");
    out.resize(CacheHeaderSize + packed);
    std::uint8_t* h = out.data();
    std::memcpy(h, CacheMagic, 8);
    put32(h + 8, CacheVersion);
    put32(h + 12, edition_number(image.edition));
    put32(h + 16, image.base);
    put32(h + 20, std::uint32_t(image.bytes.size()));
    put32(h + 24, std::uint32_t(packed));
    const Sha256Digest id = exe_manifest_id(image.edition);
    std::memcpy(h + 32, id.data(), id.size());
    return write_file_atomic(path, out, error);
}

bool exe_image_load_cache(const fs::path& path, Edition want, ExeImage& out, std::string* error, const Progress& progress) {
    std::vector<std::uint8_t> file;
    if (!read_file(path, file, error)) return false;
    if (file.size() < CacheHeaderSize || std::memcmp(file.data(), CacheMagic, 8) != 0) return fail(error, "not a prepared image: " + path.string());
    const std::uint8_t* h = file.data();
    if (get32(h + 8) != CacheVersion) return fail(error, "prepared image from another version of the port; prepare again");
    const EditionInfo& info = edition_info(want);
    if (get32(h + 12) != edition_number(want)) return fail(error, "prepared image is not the " + std::string(info.id) + " edition");
    const std::uint32_t base = get32(h + 16), size = get32(h + 20), packed = get32(h + 24);
    if (base != info.image_base || size != info.image_size || CacheHeaderSize + std::uint64_t(packed) != file.size())
        return fail(error, "prepared image is damaged");
    std::vector<std::uint8_t> image(size);
    uLongf unpacked = size;
    if (uncompress(image.data(), &unpacked, h + CacheHeaderSize, packed) != Z_OK || unpacked != size)
        return fail(error, "prepared image is damaged (zlib)");
    if (auto bad = exe_first_mismatch(image, base, exe_manifest(want), progress)) {
        const ExeRange& r = exe_manifest(want)[*bad];
        return fail(error, "prepared image fails verification at " + hex32(r.va) + "; prepare again");
    }
    out.edition = want;
    out.base = base;
    out.bytes = std::move(image);
    return true;
}

}

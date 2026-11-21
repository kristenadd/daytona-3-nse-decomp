#include "system/edition.hpp"

#include <stdexcept>

namespace dcusa {
namespace {

constexpr Sha256Digest digest(const char (&hex)[65]) {
    auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    Sha256Digest d{};
    for (std::size_t i = 0; i < 32; ++i) d[i] = std::uint8_t(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
    return d;
}

constexpr EditionInfo Editions[] = {
    {Edition::Dcu2017, "2017", "Daytona Championship USA (2017)", 18366464,
     digest("f5d6fa896a22ca05d31754b46afdfd5361ae5fe02c08803429f1f5d9e9010deb"),
     0x00400000u, 0x01f67000u, 0x008214f0u, "DCUSA2017.cache"},
    {Edition::Nse2019, "2019", "Daytona Championship USA New Season Edition (2019)", 18442240,
     digest("8ba4048f9a6920e3ecdc991059301e17c53c3c605c0cc639454c14d2665d379e"),
     0x00400000u, 0x01f7c000u, 0x00826d00u, "DCUSA2019.cache"},
};

constexpr RefusedExe Refused[] = {
    {18366464, digest("9464f87f650bb17cf551ce5e956a5fe5605881d7d5f55d6d14abcfc5c1d81ae4"), Edition::Dcu2017,
     "this Daytona.exe has the TeknoParrot security patch applied; the port needs the unpatched file "
     "(it is usually kept beside it as Daytona.exe.original)"},
};

}

std::span<const EditionInfo> editions() { return Editions; }

const EditionInfo& edition_info(Edition edition) {
    for (const EditionInfo& e : Editions)
        if (e.edition == edition) return e;
    throw std::invalid_argument("unknown edition");
}

std::optional<Edition> edition_from_id(std::string_view id) {
    for (const EditionInfo& e : Editions)
        if (e.id == id) return e.edition;
    return std::nullopt;
}

std::span<const RefusedExe> refused_exes() { return Refused; }

}

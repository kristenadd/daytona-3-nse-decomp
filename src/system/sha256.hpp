#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace dcusa {

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256 {
public:
    Sha256();
    void update(std::span<const std::uint8_t> bytes);
    Sha256Digest finish();

private:
    void block(const std::uint8_t* p);
    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

Sha256Digest sha256(std::span<const std::uint8_t> bytes);
std::string to_hex(const Sha256Digest& digest);
// 64 hex digits (either case) or nothing.
std::optional<Sha256Digest> digest_from_hex(std::string_view hex);

}

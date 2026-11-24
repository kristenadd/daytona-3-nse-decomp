#include "gpu/bc_decode.hpp"

#include <algorithm>
#include <cstring>

namespace dcusa::gpu {
namespace {

enum class Kind { None, BC1, BC2, BC3, BC4, BC5, BC6U, BC6S, BC7 };

Kind kind(std::uint32_t dxgi) {
    switch (dxgi) {
    case 70: case 71: case 72: return Kind::BC1;
    case 73: case 74: case 75: return Kind::BC2;
    case 76: case 77: case 78: return Kind::BC3;
    case 79: case 80: return Kind::BC4;
    case 82: case 83: return Kind::BC5;
    case 94: case 95: return Kind::BC6U;
    case 96: return Kind::BC6S;
    case 97: case 98: case 99: return Kind::BC7;
    default: return Kind::None;
    }
}

std::uint32_t block_bytes(Kind k) { return k == Kind::BC1 || k == Kind::BC4 ? 8u : 16u; }

// Interpolation as Apple GPUs do it for every BC format: 6-bit weights (BC7's
// tables), rounded to the nearest 8-bit value.
std::uint8_t lerp(std::uint32_t a, std::uint32_t b, std::uint32_t w) {
    return static_cast<std::uint8_t>(((64 - w) * a + w * b + 32) >> 6);
}

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
std::uint32_t le32(const std::uint8_t* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

// BC1's colour block (also the colour half of BC2 and BC3, which always use
// the four-colour mode) into RGBA, 4 bytes a pixel.
void colour_block(const std::uint8_t* b, std::uint8_t* out, bool bc1) {
    const std::uint16_t c0 = le16(b), c1 = le16(b + 2);
    std::uint8_t pal[4][4];
    auto expand = [](std::uint16_t c, std::uint8_t* p) {
        const std::uint32_t r = c >> 11 & 31u, g = c >> 5 & 63u, bl = c & 31u;
        p[0] = static_cast<std::uint8_t>(r << 3 | r >> 2);
        p[1] = static_cast<std::uint8_t>(g << 2 | g >> 4);
        p[2] = static_cast<std::uint8_t>(bl << 3 | bl >> 2);
        p[3] = 255;
    };
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    if (!bc1 || c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = lerp(pal[0][i], pal[1][i], 21);
            pal[3][i] = lerp(pal[0][i], pal[1][i], 43);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) pal[2][i] = lerp(pal[0][i], pal[1][i], 32);
        pal[2][3] = 255;
        std::memset(pal[3], 0, 4);
    }
    const std::uint32_t idx = le32(b + 4);
    for (int i = 0; i < 16; ++i) std::memcpy(out + 4 * i, pal[idx >> (2 * i) & 3u], 4);
}

// BC4's block (BC3's alpha, BC5's two channels): 16 values, one byte apart
// every `stride` bytes.
void single_block(const std::uint8_t* b, std::uint8_t* out, int stride) {
    const std::uint32_t r0 = b[0], r1 = b[1];
    std::uint8_t pal[8];
    pal[0] = static_cast<std::uint8_t>(r0);
    pal[1] = static_cast<std::uint8_t>(r1);
    static constexpr std::uint8_t kSevenths[6] = {9, 18, 27, 37, 46, 55}, kFifths[4] = {13, 26, 38, 51};
    if (r0 > r1) {
        for (int i = 0; i < 6; ++i) pal[i + 2] = lerp(r0, r1, kSevenths[i]);
    } else {
        for (int i = 0; i < 4; ++i) pal[i + 2] = lerp(r0, r1, kFifths[i]);
        pal[6] = 0;
        pal[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i * stride] = pal[bits >> (3 * i) & 7u];
}

// ---- BC7 -------------------------------------------------------------------

// Subset of each pixel for the 64 two-subset and 64 three-subset partitions,
// two bits a pixel, pixel 0 in the low bits.
constexpr std::uint8_t kPart2[64][16] = {
    {0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1}, {0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1}, {0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},
    {0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1}, {0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1},
    {0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1},
    {0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
    {0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1}, {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1},
    {0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1}, {0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1}, {0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0}, {0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0}, {0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0}, {0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0}, {0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
    {0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0}, {0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0}, {0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0},
    {0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0}, {0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0}, {0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},
    {0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0}, {0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0}, {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},
    {0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1}, {0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0}, {0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
    {0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0}, {0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0}, {0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1},
    {0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1}, {0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0}, {0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0},
    {0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0}, {0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0}, {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0},
    {0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1}, {0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1}, {0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
    {0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0}, {0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0}, {0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0},
    {0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0}, {0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1}, {0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1},
    {0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0}, {0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0}, {0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1},
    {0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1}, {0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1}, {0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
    {0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1}, {0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0}, {0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0},
    {0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1},
};

constexpr std::uint8_t kPart3[64][16] = {
    {0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2}, {0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1}, {0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},
    {0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1}, {0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2}, {0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},
    {0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1}, {0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1}, {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},
    {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2}, {0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2}, {0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
    {0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2}, {0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2}, {0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},
    {0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0}, {0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2}, {0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},
    {0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2}, {0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1}, {0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},
    {0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1}, {0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2}, {0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
    {0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0}, {0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2}, {0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},
    {0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1}, {0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2}, {0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},
    {0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1}, {0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1}, {0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},
    {0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1}, {0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2}, {0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
    {0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0}, {0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0}, {0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},
    {0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1}, {0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1}, {0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},
    {0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1}, {0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2}, {0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},
    {0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1}, {0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1}, {0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
    {0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2}, {0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1}, {0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},
    {0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2}, {0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2}, {0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},
    {0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2}, {0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2}, {0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},
    {0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2}, {0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2}, {0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
    {0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1}, {0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2}, {0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},
    {0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0},
};

// The pixel whose index drops its top bit, for subset 1 of the two-subset
// partitions, and subsets 1 and 2 of the three-subset ones (subset 0's is pixel 0).
constexpr std::uint8_t kAnchor2[64] = {
    15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15, 15, 2, 8, 2, 2, 8, 8,15,  2, 8, 2, 2, 8, 8, 2, 2,
    15,15, 6, 8, 2, 8,15,15,  2, 8, 2, 2, 2,15,15, 6,  6, 2, 6, 8,15,15, 2, 2, 15,15,15,15,15, 2, 2,15,
};
constexpr std::uint8_t kAnchor3a[64] = {
     3, 3,15,15, 8, 3,15,15,  8, 8, 6, 6, 6, 5, 3, 3,  3, 3, 8,15, 3, 3, 6,10,  5, 8, 8, 6, 8, 5,15,15,
     8,15, 3, 5, 6,10, 8,15, 15, 3,15, 5,15,15,15,15,  3,15, 5, 5, 5, 8, 5,10,  5,10, 8,13,15,12, 3, 3,
};
constexpr std::uint8_t kAnchor3b[64] = {
    15, 8, 8, 3,15,15, 3, 8, 15,15,15,15,15,15,15, 8, 15, 8,15, 3,15, 8,15, 8,  3,15, 6,10,15,15,10, 8,
    15, 3,15,10,10, 8, 9,10,  6,15, 8,15, 3, 6, 6, 8, 15, 3,15,15,15,15,15,15, 15,15,15,15, 3,15,15, 8,
};

constexpr std::uint8_t kWeight2[4] = {0, 21, 43, 64};
constexpr std::uint8_t kWeight3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::uint8_t kWeight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const std::uint8_t* weights(int bits) { return bits == 2 ? kWeight2 : bits == 3 ? kWeight3 : kWeight4; }

struct Bits {
    std::uint64_t lo, hi;
    int at = 0;
    std::uint32_t take(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i, ++at) {
            const std::uint64_t word = at < 64 ? lo : hi;
            v |= static_cast<std::uint32_t>(word >> (at & 63) & 1u) << i;
        }
        return v;
    }
};

struct Mode {
    int subsets, partition_bits, rotation_bits, selector_bits, colour_bits, alpha_bits, endpoint_pbits, shared_pbits,
        index_bits, index2_bits;
};
constexpr Mode kModes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

void bc7_block(const std::uint8_t* b, std::uint8_t* out) {
    int mode = 0;
    while (mode < 8 && !(b[0] >> mode & 1u)) ++mode;
    if (mode == 8) {  // reserved: black (opaque, as Apple GPUs return it)
        for (int i = 0; i < 16; ++i) out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = 0, out[4 * i + 3] = 255;
        return;
    }
    Bits bits{};
    std::memcpy(&bits.lo, b, 8);
    std::memcpy(&bits.hi, b + 8, 8);
    bits.at = mode + 1;
    const Mode& m = kModes[mode];
    const std::uint32_t partition = bits.take(m.partition_bits);
    const std::uint32_t rotation = bits.take(m.rotation_bits);
    const std::uint32_t selector = bits.take(m.selector_bits);
    const int endpoints = 2 * m.subsets;
    std::uint32_t ep[6][4] = {};  // endpoint, channel
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < endpoints; ++e) ep[e][c] = bits.take(m.colour_bits);
    for (int e = 0; e < endpoints; ++e) ep[e][3] = m.alpha_bits ? bits.take(m.alpha_bits) : 255u;
    int cbits = m.colour_bits, abits = m.alpha_bits;
    if (m.endpoint_pbits || m.shared_pbits) {
        std::uint32_t p[6];
        if (m.endpoint_pbits)
            for (int e = 0; e < endpoints; ++e) p[e] = bits.take(1);
        else
            for (int s = 0; s < m.subsets; ++s) p[2 * s] = p[2 * s + 1] = bits.take(1);
        for (int e = 0; e < endpoints; ++e)
            for (int c = 0; c < 4; ++c)
                if (c < 3 || abits) ep[e][c] = ep[e][c] << 1 | p[e];
        ++cbits;
        if (abits) ++abits;
    }
    for (int e = 0; e < endpoints; ++e)
        for (int c = 0; c < 4; ++c) {
            const int n = c < 3 ? cbits : abits;
            if (n == 0) continue;
            ep[e][c] <<= 8 - n;
            ep[e][c] |= ep[e][c] >> n;
        }
    const std::uint8_t* subset_of =
        m.subsets == 2 ? kPart2[partition] : m.subsets == 3 ? kPart3[partition] : nullptr;
    auto anchor = [&](int px) {
        if (px == 0) return true;
        if (m.subsets == 2) return px == kAnchor2[partition];
        if (m.subsets == 3) return px == kAnchor3a[partition] || px == kAnchor3b[partition];
        return false;
    };
    std::uint32_t index[16], index2[16];
    for (int px = 0; px < 16; ++px) index[px] = bits.take(m.index_bits - (anchor(px) ? 1 : 0));
    if (m.index2_bits)
        for (int px = 0; px < 16; ++px) index2[px] = bits.take(m.index2_bits - (px == 0 ? 1 : 0));
    for (int px = 0; px < 16; ++px) {
        const int s = subset_of ? subset_of[px] : 0;
        const std::uint32_t* e0 = ep[2 * s];
        const std::uint32_t* e1 = ep[2 * s + 1];
        std::uint8_t* o = out + 4 * px;
        if (m.index2_bits) {
            // Mode 4 and 5: colour and alpha take separate indices; mode 4's
            // selector bit swaps which one is the 3-bit set.
            const bool swap = selector != 0;
            const int colour_n = swap ? m.index2_bits : m.index_bits;
            const int alpha_n = swap ? m.index_bits : m.index2_bits;
            const std::uint32_t ci = swap ? index2[px] : index[px];
            const std::uint32_t ai = swap ? index[px] : index2[px];
            for (int c = 0; c < 3; ++c) o[c] = lerp(e0[c], e1[c], weights(colour_n)[ci]);
            o[3] = lerp(e0[3], e1[3], weights(alpha_n)[ai]);
        } else {
            const std::uint8_t w = weights(m.index_bits)[index[px]];
            for (int c = 0; c < 4; ++c) o[c] = lerp(e0[c], e1[c], w);
        }
        if (rotation) std::swap(o[3], o[rotation - 1]);
    }
}


// ---- BC6H --------------------------------------------------------------------

// Endpoint fields: w x (subset 0), y z (subset 1); d is the partition.
enum Field : std::uint8_t { RW, GW, BW, RX, GX, BX, RY, GY, BY, RZ, GZ, BZ, D };

// One run of header bits: they go to `field`, from bit `first` to bit `last`
// (downward when last < first, as modes 13 and 14 store their top bits).
struct Seg {
    std::uint8_t field, first, last;
};

struct Bc6Mode {
    bool transformed;
    int subsets, endpoint_bits, delta_bits[3];
    Seg segs[32];
};

#define W10 {RW, 0, 9}, {GW, 0, 9}, {BW, 0, 9}
constexpr Bc6Mode kBc6Modes[14] = {
    {true, 2, 10, {5, 5, 5}, {{GY, 4, 4}, {BY, 4, 4}, {BZ, 4, 4}, W10, {RX, 0, 4}, {GZ, 4, 4}, {GY, 0, 3}, {GX, 0, 4},
                              {BZ, 0, 0}, {GZ, 0, 3}, {BX, 0, 4}, {BZ, 1, 1}, {BY, 0, 3}, {RY, 0, 4}, {BZ, 2, 2},
                              {RZ, 0, 4}, {BZ, 3, 3}, {D, 0, 4}}},
    {true, 2, 7, {6, 6, 6}, {{GY, 5, 5}, {GZ, 4, 4}, {GZ, 5, 5}, {RW, 0, 6}, {BZ, 0, 0}, {BZ, 1, 1}, {BY, 4, 4},
                             {GW, 0, 6}, {BY, 5, 5}, {BZ, 2, 2}, {GY, 4, 4}, {BW, 0, 6}, {BZ, 3, 3}, {BZ, 5, 5},
                             {BZ, 4, 4}, {RX, 0, 5}, {GY, 0, 3}, {GX, 0, 5}, {GZ, 0, 3}, {BX, 0, 5}, {BY, 0, 3},
                             {RY, 0, 5}, {RZ, 0, 5}, {D, 0, 4}}},
    {true, 2, 11, {5, 4, 4}, {W10, {RX, 0, 4}, {RW, 10, 10}, {GY, 0, 3}, {GX, 0, 3}, {GW, 10, 10}, {BZ, 0, 0},
                              {GZ, 0, 3}, {BX, 0, 3}, {BW, 10, 10}, {BZ, 1, 1}, {BY, 0, 3}, {RY, 0, 4}, {BZ, 2, 2},
                              {RZ, 0, 4}, {BZ, 3, 3}, {D, 0, 4}}},
    {true, 2, 11, {4, 5, 4}, {W10, {RX, 0, 3}, {RW, 10, 10}, {GZ, 4, 4}, {GY, 0, 3}, {GX, 0, 4}, {GW, 10, 10},
                              {GZ, 0, 3}, {BX, 0, 3}, {BW, 10, 10}, {BZ, 1, 1}, {BY, 0, 3}, {RY, 0, 3}, {BZ, 0, 0},
                              {BZ, 2, 2}, {RZ, 0, 3}, {GY, 4, 4}, {BZ, 3, 3}, {D, 0, 4}}},
    {true, 2, 11, {4, 4, 5}, {W10, {RX, 0, 3}, {RW, 10, 10}, {BY, 4, 4}, {GY, 0, 3}, {GX, 0, 3}, {GW, 10, 10},
                              {BZ, 0, 0}, {GZ, 0, 3}, {BX, 0, 4}, {BW, 10, 10}, {BY, 0, 3}, {RY, 0, 3}, {BZ, 1, 1},
                              {BZ, 2, 2}, {RZ, 0, 3}, {BZ, 4, 4}, {BZ, 3, 3}, {D, 0, 4}}},
    {true, 2, 9, {5, 5, 5}, {{RW, 0, 8}, {BY, 4, 4}, {GW, 0, 8}, {GY, 4, 4}, {BW, 0, 8}, {BZ, 4, 4}, {RX, 0, 4},
                             {GZ, 4, 4}, {GY, 0, 3}, {GX, 0, 4}, {BZ, 0, 0}, {GZ, 0, 3}, {BX, 0, 4}, {BZ, 1, 1},
                             {BY, 0, 3}, {RY, 0, 4}, {BZ, 2, 2}, {RZ, 0, 4}, {BZ, 3, 3}, {D, 0, 4}}},
    {true, 2, 8, {6, 5, 5}, {{RW, 0, 7}, {GZ, 4, 4}, {BY, 4, 4}, {GW, 0, 7}, {BZ, 2, 2}, {GY, 4, 4}, {BW, 0, 7},
                             {BZ, 3, 3}, {BZ, 4, 4}, {RX, 0, 5}, {GY, 0, 3}, {GX, 0, 4}, {BZ, 0, 0}, {GZ, 0, 3},
                             {BX, 0, 4}, {BZ, 1, 1}, {BY, 0, 3}, {RY, 0, 5}, {RZ, 0, 5}, {D, 0, 4}}},
    {true, 2, 8, {5, 6, 5}, {{RW, 0, 7}, {BZ, 0, 0}, {BY, 4, 4}, {GW, 0, 7}, {GY, 5, 5}, {GY, 4, 4}, {BW, 0, 7},
                             {GZ, 5, 5}, {BZ, 4, 4}, {RX, 0, 4}, {GZ, 4, 4}, {GY, 0, 3}, {GX, 0, 5}, {GZ, 0, 3},
                             {BX, 0, 4}, {BZ, 1, 1}, {BY, 0, 3}, {RY, 0, 4}, {BZ, 2, 2}, {RZ, 0, 4}, {BZ, 3, 3},
                             {D, 0, 4}}},
    {true, 2, 8, {5, 5, 6}, {{RW, 0, 7}, {BZ, 1, 1}, {BY, 4, 4}, {GW, 0, 7}, {BY, 5, 5}, {GY, 4, 4}, {BW, 0, 7},
                             {BZ, 5, 5}, {BZ, 4, 4}, {RX, 0, 4}, {GZ, 4, 4}, {GY, 0, 3}, {GX, 0, 4}, {BZ, 0, 0},
                             {GZ, 0, 3}, {BX, 0, 5}, {BY, 0, 3}, {RY, 0, 4}, {BZ, 2, 2}, {RZ, 0, 4}, {BZ, 3, 3},
                             {D, 0, 4}}},
    {false, 2, 6, {6, 6, 6}, {{RW, 0, 5}, {GZ, 4, 4}, {BZ, 0, 0}, {BZ, 1, 1}, {BY, 4, 4}, {GW, 0, 5}, {GY, 5, 5},
                              {BY, 5, 5}, {BZ, 2, 2}, {GY, 4, 4}, {BW, 0, 5}, {GZ, 5, 5}, {BZ, 3, 3}, {BZ, 5, 5},
                              {BZ, 4, 4}, {RX, 0, 5}, {GY, 0, 3}, {GX, 0, 5}, {GZ, 0, 3}, {BX, 0, 5}, {BY, 0, 3},
                              {RY, 0, 5}, {RZ, 0, 5}, {D, 0, 4}}},
    {false, 1, 10, {10, 10, 10}, {W10, {RX, 0, 9}, {GX, 0, 9}, {BX, 0, 9}}},
    {true, 1, 11, {9, 9, 9}, {W10, {RX, 0, 8}, {RW, 10, 10}, {GX, 0, 8}, {GW, 10, 10}, {BX, 0, 8}, {BW, 10, 10}}},
    {true, 1, 12, {8, 8, 8}, {W10, {RX, 0, 7}, {RW, 11, 10}, {GX, 0, 7}, {GW, 11, 10}, {BX, 0, 7}, {BW, 11, 10}}},
    {true, 1, 16, {4, 4, 4}, {W10, {RX, 0, 3}, {RW, 15, 10}, {GX, 0, 3}, {GW, 15, 10}, {BX, 0, 3}, {BW, 15, 10}}},
};
#undef W10

int sign_extend(std::uint32_t v, int bits) {
    const std::uint32_t m = 1u << (bits - 1);
    return static_cast<int>((v ^ m) - m);
}

int unquantize(int v, int bits, bool sign) {
    if (!sign) {
        if (bits >= 15 || v == 0) return v;
        if (v == (1 << bits) - 1) return 0xFFFF;
        return ((v << 16) + 0x8000) >> bits;
    }
    if (bits >= 16) return v;
    const bool neg = v < 0;
    int a = neg ? -v : v;
    if (a == 0) return 0;
    if (a >= (1 << (bits - 1)) - 1)
        a = 0x7FFF;
    else
        a = ((a << 15) + 0x4000) >> (bits - 1);
    return neg ? -a : a;
}

std::uint16_t finish(int v, bool sign) {
    if (!sign) return static_cast<std::uint16_t>((v * 31) >> 6);
    const bool neg = v < 0;
    const int a = ((neg ? -v : v) * 31) >> 5;
    return static_cast<std::uint16_t>(a | (neg ? 0x8000 : 0));
}

// Out: RGBA16Float, alpha 1.
void bc6_block(const std::uint8_t* b, std::uint16_t* out, bool sign) {
    Bits bits{};
    std::memcpy(&bits.lo, b, 8);
    std::memcpy(&bits.hi, b + 8, 8);
    std::uint32_t m = bits.take(2);
    int mode;
    if (m < 2) {
        mode = static_cast<int>(m);
    } else {
        m |= bits.take(3) << 2;
        switch (m) {
        case 0x02: mode = 2; break;
        case 0x06: mode = 3; break;
        case 0x0A: mode = 4; break;
        case 0x0E: mode = 5; break;
        case 0x12: mode = 6; break;
        case 0x16: mode = 7; break;
        case 0x1A: mode = 8; break;
        case 0x1E: mode = 9; break;
        case 0x03: mode = 10; break;
        case 0x07: mode = 11; break;
        case 0x0B: mode = 12; break;
        case 0x0F: mode = 13; break;
        default: mode = -1; break;
        }
    }
    if (mode < 0) {  // reserved
        for (int i = 0; i < 16; ++i) out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = 0, out[4 * i + 3] = 0x3C00;
        return;
    }
    const Bc6Mode& md = kBc6Modes[mode];
    std::uint32_t f[13] = {};
    for (const Seg& sg : md.segs) {
        if (sg.first == 0 && sg.last == 0 && sg.field == RW) break;  // end of the list
        const int step = sg.last >= sg.first ? 1 : -1;
        for (int bit = sg.first;; bit += step) {
            f[sg.field] |= bits.take(1) << bit;
            if (bit == sg.last) break;
        }
    }
    const int eb = md.endpoint_bits;
    int ep[4][3];  // w x y z, channel
    for (int c = 0; c < 3; ++c) {
        const std::uint32_t raw[4] = {f[RW + c], f[RX + c], f[RY + c], f[RZ + c]};
        ep[0][c] = sign ? sign_extend(raw[0], eb) : static_cast<int>(raw[0]);
        for (int e = 1; e < 2 * md.subsets; ++e) {
            if (md.transformed) {
                const int delta = sign_extend(raw[e], md.delta_bits[c]);
                const std::uint32_t v = static_cast<std::uint32_t>(static_cast<int>(raw[0]) + delta) & ((1u << eb) - 1u);
                ep[e][c] = sign ? sign_extend(v, eb) : static_cast<int>(v);
            } else {
                ep[e][c] = sign ? sign_extend(raw[e], eb) : static_cast<int>(raw[e]);
            }
        }
        for (int e = 0; e < 2 * md.subsets; ++e) ep[e][c] = unquantize(ep[e][c], eb, sign);
    }
    const std::uint32_t partition = f[D];
    const int ib = md.subsets == 2 ? 3 : 4;
    const std::uint8_t* w = ib == 3 ? kWeight3 : kWeight4;
    for (int px = 0; px < 16; ++px) {
        const bool anchor = px == 0 || (md.subsets == 2 && px == kAnchor2[partition]);
        const std::uint32_t idx = bits.take(ib - (anchor ? 1 : 0));
        const int s = md.subsets == 2 ? kPart2[partition][px] : 0;
        for (int c = 0; c < 3; ++c) {
            const int v = ((64 - w[idx]) * ep[2 * s][c] + w[idx] * ep[2 * s + 1][c] + 32) >> 6;
            out[4 * px + c] = finish(v, sign);
        }
        out[4 * px + 3] = 0x3C00;
    }
}

}  // namespace

std::uint32_t bc_decoded_bytes(std::uint32_t dxgi) {
    switch (kind(dxgi)) {
    case Kind::BC4: return 1;
    case Kind::BC5: return 2;
    case Kind::BC6U: case Kind::BC6S: return 8;
    case Kind::None: return 0;
    default: return 4;
    }
}

void bc_decode_block(std::uint32_t dxgi, const std::uint8_t* b, std::uint8_t* out) {
    switch (kind(dxgi)) {
    case Kind::BC1: colour_block(b, out, true); break;
    case Kind::BC2:
        colour_block(b + 8, out, false);
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t a = b[i / 2] >> (4 * (i & 1)) & 15u;
            out[4 * i + 3] = static_cast<std::uint8_t>(a << 4 | a);
        }
        break;
    case Kind::BC3:
        colour_block(b + 8, out, false);
        single_block(b, out + 3, 4);
        break;
    case Kind::BC4: single_block(b, out, 1); break;
    case Kind::BC5:
        single_block(b, out, 2);
        single_block(b + 8, out + 1, 2);
        break;
    case Kind::BC6U: case Kind::BC6S: {
        std::uint16_t px[64];
        bc6_block(b, px, kind(dxgi) == Kind::BC6S);
        std::memcpy(out, px, sizeof px);
        break;
    }
    case Kind::BC7: bc7_block(b, out); break;
    case Kind::None: break;
    }
}

bool bc_decode(std::uint32_t dxgi, const std::uint8_t* src, std::size_t size, std::uint32_t row_pitch, std::uint32_t width,
               std::uint32_t height, std::uint8_t* dst, std::uint32_t dst_pitch) {
    const Kind k = kind(dxgi);
    if (k == Kind::None) return false;
    const std::uint32_t bpp = bc_decoded_bytes(dxgi), bb = block_bytes(k);
    const std::uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (row_pitch < bw * bb || static_cast<std::size_t>(row_pitch) * (bh - 1) + bw * bb > size) return false;
    std::uint8_t px[16 * 8];
    for (std::uint32_t by = 0; by < bh; ++by)
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            bc_decode_block(dxgi, src + static_cast<std::size_t>(by) * row_pitch + bx * bb, px);
            const std::uint32_t w = std::min(4u, width - bx * 4), h = std::min(4u, height - by * 4);
            for (std::uint32_t y = 0; y < h; ++y)
                std::memcpy(dst + static_cast<std::size_t>(by * 4 + y) * dst_pitch + bx * 4 * bpp, px + y * 4 * bpp, w * bpp);
        }
    return true;
}

}  // namespace dcusa::gpu

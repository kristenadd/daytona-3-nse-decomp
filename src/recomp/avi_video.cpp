#include "recomp/avi_video.hpp"

#include <cstdlib>
#include <cstring>

namespace dcusa::recomp {
namespace {

constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return static_cast<std::uint32_t>(s[0]) | static_cast<std::uint32_t>(s[1]) << 8 |
           static_cast<std::uint32_t>(s[2]) << 16 | static_cast<std::uint32_t>(s[3]) << 24;
}

std::uint32_t le32(const std::uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

}  // namespace

bool AviVideo::open(Reader read, std::uint64_t size) {
    read_ = std::move(read);
    chunks_.clear();
    decoded_ = -1;
    std::uint8_t h[12];
    if (size < 12 || !read_(0, 12, h) || le32(h) != fourcc("RIFF") || le32(h + 8) != fourcc("AVI ")) return false;
    bool video = false;
    std::uint32_t bits = 0, compression = 0;
    std::uint64_t movi = 0, movi_end = 0, idx1 = 0;
    std::uint32_t idx1_size = 0;
    // Walk the top-level and LIST chunks we need: hdrl/strl (strh, strf), movi, idx1.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans{{12, std::min<std::uint64_t>(size, 8ull + le32(h + 4))}};
    int stream = -1;
    while (!spans.empty()) {
        auto [at, end] = spans.back();
        spans.pop_back();
        while (at + 8 <= end) {
            std::uint8_t c[12];
            if (!read_(at, 8, c)) return false;
            const std::uint32_t id = le32(c), len = le32(c + 4);
            const std::uint64_t data = at + 8, next = data + len + (len & 1u);
            if (id == fourcc("LIST") && len >= 4) {
                if (!read_(data, 4, c + 8)) return false;
                const std::uint32_t type = le32(c + 8);
                if (type == fourcc("movi")) {
                    movi = data;  // idx1 offsets count from the 'movi' fourcc
                    movi_end = std::min(next, end);
                } else if (type == fourcc("hdrl") || type == fourcc("strl")) {
                    if (type == fourcc("strl")) ++stream;
                    spans.emplace_back(next, end);
                    at = data + 4;
                    end = std::min<std::uint64_t>(data + len, end);
                    continue;
                }
            } else if (id == fourcc("strh") && len >= 56 && stream == 0) {
                std::uint8_t s[56];
                if (!read_(data, 56, s)) return false;
                video = le32(s) == fourcc("vids");
                handler_ = le32(s + 4);
                scale_ = le32(s + 20) ? le32(s + 20) : 1;
                rate_ = le32(s + 24);
            } else if (id == fourcc("strf") && len >= 40 && stream == 0) {
                std::uint8_t b[40];
                if (!read_(data, 40, b)) return false;
                width_ = le32(b + 4);
                height_ = static_cast<std::uint32_t>(std::abs(static_cast<std::int32_t>(le32(b + 8))));
                bits = le16(b + 14);
                compression = le32(b + 16);
            } else if (id == fourcc("idx1")) {
                idx1 = data;
                idx1_size = len;
            }
            at = next;
        }
    }
    const bool video1 = compression == fourcc("CRAM") || compression == fourcc("cram") || compression == fourcc("MSVC") ||
                        compression == fourcc("msvc") || compression == fourcc("WHAM") || compression == fourcc("wham");
    if (!video || !video1 || bits != 16 || width_ == 0 || height_ == 0 || width_ % 4 || height_ % 4 || movi == 0) return false;
    // The frames: '00dc' / '00db' chunks of the movi list, in order. idx1 gives
    // the key frames; without it every frame is decoded from the first.
    std::vector<bool> keys;
    if (idx1 != 0) {
        std::vector<std::uint8_t> ix(idx1_size);
        if (!read_(idx1, idx1_size, ix.data())) return false;
        for (std::uint32_t i = 0; i + 16 <= idx1_size; i += 16) {
            const std::uint32_t id = le32(&ix[i]);
            if ((id & 0xFFFFu) != (fourcc("00dc") & 0xFFFFu)) continue;  // stream 00
            const std::uint32_t kind = id >> 16;
            if (kind != (fourcc("00dc") >> 16) && kind != (fourcc("00db") >> 16)) continue;
            keys.push_back((le32(&ix[i + 4]) & 0x10u) != 0);
        }
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> lists{{movi + 4, movi_end}};
    while (!lists.empty()) {
        auto [at, end] = lists.front();
        lists.erase(lists.begin());
        while (at + 8 <= end) {
            std::uint8_t c[12];
            if (!read_(at, 8, c)) return false;
            const std::uint32_t id = le32(c), len = le32(c + 4);
            const std::uint64_t data = at + 8, next = data + len + (len & 1u);
            if (id == fourcc("LIST")) {  // 'rec ' groups
                lists.insert(lists.begin(), {data + 4, std::min(next, end)});
                at = next;
                continue;
            }
            if (id == fourcc("00dc") || id == fourcc("00db")) {
                Chunk ch;
                ch.offset = data;
                ch.size = len;
                ch.key = chunks_.size() < keys.size() ? keys[chunks_.size()] : chunks_.empty();
                chunks_.push_back(ch);
            }
            at = next;
        }
    }
    if (chunks_.empty()) return false;
    chunks_[0].key = true;
    image_.assign(static_cast<std::size_t>(width_) * height_, 0);
    return true;
}

// Microsoft Video 1, 16-bit: 4x4 blocks, bottom block row first. Each block is
// skipped (kept from the last frame), one colour, two colours with a 16-bit
// mask, or eight colours (two per 2x2 quadrant).
bool AviVideo::decode(const std::vector<std::uint8_t>& data) {
    const std::uint32_t bw = width_ / 4, bh = height_ / 4;
    std::size_t at = 0;
    std::uint32_t skip = 0;
    std::uint32_t left = bw * bh;
    auto byte = [&]() -> int { return at < data.size() ? data[at++] : -1; };
    for (std::uint32_t by = bh; by > 0; --by) {
        for (std::uint32_t bx = 0; bx < bw; ++bx) {
            if (skip) {
                --skip;
                continue;
            }
            // Row (by*4 - 1) is the block's bottom row; rows go upward.
            std::uint16_t* block = &image_[static_cast<std::size_t>(by * 4 - 1) * width_ + bx * 4];
            const int a = byte(), b = byte();
            if (a < 0 || b < 0) return left == 0 || at >= data.size();
            if (a == 0 && b == 0 && left == 0) return true;
            if ((b & 0xFC) == 0x84) {
                skip = static_cast<std::uint32_t>(((b - 0x84) << 8) + a) - 1;
            } else if (b < 0x80) {
                std::uint32_t flags = static_cast<std::uint32_t>(b << 8 | a);
                if (at + 4 > data.size()) return false;
                std::uint16_t col[8];
                col[0] = le16(&data[at]);
                col[1] = le16(&data[at + 2]);
                at += 4;
                if (col[0] & 0x8000u) {
                    if (at + 12 > data.size()) return false;
                    for (int k = 2; k < 8; ++k, at += 2) col[k] = le16(&data[at]);
                    for (int y = 0; y < 4; ++y) {
                        std::uint16_t* row = block - static_cast<std::ptrdiff_t>(y) * width_;
                        for (int x = 0; x < 4; ++x, flags >>= 1)
                            row[x] = col[((y & 2) << 1) + (x & 2) + ((flags & 1u) ^ 1u)] & 0x7FFFu;
                    }
                } else {
                    for (int y = 0; y < 4; ++y) {
                        std::uint16_t* row = block - static_cast<std::ptrdiff_t>(y) * width_;
                        for (int x = 0; x < 4; ++x, flags >>= 1) row[x] = col[(flags & 1u) ^ 1u];
                    }
                }
            } else {
                const std::uint16_t colour = static_cast<std::uint16_t>((b << 8 | a) & 0x7FFF);
                for (int y = 0; y < 4; ++y) {
                    std::uint16_t* row = block - static_cast<std::ptrdiff_t>(y) * width_;
                    for (int x = 0; x < 4; ++x) row[x] = colour;
                }
            }
            --left;
        }
    }
    return true;
}

const std::vector<std::uint8_t>& AviVideo::frame_bgr24(std::uint32_t index) {
    static const std::vector<std::uint8_t> none;
    if (index >= chunks_.size()) return none;
    if (decoded_ != static_cast<std::int64_t>(index)) {
        // From the last key frame at or before `index`, unless the frame held is on the way.
        std::uint32_t start = index;
        while (start > 0 && !chunks_[start].key) --start;
        if (decoded_ >= static_cast<std::int64_t>(start) && decoded_ < static_cast<std::int64_t>(index))
            start = static_cast<std::uint32_t>(decoded_ + 1);
        std::vector<std::uint8_t> data;
        for (std::uint32_t f = start; f <= index; ++f) {
            const Chunk& ch = chunks_[f];
            if (ch.size == 0) continue;  // a dropped frame repeats the last one
            data.resize(ch.size);
            if (!read_(ch.offset, ch.size, data.data()) || !decode(data)) {
                decoded_ = -1;
                return none;
            }
        }
        decoded_ = index;
        out_.assign(static_cast<std::size_t>(row_bytes()) * height_, 0);
        for (std::uint32_t y = 0; y < height_; ++y) {
            const std::uint16_t* src = &image_[static_cast<std::size_t>(height_ - 1 - y) * width_];  // DIB: bottom row first
            std::uint8_t* dst = &out_[static_cast<std::size_t>(y) * row_bytes()];
            for (std::uint32_t x = 0; x < width_; ++x) {
                const std::uint32_t p = src[x];
                const std::uint32_t r = (p >> 10) & 31u, g = (p >> 5) & 31u, bl = p & 31u;
                dst[3 * x + 0] = static_cast<std::uint8_t>(bl << 3 | bl >> 2);
                dst[3 * x + 1] = static_cast<std::uint8_t>(g << 3 | g >> 2);
                dst[3 * x + 2] = static_cast<std::uint8_t>(r << 3 | r >> 2);
            }
        }
    }
    return out_;
}

}  // namespace dcusa::recomp

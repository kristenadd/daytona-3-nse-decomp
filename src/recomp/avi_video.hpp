#pragma once
// AVI files with Microsoft Video 1 (CRAM, 16-bit RGB555) video, as the two in
// rom\movie are: the name-entry letter strip and the attract camera stand-in.
// OpenCV reads them through Video for Windows (win32.cpp AVIFile* / AVIStream*),
// which hands out decoded frames as 24-bit bottom-up DIBs.
#include <cstdint>
#include <functional>
#include <vector>

namespace dcusa::recomp {

class AviVideo {
public:
    // Reads `n` bytes at `offset` into `dst`; false on a short read.
    using Reader = std::function<bool(std::uint64_t offset, std::uint32_t n, std::uint8_t* dst)>;

    // Parses the headers and the frame index. False when the file is not an
    // AVI with one Microsoft Video 1 16-bit video stream.
    bool open(Reader read, std::uint64_t size);

    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }
    std::uint32_t frames() const { return static_cast<std::uint32_t>(chunks_.size()); }
    std::uint32_t rate() const { return rate_; }
    std::uint32_t scale() const { return scale_; }
    std::uint32_t handler() const { return handler_; }

    // Frame `index` as 24-bit BGR rows, bottom row first, each row padded to a
    // multiple of 4 bytes (a DIB's layout). Empty when out of range or corrupt.
    const std::vector<std::uint8_t>& frame_bgr24(std::uint32_t index);
    std::uint32_t row_bytes() const { return (width_ * 3u + 3u) & ~3u; }

private:
    struct Chunk {
        std::uint64_t offset = 0;  // of the data
        std::uint32_t size = 0;
        bool key = false;
    };
    bool decode(const std::vector<std::uint8_t>& data);

    Reader read_;
    std::uint32_t width_ = 0, height_ = 0, rate_ = 0, scale_ = 1, handler_ = 0;
    std::vector<Chunk> chunks_;
    std::vector<std::uint16_t> image_;  // the decoded frame, top row first (Video 1 frames build on the last)
    std::int64_t decoded_ = -1;         // which frame image_ holds
    std::vector<std::uint8_t> out_;
};

}  // namespace dcusa::recomp

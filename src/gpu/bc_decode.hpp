#pragma once
// Block-compressed textures (BC1-BC5, BC7) decoded to plain pixels, for GPUs
// without BC support: the iOS Simulator and iPhones before the A-series
// chips that added it (MTLDevice.supportsBCTextureCompression). The decoded
// pixels are what the GPU's own BC sampling returns, bit for bit
// (tests/test_bc_decode.mm checks this against the Mac's GPU).
#include <cstddef>
#include <cstdint>

namespace dcusa::gpu {

// What a BC format decodes to: RGBA8 (BC1-3, BC7; the sRGB variants too, the
// bytes are the same), R8 (BC4) or RG8 (BC5). Bytes per pixel, or 0 when the
// format is not one this decodes (BC6H, the signed BC4/BC5, anything else).
std::uint32_t bc_decoded_bytes(std::uint32_t dxgi);

// Decodes a `width` x `height` image whose 4x4 blocks are `row_pitch` bytes
// per row of blocks into `dst`, `dst_pitch` bytes per row. False when the
// format is not decoded or `size` is too small.
bool bc_decode(std::uint32_t dxgi, const std::uint8_t* src, std::size_t size, std::uint32_t row_pitch, std::uint32_t width,
               std::uint32_t height, std::uint8_t* dst, std::uint32_t dst_pitch);

// One block to 16 pixels of bc_decoded_bytes(dxgi) bytes each, row by row.
void bc_decode_block(std::uint32_t dxgi, const std::uint8_t* block, std::uint8_t* out);

}  // namespace dcusa::gpu

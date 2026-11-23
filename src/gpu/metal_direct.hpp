#pragma once
// The Metal device behind gpu:: (metal.hpp), called directly. Only the render
// thread in gpu_queue.mm calls these, or the guest's thread while the render
// thread is idle; metal.hpp documents each one.
#include "gpu/metal.hpp"

namespace dcusa::gpu::direct {

void create_texture(std::uint32_t id, const TextureDesc& desc);
void upload_texture(std::uint32_t id, const std::uint8_t* bytes, std::size_t size, std::uint32_t row_pitch);
void upload_texture_sub(std::uint32_t id, std::uint32_t slice, std::uint32_t mip, const std::uint8_t* bytes, std::size_t size,
                        std::uint32_t row_pitch);
void generate_mips(std::uint32_t id);
void upload_buffer(std::uint32_t id, const std::uint8_t* bytes, std::size_t size);
void create_sampler(std::uint32_t id, const SamplerDesc& desc);
void destroy(std::uint32_t id);
void clear_target(std::uint32_t texture, std::uint32_t mip, std::uint32_t slice, const float rgba[4]);
void clear_depth(std::uint32_t texture, float depth, std::uint8_t stencil, std::uint32_t flags);
void draw_indexed(const DrawState& s, std::uint32_t count, std::uint32_t start, std::int32_t base);
void draw(const DrawState& s, std::uint32_t count, std::uint32_t start);
void submit();
void set_layer(void* ca_metal_layer);
void show(std::uint32_t texture);
bool read_rgba8(std::uint32_t texture, std::vector<std::uint8_t>* out, std::uint32_t* width, std::uint32_t* height);
void scan_targets();
void set_trace(bool on);
const Stats& stats();

}  // namespace dcusa::gpu::direct

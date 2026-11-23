#pragma once
// The offscreen Metal device behind the guest's D3D11 calls.
//
// Everything is keyed by the guest address of the D3D11 object (texture,
// buffer, sampler). src/recomp/d3d11.cpp keeps the CPU copies and calls in here
// whenever the guest writes data (a create with initial data, UpdateSubresource,
// Unmap) and for every clear and draw. A write after a draw that used the old
// contents gets a new Metal object, so draws already encoded keep what they
// saw: the D3D11 behaviour for constant buffers rewritten between draws.
//
// Shaders are the translated MSL in $DCUSA_SHADERS/msl (tools/shaderprep),
// looked up by compile request key. Nothing is drawn into a window: the back
// buffer is read back at Present for the frame image (PHASE3 item 4).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dcusa::gpu {

// DCUSA_METAL=1, a Metal device, and a shader folder. Checked once.
bool enabled();

struct TextureDesc {
    std::uint32_t width = 0, height = 0, mips = 1, array = 1;
    std::uint32_t format = 0;  // DXGI_FORMAT
    std::uint32_t bind = 0;    // D3D11_BIND_*: 0x8 shader resource, 0x20 render target, 0x40 depth stencil
    std::uint32_t misc = 0;    // D3D11_RESOURCE_MISC_*: 0x1 generate mips
};
void create_texture(std::uint32_t id, const TextureDesc& desc);
// Mip 0 of slice 0, rows `row_pitch` bytes apart (block rows for BCn).
void upload_texture(std::uint32_t id, const std::uint8_t* bytes, std::size_t size, std::uint32_t row_pitch);
void generate_mips(std::uint32_t id);

void upload_buffer(std::uint32_t id, const std::uint8_t* bytes, std::size_t size);

// D3D11_SAMPLER_DESC, as the guest passed it.
struct SamplerDesc {
    std::uint32_t filter = 0, address[3] = {1, 1, 1};
    float mip_bias = 0;
    std::uint32_t max_aniso = 1, compare = 0;
    float border[4] = {};
    float min_lod = 0, max_lod = 3.402823466e+38f;
};
void create_sampler(std::uint32_t id, const SamplerDesc& desc);

struct BlendTarget {
    bool enable = false;
    std::uint32_t src = 2, dst = 1, op = 1, src_alpha = 2, dst_alpha = 1, op_alpha = 1;  // D3D11_BLEND / _OP
    std::uint32_t write_mask = 0xF;                                                     // D3D11 R1 G2 B4 A8
};
struct DepthDesc {
    bool enable = true, write = true;
    std::uint32_t func = 2;  // D3D11_COMPARISON_LESS
};
struct RasterDesc {
    std::uint32_t cull = 3;  // D3D11_CULL_BACK
    bool front_ccw = false, scissor = false;
};

struct VertexElement {
    std::string semantic;
    std::uint32_t index = 0, format = 0, slot = 0, offset = 0;
    bool instance = false;
    std::uint32_t step = 0;
};

constexpr unsigned kTargets = 8, kSlots = 16, kBuffers = 14, kStreams = 8;

// Everything a draw reads. Resource fields are guest object addresses (0 = none).
struct DrawState {
    std::uint64_t vs = 0, ps = 0;  // compile request keys
    std::vector<VertexElement> layout;
    std::uint32_t vb[kStreams]{}, stride[kStreams]{}, voff[kStreams]{};
    std::uint32_t ib = 0, ib_format = 0, ib_off = 0, topology = 4;
    std::uint32_t vs_cb[kBuffers]{}, ps_cb[kBuffers]{};
    std::uint32_t vs_srv[kSlots]{}, ps_srv[kSlots]{};  // the textures behind the views
    std::uint32_t vs_samp[kSlots]{}, ps_samp[kSlots]{};
    std::uint32_t rtv[kTargets]{};  // render target textures
    std::uint32_t dsv = 0;          // depth texture
    float viewport[6] = {0, 0, 1920, 1080, 0, 1};
    int scissor[4] = {0, 0, 1920, 1080};
    BlendTarget blend[kTargets];
    float blend_factor[4] = {1, 1, 1, 1};
    DepthDesc depth;
    RasterDesc raster;
};

// The guest object went away: drop its texture, buffer or sampler.
void destroy(std::uint32_t id);

void clear_target(std::uint32_t texture, const float rgba[4]);
void clear_depth(std::uint32_t texture, float depth, std::uint8_t stencil, std::uint32_t flags);
void draw_indexed(const DrawState& s, std::uint32_t count, std::uint32_t start, std::int32_t base);
void draw(const DrawState& s, std::uint32_t count, std::uint32_t start);

// Commits the frame without waiting for it (at most two frames stay queued).
void submit();

// Commits the frame, waits, and copies `texture` (an RGBA8 or BGRA8 target) out
// as RGBA8 rows. False when it has no Metal texture.
bool read_rgba8(std::uint32_t texture, std::vector<std::uint8_t>* out, std::uint32_t* width, std::uint32_t* height);

// Debug: reads back every colour render target and prints how many texels are
// non-zero (DCUSA_METAL_SCAN=<present number> in d3d11.cpp).
void scan_targets();
void set_trace(bool on);

struct Stats {
    unsigned draws = 0, skipped = 0, pipelines = 0, pipeline_failures = 0, textures = 0, uploads = 0, clears = 0;
    unsigned frames = 0;
    std::string last_problem;
};
const Stats& stats();

}  // namespace dcusa::gpu

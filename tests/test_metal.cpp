// The offscreen Metal device draws what the D3D11 state says: a vertex buffer
// through an input layout matched by semantic, a constant buffer, a texture and
// sampler, a viewport, blending, and a clear; then Present's read-back.
// Shaders are written here in the form tools/shaderprep produces (MSL with
// main0, a .refl beside it). Skips (77) where there is no Metal device.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "check.hpp"
#include "gpu/metal.hpp"

namespace fs = std::filesystem;
using namespace dcusa;

namespace {

constexpr std::uint64_t kVs = 0x1111111111111111ull, kPs = 0x2222222222222222ull;
constexpr std::uint32_t kTarget = 0x1000, kTex = 0x1100, kVb = 0x1200, kIb = 0x1300, kCb = 0x1400, kSamp = 0x1500,
                        kDepth = 0x1600;

const char* kVsSource = R"(#include <metal_stdlib>
using namespace metal;
struct main0_in { float4 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };
struct main0_out { float2 uv [[user(locn0)]]; float4 gl_Position [[position]]; };
vertex main0_out main0(main0_in in [[stage_in]]) {
    main0_out out = {};
    out.gl_Position = in.pos;
    out.uv = in.uv;
    return out;
}
)";

// Colour from the constant buffer times the texture.
const char* kPsSource = R"(#include <metal_stdlib>
using namespace metal;
struct _Global { float4 tint; };
struct main0_out { float4 color [[color(0)]]; };
struct main0_in { float2 uv [[user(locn0)]]; };
fragment main0_out main0(main0_in in [[stage_in]], constant _Global& g [[buffer(0)]],
                         texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
    main0_out out = {};
    out.color = g.tint * tex.sample(s, in.uv);
    return out;
}
)";

void write(const fs::path& p, const char* text) { std::ofstream(p) << text; }

std::uint8_t at(const std::vector<std::uint8_t>& px, std::uint32_t w, std::uint32_t x, std::uint32_t y, int ch) {
    return px[(static_cast<std::size_t>(y) * w + x) * 4 + static_cast<std::size_t>(ch)];
}

}  // namespace

int main() {
    const fs::path dir = fs::current_path() / "metal-test";
    fs::remove_all(dir);
    fs::create_directories(dir / "msl");
    write(dir / "msl" / "1111111111111111.metal", kVsSource);
    write(dir / "msl" / "1111111111111111.refl", "stage vert\ninput 0 POSITION 0\ninput 1 TEXCOORD 0\n");
    write(dir / "msl" / "2222222222222222.metal", kPsSource);
    write(dir / "msl" / "2222222222222222.refl", "stage frag\ntexture 0 4 tex\nsampler 0 0 s\ncbuffer 0 16 $Globals\n");
    setenv("DCUSA_METAL", "1", 1);
    setenv("DCUSA_SHADERS", dir.c_str(), 1);
    if (!gpu::enabled()) {
        fs::remove_all(dir);
        return SkipTest;
    }

    // 64x64 RGBA8 target; a 2x1 texture, left white and right 50% grey.
    gpu::TextureDesc target;
    target.width = target.height = 64;
    target.format = 28;
    target.bind = 0x28;
    gpu::create_texture(kTarget, target);
    gpu::TextureDesc tex;
    tex.width = 2;
    tex.height = 1;
    tex.format = 28;
    tex.bind = 0x8;
    gpu::create_texture(kTex, tex);
    const std::uint8_t texels[8] = {255, 255, 255, 255, 128, 128, 128, 255};
    gpu::upload_texture(kTex, texels, sizeof texels, 8);
    gpu::SamplerDesc point;
    point.filter = 0;  // MIN_MAG_MIP_POINT
    point.address[0] = point.address[1] = point.address[2] = 3;
    gpu::create_sampler(kSamp, point);

    // A quad over the left half of the viewport: x in [-1, 0], u in [0, 1].
    struct Vertex {
        float x, y, z, u, v;
    };
    const Vertex quad[4] = {{-1, 1, 0, 0, 0}, {0, 1, 0, 1, 0}, {-1, -1, 0, 0, 1}, {0, -1, 0, 1, 1}};
    gpu::upload_buffer(kVb, reinterpret_cast<const std::uint8_t*>(quad), sizeof quad);
    const std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
    gpu::upload_buffer(kIb, reinterpret_cast<const std::uint8_t*>(indices), sizeof indices);
    const float red[4] = {1, 0, 0, 1};
    gpu::upload_buffer(kCb, reinterpret_cast<const std::uint8_t*>(red), sizeof red);

    const float blue[4] = {0, 0, 1, 1};
    gpu::clear_target(kTarget, 0, blue);

    gpu::DrawState s;
    s.vs = kVs;
    s.ps = kPs;
    s.layout = {{"POSITION", 0, 6, 0, 0, false, 0}, {"TEXCOORD", 0, 16, 0, 12, false, 0}};
    s.vb[0] = kVb;
    s.stride[0] = sizeof(Vertex);
    s.ib = kIb;
    s.ib_format = 57;
    s.topology = 4;
    s.ps_cb[0] = kCb;
    s.ps_srv[0] = kTex;
    s.ps_samp[0] = kSamp;
    s.rtv[0] = kTarget;
    s.raster.cull = 1;
    for (float& f : s.viewport) f = 0;
    s.viewport[2] = s.viewport[3] = 64;
    s.viewport[5] = 1;
    s.depth.enable = false;
    gpu::draw_indexed(s, 6, 0, 0);

    // The constant buffer changes after the first draw; the second draw, over
    // the right half (base vertex shifts nothing, the viewport moves), sees the new
    // value and the first keeps the old one.
    const float green[4] = {0, 1, 0, 1};
    gpu::upload_buffer(kCb, reinterpret_cast<const std::uint8_t*>(green), sizeof green);
    s.viewport[0] = 32;
    s.viewport[2] = 32;
    gpu::draw_indexed(s, 6, 0, 0);

    std::vector<std::uint8_t> px;
    std::uint32_t w = 0, h = 0;
    REQUIRE(gpu::read_rgba8(kTarget, &px, &w, &h));
    CHECK(w == 64 && h == 64);
    // Left quarter: red x white texel. Second quarter: red x grey texel.
    CHECK(at(px, w, 4, 32, 0) == 255 && at(px, w, 4, 32, 1) == 0 && at(px, w, 4, 32, 2) == 0);
    CHECK(at(px, w, 20, 32, 0) >= 127 && at(px, w, 20, 32, 0) <= 129 && at(px, w, 20, 32, 2) == 0);
    // Third quarter: green (the second draw, new constants) x white.
    CHECK(at(px, w, 36, 32, 1) == 255 && at(px, w, 36, 32, 0) == 0);
    // The second viewport is [32, 64) and the quad its left half: x in
    // [48, 64) keeps the blue clear.
    CHECK(at(px, w, 56, 32, 2) == 255 && at(px, w, 56, 32, 1) == 0);

    // A depth texture read where a shader expects a colour texture gets its
    // R32Float copy; the clear value comes through (red channel = depth).
    gpu::TextureDesc depth;
    depth.width = depth.height = 64;
    depth.format = 40;
    depth.bind = 0x48;
    gpu::create_texture(kDepth, depth);
    gpu::clear_depth(kDepth, 0.5f, 0, 1);
    const float white[4] = {1, 1, 1, 1};
    gpu::upload_buffer(kCb, reinterpret_cast<const std::uint8_t*>(white), sizeof white);
    s.ps_srv[0] = kDepth;
    s.viewport[0] = 0;
    s.viewport[2] = 64;
    gpu::draw_indexed(s, 6, 0, 0);
    REQUIRE(gpu::read_rgba8(kTarget, &px, &w, &h));
    CHECK(at(px, w, 4, 32, 0) >= 126 && at(px, w, 4, 32, 0) <= 129);
    CHECK(at(px, w, 4, 32, 1) == 0);

    // An unbound constant buffer reads as zeros (a stand-in is bound): black.
    s.ps_cb[0] = 0;
    gpu::draw_indexed(s, 6, 0, 0);
    REQUIRE(gpu::read_rgba8(kTarget, &px, &w, &h));
    CHECK(at(px, w, 4, 32, 0) == 0 && at(px, w, 4, 32, 3) == 0);

    CHECK(gpu::stats().pipeline_failures == 0);
    CHECK(gpu::stats().draws == 4);
    fs::remove_all(dir);
    return check_result();
}

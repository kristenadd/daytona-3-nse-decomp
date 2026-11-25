#include "recomp/d3d11.hpp"

#include <TargetConditionals.h>
#include "runtime/hitch.hpp"

#include <mach/mach.h>
#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio/ds_mixer.hpp"
#include "gpu/metal.hpp"
#include "recomp/guest.hpp"
#include "recomp/guest_memory.hpp"
#include "recomp/loader.hpp"
#include "recomp/host_files.hpp"
#include "recomp/threads.hpp"
#include "recomp/overlay.hpp"
#include "gpu/boot_screen.hpp"
#include "build_info.hpp"
#include "recomp/win32.hpp"
#include "recomp/x86ops.hpp"
#include "runtime/fail_closed.hpp"

namespace dcusa::recomp {

std::vector<std::uint8_t> g_frame;
dcusa::Presenter* g_presenter = nullptr;

audio::DsMixer& sound_mixer() {
    static audio::DsMixer mixer;
    return mixer;
}
std::uint32_t g_frame_w = 0;
std::uint32_t g_frame_h = 0;

namespace {

int g_frames_after_hitch = 0;  // report_hitch: frames to write out after a hitch

// Environment switches, read once: getenv walks the whole environment and
// several of these sit on per-draw paths.
bool stats_on() {
    static const bool on = std::getenv("DCUSA_STATS") != nullptr;
    return on;
}

constexpr std::uint32_t kSOk = 0;
constexpr std::uint32_t kNotFound = 0x887A0002u;
// Vtables, the singleton objects and the object pool live below the image, in
// memory nothing else hands out. They used to sit at 0x25100000, inside the
// process heap, where heap blocks and D3D objects overwrote each other once the
// boot had allocated 17 MB.
constexpr std::uint32_t kVt = 0x00100000u;
constexpr std::uint32_t kObj = 0x00110000u;
// ID3D11DeviceContext::ClearState is index 110 (byte offset 0x1B8). 64 slots put that
// load in the next interface's table.
constexpr std::uint32_t kSlots = 256;
constexpr std::uint32_t kVtStride = kSlots * 4u;

enum Kind : std::uint32_t {
    Device, Context, Dxgi, Adapter, Factory, Output, Swap, Texture, Srv, Sampler, Rtv, Query, Blob, Shader,
    Reflector, Buffer, Dsv, Layout, Raster, DepthState, Blend, DInput, DInputDev, DSound, DSoundBuf, ReflBuffer,
    ReflVariable, ReflType, KindCount
};

// The object pool is in high memory nothing else uses (above the heap's third
// region, below the COM and import thunks): the boot alone keeps over 12,000
// buffers alive, which D3D11 allows without complaint.
constexpr std::uint32_t kPool = 0xE0000000u;
// 0x4092A1 copies into the pointer Map returned. The scratch sits above the guest stack.
constexpr std::uint32_t kMapScratch = 0x31000000u;
constexpr std::uint32_t kMapBytes = 64u << 20;
// 0x6B28C0 creates 562 queries in one pass, on top of the textures made earlier.
// Shader reflection adds a buffer, variable and type object per constant. The
// pool ends at 0x220000, below the image at 0x400000.
constexpr std::uint32_t kPoolCap = 1u << 20;

std::uint32_t pool_used = 0;

struct StoredBytes {
    std::vector<std::uint8_t> bytes;
    std::uint32_t row = 0;
    std::uint32_t format = 0;
    // A resource only the GPU reads (Metal on; a texture of DEFAULT or IMMUTABLE
    // usage with no CPU access, render targets included: their CPU copy fed
    // only the software reference; or an IMMUTABLE buffer): its bytes are dropped once the Metal device has them,
    // `full` keeps their size. Over 1.5 GB of textures in attract.
    bool gpu_only = false;
    std::uint32_t full = 0;
};

// The CPU copy of a GPU-only texture, once uploaded.
void drop_cpu_copy(StoredBytes& s) {
    if (s.gpu_only) std::vector<std::uint8_t>().swap(s.bytes);
}

struct LiveMap {
    std::uint32_t resource = 0;
    std::uint32_t at = 0;
    std::uint32_t size = 0;
    bool write = false;
    bool home = false;  // `at` is the buffer's home (buffer_home), not scratch
};

// Buffer homes (with Metal): each buffer the game maps gets guest memory of its
// own in [kBufferHomes, +kBufferHomeBytes), filled once from its stored bytes.
// Map hands out the home, so its contents are always there (no copy in), and
// Unmap uploads straight from it (no copy into the stored bytes, which go
// stale). A race maps about 650 buffers a frame, 4.4 MB on the Mac: the copies
// were about a tenth of an iPhone's main thread. UpdateSubresource on such a
// buffer writes its home too.
// In high memory nothing else uses: above the object pool (kPool), below the
// COM thunks. They were at 0x37000000 at first, which is the process heap's
// second region (kHeap2): the game's constants were written over heap objects
// and the game faulted minutes later (attract, after races).
constexpr std::uint32_t kBufferHomes = 0xF0000000u;
constexpr std::uint32_t kBufferHomeBytes = 64u << 20;
static_assert(kBufferHomes >= kPool + kPoolCap * 0x40u, "buffer homes overlap the object pool");
static_assert(std::uint64_t(kBufferHomes) + kBufferHomeBytes <= kComCall, "buffer homes overlap the COM thunks");
static_assert(kBufferHomes >= kHeap2 + kHeap2Bytes || std::uint64_t(kBufferHomes) + kBufferHomeBytes <= kHeap2,
              "buffer homes overlap the heap's second region");
static_assert(kBufferHomes >= 0xE0000000u, "buffer homes overlap the heap's third region or VirtualAlloc's");
std::unordered_map<std::uint32_t, std::uint32_t> buffer_home;  // resource -> home
std::unordered_map<std::uint32_t, std::uint32_t> home_size;    // home -> its 64-byte multiple
std::unordered_multimap<std::uint32_t, std::uint32_t> free_homes;  // size -> home, from released buffers
std::uint32_t buffer_home_used = 0;

std::unordered_map<std::uint32_t, StoredBytes> stored;
// TIMESTAMP queries: where in the command stream each last ended (query_end):
// the present it belongs to, and whether a draw came before it in that frame.
struct QueryMark {
    std::uint64_t frame = 0;
    bool after_draws = false;
};
std::unordered_map<std::uint32_t, QueryMark> query_marks;
std::uint64_t g_frames_presented = 0;  // presents so far: the frame being recorded
std::uint32_t g_frame_draws = 0;       // draws in it so far
std::vector<LiveMap> live_maps;
std::uint32_t scratch_used = 0;

struct Element {
    char name[24]{};
    std::uint32_t format = 0;
    std::uint32_t slot = 0;
    std::uint32_t offset = 0;
};

struct LayoutDesc {
    std::vector<Element> elements;
};

struct Pipe {
    std::uint32_t vb[8]{};
    std::uint32_t stride[8]{};
    std::uint32_t voff[8]{};
    std::uint32_t ib = 0;
    std::uint32_t ib_format = 0;
    std::uint32_t ib_off = 0;
    std::uint32_t topology = 0;
    std::uint32_t layout = 0;
    std::uint32_t target = 0;
    std::uint32_t backbuffer = 0;
    std::uint32_t shader = 0;
    std::uint32_t vs_cb = 0;
    std::uint32_t vs_cb_count = 0;
    std::uint32_t vs_ids[8]{};
    std::uint32_t texture = 0;
    float vx = 0, vy = 0, vw = 0, vh = 0;
    bool scissor = false;
    int left = 0, top = 0, right = 0, bottom = 0;
};

std::unordered_map<std::uint32_t, LayoutDesc> layouts;
Pipe pipe;

// The whole D3D11 state the Metal device draws with (src/gpu/metal.hpp). The
// 2D reference above reads `pipe`; this is kept beside it.
gpu::DrawState gs;
struct ShaderObjects {
    std::uint32_t vs = 0, ps = 0;
} bound_shaders;
std::unordered_map<std::uint32_t, gpu::BlendTarget[gpu::kTargets]> blend_states;
std::unordered_set<std::uint32_t> alpha_to_coverage_states;  // blend states with AlphaToCoverageEnable
std::unordered_map<std::uint32_t, gpu::DepthDesc> depth_states;
// The state objects are immutable once created, so setting the one already
// set changes nothing; the last one set skips the lookups (every draw sets
// them). A destroyed object's id resets the memo (destroy).
std::uint32_t g_last_blend = ~0u, g_last_depth = ~0u, g_last_raster = ~0u;

std::unordered_map<std::uint32_t, gpu::RasterDesc> raster_states;
// Each InputLayout's elements, kept for good (a draw state points at them).
struct StoredLayout {
    const std::vector<gpu::VertexElement>* elements = nullptr;
    std::uint32_t serial = 0;
};
std::unordered_map<std::uint32_t, StoredLayout> gpu_layouts;
std::deque<std::vector<gpu::VertexElement>> layout_store;
float g_proj[16]{};
bool g_have_proj = false;
bool g_world_tri = false;

bool project_world(float* x, float* y, float z) {
    const float cx = *x * g_proj[0] + *y * g_proj[4] + z * g_proj[8] + g_proj[12];
    const float cy = *x * g_proj[1] + *y * g_proj[5] + z * g_proj[9] + g_proj[13];
    const float cw = *x * g_proj[3] + *y * g_proj[7] + z * g_proj[11] + g_proj[15];
    if (!(cw > 0.001f)) return false;
    *x = cx / cw;
    *y = cy / cw;
    return std::fabs(*x) < 4.0f && std::fabs(*y) < 4.0f;
}
std::unordered_map<std::uint32_t, std::uint32_t> shader_code;
std::unordered_map<std::uint32_t, std::uint64_t> shader_key;  // bytecode address -> compile request key
unsigned g_prepared = 0, g_unprepared = 0;                     // D3DCompile with / without a translation
unsigned g_refl_buffers = 0, g_refl_vars = 0, g_refl_types = 0;  // reflection GetDesc calls answered

// The bytecode D3DCompile returns is 16 bytes: "DXBC", "DCUS", then the
// compile request key. The game writes these into its shader cache
// (GameData/.shaders/shader_cache.bin, in the container) and later creates
// shaders from the cached copy without compiling, so the key has to travel
// with the bytes.
constexpr std::uint32_t kDxbc = 0x43425844u, kBlobMarker = 0x53554344u;

bool blob_key(const std::uint8_t* mem, std::uint32_t code, std::uint64_t* key) {
    if (code == 0 || ld32(mem, code) != kDxbc || ld32(mem, code + 4) != kBlobMarker) return false;
    *key = static_cast<std::uint64_t>(ld32(mem, code + 8)) | static_cast<std::uint64_t>(ld32(mem, code + 12)) << 32;
    return true;
}

// Shader object -> the compile request key of its bytecode (0 when unknown).
std::unordered_map<std::uint32_t, std::uint64_t> shader_obj_key;
std::uint64_t key_of_shader(std::uint32_t shader) {
    const auto key = shader_obj_key.find(shader);
    return key == shader_obj_key.end() ? 0 : key->second;
}
void note_shader(std::uint32_t shader);
std::uint32_t g_draws = 0;
std::uint32_t g_drawn = 0;
std::uint32_t g_skipped = 0;
std::uint32_t g_binds = 0;
std::uint32_t g_textures = 0;
std::uint32_t g_srvs = 0;
std::uint32_t g_meshes = 0;
std::uint32_t g_back_draws = 0;
std::uint32_t g_rgba_draws = 0;
std::uint32_t g_rgba_back = 0;
std::uint32_t g_rgba_tex = 0;

bool name_has(const char* name, const char* part) {
    for (std::uint32_t i = 0; name[i] != 0 && i < 23u; ++i) {
        bool match = true;
        for (std::uint32_t k = 0; part[k] != 0; ++k) {
            if (i + k >= 23u || name[i + k] == 0) {
                match = false;
                break;
            }
            char al = name[i + k];
            char b = part[k];
            if (al >= 'A' && al <= 'Z') al = static_cast<char>(al - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (al != b) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

void copy_name(char* dst, const std::uint8_t* mem, std::uint32_t va) {
    dst[0] = 0;
    if (va == 0) return;
    for (int i = 0; i < 23; ++i) {
        const std::uint8_t b = mem[va + static_cast<std::uint32_t>(i)];
        dst[i] = b < 128 ? static_cast<char>(b) : 0;
        dst[i + 1] = 0;
        if (b == 0) break;
    }
}

bool read_xy(const std::uint8_t* mem, std::uint32_t format, float* x, float* y) {
    if (format != 2 && format != 6 && format != 16) return false;
    std::memcpy(x, mem, 4);
    std::memcpy(y, mem + 4, 4);
    return std::isfinite(*x) && std::isfinite(*y);
}

void read_color(const std::uint8_t* mem, std::uint32_t format, std::uint8_t rgba[4]) {
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 255;
    if (format == 28) {
        std::memcpy(rgba, mem, 4);
    } else if (format == 87) {
        rgba[0] = mem[2];
        rgba[1] = mem[1];
        rgba[2] = mem[0];
        rgba[3] = mem[3];
    } else if (format == 2) {
        float f[4];
        std::memcpy(f, mem, 16);
        for (int i = 0; i < 4; ++i) {
            float v = f[i];
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            rgba[i] = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
        }
    }
}

std::uint32_t target_bpp(std::uint32_t format);
void store_color(std::uint8_t* dst, std::uint32_t format, const float rgba[4]);

void paint(StoredBytes& target, float x, float y, const std::uint8_t rgba[4]) {
    const std::uint32_t bpp = target_bpp(target.format);
    if (target.row < bpp) return;
    const int w = static_cast<int>(target.row / bpp);
    const int h = static_cast<int>(target.bytes.size() / target.row);
    const int px = static_cast<int>(std::floor(x));
    const int py = static_cast<int>(std::floor(y));
    if (px < 0 || py < 0 || px >= w || py >= h) return;
    if (pipe.scissor && (px < pipe.left || py < pipe.top || px >= pipe.right || py >= pipe.bottom)) return;
    std::uint8_t* dst = target.bytes.data() + static_cast<std::size_t>(py) * target.row + static_cast<std::size_t>(px) * bpp;
    const bool native = target.format == 10 || target.format == 34 || target.format == 54 || target.format == 39 ||
                        target.format == 41;
    if (native) {
        float pxf[4];
        for (int i = 0; i < 4; ++i) pxf[i] = static_cast<float>(rgba[i]) / 255.0f;
        store_color(dst, target.format, pxf);
        return;
    }
    const unsigned a = rgba[3];
    if (a == 255) {
        std::memcpy(dst, rgba, 4);
        return;
    }
    if (a == 0) return;
    for (int i = 0; i < 3; ++i)
        dst[i] = static_cast<std::uint8_t>((rgba[i] * a + dst[i] * (255u - a) + 127u) / 255u);
    dst[3] = static_cast<std::uint8_t>(a + dst[3] * (255u - a) / 255u);
}

void bc_color(std::uint16_t packed, std::uint8_t rgb[3]) {
    rgb[0] = static_cast<std::uint8_t>(((packed >> 11) & 31) * 255 / 31);
    rgb[1] = static_cast<std::uint8_t>(((packed >> 5) & 63) * 255 / 63);
    rgb[2] = static_cast<std::uint8_t>((packed & 31) * 255 / 31);
}

std::uint32_t texel_bytes(std::uint32_t format);

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h >> 15) & 1u;
    std::uint32_t exp = (h >> 10) & 0x1fu;
    std::uint32_t mant = h & 0x3ffu;
    std::uint32_t bits = 0;
    if (exp == 0) {
        if (mant != 0) {
            exp = 127 - 14;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3ffu;
            bits = (sign << 31) | (exp << 23) | (mant << 13);
        } else {
            bits = sign << 31;
        }
    } else if (exp == 31) {
        bits = (sign << 31) | (0xffu << 23) | (mant << 13);
    } else {
        bits = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float out = 0;
    std::memcpy(&out, &bits, 4);
    return out;
}

std::uint16_t float_to_half(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t exp = (bits >> 23) & 0xffu;
    const std::uint32_t mant = bits & 0x7fffffu;
    if (exp == 255u) return static_cast<std::uint16_t>(sign | 0x7c00u | (mant != 0u ? 0x200u : 0u));
    const std::int32_t half_exp = static_cast<std::int32_t>(exp) - 127 + 15;
    if (half_exp >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u);
    if (half_exp <= 0) {
        if (half_exp < -10) return static_cast<std::uint16_t>(sign);
        const std::uint32_t sig = (mant | 0x800000u) >> static_cast<std::uint32_t>(1 - half_exp + 13);
        return static_cast<std::uint16_t>(sign | sig);
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(half_exp) << 10) | (mant >> 13));
}

std::uint32_t target_bpp(std::uint32_t format) {
    if (format == 10) return 8u;
    if (format == 54) return 2u;
    return 4u;
}

std::uint8_t unit_byte(float v) {
    if (!(v > 0)) return 0;
    if (v > 1) v = 1;
    return static_cast<std::uint8_t>(v * 255.0f + 0.5f);
}

void store_color(std::uint8_t* dst, std::uint32_t format, const float rgba[4]) {
    auto channel = [](float v) {
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        return float_to_half(v);
    };
    if (format == 10) {
        const std::uint16_t px[4] = {channel(rgba[0]), channel(rgba[1]), channel(rgba[2]), channel(rgba[3])};
        std::memcpy(dst, px, 8);
        return;
    }
    if (format == 34) {
        const std::uint16_t px[2] = {channel(rgba[0]), channel(rgba[1])};
        std::memcpy(dst, px, 4);
        return;
    }
    if (format == 54) {
        const std::uint16_t px = channel(rgba[0]);
        std::memcpy(dst, &px, 2);
        return;
    }
    if (format == 39 || format == 41) {
        float v = rgba[0];
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        std::memcpy(dst, &v, 4);
        return;
    }
    std::uint8_t px[4];
    for (int i = 0; i < 4; ++i) px[i] = unit_byte(rgba[i]);
    std::memcpy(dst, px, 4);
}

bool sample_texture(const StoredBytes& tex, float u, float v, std::uint8_t rgba[4]) {
    const std::uint32_t bpp = texel_bytes(tex.format == 0 ? 28u : tex.format);
    if (tex.row < bpp || tex.bytes.empty()) return false;
    const int w = static_cast<int>(tex.row / bpp);
    const int h = static_cast<int>(tex.bytes.size() / tex.row);
    if (w <= 0 || h <= 0) return false;
    if (u < 0) u = 0;
    if (v < 0) v = 0;
    if (u > 1) u = 1;
    if (v > 1) v = 1;
    const int x = std::min(w - 1, static_cast<int>(u * static_cast<float>(w)));
    const int y = std::min(h - 1, static_cast<int>(v * static_cast<float>(h)));
    if (tex.format == 28 || tex.format == 29 || tex.format == 87 || tex.format == 0) {
        const std::uint8_t* p = tex.bytes.data() + static_cast<std::size_t>(y) * tex.row + static_cast<std::size_t>(x) * 4u;
        if (tex.format == 87) {
            rgba[0] = p[2];
            rgba[1] = p[1];
            rgba[2] = p[0];
            rgba[3] = p[3];
        } else {
            std::memcpy(rgba, p, 4);
        }
        return true;
    }
    const std::uint8_t* texel = tex.bytes.data() + static_cast<std::size_t>(y) * tex.row + static_cast<std::size_t>(x) * bpp;
    if (tex.format == 34) {
        std::uint16_t h0 = 0, h1 = 0;
        std::memcpy(&h0, texel, 2);
        std::memcpy(&h1, texel + 2, 2);
        rgba[0] = unit_byte(half_to_float(h0));
        rgba[1] = unit_byte(half_to_float(h1));
        rgba[2] = 0;
        rgba[3] = 255;
        return true;
    }
    if (tex.format == 10) {
        for (int i = 0; i < 4; ++i) {
            std::uint16_t half = 0;
            std::memcpy(&half, texel + i * 2, 2);
            rgba[i] = unit_byte(half_to_float(half));
        }
        return true;
    }
    if (tex.format == 11) {
        for (int i = 0; i < 4; ++i) {
            std::uint16_t half = 0;
            std::memcpy(&half, texel + i * 2, 2);
            rgba[i] = static_cast<std::uint8_t>((half * 255u) / 65535u);
        }
        return true;
    }
    if (tex.format == 54) {
        std::uint16_t half = 0;
        std::memcpy(&half, texel, 2);
        rgba[0] = rgba[1] = rgba[2] = unit_byte(half_to_float(half));
        rgba[3] = 255;
        return true;
    }
    if (tex.format == 39 || tex.format == 41 || tex.format == 40) {
        float f = 0;
        std::memcpy(&f, texel, 4);
        rgba[0] = rgba[1] = rgba[2] = unit_byte(f);
        rgba[3] = 255;
        return true;
    }
    const bool bc1 = tex.format == 71 || tex.format == 72;
    const bool bc3 = tex.format == 77 || tex.format == 78;
    if (!bc1 && !bc3) return false;
    const int bx = x / 4;
    const int by = y / 4;
    const int blocks_x = (w + 3) / 4;
    const std::size_t block_bytes = bc1 ? 8u : 16u;
    const std::size_t at = (static_cast<std::size_t>(by) * static_cast<std::size_t>(blocks_x) + static_cast<std::size_t>(bx)) * block_bytes;
    if (at + block_bytes > tex.bytes.size()) return false;
    const std::uint8_t* block = tex.bytes.data() + at;
    std::uint8_t alpha = 255;
    const std::uint8_t* color = block;
    if (bc3) {
        const std::uint8_t a0 = block[0];
        const std::uint8_t a1 = block[1];
        std::uint64_t bits = 0;
        std::memcpy(&bits, block + 2, 6);
        const int idx = (y & 3) * 4 + (x & 3);
        const unsigned sel = static_cast<unsigned>((bits >> (idx * 3)) & 7u);
        if (sel == 0) alpha = a0;
        else if (sel == 1) alpha = a1;
        else if (a0 > a1) alpha = static_cast<std::uint8_t>(((8 - sel) * a0 + (sel - 1) * a1) / 7);
        else if (sel == 6) alpha = 0;
        else if (sel == 7) alpha = 255;
        else alpha = static_cast<std::uint8_t>(((6 - sel) * a0 + (sel - 1) * a1) / 5);
        color = block + 8;
    }
    std::uint16_t c0 = 0, c1 = 0;
    std::memcpy(&c0, color, 2);
    std::memcpy(&c1, color + 2, 2);
    std::uint8_t pal[4][3];
    bc_color(c0, pal[0]);
    bc_color(c1, pal[1]);
    if (c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = static_cast<std::uint8_t>((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = static_cast<std::uint8_t>((pal[0][i] + 2 * pal[1][i]) / 3);
        }
    } else {
        for (int i = 0; i < 3; ++i) pal[2][i] = static_cast<std::uint8_t>((pal[0][i] + pal[1][i]) / 2);
        pal[3][0] = pal[3][1] = pal[3][2] = 0;
        if (bc1) alpha = 0;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, color + 4, 4);
    const int idx = (y & 3) * 4 + (x & 3);
    const unsigned sel = (bits >> (idx * 2)) & 3u;
    rgba[0] = pal[sel][0];
    rgba[1] = pal[sel][1];
    rgba[2] = pal[sel][2];
    rgba[3] = alpha;
    return true;
}

void fill_triangle(StoredBytes& target, const float p[3][2], const std::uint8_t c[3][4], const float uv[3][2],
                    const StoredBytes* tex) {
    float area = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1]);
    if (area < 0) {
        area = -area;
    }
    if (area < 0.5f) return;
    float minx = p[0][0], maxx = p[0][0], miny = p[0][1], maxy = p[0][1];
    for (int i = 1; i < 3; ++i) {
        minx = std::min(minx, p[i][0]);
        maxx = std::max(maxx, p[i][0]);
        miny = std::min(miny, p[i][1]);
        maxy = std::max(maxy, p[i][1]);
    }
    const std::uint32_t bpp = target_bpp(target.format);
    const int w = static_cast<int>(target.row / bpp);
    const int h = static_cast<int>(target.bytes.size() / target.row);
    int x0 = std::max(0, static_cast<int>(std::floor(minx)));
    int y0 = std::max(0, static_cast<int>(std::floor(miny)));
    int x1 = std::min(w - 1, static_cast<int>(std::floor(maxx)));
    int y1 = std::min(h - 1, static_cast<int>(std::floor(maxy)));
    if (pipe.scissor) {
        x0 = std::max(x0, pipe.left);
        y0 = std::max(y0, pipe.top);
        x1 = std::min(x1, pipe.right - 1);
        y1 = std::min(y1, pipe.bottom - 1);
    }
    if (x1 < x0 || y1 < y0) return;
    auto edge = [](float ax, float ay, float bx, float by, float cx, float cy) {
        return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
    };
    const float e01 = edge(p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1]);
    const float sign = e01 < 0 ? -1.0f : 1.0f;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float sx = static_cast<float>(x) + 0.5f;
            const float sy = static_cast<float>(y) + 0.5f;
            const float w0 = edge(p[1][0], p[1][1], p[2][0], p[2][1], sx, sy) * sign;
            const float w1 = edge(p[2][0], p[2][1], p[0][0], p[0][1], sx, sy) * sign;
            const float w2 = edge(p[0][0], p[0][1], p[1][0], p[1][1], sx, sy) * sign;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const float sum = w0 + w1 + w2;
            if (sum <= 0) continue;
            std::uint8_t rgba[4];
            if (tex != nullptr) {
                const float tu = (uv[0][0] * w0 + uv[1][0] * w1 + uv[2][0] * w2) / sum;
                const float tv = (uv[0][1] * w0 + uv[1][1] * w1 + uv[2][1] * w2) / sum;
                if (!sample_texture(*tex, tu, tv, rgba)) continue;
            } else {
                for (int k = 0; k < 4; ++k) {
                    const float v = (c[0][k] * w0 + c[1][k] * w1 + c[2][k] * w2) / sum;
                    rgba[k] = static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
                }
            }
            paint(target, sx, sy, rgba);
        }
    }
}

bool fetch_vert(std::uint32_t index, std::int32_t base_vertex, float* x, float* y, std::uint8_t rgba[4], float* u,
                float* v) {
    const auto layout = layouts.find(pipe.layout);
    if (layout == layouts.end()) return false;
    const Element* pos = nullptr;
    const Element* color = nullptr;
    const Element* uv = nullptr;
    for (const Element& e : layout->second.elements) {
        if (pos == nullptr && name_has(e.name, "position")) pos = &e;
        if (color == nullptr && name_has(e.name, "color")) color = &e;
        if (uv == nullptr && name_has(e.name, "tex")) uv = &e;
    }
    if (pos == nullptr) return false;
    if (pos->slot >= 8 || pipe.vb[pos->slot] == 0 || pipe.stride[pos->slot] == 0) return false;
    const auto buf = stored.find(pipe.vb[pos->slot]);
    if (buf == stored.end()) return false;
    const std::int64_t vert = static_cast<std::int64_t>(index) + base_vertex;
    if (vert < 0) return false;
    const std::uint64_t at = static_cast<std::uint64_t>(pipe.voff[pos->slot]) +
                             static_cast<std::uint64_t>(vert) * pipe.stride[pos->slot] + pos->offset;
    if (at + 8 > buf->second.bytes.size()) return false;
    if (!read_xy(buf->second.bytes.data() + at, pos->format, x, y)) return false;
    if (g_have_proj && (std::fabs(*x) > 100.0f || std::fabs(*y) > 100.0f) &&
        (pos->format == 6 || pos->format == 2) && at + 12 <= buf->second.bytes.size()) {
        float z = 0;
        std::memcpy(&z, buf->second.bytes.data() + at + 8, 4);
        if (!project_world(x, y, z)) return false;
        g_world_tri = true;
    }
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 255;
    if (color != nullptr && color->slot < 8 && pipe.vb[color->slot] != 0) {
        const auto cb = stored.find(pipe.vb[color->slot]);
        if (cb != stored.end()) {
            const std::uint64_t ca = static_cast<std::uint64_t>(pipe.voff[color->slot]) +
                                     static_cast<std::uint64_t>(vert) * pipe.stride[color->slot] + color->offset;
            const std::uint32_t need = color->format == 2 ? 16u : 4u;
            if (ca + need <= cb->second.bytes.size()) read_color(cb->second.bytes.data() + ca, color->format, rgba);
        }
    }
    *u = 0;
    *v = 0;
    if (uv != nullptr && uv->slot < 8 && pipe.vb[uv->slot] != 0) {
        const auto tb = stored.find(pipe.vb[uv->slot]);
        if (tb != stored.end()) {
            const std::uint64_t ta = static_cast<std::uint64_t>(pipe.voff[uv->slot]) +
                                     static_cast<std::uint64_t>(vert) * pipe.stride[uv->slot] + uv->offset;
            if (ta + 8 <= tb->second.bytes.size()) read_xy(tb->second.bytes.data() + ta, 16, u, v);
        }
    }
    return std::fabs(*x) < 8000.0f && std::fabs(*y) < 8000.0f;
}

void to_screen(float xy[3][2]) {
    bool ndc = true;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(xy[i][0]) > 2.0f || std::fabs(xy[i][1]) > 2.0f) ndc = false;
    }
    if (!ndc) return;
    const auto target = stored.find(pipe.target);
    float width = pipe.vw;
    float height = pipe.vh;
    if (target != stored.end() && target->second.row >= 4) {
        if (width <= 0) width = static_cast<float>(target->second.row / 4u);
        if (height <= 0) height = static_cast<float>(target->second.bytes.size() / target->second.row);
    }
    for (int i = 0; i < 3; ++i) {
        xy[i][0] = pipe.vx + (xy[i][0] * 0.5f + 0.5f) * width;
        xy[i][1] = pipe.vy + (0.5f - xy[i][1] * 0.5f) * height;
    }
}

std::uint32_t fetch_index(std::uint32_t which) {
    if (pipe.ib == 0) return which;
    const auto buf = stored.find(pipe.ib);
    if (buf == stored.end()) return which;
    if (pipe.ib_format == 57) {
        const std::uint64_t at = static_cast<std::uint64_t>(pipe.ib_off) + static_cast<std::uint64_t>(which) * 2u;
        if (at + 2 > buf->second.bytes.size()) return 0;
        std::uint16_t v = 0;
        std::memcpy(&v, buf->second.bytes.data() + at, 2);
        return v;
    }
    const std::uint64_t at = static_cast<std::uint64_t>(pipe.ib_off) + static_cast<std::uint64_t>(which) * 4u;
    if (at + 4 > buf->second.bytes.size()) return 0;
    std::uint32_t v = 0;
    std::memcpy(&v, buf->second.bytes.data() + at, 4);
    return v;
}

void draw_triangles(std::uint32_t count, std::uint32_t start, std::int32_t base_vertex) {
    ++g_draws;
    if (count > 8) ++g_meshes;
    if (pipe.target == pipe.backbuffer) {
        ++g_back_draws;
    }
    if (const auto image = stored.find(pipe.texture); image != stored.end()) {
        const std::uint32_t fmt = image->second.format;
        if ((fmt == 28 || fmt == 29 || fmt == 87) && image->second.bytes.size() >= 1024u) {
            ++g_rgba_draws;
            if (pipe.target == pipe.backbuffer) ++g_rgba_back;
            if (g_rgba_tex == 0) g_rgba_tex = pipe.texture;
        }
    }
    auto target = stored.find(pipe.target);
    if (target == stored.end() || target->second.bytes.empty()) {
        ++g_skipped;
        return;
    }
    if (count > 30000u) count = 30000u;
    const bool strip = pipe.topology == 5;
    const std::uint32_t step = strip ? 1u : 3u;
    if (!strip && count < 3) return;
    if (strip && count < 3) return;
    const std::uint32_t last = strip ? count - 2u : count / 3u;
    std::uint32_t painted = 0;
    for (std::uint32_t t = 0; t < last && painted < 4000u; ++t, ++painted) {
        float xy[3][2]{};
        float uv[3][2]{};
        std::uint8_t col[3][4];
        bool ok = true;
        g_world_tri = false;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t n = strip ? start + t + static_cast<std::uint32_t>(k) : start + t * step + static_cast<std::uint32_t>(k);
            const std::uint32_t index = fetch_index(n);
            if (!fetch_vert(index, base_vertex, &xy[k][0], &xy[k][1], col[k], &uv[k][0], &uv[k][1])) ok = false;
        }
        if (!ok) {
            ++g_skipped;
            if (count > 8 && stats_on()) {
                static int noted = 0;
                if (noted < 3) {
                    ++noted;
                    note_shader(pipe.shader);
                    std::fprintf(stderr, "  cb slots %u\n", pipe.vs_cb_count);
                    for (std::uint32_t s = 0; s < pipe.vs_cb_count; ++s) {
                        const auto cb = stored.find(pipe.vs_ids[s]);
                        if (cb == stored.end() || cb->second.bytes.size() < 64) {
                            std::fprintf(stderr, "  slot %u id %u\n", s, pipe.vs_ids[s]);
                            continue;
                        }
                        float m[8]{};
                        std::memcpy(m, cb->second.bytes.data(), sizeof m);
                        std::fprintf(stderr, "  slot %u", s);
                        for (float v : m) std::fprintf(stderr, " %.4g", v);
                        std::fprintf(stderr, "\n");
                    }
                    if (noted != 1) continue;
                    const auto layout = layouts.find(pipe.layout);
                    const std::uint32_t index = fetch_index(start);
                    std::fprintf(stderr, "mesh miss idx %u index %u layout %u vb %u stride %u\n", count, index,
                                 pipe.layout, pipe.vb[0], pipe.stride[0]);
                    if (layout != layouts.end()) {
                        for (const Element& e : layout->second.elements)
                            std::fprintf(stderr, "  elem %s fmt %u slot %u off %u\n", e.name, e.format, e.slot,
                                         e.offset);
                    }
                    const auto buf = stored.find(pipe.vb[0]);
                    if (buf != stored.end() && pipe.stride[0] >= 12 && buf->second.bytes.size() >= pipe.stride[0]) {
                        unsigned finite = 0;
                        unsigned huge = 0;
                        float sample[3]{};
                        const std::uint32_t stride = pipe.stride[0];
                        const std::uint32_t verts = static_cast<std::uint32_t>(buf->second.bytes.size() / stride);
                        for (std::uint32_t v = 0; v < verts; ++v) {
                            float f[3]{};
                            std::memcpy(f, buf->second.bytes.data() + static_cast<std::size_t>(v) * stride, 12);
                            if (std::fabs(f[0]) < 8000.0f && std::fabs(f[1]) < 8000.0f && std::fabs(f[2]) < 8000.0f) {
                                if (finite == 0) std::memcpy(sample, f, sizeof sample);
                                ++finite;
                            } else {
                                ++huge;
                            }
                        }
                        std::fprintf(stderr, "  vb verts %u finite %u huge %u sample (%.2f,%.2f,%.2f) ib %u\n", verts, finite,
                                     huge, sample[0], sample[1], sample[2], pipe.ib_format);
                        for (std::uint32_t i = 0; i < 6; ++i) {
                            const std::uint32_t vi = fetch_index(start + i);
                            float f[3]{};
                            if (static_cast<std::uint64_t>(vi) * stride + 12 <= buf->second.bytes.size())
                                std::memcpy(f, buf->second.bytes.data() + static_cast<std::size_t>(vi) * stride, 12);
                            std::fprintf(stderr, "  vi %u (%.2f,%.2f,%.2f)\n", vi, f[0], f[1], f[2]);
                        }
                        note_shader(pipe.shader);
                        std::fprintf(stderr, "  vs_cb %u\n", pipe.vs_cb);
                        const auto cb = stored.find(pipe.vs_cb);
                        if (cb != stored.end() && cb->second.bytes.size() >= 64) {
                            float m[16]{};
                            std::memcpy(m, cb->second.bytes.data(), sizeof m);
                            std::fprintf(stderr, "  cb");
                            for (float v : m) std::fprintf(stderr, " %.4g", v);
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
            }
        } else {
            const auto image = stored.find(pipe.texture);
            const StoredBytes* tex = image == stored.end() ? nullptr : &image->second;
            if (g_world_tri && tex != nullptr) {
                bool any = false;
                const std::size_t n = tex->bytes.size();
                const std::size_t probe = std::max<std::size_t>(n / 16u, 1u);
                for (std::size_t i = 0; i < n && !any; i += probe) any = tex->bytes[i] != 0;
                if (!any) tex = nullptr;
            }
            const bool world = g_world_tri;
            if (count > 8 && stats_on()) {
                static int mesh_note = 0;
                if (mesh_note < 2) {
                    ++mesh_note;
                    std::fprintf(stderr, "mesh %u idx %u ndc (%.2f,%.2f) col %u %u %u target %u\n", g_draws, count,
                                 xy[0][0], xy[0][1], col[0][0], col[0][1], col[0][2], pipe.target);
                }
            }
            if (g_draws <= 8 && stats_on()) {
                const auto tgt = stored.find(pipe.target);
                std::fprintf(stderr,
                             "draw %u idx %u raw (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f) target %u/%u tex %u/%u\n", g_draws,
                             count, xy[0][0], xy[0][1], xy[1][0], xy[1][1], xy[2][0], xy[2][1], pipe.target,
                             tgt == stored.end() ? 0u : tgt->second.format, pipe.texture,
                             tex != nullptr ? tex->format : 0u);
            }
            to_screen(xy);
            const float span = std::max(std::max(std::fabs(xy[1][0] - xy[0][0]), std::fabs(xy[2][0] - xy[0][0])),
                                        std::max(std::fabs(xy[1][1] - xy[0][1]), std::fabs(xy[2][1] - xy[0][1])));
            if (tex == nullptr && !world && span > 64.0f) {
                ++g_skipped;
                continue;
            }
            if (g_drawn == 0 && stats_on()) {
                const auto layout = layouts.find(pipe.layout);
                std::fprintf(stderr,
                             "tri (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) uv (%.3f,%.3f) (%.3f,%.3f) (%.3f,%.3f) col %u %u "
                             "tex %u fmt %u bytes %zu row %u\n",
                             xy[0][0], xy[0][1], xy[1][0], xy[1][1], xy[2][0], xy[2][1], uv[0][0], uv[0][1], uv[1][0],
                             uv[1][1], uv[2][0], uv[2][1], col[0][0], col[0][1], pipe.texture,
                             tex != nullptr ? tex->format : 0u, tex != nullptr ? tex->bytes.size() : 0u,
                             tex != nullptr ? tex->row : 0u);
                if (tex != nullptr && tex->bytes.size() >= 16) {
                    unsigned uniq = 0;
                    std::uint8_t seenb[8][4]{};
                    const std::size_t pixels = tex->bytes.size() / 4u;
                    const std::size_t stride = std::max<std::size_t>(pixels / 2000u, 1u);
                    for (std::size_t i = 0; i < pixels && uniq < 8; i += stride) {
                        const std::uint8_t* p = tex->bytes.data() + i * 4u;
                        bool known = false;
                        for (unsigned k = 0; k < uniq; ++k)
                            if (std::memcmp(seenb[k], p, 4) == 0) known = true;
                        if (!known) std::memcpy(seenb[uniq++], p, 4);
                    }
                    std::fprintf(stderr, "  tex kinds %u first %02x %02x %02x %02x\n", uniq, tex->bytes[0], tex->bytes[1],
                                 tex->bytes[2], tex->bytes[3]);
                }
                note_shader(pipe.shader);
                if (layout != layouts.end()) {
                    for (const Element& e : layout->second.elements)
                        std::fprintf(stderr, "  elem %s fmt %u slot %u off %u\n", e.name, e.format, e.slot, e.offset);
                }
            }
            fill_triangle(target->second, xy, col, uv, tex);
            ++g_drawn;
        }
    }
}

// `fresh`: the stored bytes hold this frame. With Metal they are only read back
// for the frames DCUSA_FRAME writes; copying 8 MB of stale bytes every present
// cost about 2% of the main thread.
void publish_frame(std::uint32_t resource, bool fresh) {
    const auto it = stored.find(resource);
    if (it == stored.end() || it->second.row < 4 || it->second.bytes.empty()) return;
    g_frame_w = it->second.row / 4u;
    g_frame_h = static_cast<std::uint32_t>(it->second.bytes.size() / it->second.row);
    if (fresh) g_frame = it->second.bytes;
    static int seen = 0;
    ++seen;
    if (stats_on() && seen == 1) {
        unsigned live = 0;
        for (const auto& item : stored) {
            if (item.second.bytes.size() < 4096u) continue;
            bool any = false;
            const std::size_t n = item.second.bytes.size();
            const std::size_t stride = std::max<std::size_t>(n / 64u, 1u);
            for (std::size_t i = 0; i < n; i += stride) {
                if (item.second.bytes[i] != 0) {
                    any = true;
                    break;
                }
            }
            if (!any) continue;
            ++live;
            const bool color = item.second.format == 28 || item.second.format == 29 || item.second.format == 71 ||
                               item.second.format == 77 || item.second.format == 87;
            if (color || live <= 4)
                std::fprintf(stderr, "live %08x bytes %zu fmt %u\n", static_cast<unsigned>(item.first),
                             item.second.bytes.size(), item.second.format);
        }
        std::fprintf(stderr, "live textures %u\n", live);
    }
    // DCUSA_EVERY=N also reports (and with DCUSA_FRAME writes) every Nth present.
    const char* every_env = std::getenv("DCUSA_EVERY");
    const int every = every_env ? std::atoi(every_env) : 0;
    const bool periodic = every > 0 && seen % every == 0;
    if (stats_on() && (seen == 1 || seen == 30 || periodic)) {
        std::fprintf(stderr,
                     "present %d draws %u painted %u skipped %u binds %u meshes %u tex %u textures %u target %u back %u rgba %u/%u first %u\n",
                     seen, g_draws, g_drawn, g_skipped, g_binds, g_meshes, pipe.texture, g_textures, resource, g_back_draws,
                     g_rgba_back, g_rgba_draws, g_rgba_tex);
        // The frame-rate profile since the last report: wall time and GPU time per
        // frame, and draws per frame.
        if (gpu::enabled()) {
            static auto last_t = std::chrono::steady_clock::now();
            static double last_gpu = 0;
            static unsigned last_buffers = 0, last_draws = 0;
            static int last_seen = 0;
            const gpu::Stats& m = gpu::stats();
            const auto now = std::chrono::steady_clock::now();
            const int frames = seen - last_seen;
            if (frames > 0 && m.gpu_buffers > last_buffers) {
                const double wall = std::chrono::duration<double, std::milli>(now - last_t).count() / frames;
                const double gpu_ms = (m.gpu_seconds - last_gpu) * 1000.0 / (m.gpu_buffers - last_buffers);
                std::fprintf(stderr, "frame %.2f ms (%.1f fps), gpu %.2f ms per frame, %u draws per frame\n", wall,
                             1000.0 / wall, gpu_ms, (m.draws - last_draws) / static_cast<unsigned>(frames));
            }
            last_t = now;
            last_gpu = m.gpu_seconds;
            last_buffers = m.gpu_buffers;
            last_draws = m.draws;
            last_seen = seen;
        }
        std::fprintf(stderr, "srvs %u shaders translated %u not %u reflected buffers %u variables %u types %u\n", g_srvs,
                     g_prepared, g_unprepared, g_refl_buffers, g_refl_vars, g_refl_types);
        if (gpu::enabled()) {
            const gpu::Stats& m = gpu::stats();
            std::fprintf(stderr, "metal draws %u skipped %u pipelines %u failed %u textures %u uploads %u clears %u%s%s\n",
                         m.draws, m.skipped, m.pipelines, m.pipeline_failures, m.textures, m.uploads, m.clears,
                         m.last_problem.empty() ? "" : " last: ", m.last_problem.c_str());
        }
    }
    if (const char* path = std::getenv("DCUSA_FRAME")) {
        if (seen != 1 && seen != 30 && !periodic && g_frames_after_hitch == 0) return;
        if (g_frames_after_hitch > 0) --g_frames_after_hitch;
        char file[512];
        std::snprintf(file, sizeof file, "%s.%d.ppm", path, seen);
        std::ofstream out(file, std::ios::binary);
        if (!out) return;
        out << "P6\n" << g_frame_w << " " << g_frame_h << "\n255\n";
        for (std::uint32_t i = 0; i + 3 < g_frame.size(); i += 4) out.write(reinterpret_cast<const char*>(g_frame.data() + i), 3);
    }
}

// The guest wrote `obj`'s bytes (create, UpdateSubresource, Unmap): give the
// Metal device the same bytes.
void sync_gpu(const std::uint8_t* mem, std::uint32_t obj) {
    if (!gpu::enabled()) return;
    const auto it = stored.find(obj);
    if (it == stored.end() || it->second.bytes.empty()) return;
    const std::uint32_t kind = ld32(mem, obj + 4);
    if (kind == Texture) {
        gpu::upload_texture(obj, it->second.bytes.data(), it->second.bytes.size(), it->second.row);
        drop_cpu_copy(it->second);
    } else {
        gpu::upload_buffer(obj, it->second.bytes.data(), it->second.bytes.size());
        drop_cpu_copy(it->second);
    }
}

std::uint32_t texel_bytes(std::uint32_t format) {
    if (format >= 70 && format <= 84) return format <= 72 || (format >= 79 && format <= 81) ? 8u : 16u;
    if (format >= 94 && format <= 99) return 16u;
    if (format >= 1 && format <= 4) return 16u;
    if (format >= 5 && format <= 8) return 12u;
    if ((format >= 9 && format <= 14) || (format >= 15 && format <= 18)) return 8u;
    if (format == 61 || format == 62 || format == 63 || format == 64 || format == 65) return 1u;
    if (format >= 53 && format <= 59) return 2u;
    return 4u;
}

std::uint32_t texture_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t format, std::uint32_t* row) {
    if (width == 0 || height == 0) {
        *row = 0;
        return 0;
    }
    const std::uint32_t bpp = texel_bytes(format);
    std::uint64_t bytes = 0;
    std::uint32_t pitch = 0;
    const bool block = (format >= 70 && format <= 84) || (format >= 94 && format <= 99);
    if (block) {
        const std::uint64_t blocks_x = (static_cast<std::uint64_t>(width) + 3u) / 4u;
        const std::uint64_t blocks_y = (static_cast<std::uint64_t>(height) + 3u) / 4u;
        pitch = static_cast<std::uint32_t>(blocks_x * bpp);
        bytes = blocks_x * blocks_y * bpp;
    } else {
        pitch = width * bpp;
        bytes = static_cast<std::uint64_t>(pitch) * height;
    }
    *row = pitch;
    if (bytes > kMapBytes) return 0;
    return static_cast<std::uint32_t>(bytes);
}

void remember(std::uint32_t obj, std::uint32_t size, std::uint32_t row) {
    StoredBytes& s = stored[obj];
    s.bytes.assign(size, 0);
    s.row = row == 0 ? size : row;
}

void copy_from_guest(std::uint32_t obj, const std::uint8_t* mem, std::uint32_t src, std::uint32_t src_row) {
    auto it = stored.find(obj);
    if (it == stored.end() || src == 0) return;
    // A whole new image for a GPU-only texture: held until sync_gpu uploads it.
    if (it->second.bytes.empty() && it->second.gpu_only) it->second.bytes.resize(it->second.full);
    if (it->second.bytes.empty()) return;
    const std::uint32_t row = it->second.row == 0 ? static_cast<std::uint32_t>(it->second.bytes.size()) : it->second.row;
    const std::uint32_t rows = row == 0 ? 0 : static_cast<std::uint32_t>(it->second.bytes.size() / row);
    if (rows <= 1 || src_row == 0 || src_row == row) {
        const std::uint32_t n = static_cast<std::uint32_t>(it->second.bytes.size());
        std::memcpy(it->second.bytes.data(), mem + src, n);
        if (stats_on()) {
            static int uploads = 0;
            bool any = false;
            const std::uint32_t step = n > 4096u ? n / 32u : 1u;
            for (std::uint32_t i = 0; i < n && !any; i += step) any = mem[src + i] != 0;
            if (any && uploads < 8) {
                ++uploads;
                std::fprintf(stderr, "upload %08x fmt %u bytes %u\n", obj, it->second.format, n);
            }
        }
        return;
    }
    for (std::uint32_t y = 0; y < rows; ++y)
        std::memcpy(it->second.bytes.data() + static_cast<std::size_t>(y) * row, mem + src + static_cast<std::size_t>(y) * src_row, row);
    if (stats_on()) {
        static int row_uploads = 0;
        bool any = false;
        for (std::uint32_t y = 0; y < rows && !any; y += std::max(rows / 16u, 1u))
            any = mem[src + static_cast<std::size_t>(y) * src_row] != 0;
        if (any && row_uploads < 8) {
            ++row_uploads;
            std::fprintf(stderr, "upload row %08x fmt %u rows %u\n", obj, it->second.format, rows);
        }
    }
}

std::uint32_t arg(const Cpu& c, unsigned i) { return ld32(c.mem, c.r[Cpu::ESP] + 4u + 4u * i); }

void ret_stdcall(Cpu& c, std::uint32_t eax, unsigned arg_bytes) {
    c.r[Cpu::EAX] = eax;
    c.r[Cpu::ESP] += 4u + arg_bytes;
}

std::uint32_t object(Kind kind) { return kObj + static_cast<std::uint32_t>(kind) * 0x100u; }
static_assert(kVt + KindCount * kVtStride <= kObj, "vtables overlap the objects");
static_assert(kObj + KindCount * 0x100u <= 0x00230000u, "objects overlap the reflection names");
static_assert(kPool + kPoolCap * 0x40u <= 0xF0000000u, "the pool reaches the thunks");

std::uint32_t method(Kind kind, std::uint32_t slot) {
    return kComCall + (static_cast<std::uint32_t>(kind) << 12) + slot;
}

bool guid_is(const std::uint8_t* mem, std::uint32_t va, std::uint32_t d1, std::uint16_t d2, std::uint16_t d3,
             std::uint64_t rest) {
    if (ld32(mem, va) != d1 || ld16(mem, va + 4) != d2 || ld16(mem, va + 6) != d3) return false;
    for (std::uint32_t i = 0; i < 8; ++i) {
        if (ld8(mem, va + 8 + i) != static_cast<std::uint8_t>(rest >> (8 * (7 - i)))) return false;
    }
    return true;
}

void refuse(Kind kind, std::uint32_t slot) {
    static const char* names[] = {
        "ID3D11Device", "ID3D11DeviceContext", "IDXGIDevice1", "IDXGIAdapter", "IDXGIFactory", "IDXGIOutput",
        "IDXGISwapChain", "ID3D11Texture2D", "ID3D11ShaderResourceView", "ID3D11SamplerState",
        "ID3D11RenderTargetView", "ID3D11Query", "ID3DBlob", "ID3D11Shader", "ID3D11ShaderReflection",
        "ID3D11Buffer", "ID3D11DepthStencilView", "ID3D11InputLayout", "ID3D11RasterizerState",
        "ID3D11DepthStencilState", "ID3D11BlendState", "IDirectInput8", "IDirectInputDevice8",
        "IDirectSound8", "IDirectSoundBuffer", "ID3D11ShaderReflectionConstantBuffer",
        "ID3D11ShaderReflectionVariable", "ID3D11ShaderReflectionType"};
    char buf[80];
    std::snprintf(buf, sizeof buf, "%s+%02X", kind < KindCount ? names[kind] : "COM", slot);
    fail_closed(Unported::Service, slot, buf);
}

void refuse_iid(const std::uint8_t* mem, std::uint32_t iid) {
    char buf[80];
    std::snprintf(buf, sizeof buf, "QueryInterface %08X-%04X-%04X", ld32(mem, iid), ld16(mem, iid + 4),
                  ld16(mem, iid + 6));
    fail_closed(Unported::Service, iid, buf);
}

// Names the reflection interfaces hand out (binding, buffer, variable and type
// names). Each distinct string gets one address for the whole run, after the pool.
constexpr std::uint32_t kStrings = 0x00230000u;
constexpr std::uint32_t kStringsEnd = 0x003F0000u;
std::uint32_t strings_used = 0;
std::unordered_map<std::string, std::uint32_t> interned;

std::uint32_t intern(std::uint8_t* mem, const std::string& text) {
    const auto it = interned.find(text);
    if (it != interned.end()) return it->second;
    const std::uint32_t at = kStrings + strings_used;
    if (at + text.size() + 1 > kStringsEnd) fail_closed(Unported::Service, at, "reflection name pool");
    std::memcpy(mem + at, text.c_str(), text.size() + 1);
    strings_used += static_cast<std::uint32_t>((text.size() + 4) & ~3u);
    interned.emplace(text, at);
    return at;
}

std::string guest_cstr(const std::uint8_t* mem, std::uint32_t va, std::size_t cap = 4096) {
    std::string out;
    if (va == 0) return out;
    for (std::size_t i = 0; i < cap && mem[va + i] != 0; ++i) out.push_back(static_cast<char>(mem[va + i]));
    return out;
}

bool commit_objects(std::uint8_t* mem) {
    static bool pool_done = false;
    if (!pool_done) {
        // Committed once; pages are backed only when an object lands on them.
        if (mprotect(mem + kPool, kPoolCap * 0x40u, PROT_READ | PROT_WRITE) != 0) return false;
        pool_done = true;
    }
    const std::uint64_t page = GuestMemory::PageSize;
    const std::uint64_t lo = kVt & ~(page - 1);
    const std::uint64_t hi = (kStringsEnd + page - 1) & ~(page - 1);
    return mprotect(mem + lo, static_cast<std::size_t>(hi - lo), PROT_READ | PROT_WRITE) == 0;
}

void publish(std::uint8_t* mem) {
    if (!commit_objects(mem)) fail_closed(Unported::Service, kObj, "D3D11 object memory");
    for (std::uint32_t kind = 0; kind < KindCount; ++kind) {
        const std::uint32_t vt = kVt + kind * kVtStride;
        const auto k = static_cast<Kind>(kind);
        for (std::uint32_t i = 0; i < kSlots; ++i) st32(mem, vt + i * 4, method(k, i * 4));
        const std::uint32_t obj = object(k);
        st32(mem, obj, vt);
        st32(mem, obj + 4, kind);
        st32(mem, obj + 8, 1);
    }
}

bool pooled(std::uint32_t obj) { return obj >= kPool && obj < kPool + kPoolCap * 0x40u && (obj - kPool) % 0x40u == 0; }

void destroy(std::uint8_t* mem, std::uint32_t obj);

std::uint32_t add_ref(std::uint8_t* mem, std::uint32_t obj) {
    if (!pooled(obj)) return 2;
    const std::uint32_t n = ld32(mem, obj + 8) + 1;
    st32(mem, obj + 8, n);
    return n;
}

std::uint32_t drop_ref(std::uint8_t* mem, std::uint32_t obj) {
    if (!pooled(obj)) return 1;
    const std::uint32_t n = ld32(mem, obj + 8);
    if (n == 0) return 0;  // already destroyed; a stale Release changes nothing
    st32(mem, obj + 8, n - 1);
    if (n == 1) destroy(mem, obj);
    return n - 1;
}

void addref(Cpu& c) { ret_stdcall(c, add_ref(c.mem, arg(c, 0)), 4); }

void release(Cpu& c) { ret_stdcall(c, drop_ref(c.mem, arg(c, 0)), 4); }

void query(Cpu& c, Kind self) {
    const std::uint32_t iid = arg(c, 1);
    const std::uint32_t out = arg(c, 2);
    Kind found = self;
    bool known = false;
    if (guid_is(c.mem, iid, 0x00000000u, 0x0000, 0x0000, 0xC000000000000046ull)) {
        known = true;
    } else if (guid_is(c.mem, iid, 0xDB6F6DDBu, 0xAC77, 0x4E88, 0x8253819DF9BBF140ull)) {
        found = Device;
        known = true;
    } else if (guid_is(c.mem, iid, 0xC0BFA96Cu, 0xE089, 0x44FB, 0x8EAF26F8796190DAull)) {
        found = Context;
        known = true;
    } else if (guid_is(c.mem, iid, 0x77DB970Fu, 0x6276, 0x48BA, 0xBA28070143B4392Cull)) {
        found = Dxgi;
        known = true;
    } else if (guid_is(c.mem, iid, 0x7B7166ECu, 0x21C7, 0x44AE, 0xB21AC9AE321AE369ull)) {
        found = Factory;
        known = true;
    } else if (self == DSoundBuf &&
               guid_is(c.mem, iid, 0x6825A449u, 0x7524, 0x4D82, 0x920F50E36AB3AB1Eull)) {
        // IID_IDirectSoundBuffer8: the same object. Its extra methods (SetFX,
        // AcquireResources, GetObjectInPath at +0x54..) still fail closed.
        found = DSoundBuf;
        known = true;
    } else if (self == DSound &&
               guid_is(c.mem, iid, 0xC50A7E93u, 0xF395, 0x4834, 0x9EF67FA99DE50966ull)) {
        // IID_IDirectSound8. CRI's SetupSound rejects the object without it.
        found = DSound;
        known = true;
    }
    if (!known) refuse_iid(c.mem, iid);
    const std::uint32_t self_obj = arg(c, 0);
    if (found == self && pooled(self_obj)) {
        // The object itself (IUnknown, or its own interface), one more reference.
        add_ref(c.mem, self_obj);
        if (out != 0) st32(c.mem, out, self_obj);
    } else if (out != 0) {
        st32(c.mem, out, object(found));
    }
    ret_stdcall(c, kSOk, 12);
}

void check_feature(Cpu& c) {
    const std::uint32_t feature = arg(c, 1);
    const std::uint32_t data = arg(c, 2);
    const std::uint32_t size = arg(c, 3);
    // D3D11_FEATURE_THREADING. Both flags stay false so the game does not
    // create resources on a second thread.
    if (feature != 0 || size < 8 || data == 0) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "CheckFeatureSupport %u", feature);
        fail_closed(Unported::Service, feature, buf);
    }
    st32(c.mem, data, 0);
    st32(c.mem, data + 4, 0);
    ret_stdcall(c, kSOk, 16);
}

// Released objects' slots, oldest first: a slot is reused as late as possible,
// so a pointer the game still holds after its last Release (D3D keeps bound
// objects alive inside the context) stays harmless for as long as can be.
std::deque<std::uint32_t> free_slots;

std::uint32_t spawn(std::uint8_t* mem, Kind kind) {
    std::uint32_t obj = 0;
    if (pool_used < kPoolCap) {
        obj = kPool + pool_used++ * 0x40u;
    } else if (!free_slots.empty()) {
        obj = free_slots.front();
        free_slots.pop_front();
    } else {
    {
        unsigned by_kind[KindCount + 1] = {};
        for (std::uint32_t i = 0; i < kPoolCap; ++i) {
            const std::uint32_t k = ld32(mem, kPool + i * 0x40u + 4);
            ++by_kind[k < KindCount ? k : KindCount];
        }
        std::fprintf(stderr, "live objects by kind:");
        for (unsigned k = 0; k <= KindCount; ++k)
            if (by_kind[k]) std::fprintf(stderr, " %u:%u", k, by_kind[k]);
        std::fprintf(stderr, "\n");
        fail_closed(Unported::Service, kPool, "D3D11 object pool: every object is still referenced");
    }
    }
    std::memset(mem + obj, 0, 0x40);
    st32(mem, obj, kVt + static_cast<std::uint32_t>(kind) * kVtStride);
    st32(mem, obj + 4, kind);
    st32(mem, obj + 8, 1);
    return obj;
}

// 0x6D62A3. A negative HRESULT fails the load. E_OUTOFMEMORY and E_INVALIDARG are logged
// the same way, so success is a buffer pointer and S_OK.
void create_buffer(Cpu& c) {
    hitch::Timer timer(hitch::Buffer);
    const std::uint32_t desc = arg(c, 1);
    const std::uint32_t out = arg(c, 3);
    if (desc == 0) fail_closed(Unported::Service, desc, "CreateBuffer");
    const std::uint32_t obj = spawn(c.mem, Buffer);
    const std::uint32_t width = ld32(c.mem, desc);
    // D3D11_BUFFER_DESC: ByteWidth, Usage, BindFlags, CPUAccessFlags,
    // MiscFlags, StructureByteStride; GetDesc hands them back.
    st32(c.mem, obj + 12, width);
    st32(c.mem, obj + 16, ld32(c.mem, desc + 4));
    st32(c.mem, obj + 20, ld32(c.mem, desc + 8));
    st32(c.mem, obj + 24, ld32(c.mem, desc + 12));
    st32(c.mem, obj + 28, ld32(c.mem, desc + 16));
    st32(c.mem, obj + 32, ld32(c.mem, desc + 20));
    if (width <= kMapBytes) {
        remember(obj, width, width);
        // An IMMUTABLE buffer never changes after this: the GPU's copy is enough.
        StoredBytes& sb = stored[obj];
        sb.full = width;
        sb.gpu_only = gpu::enabled() && ld32(c.mem, desc + 4) == 1u;
    }
    const std::uint32_t initial = arg(c, 2);
    if (initial != 0) copy_from_guest(obj, c.mem, ld32(c.mem, initial), width);
    sync_gpu(c.mem, obj);
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 16);
}

// D3D11_RENDER_TARGET_VIEW_DESC: Format, ViewDimension, then for TEXTURE2D (4)
// and TEXTURE2DARRAY (5) MipSlice first. The view keeps its resource at +12 and
// the level it writes at +16 (the bloom chain renders level after level).
void create_rtv(Cpu& c) {
    const std::uint32_t desc = arg(c, 2);
    const std::uint32_t out = arg(c, 3);
    const std::uint32_t obj = spawn(c.mem, Rtv);
    st32(c.mem, obj + 12, arg(c, 1));
    if (desc != 0) {
        const std::uint32_t dim = ld32(c.mem, desc + 4);
        if (dim == 4 || dim == 5) st32(c.mem, obj + 16, ld32(c.mem, desc + 8));
        // TEXTURE2DARRAY: FirstArraySlice, kept at +20 (a cube map's face).
        if (dim == 5) st32(c.mem, obj + 20, ld32(c.mem, desc + 12));
    }
    add_ref(c.mem, arg(c, 1));  // a view keeps its resource alive
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 16);
}

// 0x7915E5. CreateBlendState. 0x7915EC returns success when EAX is non-negative.
// D3D11_BLEND_DESC: AlphaToCoverage, IndependentBlend, then 8 targets of
// {BlendEnable, Src, Dest, Op, SrcAlpha, DestAlpha, OpAlpha, WriteMask (byte)}.
void create_blend(Cpu& c) {
    const std::uint32_t desc = arg(c, 1);
    const std::uint32_t out = arg(c, 2);
    const std::uint32_t obj = spawn(c.mem, Blend);
    if (desc != 0) {
        if (ld32(c.mem, desc) != 0) alpha_to_coverage_states.insert(obj);
        const bool independent = ld32(c.mem, desc + 4) != 0;
        for (unsigned i = 0; i < gpu::kTargets; ++i) {
            const std::uint32_t at = desc + 8 + 32 * (independent ? i : 0u);
            gpu::BlendTarget& b = blend_states[obj][i];
            b.enable = ld32(c.mem, at) != 0;
            b.src = ld32(c.mem, at + 4);
            b.dst = ld32(c.mem, at + 8);
            b.op = ld32(c.mem, at + 12);
            b.src_alpha = ld32(c.mem, at + 16);
            b.dst_alpha = ld32(c.mem, at + 20);
            b.op_alpha = ld32(c.mem, at + 24);
            b.write_mask = ld8(c.mem, at + 28);
        }
    }
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 12);
}

// 0x78F744. CreateDepthStencilState. 0x78F749 takes the failure path when EAX is negative.
// D3D11_DEPTH_STENCIL_DESC: DepthEnable, DepthWriteMask, DepthFunc, then stencil.
void create_depth_state(Cpu& c) {
    const std::uint32_t desc = arg(c, 1);
    const std::uint32_t out = arg(c, 2);
    const std::uint32_t obj = spawn(c.mem, DepthState);
    if (desc != 0) {
        gpu::DepthDesc& d = depth_states[obj];
        d.enable = ld32(c.mem, desc) != 0;
        d.write = ld32(c.mem, desc + 4) != 0;
        d.func = ld32(c.mem, desc + 8);
    }
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 12);
}

// 0x78E15E. CreateRasterizerState. 0x78E164 returns success when EAX is non-negative.
// D3D11_RASTERIZER_DESC: FillMode, CullMode, FrontCounterClockwise, DepthBias,
// DepthBiasClamp, SlopeScaledDepthBias, DepthClipEnable, ScissorEnable, ...
void create_rasterizer(Cpu& c) {
    const std::uint32_t desc = arg(c, 1);
    const std::uint32_t out = arg(c, 2);
    const std::uint32_t obj = spawn(c.mem, Raster);
    if (desc != 0) {
        gpu::RasterDesc& r = raster_states[obj];
        r.cull = ld32(c.mem, desc + 4);
        r.front_ccw = ld32(c.mem, desc + 8) != 0;
        r.scissor = ld32(c.mem, desc + 28) != 0;
    }
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 12);
}

// 0x792DD6. CreateInputLayout. 0x792DDE returns success when EAX is non-negative.
void create_layout(Cpu& c) {
    hitch::Timer timer(hitch::Shader);
    const std::uint32_t descs = arg(c, 1);
    const std::uint32_t count = arg(c, 2);
    const std::uint32_t out = arg(c, 5);
    const std::uint32_t obj = spawn(c.mem, Layout);
    LayoutDesc layout;
    const std::uint32_t n = count > 32u ? 32u : count;
    std::vector<gpu::VertexElement> elements;
    std::uint32_t next_offset[gpu::kStreams]{};
    for (std::uint32_t i = 0; i < n && descs != 0; ++i) {
        const std::uint32_t at = descs + i * 28u;
        Element e;
        copy_name(e.name, c.mem, ld32(c.mem, at));
        e.format = ld32(c.mem, at + 8);
        e.slot = ld32(c.mem, at + 12);
        e.offset = ld32(c.mem, at + 16);
        layout.elements.push_back(e);
        // D3D11_INPUT_ELEMENT_DESC: name, index, format, slot, offset, class, step.
        gpu::VertexElement g;
        g.semantic = e.name;
        g.index = ld32(c.mem, at + 4);
        g.format = e.format;
        g.slot = e.slot;
        g.offset = e.offset;
        g.instance = ld32(c.mem, at + 20) == 1;
        g.step = ld32(c.mem, at + 24);
        if (g.slot < gpu::kStreams) {
            if (g.offset == 0xFFFFFFFFu) g.offset = next_offset[g.slot];  // D3D11_APPEND_ALIGNED_ELEMENT
            next_offset[g.slot] = g.offset + texel_bytes(g.format);
        }
        elements.push_back(g);
    }
    layout_store.push_back(std::move(elements));
    gpu_layouts[obj] = StoredLayout{&layout_store.back(), static_cast<std::uint32_t>(layout_store.size())};
    layouts[obj] = std::move(layout);
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 24);
}

// 0x6DFAFD. A negative HRESULT jumps to the failure path at 0x6DF99F.
void create_dsv(Cpu& c) {
    const std::uint32_t out = arg(c, 3);
    const std::uint32_t obj = spawn(c.mem, Dsv);
    st32(c.mem, obj + 12, arg(c, 1));
    add_ref(c.mem, arg(c, 1));
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 16);
}

// 0x6B28F2. The caller stores the pointer and does not test the HRESULT.
// Query is D3D11_QUERY: 4, 1, 3, 2, then 512 of type 5.
void create_query(Cpu& c) {
    const std::uint32_t desc = arg(c, 1);
    const std::uint32_t out = arg(c, 2);
    if (desc == 0) fail_closed(Unported::Service, desc, "CreateQuery");
    const std::uint32_t obj = spawn(c.mem, Query);
    st32(c.mem, obj + 12, ld32(c.mem, desc));
    st32(c.mem, obj + 16, ld32(c.mem, desc + 4));
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 12);
}

void create_shader(Cpu& c) {
    hitch::Timer timer(hitch::Shader);
    const std::uint32_t out = arg(c, 4);
    const std::uint32_t obj = spawn(c.mem, Shader);
    shader_code[obj] = arg(c, 1);
    // Kept with the shader: the blob that held the bytecode may be released
    // and its slot reused by another compile.
    std::uint64_t embedded = 0;
    if (blob_key(c.mem, arg(c, 1), &embedded)) shader_obj_key[obj] = embedded;
    else if (const auto key = shader_key.find(arg(c, 1)); key != shader_key.end()) shader_obj_key[obj] = key->second;
    else if (stats_on()) {
        static int misses = 0;
        if (misses++ < 5) std::fprintf(stderr, "create shader from %08x: no compile key (%zu keys)\n", arg(c, 1), shader_key.size());
    }
    st32(c.mem, obj + 12, arg(c, 1));
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 20);
}

// D3D11_SAMPLER_DESC: Filter, AddressU/V/W, MipLODBias, MaxAnisotropy,
// ComparisonFunc, BorderColor[4], MinLOD, MaxLOD.
void create_sampler(Cpu& c) {
    const std::uint32_t desc = arg(c, 1);
    const std::uint32_t out = arg(c, 2);
    const std::uint32_t obj = spawn(c.mem, Sampler);
    if (desc != 0 && gpu::enabled()) {
        gpu::SamplerDesc d;
        d.filter = ld32(c.mem, desc);
        for (unsigned i = 0; i < 3; ++i) d.address[i] = ld32(c.mem, desc + 4 + 4 * i);
        std::memcpy(&d.mip_bias, c.mem + desc + 16, 4);
        d.max_aniso = ld32(c.mem, desc + 20);
        d.compare = ld32(c.mem, desc + 24);
        std::memcpy(d.border, c.mem + desc + 28, 16);
        std::memcpy(&d.min_lod, c.mem + desc + 44, 4);
        std::memcpy(&d.max_lod, c.mem + desc + 48, 4);
        gpu::create_sampler(obj, d);
    }
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 12);
}

void create_srv(Cpu& c) {
    const std::uint32_t out = arg(c, 3);
    const std::uint32_t obj = spawn(c.mem, Srv);
    ++g_srvs;
    if (g_srvs <= 3 && stats_on())
        std::fprintf(stderr, "create_srv from %08x res %08x\n", ld32(c.mem, c.r[Cpu::ESP]), arg(c, 1));
    st32(c.mem, obj + 12, arg(c, 1));
    // D3D11_SHADER_RESOURCE_VIEW_DESC: Format, ViewDimension, then for TEXTURE2D
    // (4), TEXTURE2DARRAY (5), TEXTURECUBE (9) and TEXTURECUBEARRAY (10)
    // MostDetailedMip and MipLevels (-1: the rest). Kept at +16 and +20.
    if (const std::uint32_t desc = arg(c, 2); desc != 0) {
        const std::uint32_t dim = ld32(c.mem, desc + 4);
        if (dim == 4 || dim == 5 || dim == 9 || dim == 10) {
            st32(c.mem, obj + 16, ld32(c.mem, desc + 8));
            const std::uint32_t levels = ld32(c.mem, desc + 12);
            st32(c.mem, obj + 20, levels == 0xFFFFFFFFu ? 0u : levels);
        }
    }
    add_ref(c.mem, arg(c, 1));
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 16);
}

void create_texture(Cpu& c) {
    hitch::Timer timer(hitch::Texture);
    const std::uint32_t desc = arg(c, 1);
    const std::uint32_t out = arg(c, 3);
    if (desc == 0) fail_closed(Unported::Service, desc, "CreateTexture2D");
    const std::uint32_t obj = spawn(c.mem, Texture);
    if (stats_on() && ld32(c.mem, desc + 20) > 1)
        std::fprintf(stderr, "MSAA texture %08x %ux%u fmt %u samples %u bind %x from %08x\n", obj, ld32(c.mem, desc),
                     ld32(c.mem, desc + 4), ld32(c.mem, desc + 16), ld32(c.mem, desc + 20), ld32(c.mem, desc + 32),
                     ld32(c.mem, c.r[Cpu::ESP]));
    st32(c.mem, obj, kVt + static_cast<std::uint32_t>(Texture) * kVtStride);
    st32(c.mem, obj + 4, Texture);
    st32(c.mem, obj + 8, 1);
    st32(c.mem, obj + 12, ld32(c.mem, desc));
    st32(c.mem, obj + 16, ld32(c.mem, desc + 4));
    st32(c.mem, obj + 20, ld32(c.mem, desc + 16));
    st32(c.mem, obj + 24, ld32(c.mem, desc + 8));
    st32(c.mem, obj + 28, ld32(c.mem, desc + 32));
    std::uint32_t row = 0;
    const std::uint32_t bytes = texture_bytes(ld32(c.mem, desc), ld32(c.mem, desc + 4), ld32(c.mem, desc + 16), &row);
    if (bytes != 0) {
        remember(obj, bytes, row);
        StoredBytes& sb = stored[obj];
        sb.format = ld32(c.mem, desc + 16);
        sb.full = bytes;
        sb.gpu_only = gpu::enabled() && ld32(c.mem, desc + 28) <= 1u && ld32(c.mem, desc + 36) == 0;
        // Filled by initial data or UpdateSubresource, which bring their own bytes.
        if (sb.gpu_only) drop_cpu_copy(sb);
        ++g_textures;
        if (stats_on() && (bytes > 4096u || g_textures <= 4))
            std::fprintf(stderr, "tex %u %ux%u fmt %u bytes %u\n", g_textures, ld32(c.mem, desc),
                         ld32(c.mem, desc + 4), ld32(c.mem, desc + 16), bytes);
    }
    if (gpu::enabled()) {
        gpu::TextureDesc td;
        td.width = ld32(c.mem, desc);
        td.height = ld32(c.mem, desc + 4);
        td.mips = ld32(c.mem, desc + 8);
        td.array = ld32(c.mem, desc + 12);
        td.format = ld32(c.mem, desc + 16);
        td.bind = ld32(c.mem, desc + 32);
        td.misc = ld32(c.mem, desc + 40);
        td.initial = arg(c, 2) != 0;
        td.samples = std::max(1u, ld32(c.mem, desc + 20));
        gpu::create_texture(obj, td);
    }
    const std::uint32_t initial = arg(c, 2);
    std::uint32_t mips = ld32(c.mem, desc + 8);
    const std::uint32_t slices = std::max(1u, ld32(c.mem, desc + 12));
    if (mips == 0)  // MipLevels 0: the full chain
        for (std::uint32_t w = std::max(ld32(c.mem, desc), ld32(c.mem, desc + 4)); w > 0; w >>= 1) ++mips;
    if (initial != 0) {
        copy_from_guest(obj, c.mem, ld32(c.mem, initial), ld32(c.mem, initial + 4));
        if (slices == 1 && mips == 1) {
            sync_gpu(c.mem, obj);
        } else if (gpu::enabled()) {
            // Mip levels, array slices or cube faces: every subresource, D3D11_SUBRESOURCE_DATA
            // {pSysMem, SysMemPitch, SysMemSlicePitch} in the order mip + slice * mips.
            const std::uint32_t w = ld32(c.mem, desc), h = ld32(c.mem, desc + 4), format = ld32(c.mem, desc + 16);
            for (std::uint32_t slice = 0; slice < slices; ++slice) {
                for (std::uint32_t mip = 0; mip < mips; ++mip) {
                    const std::uint32_t sub = initial + 12u * (mip + slice * mips);
                    std::uint32_t row = 0;
                    const std::uint32_t bytes = texture_bytes(std::max(1u, w >> mip), std::max(1u, h >> mip), format, &row);
                    const std::uint32_t pitch = ld32(c.mem, sub + 4) != 0 ? ld32(c.mem, sub + 4) : row;
                    const std::uint32_t rows = row != 0 ? bytes / row : 0;
                    if (ld32(c.mem, sub) != 0 && bytes != 0)
                        gpu::upload_texture_sub(obj, slice, mip, c.mem + ld32(c.mem, sub), static_cast<std::size_t>(pitch) * rows, pitch);
                }
            }
            if (auto it = stored.find(obj); it != stored.end()) drop_cpu_copy(it->second);
        }
    }
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 16);
}

// 0x6C94BF sets WKPDID_D3DDebugObjectName and ignores the HRESULT.
void set_private(Cpu& c) { ret_stdcall(c, kSOk, 16); }

void texture_desc(Cpu& c) {
    const std::uint32_t self = arg(c, 0);
    const std::uint32_t desc = arg(c, 1);
    if (desc != 0) {
        std::memset(c.mem + desc, 0, 44);
        st32(c.mem, desc, ld32(c.mem, self + 12));
        st32(c.mem, desc + 4, ld32(c.mem, self + 16));
        st32(c.mem, desc + 8, ld32(c.mem, self + 24));
        st32(c.mem, desc + 12, 1);
        st32(c.mem, desc + 16, ld32(c.mem, self + 20));
        st32(c.mem, desc + 20, 1);
        st32(c.mem, desc + 32, ld32(c.mem, self + 28));
    }
    ret_stdcall(c, kSOk, 8);
}

// MSAA, 4x by default (the game's own anti-aliasing): the game is told that
// many samples are supported and draws its scene into multisampled targets,
// which Metal resolves for everything that reads them. DCUSA_MSAA=2 for 2x,
// DCUSA_MSAA=0 for none (then FXAA, DCUSA_AA, is on by default instead).
std::uint32_t msaa_samples() {
    static const std::uint32_t n = [] {
        const char* v = std::getenv("DCUSA_MSAA");
        const int k = v ? std::atoi(v) : 4;
        return k >= 4 ? 4u : k >= 2 ? 2u : k == 1 ? 4u : 0u;
    }();
    return n;
}

void check_msaa(Cpu& c) {
    const std::uint32_t count = arg(c, 2);
    const std::uint32_t out = arg(c, 3);
    if (stats_on()) std::fprintf(stderr, "CheckMultisampleQualityLevels format %u count %u\n", arg(c, 1), count);
    const std::uint32_t most = msaa_samples();
    if (out != 0) st32(c.mem, out, count == 1 || (count == 2 && most >= 2) || (count == 4 && most >= 4) ? 1u : 0u);
    ret_stdcall(c, kSOk, 16);
}

void get_adapter(Cpu& c) {
    const std::uint32_t out = arg(c, 1);
    if (out != 0) st32(c.mem, out, object(Adapter));
    ret_stdcall(c, kSOk, 8);
}

void get_desc(Cpu& c) {
    const std::uint32_t desc = arg(c, 1);
    if (desc == 0) {
        ret_stdcall(c, 0x80070057u, 8);
        return;
    }
    std::memset(c.mem + desc, 0, 292);
    const char name[] = "Port";
    for (std::uint32_t i = 0; name[i] != 0; ++i) st16(c.mem, desc + i * 2, static_cast<std::uint16_t>(name[i]));
    st32(c.mem, desc + 256, 0x1002);
    st32(c.mem, desc + 260, 0x67DF);
    st32(c.mem, desc + 272, 512u << 20);
    st32(c.mem, desc + 280, 256u << 20);
    ret_stdcall(c, kSOk, 8);
}

void get_parent(Cpu& c) {
    const std::uint32_t iid = arg(c, 1);
    const std::uint32_t out = arg(c, 2);
    if (!guid_is(c.mem, iid, 0x7B7166ECu, 0x21C7, 0x44AE, 0xB21AC9AE321AE369ull)) refuse_iid(c.mem, iid);
    if (out != 0) st32(c.mem, out, object(Factory));
    ret_stdcall(c, kSOk, 12);
}

void enum_outputs(Cpu& c) {
    const std::uint32_t index = arg(c, 1);
    const std::uint32_t out = arg(c, 2);
    if (index != 0) {
        ret_stdcall(c, kNotFound, 12);
        return;
    }
    if (out != 0) st32(c.mem, out, object(Output));
    ret_stdcall(c, kSOk, 12);
}

}  // namespace

void display_size(std::uint32_t* width, std::uint32_t* height) {
    static const std::pair<std::uint32_t, std::uint32_t> size = [] {
        std::uint32_t w = 1920, h = 1080;
        if (const char* v = std::getenv("DCUSA_RESOLUTION")) {
            unsigned a = 0, b = 0;
            if (std::sscanf(v, "%ux%u", &a, &b) == 2 && a >= 320 && b >= 180 && a <= 7680 && b <= 4320) w = a, h = b;
        }
        return std::make_pair(w, h);
    }();
    *width = size.first;
    *height = size.second;
}

namespace {

void mode_list(Cpu& c) {
    const std::uint32_t format = arg(c, 1);
    const std::uint32_t count = arg(c, 3);
    const std::uint32_t modes = arg(c, 4);
    if (count != 0) st32(c.mem, count, 1);
    if (modes != 0) {
        std::uint32_t w = 0, h = 0;
        display_size(&w, &h);
        st32(c.mem, modes, w);
        st32(c.mem, modes + 4, h);
        st32(c.mem, modes + 8, 60);
        st32(c.mem, modes + 12, 1);
        st32(c.mem, modes + 16, format);
        st32(c.mem, modes + 20, 0);
        st32(c.mem, modes + 24, 0);
    }
    ret_stdcall(c, kSOk, 20);
}

void create_swap(Cpu& c) {
    const std::uint32_t out = arg(c, 3);
    if (out != 0) st32(c.mem, out, object(Swap));
    ret_stdcall(c, kSOk, 16);
}

void associate(Cpu& c) { ret_stdcall(c, kSOk, 12); }

void set_fullscreen(Cpu& c) { ret_stdcall(c, kSOk, 12); }

// 0x6BBA27 and 0x6BBA31. Present. Both callers ignore the HRESULT.
// DCUSA_WATCH="1735608 [16791AC]+4C ..." prints those guest dwords every
// DCUSA_EVERY presents (default 100): hex addresses, [x] reads a dword, +off adds.
// Only memory that is always committed is read (the image, the low TEB / PEB
// pages, both fixed heaps); anything else prints "?".
bool watch_readable(std::uint32_t a) {
    return (a >= 0x10000u && a < 0x40000u) || (a >= 0x400000u && a < 0x2400000u) || guest_heap_committed(a);
}

bool watch_eval(const std::uint8_t* mem, const char*& p, std::uint32_t* out) {
    std::uint32_t v = 0;
    if (*p == '[') {
        ++p;
        std::uint32_t inner = 0;
        if (!watch_eval(mem, p, &inner) || *p != ']') return false;
        ++p;
        if (!watch_readable(inner)) return false;
        v = ld32(mem, inner);
    } else {
        char* end = nullptr;
        v = static_cast<std::uint32_t>(std::strtoul(p, &end, 16));
        if (end == p) return false;
        p = end;
    }
    while (*p == '+') {
        ++p;
        char* end = nullptr;
        v += static_cast<std::uint32_t>(std::strtoul(p, &end, 16));
        p = end;
    }
    *out = v;
    return true;
}

void print_watch(const std::uint8_t* mem, int present_no) {
    const char* list = std::getenv("DCUSA_WATCH");
    if (list == nullptr) return;
    const char* every_env = std::getenv("DCUSA_EVERY");
    const int every = every_env ? std::max(1, std::atoi(every_env)) : 100;
    if (present_no != 1 && present_no % every != 0) return;
    std::string text(list);
    static const auto t0 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "watch %d @%.1fs:", present_no, secs);
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find(' ', at);
        const std::string token = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        if (token.empty()) continue;
        const char* p = token.c_str();
        std::uint32_t addr = 0;
        if (watch_eval(mem, p, &addr) && watch_readable(addr)) std::fprintf(stderr, " %s=%x", token.c_str(), ld32(mem, addr));
        else std::fprintf(stderr, " %s=?", token.c_str());
    }
    std::fprintf(stderr, " probes %llu %llu\n", static_cast<unsigned long long>(g_probe[0]),
                 static_cast<unsigned long long>(g_probe[1]));
}

// The game presents with SyncInterval 0, and on the cabinet it still runs at
// the display's 60 Hz (the capture shows 60 frames a second), so a present
// returns no earlier than one 60 Hz period after the last one (SyncInterval
// periods when it is above 1). DCUSA_NOPACE=1 runs unthrottled (benchmarks).
// The same on iOS, although the layer's drawables also wait for the display:
// running unpaced there, or paced a little faster (15.5 ms) so the two would
// not stack, made races render wrong on an iPhone (static shadows and the
// course map black): the game needs its frames no faster than 60 Hz.
// The wait is given to the other guest threads first (DCUSA_PACE_IDLE=0 sleeps
// through it): they only run when the main thread yields, and the boot's
// loaders otherwise sat idle for most of every paced frame.
// Set while the boot's "Data Loading" runs unpaced (below): a frame then goes
// to the display only when the last one shown is a refresh old, since taking a
// drawable waits for the display and would pace the boot all the same.
bool g_boot_loading = false;
std::uint32_t g_boot_state = 0;  // [0x1735608], read at each present
int g_boot_percent = -1;         // "Data Loading" percentage (boot_progress), -1 before the first

// DCUSA_BOOT_SCREEN=1: until the boot state reaches 8 (attract), the display
// shows the attract title with a progress bar (gpu/boot_screen) instead of
// "Data Loading" and the network check. The title image is
// DCUSA_BOOT_SCREEN_IMAGE, else <prepared>/boot/title.png beside the shader
// folder (tools/bootscreen/capture_title.py makes it). Frame dumps still
// read the game's own frame.
constexpr std::uint32_t kBootScreenTexture = 0xFFFFB007u;
constexpr std::uint32_t kOverlayTexture = 0xFFFFB008u;

// DCUSA_SHOW_FPS=1: the pictures shown per second and the build, lower left
// (gpu::set_overlay), refreshed twice a second; R3 turns it on and off
// (toggle_fps_overlay). Called for every picture shown.
std::atomic<int> g_fps_overlay{-1};  // -1 not read yet, 0 off, 1 on

std::uint32_t race_timer(const std::uint8_t* mem);

// The clocks it also shows, so a moment of a recording can be found again:
// A, the time since attract began in game ticks (1/60 s, the same moment in
// any run of the same script), and R, the race timer the HUD shows (when one
// runs; demos included).
void count_shown_picture(int attract_ticks, std::uint32_t race64) {
    if (g_fps_overlay.load(std::memory_order_relaxed) < 0) {
        const char* e = std::getenv("DCUSA_SHOW_FPS");
        int expected = -1;
        g_fps_overlay.compare_exchange_strong(expected, e != nullptr && std::atoi(e) != 0 ? 1 : 0);
    }
    static auto since = std::chrono::steady_clock::now();
    static int shown = 0;
    static double fps = 0;
    static bool created = false, showing = false;
    const bool on = g_fps_overlay.load(std::memory_order_relaxed) == 1;
    if (!on) {
        if (showing) gpu::set_overlay(0);
        showing = false;
        return;
    }
    if (created && !showing) gpu::set_overlay(kOverlayTexture);
    showing = true;
    ++shown;
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - since).count();
    if (s >= 0.5 || !created) {
        fps = created ? shown / s : 0.0;
        since = now;
        shown = 0;
    }
    // The text redrawn ten times a second (CoreGraphics on the game thread).
    static auto drawn = std::chrono::steady_clock::time_point{};
    if (created && now - drawn < std::chrono::milliseconds(100)) return;
    drawn = now;
    char text[160];
    int n = std::snprintf(text, sizeof text, "%.0f FPS   %s", fps, DCUSA_BUILD_ID);
    if (attract_ticks >= 0 && n > 0 && n < static_cast<int>(sizeof text))
        n += std::snprintf(text + n, sizeof text - n, "   A %d:%04.1f", attract_ticks / 3600, (attract_ticks % 3600) / 60.0);
    if (race64 != 0 && n > 0 && n < static_cast<int>(sizeof text))
        std::snprintf(text + n, sizeof text - n, "   R %u:%05.2f", race64 / 3840, (race64 % 3840) / 64.0);
    std::uint32_t w = 0, h = 0;
    const auto& px = gpu::overlay_compose(text, &w, &h);
    if (!created) {
        gpu::TextureDesc d;
        d.width = w;
        d.height = h;
        d.format = 28;  // R8G8B8A8_UNORM
        d.bind = 0x8;
        gpu::create_texture(kOverlayTexture, d);
        gpu::set_overlay(kOverlayTexture);
        created = true;
    }
    gpu::upload_texture(kOverlayTexture, px.data(), px.size(), w * 4);
}
bool boot_screen_shown() {
    static const bool on = [] {
        const char* e = std::getenv("DCUSA_BOOT_SCREEN");
        if (e == nullptr || std::atoi(e) == 0) return false;
        std::string path;
        if (const char* img = std::getenv("DCUSA_BOOT_SCREEN_IMAGE")) path = img;
        else if (const char* shaders = std::getenv("DCUSA_SHADERS"))
            path = (std::filesystem::path(shaders).parent_path() / "boot" / "title.png").string();
        // DCUSA_BOOT_SCREEN_FONT=<ttf>, else <prepared>/boot/font.ttf when there is one.
        std::string font;
        if (const char* f = std::getenv("DCUSA_BOOT_SCREEN_FONT")) font = f;
        else if (!path.empty()) font = (std::filesystem::path(path).parent_path() / "font.ttf").string();
        if (!font.empty() && std::filesystem::exists(font)) gpu::boot_screen_font(font);
        gpu::boot_screen_stamp("build " DCUSA_BUILD_ID);
        if (path.empty() || !gpu::boot_screen_load(path)) {
            std::fprintf(stderr, "boot screen: no title image at %s\n", path.c_str());
            return false;
        }
        return true;
    }();
    if (!on) return false;
    // After the state turns 8 the game still shows its network screen (about
    // 45 presents), then black (about 60), then the CRIWARE logo on white:
    // hold the boot screen 110 presents more, so it gives way to the logo.
    static int after = 0;
    if (g_boot_state < 8) return true;
    return after++ < 110;
}

// The boot screen's picture for this present: uploaded when its text changes.
void show_boot_screen() {
    static std::string shown;
    static bool created = false;
    std::string label;
    if (g_boot_state == 0) {
        label = g_boot_percent < 0 ? "Loading" : "Loading  " + std::to_string(g_boot_percent) + "%";
    } else {
        // Blinks like the game's PRESS START: 0.6 s on, 0.4 s off.
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (ms % 1000 < 600) label = "Checking network";
    }
    const int percent = g_boot_state == 0 ? std::max(0, g_boot_percent) : -1;
    const std::string key = label + "|" + std::to_string(percent);
    if (key != shown) {
        if (std::getenv("DCUSA_SHOW_TRACE")) std::fprintf(stderr, "boot screen: %s (state %u)\n", key.c_str(), g_boot_state);
        std::uint32_t w = 0, h = 0;
        const auto& px = gpu::boot_screen_compose(percent, label, &w, &h);
        if (!created) {
            gpu::TextureDesc d;
            d.width = w;
            d.height = h;
            d.format = 28;  // R8G8B8A8_UNORM
            d.bind = 0x8;
            gpu::create_texture(kBootScreenTexture, d);
            created = true;
        }
        gpu::upload_texture(kBootScreenTexture, px.data(), px.size(), w * 4);
        // DCUSA_BOOT_SCREEN_DUMP=<prefix>: the picture at 0, 50 and 100% and at
        // the network check, as PPM (a headless run has no window to look at).
        if (const char* dump = std::getenv("DCUSA_BOOT_SCREEN_DUMP")) {
            static std::set<std::string> written;
            const std::string tag = percent < 0 ? (label.empty() ? "" : "network") : percent == 0 || percent == 50 || percent == 100 ? std::to_string(percent) : "";
            if (!tag.empty() && written.insert(tag).second) {
                if (std::FILE* f = std::fopen((std::string(dump) + "." + tag + ".ppm").c_str(), "wb")) {
                    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                    for (std::size_t i = 0; i < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
                    std::fclose(f);
                }
            }
        }
        shown = key;
    }
    gpu::show(kBootScreenTexture);
}

// Frame skip. The game's clock is its frames, so a frame later than 60 Hz
// slows the whole game down (an iPhone race at 20 ms a frame ran at 80%).
// When a tick reaches the scene render (0x4DC120, about 80% of a race frame's
// main thread) after its present was due, the render is left out: the game
// logic still runs, the display keeps the last picture, and the pacing
// catches up. At most kMaxSkips ticks in a row are skipped.
// DCUSA_FRAME_SKIP=0 off, 1 when late (iOS default), 2 every other tick (a test).
std::chrono::steady_clock::time_point g_pace_next{};
bool g_render_skipped = false;
// The main thread's time per tick, from the end of one present's pacing to the
// next present (the game's own work and any wait on the render thread), kept
// apart for ticks that rendered and ticks that skipped (DCUSA_SKIP_LOG).
std::chrono::steady_clock::time_point g_tick_start{};
double g_gpu_sum = 0, g_gpu_max = 0;
int g_gpu_n = 0;
double g_tick_ms[2]{}, g_tick_cpu_ms[2]{}, g_tick_draws[2]{};
std::uint32_t g_tick_n[2]{};
// The slowest tick of each logged interval: its wall and CPU time, present
// number and draws, and the time since the first present (DCUSA_SKIP_LOG).
double g_slow_pipe = 0, g_slow_tex = 0; unsigned g_slow_pipes = 0;
double g_slow_ms = 0, g_slow_cpu = 0, g_slow_handover = 0, g_slow_preempt = 0, g_slow_yield = 0, g_slow_wait = 0;
std::uint64_t g_tick_preempt0 = 0, g_tick_yield0 = 0;
std::uint64_t g_tick_turn0[32];  // sched_thread_time at the tick's start
std::uint32_t g_slow_thread = 0;  // the slowest frame's busiest other thread (start address)
double g_slow_thread_ms = 0;
int g_slow_present = 0;
std::uint32_t g_slow_draws = 0;
std::uint32_t g_slow_race = 0;  // the race timer then

// The race timer the HUD shows top left (total race time, lap times added),
// in 1/64 s: 0x19C98EC, found by its values against the HUD in Mac races.
// The slowest-frame log gives it, so a moment from a phone run can be found
// again in a Mac run (DCUSA_SKIP_LOG; 0 outside the image).
std::uint32_t race_timer(const std::uint8_t* mem) {
    constexpr std::uint32_t kRaceTimer = 0x19C98ECu;
    return g_image_lo <= kRaceTimer && kRaceTimer + 4u <= g_image_hi ? ld32(mem, kRaceTimer) : 0u;
}
double g_tick_cpu_start = 0;
double thread_cpu_ms() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
int g_skip_run = 0;
constexpr int kMaxSkips = 2;
std::uint64_t g_skipped_total = 0;

int frame_skip_mode() {
    static const int mode = [] {
        const char* e = std::getenv("DCUSA_FRAME_SKIP");
        if (e != nullptr) return std::atoi(e);
        return TARGET_OS_IPHONE ? 1 : 0;
    }();
    return mode;
}

void pace_present(Cpu& c, std::uint32_t sync_interval) {
    using clock = std::chrono::steady_clock;
    static const bool off = std::getenv("DCUSA_NOPACE") != nullptr;
    static const bool give = [] {
        const char* e = std::getenv("DCUSA_PACE_IDLE");
        return e == nullptr || std::atoi(e) != 0;
    }();
    // DCUSA_BOOT_PACE=1 paces the boot too. Until the boot state [0x1735608]
    // (2019) leaves 0 the screen is the static "Data Loading" text, and the
    // load is gated by presents: the main thread drains the resource queue
    // once a frame. Unpaced, that part takes about a third of the time, but in
    // about 12,600 presents instead of 7,000, so a pad script (DCUSA_PAD_SCRIPT,
    // timed in presents) keeps the boot paced.
    // DCUSA_BOOT_PACE=0 leaves it unpaced even with a script.
    static const bool boot_paced = [] {
        if (const char* v = std::getenv("DCUSA_BOOT_PACE")) return std::atoi(v) != 0;
        return std::getenv("DCUSA_PAD_SCRIPT") != nullptr;
    }();
    static clock::time_point& next = g_pace_next;
    // Each boot state change goes to the log with the time since the first present.
    static const clock::time_point first = clock::now();
    static std::uint32_t last_state = 0;
    static std::uint32_t presents = 0;
    ++presents;
    // Only a loaded game image has the boot state (a test's guest memory has no image there).
    const std::uint32_t state = g_image_lo <= 0x1735608u && 0x1735608u + 4u <= g_image_hi ? ld32(c.mem, 0x1735608u) : 8u;
    g_boot_state = state;
    if (state != last_state) {
        std::fprintf(stderr, "boot state %u at %.1f s, present %u\n", state,
                     std::chrono::duration<double>(clock::now() - first).count(), presents);
        last_state = state;
    }
    g_boot_loading = !boot_paced && state == 0;
    if (off) return;
    if (g_boot_loading) {
        next = {};
        return;
    }
    const auto period = std::chrono::nanoseconds(16'666'667) * std::max(1u, sync_interval);
    const auto now = clock::now();
    // Behind by more than a period (with frame skip, four): start over from now
    // rather than run fast to catch up.
    const auto slack = frame_skip_mode() != 0 ? period * 4 : period;
    if (next.time_since_epoch().count() == 0 || now > next + slack) next = now;
    else {
        if (give) sched_run_others_until(c, next);
        std::this_thread::sleep_until(next);
    }
    next += period;
}

// DCUSA_PAD_TRACE=1: when the pad changes (and for the next three presents),
// the snapshot and what the game made of it: player 0's switch bits
// at 0x15964A0 (+0x30 held, +0x20 newly pressed) and analogs (+0x40 steering
// 0..0xFF00, +0x44 accel, +0x48 brake 0..0x10000), the switch image the race
// reads (0x19C87F8: steer, accel, brake bytes; 0x19C8800/01 switch bytes), the
// gear index 0x19C880C (0..3) and the credit count (0x15278F8 + 7).
void print_pad_trace(const std::uint8_t* mem, int present_no) {
    static const bool on = std::getenv("DCUSA_PAD_TRACE") != nullptr;
    static dcusa::PadSnapshot last;
    static int left = 0;
    if (!on) return;
    const dcusa::PadSnapshot& pad = current_pad();
    if (pad.buttons != last.buttons || pad.steering != last.steering || pad.accelerator != last.accelerator ||
        pad.brake != last.brake)
        left = 4;
    last = pad;
    if (left == 0 && present_no % 120 != 0) return;
    if (left > 0) --left;
    constexpr std::uint32_t slot = 0x15964A0u;
    std::fprintf(stderr,
                 "pad %d: snap btn %04x steer %+.2f acc %.2f brk %.2f | held %08x%08x new %08x%08x "
                 "steer %05x acc %05x brk %05x | jvs %02x %02x %02x sw %02x %02x gear %u credit %u\n",
                 present_no, pad.buttons, pad.steering, pad.accelerator, pad.brake, ld32(mem, slot + 0x34),
                 ld32(mem, slot + 0x30), ld32(mem, slot + 0x24), ld32(mem, slot + 0x20), ld32(mem, slot + 0x40),
                 ld32(mem, slot + 0x44), ld32(mem, slot + 0x48), mem[0x19C87F8u], mem[0x19C87F9u], mem[0x19C87FAu],
                 mem[0x19C8800u], mem[0x19C8801u], ld32(mem, 0x19C880Cu), mem[0x15278F8u + 7u]);
}

// DCUSA_SELFTEST_MATH=1: at the first present (the CRT is initialised by then,
// so its SSE2 / x87 choice is made), runs the translated CRT cos and sqrt
// wrappers (argument and result in ST(0)) and prints them next to the host's.
void selftest_math(Cpu& c) {
    static bool done = false;
    if (done || std::getenv("DCUSA_SELFTEST_MATH") == nullptr) return;
    done = true;
    const struct {
        const char* name;
        std::uint32_t fn;
        double (*host)(double);
    } fns[] = {{"cos", 0x00C397F0u, [](double x) { return std::cos(x); }},
               {"sqrt", 0x00C398E0u, [](double x) { return std::sqrt(x); }},
               {"log?", 0x00C399E0u, [](double x) { return std::log(x); }},
               {"tan?", 0x00C3AC00u, [](double x) { return std::tan(x); }},
               {"sin?", 0x00C3AD30u, [](double x) { return std::sin(x); }}};
    const double args[] = {0.0, 0.0654498, 0.5, 1.0, 2.0, 3.14159265, 10.0, 1000.0};
    for (const auto& f : fns)
        for (double x : args) {
            const unsigned depth = c.fpu.top;
            c.fpu.push(x);
            call_guest(c, f.fn, {});
            const double got = c.fpu.at(0);
            c.fpu.pop();
            std::fprintf(stderr, "selftest %s(%.9g) = %.12g, host %.12g%s (fpu top %u -> %u)\n", f.name, x, got, f.host(x),
                         std::fabs(got - f.host(x)) > 1e-9 ? "  MISMATCH" : "", depth, c.fpu.top);
        }
    // _CIpow (0xC39410): x in ST(1), y in ST(0); the result replaces both.
    const double pairs[][2] = {{2, 0.5}, {10, 2}, {10, -0.19}, {0.5, 3}, {10, 0.05}, {2, -1}, {1.5, 2.2}};
    for (const auto& p : pairs) {
        const unsigned depth = c.fpu.top;
        c.fpu.push(p[0]);
        c.fpu.push(p[1]);
        call_guest(c, 0x00C39410u, {});
        const double got = c.fpu.at(0);
        c.fpu.pop();
        const double want = std::pow(p[0], p[1]);
        std::fprintf(stderr, "selftest pow(%g, %g) = %.12g, host %.12g%s (fpu top %u -> %u)\n", p[0], p[1], got, want,
                     std::fabs(got - want) > 1e-9 * std::max(1.0, std::fabs(want)) ? "  MISMATCH" : "", depth, c.fpu.top);
    }
}

#if DCUSA_PGO_GENERATE
extern "C" int __llvm_profile_write_file(void);  // the profile runtime's
#endif

// DCUSA_HITCH=<ms> (runtime/hitch.hpp): one line for each frame that took
// longer, with the time host code spent in each slot during it.
void report_hitch(int present_no) {
    if (!hitch::enabled()) return;
    using clock = std::chrono::steady_clock;
    static const double limit = std::atof(std::getenv("DCUSA_HITCH"));
    static clock::time_point last{};
    const auto now = clock::now();
    const double ms = last.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(now - last).count() : 0;
    last = now;
    static const char* const names[hitch::Count] = {"io", "wait", "texture", "buffer", "shader", "render-thread", "pipeline"};
    if (ms > limit) {
        // With DCUSA_FRAME set, the frames presented right after a hitch are
        // written too: what was on screen when it happened.
        if (std::getenv("DCUSA_FRAME") != nullptr && ms > 2 * limit) g_frames_after_hitch = 2;
        std::fprintf(stderr, "hitch present %d: %.1f ms |", present_no, ms);
        for (int i = 0; i < hitch::Count; ++i)
            if (hitch::g_calls[i])
                std::fprintf(stderr, " %s %.1f ms (%u)", names[i], hitch::g_ns[i] / 1e6, hitch::g_calls[i].load());
        std::fprintf(stderr, "\n");
    }
    for (int i = 0; i < hitch::Count; ++i) {
        hitch::g_ns[i] = 0;
        hitch::g_calls[i] = 0;
    }
}

// DCUSA_MEMDUMP=<file prefix>:<n>,<n>,...: at those presents after attract
// starts, the guest's committed memory (as DCUSA_WATCH reads it) to
// <prefix>.<n>: runs of [u32 address][u32 length][bytes]. For finding a value
// by how it changes between dumps (the race timer, say).
void memdump(const std::uint8_t* mem, int present_no) {
    static const char* spec = std::getenv("DCUSA_MEMDUMP");
    if (spec == nullptr) return;
    static int attract_at = 0;
    if (attract_at == 0 && g_boot_state >= 8) attract_at = present_no;
    if (attract_at == 0) return;
    const char* colon = std::strrchr(spec, ':');
    if (colon == nullptr) return;
    const int n = present_no - attract_at;
    bool want = false;
    for (const char* p = colon + 1; *p;) {
        char* end = nullptr;
        if (std::strtol(p, &end, 10) == n) want = true;
        p = *end == ',' ? end + 1 : end;
        if (end == p && *p) break;
    }
    if (!want) return;
    const std::string path = std::string(spec, colon) + "." + std::to_string(n);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    constexpr std::uint32_t kPage = 0x1000;
    std::uint32_t run = 0, len = 0;
    auto flush = [&] {
        if (len == 0) return;
        std::fwrite(&run, 4, 1, f);
        std::fwrite(&len, 4, 1, f);
        // Through a copy: the file write cannot read the guest's pages directly.
        static std::vector<std::uint8_t> buf;
        buf.assign(mem + run, mem + run + len);
        if (std::fwrite(buf.data(), 1, len, f) != len) std::fprintf(stderr, "memdump: short write at %x\n", run);
        len = 0;
    };
    // Pages the host cannot read (reserved, no access) are left out: each is
    // copied with vm_read_overwrite, which fails instead of faulting.
    static std::vector<std::uint8_t> page(kPage);
    auto mapped = [&](std::uint64_t a) {
        vm_size_t got = 0;
        return vm_read_overwrite(mach_task_self(), reinterpret_cast<vm_address_t>(mem + a), kPage,
                                 reinterpret_cast<vm_address_t>(page.data()), &got) == KERN_SUCCESS &&
               got == kPage;
    };
    // DCUSA_MEMDUMP_IMAGE=1: only the program image (its globals).
    static const bool image_only = std::getenv("DCUSA_MEMDUMP_IMAGE") != nullptr;
    const std::uint64_t lo = image_only ? g_image_lo : 0x10000, hi = image_only ? g_image_hi : 0x80000000ull;
    for (std::uint64_t a = lo / kPage * kPage; a < hi; a += kPage) {
        if (watch_readable(static_cast<std::uint32_t>(a)) && mapped(a)) {
            if (len == 0) run = static_cast<std::uint32_t>(a);
            len += kPage;
        } else {
            flush();
        }
    }
    flush();
    std::fclose(f);
    std::fprintf(stderr, "memdump %d: %s\n", n, path.c_str());
}

void present(Cpu& c) {
    selftest_math(c);
    {
        static int all_presents = 0;
        print_watch(c.mem, ++all_presents);
        memdump(c.mem, all_presents);
        // DCUSA_EXIT_AT=<n>: a clean exit after n presents (atexit handlers run,
        // so an instrumented build writes its profile).
        static const int exit_at = [] {
            const char* v = std::getenv("DCUSA_EXIT_AT");
            return v ? std::atoi(v) : 0;
        }();
        if (exit_at > 0 && all_presents >= exit_at) {
            std::fprintf(stderr, "DCUSA_EXIT_AT: exit after %d presents\n", all_presents);
            // Not std::exit: static destructors would run under the other
            // threads. An instrumented build's profile is written first.
#if DCUSA_PGO_GENERATE
            __llvm_profile_write_file();
#endif
            std::fflush(nullptr);
            std::_Exit(0);
        }
        print_pad_trace(c.mem, all_presents);
        // DCUSA_MEM=<n>: every n presents, where the host memory goes (MB).
        // With DCUSA_GPU_THREAD=0 (the report reads the device's tables).
        static const int mem_every = [] {
            const char* v = std::getenv("DCUSA_MEM");
            return v ? std::atoi(v) : 0;
        }();
        if (mem_every > 0 && all_presents % mem_every == 0) {
            double tex = 0, buf = 0, other = 0;
            unsigned n_tex = 0, n_buf = 0;
            for (const auto& [obj, sb] : stored) {
                const double mb = static_cast<double>(sb.bytes.capacity()) / 1048576.0;
                const std::uint32_t kind = ld32(c.mem, obj + 4);
                if (kind == Texture) tex += mb, ++n_tex;
                else if (kind == Buffer) buf += mb, ++n_buf;
                else other += mb;
            }
            std::fprintf(stderr, "mem %d: stored textures %.0f (%u) buffers %.0f (%u) other %.0f | metal %s\n",
                         all_presents, tex, n_tex, buf, n_buf, other, gpu::memory_report().c_str());
        }
        // With DCUSA_HITCH on, the skip log also sums the waits (ms per tick):
        // on the render thread (handover) and on guest threads (wait).
        static double handover_ms = 0, wait_ms = 0;
        handover_ms += hitch::g_ns[hitch::Handover].load() / 1e6;
        wait_ms += hitch::g_ns[hitch::Wait].load() / 1e6;
        const double tick_handover = hitch::g_ns[hitch::Handover].load() / 1e6, tick_wait = hitch::g_ns[hitch::Wait].load() / 1e6;
        const double tick_pipe = hitch::g_ns[hitch::Pipeline].load() / 1e6, tick_tex = (hitch::g_ns[hitch::Texture].load() + hitch::g_ns[hitch::Buffer].load()) / 1e6;
        const unsigned tick_pipes = hitch::g_calls[hitch::Pipeline].load();
        report_hitch(all_presents);
        // DCUSA_SKIP_LOG=<n>: every n presents, the game's tick rate (60 is full
        // speed) and how many of those ticks left out the scene render.
        if (g_tick_start.time_since_epoch().count() != 0) {
            const int kind = g_render_skipped ? 1 : 0;
            g_tick_ms[kind] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_tick_start).count();
            const double cpu_now = thread_cpu_ms() - g_tick_cpu_start;
            g_tick_cpu_ms[kind] += cpu_now;
            if (const double wall_now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_tick_start).count();
                wall_now > g_slow_ms) {
                g_slow_ms = wall_now;
                g_slow_cpu = cpu_now;
                g_slow_present = all_presents;
                g_slow_draws = g_frame_draws;
                g_slow_race = race_timer(c.mem);
                {
                    std::uint64_t ns[32];
                    std::uint32_t starts[32];
                    sched_thread_time(ns, starts);
                    g_slow_thread = 0;
                    g_slow_thread_ms = 0;
                    for (unsigned i = 1; i < 32; ++i)
                        if (const double ms = (ns[i] - g_tick_turn0[i]) / 1e6; ms > g_slow_thread_ms)
                            g_slow_thread_ms = ms, g_slow_thread = starts[i];
                }
                g_slow_handover = tick_handover;
                g_slow_pipe = tick_pipe;
                g_slow_pipes = tick_pipes;
                g_slow_tex = tick_tex;
                g_slow_wait = tick_wait;
                g_slow_preempt = (g_preempt_ns.load() - g_tick_preempt0) / 1e6;
                g_slow_yield = (g_yield_ns.load() - g_tick_yield0) / 1e6;
            }
            g_tick_draws[kind] += g_frame_draws;
            ++g_tick_n[kind];
        }
        if (static const int skip_every = std::getenv("DCUSA_SKIP_LOG") ? std::atoi(std::getenv("DCUSA_SKIP_LOG")) : 0;
            skip_every > 0 && all_presents % skip_every == 0) {
            static auto last = std::chrono::steady_clock::now();
            static std::uint64_t last_skipped = 0;
            const auto t = std::chrono::steady_clock::now();
            const double s = std::chrono::duration<double>(t - last).count();
            std::fprintf(stderr, "ticks %d: %.1f per second, %llu of %d skipped the render, tick %.2f ms rendered %.2f ms skipped, cpu %.2f ms rendered %.2f ms skipped, draws %.0f rendered %.0f skipped\n",
                         all_presents, skip_every / s, static_cast<unsigned long long>(g_skipped_total - last_skipped),
                         skip_every, g_tick_n[0] ? g_tick_ms[0] / g_tick_n[0] : 0.0, g_tick_n[1] ? g_tick_ms[1] / g_tick_n[1] : 0.0,
                         g_tick_n[0] ? g_tick_cpu_ms[0] / g_tick_n[0] : 0.0, g_tick_n[1] ? g_tick_cpu_ms[1] / g_tick_n[1] : 0.0,
                         g_tick_n[0] ? g_tick_draws[0] / g_tick_n[0] : 0.0, g_tick_n[1] ? g_tick_draws[1] / g_tick_n[1] : 0.0);
            {
                // GPU time per frame over the interval (command buffer start to end).
                std::fprintf(stderr, "gpu %d: %.2f ms mean, %.2f ms max per frame\n", all_presents, g_gpu_n ? g_gpu_sum / g_gpu_n : 0.0, g_gpu_max);
                g_gpu_sum = 0;
                g_gpu_max = 0;
                g_gpu_n = 0;
            }
            {
                static std::uint64_t last_yield = 0;
                const std::uint64_t y = g_yield_ns.load();
                static std::uint64_t last_pre = 0;
                const std::uint64_t pre = g_preempt_ns.load();
                std::fprintf(stderr, "yield %d: %.2f ms per tick to other guest threads, %.2f ms preempted\n", all_presents,
                             (y - last_yield) / 1e6 / skip_every, (pre - last_pre) / 1e6 / skip_every);
                last_yield = y;
                last_pre = pre;
            }
            if (hitch::enabled())
                std::fprintf(stderr, "waits %d: handover %.2f ms, guest threads %.2f ms per tick\n", all_presents,
                             handover_ms / skip_every, wait_ms / skip_every);
            {
                static const auto first = std::chrono::steady_clock::now();
                std::fprintf(stderr, "slowest %d: present %d at %.1f s, race %u'%05.2f, %.1f ms (cpu %.1f ms), %u draws, handover %.1f wait %.1f preempt %.1f yield %.1f ms, pipelines %u %.1f ms, resources %.1f ms, busiest thread %08X %.1f ms\n", all_presents,
                             g_slow_present, std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count(),
                             g_slow_race / 3840, (g_slow_race % 3840) / 64.0, g_slow_ms, g_slow_cpu, g_slow_draws, g_slow_handover, g_slow_wait, g_slow_preempt, g_slow_yield, g_slow_pipes, g_slow_pipe, g_slow_tex, g_slow_thread, g_slow_thread_ms);
                g_slow_ms = 0;
            }
            handover_ms = wait_ms = 0;
            g_tick_ms[0] = g_tick_ms[1] = 0;
            g_tick_cpu_ms[0] = g_tick_cpu_ms[1] = 0;
            g_tick_draws[0] = g_tick_draws[1] = 0;
            g_tick_n[0] = g_tick_n[1] = 0;
            last = t;
            last_skipped = g_skipped_total;
        }
    }
    if (static bool shown = false; !shown && stats_on()) {
        shown = true;
        std::fprintf(stderr, "present: sync interval %u flags %u\n", arg(c, 1), arg(c, 2));
    }
    pace_present(c, arg(c, 1));
    g_tick_start = std::chrono::steady_clock::now();
    if (g_frames_presented > 4) {  // a frame the GPU has finished by now
        std::uint64_t a = 0, b = 0;
        if (gpu::frame_gpu_times(g_frames_presented - 4, &a, &b) && b > a) {
            const double ms = (b - a) / 1000.0;
            g_gpu_sum += ms;
            g_gpu_max = std::max(g_gpu_max, ms);
            ++g_gpu_n;
        }
    }
    g_tick_preempt0 = g_preempt_ns.load();
    g_tick_yield0 = g_yield_ns.load();
    {
        std::uint32_t starts[32];
        sched_thread_time(g_tick_turn0, starts);
    }
    g_tick_cpu_start = thread_cpu_ms();
    // DCUSA_PAD_SCRIPT_FROM=attract: a pad script's frame 1 is the first present
    // of attract (boot state 8), so one script fits a paced or unpaced boot.
    static const bool from_attract = [] {
        const char* v = std::getenv("DCUSA_PAD_SCRIPT_FROM");
        return v != nullptr && std::strcmp(v, "attract") == 0;
    }();
    if (!from_attract || g_boot_state >= 8) sample_pad();  // the controls for the frame the game builds next
    if (gpu::enabled()) {
        static int presents_count = 0;
        ++presents_count;
        // DCUSA_FRAMES_FROM=attract: the numbers for DCUSA_METAL_SCAN / _TRACE,
        // DCUSA_FRAME_RANGE and frame file names count from the start of attract
        // (boot state 8), which moves from run to run; before it they are negative.
        static const bool from_attract = [] {
            const char* v = std::getenv("DCUSA_FRAMES_FROM");
            return v != nullptr && std::strcmp(v, "attract") == 0;
        }();
        static int attract_at = 0;
        if (from_attract && attract_at == 0 && g_boot_state >= 8) attract_at = presents_count;
        const int presents = !from_attract ? presents_count : attract_at != 0 ? presents_count - attract_at : -presents_count;
        if (const char* scan = std::getenv("DCUSA_METAL_SCAN"); scan != nullptr && std::atoi(scan) == presents)
            gpu::scan_targets();
        // DCUSA_METAL_TRACE=<n>: one line per draw of the frame that present n ends.
        // DCUSA_METAL_TRACE_FRAMES=<k>: k consecutive frames from there.
        if (const char* trace = std::getenv("DCUSA_METAL_TRACE")) {
            static const int frames = std::getenv("DCUSA_METAL_TRACE_FRAMES") ? std::atoi(std::getenv("DCUSA_METAL_TRACE_FRAMES")) : 1;
            const int first = std::atoi(trace), next = presents + 1;
            const bool on = next >= first && next < first + frames;
            if (on) std::fprintf(stderr, "trace frame %d\n", next);
            gpu::set_trace(on);
        }
        // Reading the frame back waits for the GPU, so only the frames that are
        // written out or reported pay for it (DCUSA_READBACK=1: every frame).
        static const bool every_frame = std::getenv("DCUSA_READBACK") != nullptr;
        static const bool dumping = std::getenv("DCUSA_FRAME") != nullptr || stats_on();
        static const int every = [] {
            const char* e = std::getenv("DCUSA_EVERY");
            return e ? std::atoi(e) : 0;
        }();
        // DCUSA_FRAME_RANGE=<a>-<b>: write every present from a to b (frame-to-frame changes).
        static const std::pair<int, int> range = [] {
            const char* v = std::getenv("DCUSA_FRAME_RANGE");
            int a = 0, b = -1;
            if (v != nullptr && std::sscanf(v, "%d-%d", &a, &b) != 2) b = -1;
            return std::make_pair(a, b);
        }();
        // DCUSA_RACE_RANGE=<a>-<b>: also every present while the race timer
        // (race_timer, seconds as the HUD shows them) is from a to b.
        static const std::pair<double, double> race_range = [] {
            const char* v = std::getenv("DCUSA_RACE_RANGE");
            double a = 0, b = -1;
            if (v != nullptr && std::sscanf(v, "%lf-%lf", &a, &b) != 2) b = -1;
            return std::make_pair(a, b);
        }();
        const double race_s = race_range.second >= 0 ? race_timer(c.mem) / 64.0 : -1;
        const bool in_range = (presents >= range.first && presents <= range.second) ||
                              (race_s >= race_range.first && race_s <= race_range.second);
        if (in_range) g_frames_after_hitch = std::max(g_frames_after_hitch, 1);
        const bool wanted = every_frame || g_frames_after_hitch > 0 || (dumping && (presents == 1 || presents == 30 || (every > 0 && presents % every == 0)));
        static std::chrono::steady_clock::time_point last_shown{};
        const auto now = std::chrono::steady_clock::now();
        const bool skipped = g_render_skipped;
        g_render_skipped = false;
        if (!skipped && (!g_boot_loading || now - last_shown >= std::chrono::milliseconds(16))) {
            {
                static int attract_at = 0;  // the present attract began (boot state 8)
                if (attract_at == 0 && g_boot_state >= 8) attract_at = presents_count;
                count_shown_picture(attract_at != 0 ? presents_count - attract_at : -1, race_timer(c.mem));
            }
            if (boot_screen_shown()) {
                show_boot_screen();
            } else {
                gpu::antialias(pipe.backbuffer);  // FXAA (DCUSA_AA=0: off)
                gpu::show(pipe.backbuffer);  // into the window, when there is one
            }
            last_shown = now;
        }
        std::uint32_t w = 0, h = 0;
        if (!wanted) gpu::submit();
        else if (gpu::read_rgba8(pipe.backbuffer, &stored[pipe.backbuffer].bytes, &w, &h)) stored[pipe.backbuffer].row = w * 4u;
        ++g_frames_presented;  // one command buffer per present (frame_gpu_times)
        g_frame_draws = 0;
        publish_frame(pipe.backbuffer, wanted);
        if (g_presenter != nullptr) g_presenter->present();
        ret_stdcall(c, kSOk, 12);
        return;
    }
    publish_frame(pipe.target != 0 ? pipe.target : pipe.backbuffer, true);
    ret_stdcall(c, kSOk, 12);
}

// 0x6BB6F1. GetDesc. The caller divides the refresh denominator by the numerator,
// so the numerator stays non-zero.
void swap_desc(Cpu& c) {
    const std::uint32_t desc = arg(c, 1);
    if (desc != 0) {
        std::uint32_t w = 0, h = 0;
        display_size(&w, &h);
        st32(c.mem, desc, w);
        st32(c.mem, desc + 4, h);
        st32(c.mem, desc + 8, 60);
        st32(c.mem, desc + 12, 1);
        st32(c.mem, desc + 16, 28);
    }
    ret_stdcall(c, kSOk, 8);
}

// 0x6BB683. GetContainingOutput. The HRESULT is ignored. A null output takes the
// fallback clock; a real one is released later through the ordinary vtable.
void get_output(Cpu& c) {
    publish(c.mem);
    const std::uint32_t out = arg(c, 1);
    if (out != 0) st32(c.mem, out, object(Output));
    ret_stdcall(c, kSOk, 8);
}

// 0x6BBAC1. WaitForVBlank. The next call overwrites EAX.
void wait_vblank(Cpu& c) { ret_stdcall(c, kSOk, 4); }

// 0x6BB65A. GetFullscreenState. A non-zero HRESULT leaves the old flag. Success
// compares the BOOL with EAX, so windowed (0) clears the fullscreen byte.
void get_fullscreen(Cpu& c) {
    const std::uint32_t full = arg(c, 1);
    const std::uint32_t target = arg(c, 2);
    if (full != 0) st32(c.mem, full, 0);
    if (target != 0) st32(c.mem, target, 0);
    ret_stdcall(c, kSOk, 12);
}

// 0x6B9E56 asks the back buffer for ID3D11Texture2D.
void get_buffer(Cpu& c) {
    const std::uint32_t iid = arg(c, 2);
    const std::uint32_t out = arg(c, 3);
    if (!guid_is(c.mem, iid, 0x6F15AAF2u, 0xD208, 0x4E89, 0x9AB4489535D34F9Cull)) refuse_iid(c.mem, iid);
    const std::uint32_t obj = spawn(c.mem, Texture);
    std::uint32_t w = 0, h = 0;
    display_size(&w, &h);
    st32(c.mem, obj + 12, w);
    st32(c.mem, obj + 16, h);
    st32(c.mem, obj + 20, 28);
    st32(c.mem, obj + 24, 1);
    remember(obj, w * h * 4u, w * 4u);
    pipe.backbuffer = obj;
    if (gpu::enabled()) {
        gpu::TextureDesc td;
        td.width = w;
        td.height = h;
        td.format = 28;
        td.bind = 0x28;  // render target, shader resource
        gpu::create_texture(obj, td);
    }
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 16);
}

// Slot 0xC0 is UpdateSubresource. The bytes are kept with the resource.
// UpdateSubresource(resource, subresource, box, src, row pitch, depth pitch).
// Only subresource 0 (mip 0, slice 0) is kept, so others are dropped rather
// than written over it. A D3D11_BOX {left, top, front, right, bottom, back}
// updates that part only, and the source holds just that part.
void update_subresource(Cpu& c) {
    hitch::Timer timer(hitch::Texture);
    const std::uint32_t res = arg(c, 1);
    const std::uint32_t sub = arg(c, 2);
    const std::uint32_t box = arg(c, 3);
    const std::uint32_t src = arg(c, 4);
    const std::uint32_t src_row = arg(c, 5);
    auto it = stored.find(res);
    if (sub != 0 && src != 0 && box == 0 && gpu::enabled() && ld32(c.mem, res + 4) == Texture) {
        // A smaller level (or another slice) of a texture: straight to the GPU.
        const std::uint32_t mips = std::max(1u, ld32(c.mem, res + 24)), mip = sub % mips;
        std::uint32_t row = 0;
        const std::uint32_t bytes = texture_bytes(std::max(1u, ld32(c.mem, res + 12) >> mip),
                                                  std::max(1u, ld32(c.mem, res + 16) >> mip), ld32(c.mem, res + 20), &row);
        if (bytes != 0 && row != 0) {
            const std::uint32_t pitch = src_row != 0 ? src_row : row;
            gpu::upload_texture_sub(res, sub / mips, mip, c.mem + src, static_cast<std::size_t>(pitch) * (bytes / row), pitch);
        }
        ret_stdcall(c, kSOk, 28);
        return;
    }
    if (sub != 0 || src == 0 || it == stored.end() || (it->second.bytes.empty() && !it->second.gpu_only)) {
        ret_stdcall(c, kSOk, 28);
        return;
    }
    if (auto h = buffer_home.find(res); h != buffer_home.end() && ld32(c.mem, res + 4) != Texture) {
        // A buffer with a home: its bytes live there (the stored ones are stale).
        const std::uint32_t size = static_cast<std::uint32_t>(it->second.bytes.size());
        std::uint32_t left = 0, right = size;
        if (box != 0) left = ld32(c.mem, box), right = std::min(ld32(c.mem, box + 12), size);
        if (right > left) std::memcpy(c.mem + h->second + left, c.mem + src, right - left);
        gpu::upload_buffer(res, c.mem + h->second, size);
        ret_stdcall(c, kSOk, 28);
        return;
    }
    if (box != 0 && it->second.gpu_only && it->second.bytes.empty()) {
        // Part of a GPU-only texture: straight to the GPU, the rest stays.
        const std::uint32_t left = ld32(c.mem, box), top = ld32(c.mem, box + 4);
        const std::uint32_t right = ld32(c.mem, box + 12), bottom = ld32(c.mem, box + 16);
        const std::uint32_t f = it->second.format;
        const bool bc = (f >= 70 && f <= 84) || (f >= 94 && f <= 99);
        if (right > left && bottom > top && src_row != 0) {
            const std::uint32_t rows = bc ? (bottom - top + 3) / 4 : bottom - top;
            gpu::upload_texture_region(res, left, top, right - left, bottom - top, c.mem + src,
                                       static_cast<std::size_t>(src_row) * rows, src_row);
        }
        ret_stdcall(c, kSOk, 28);
        return;
    }
    if (box == 0) {
        copy_from_guest(res, c.mem, src, src_row);
    } else {
        StoredBytes& sb = it->second;
        const std::uint32_t left = ld32(c.mem, box), top = ld32(c.mem, box + 4);
        const std::uint32_t right = ld32(c.mem, box + 12), bottom = ld32(c.mem, box + 16);
        if (ld32(c.mem, res + 4) != Texture) {
            if (right > left && right <= sb.bytes.size()) std::memcpy(sb.bytes.data() + left, c.mem + src, right - left);
        } else if (right > left && bottom > top) {
            const std::uint32_t f = sb.format;
            const bool bc = (f >= 70 && f <= 84) || (f >= 94 && f <= 99);
            const std::uint32_t block = bc ? (f <= 72 || (f >= 79 && f <= 81) ? 8u : 16u) : texel_bytes(f);
            const std::uint32_t x0 = bc ? left / 4 : left, x1 = bc ? (right + 3) / 4 : right;
            const std::uint32_t y0 = bc ? top / 4 : top, y1 = bc ? (bottom + 3) / 4 : bottom;
            const std::uint32_t bytes = (x1 - x0) * block;
            for (std::uint32_t y = y0; y < y1; ++y) {
                const std::size_t at = static_cast<std::size_t>(y) * sb.row + static_cast<std::size_t>(x0) * block;
                if (at + bytes > sb.bytes.size()) break;
                std::memcpy(sb.bytes.data() + at, c.mem + src + static_cast<std::size_t>(y - y0) * src_row, bytes);
            }
        }
    }
    sync_gpu(c.mem, res);
    ret_stdcall(c, kSOk, 28);
}

// 0x6BBEF7 unbinds the targets. The caller does not read EAX.
void om_set_targets(Cpu& c) {
    const std::uint32_t count = arg(c, 1);
    const std::uint32_t views = arg(c, 2);
    const std::uint32_t dsv = arg(c, 3);
    for (unsigned i = 0; i < gpu::kTargets; ++i) {
        const std::uint32_t view = views != 0 && i < count ? ld32(c.mem, views + 4 * i) : 0u;
        gs.rtv[i] = view != 0 ? ld32(c.mem, view + 12) : 0u;
        gs.rtv_mip[i] = static_cast<std::uint8_t>(view != 0 ? ld32(c.mem, view + 16) : 0u);
        gs.rtv_slice[i] = static_cast<std::uint8_t>(view != 0 ? ld32(c.mem, view + 20) : 0u);
    }
    gs.dsv = dsv != 0 ? ld32(c.mem, dsv + 12) : 0u;
    if (count != 0 && views != 0) {
        const std::uint32_t view = ld32(c.mem, views);
        if (view != 0) pipe.target = ld32(c.mem, view + 12);
    } else {
        pipe.target = 0;
    }
    ret_stdcall(c, kSOk, 16);
}

// 0x6BE5AF passes one viewport and keeps its own copy. EAX is not read.
void rs_set_viewports(Cpu& c) {
    const std::uint32_t count = arg(c, 1);
    const std::uint32_t ptr = arg(c, 2);
    if (count != 0 && ptr != 0) {
        std::memcpy(&pipe.vx, c.mem + ptr, 4);
        std::memcpy(&pipe.vy, c.mem + ptr + 4, 4);
        std::memcpy(&pipe.vw, c.mem + ptr + 8, 4);
        std::memcpy(&pipe.vh, c.mem + ptr + 12, 4);
        std::memcpy(gs.viewport, c.mem + ptr, 24);
    }
    ret_stdcall(c, kSOk, 12);
}

// 0x6E0454 passes one rectangle. The function returns without reading EAX.
void rs_set_scissors(Cpu& c) {
    const std::uint32_t count = arg(c, 1);
    const std::uint32_t ptr = arg(c, 2);
    pipe.scissor = count != 0 && ptr != 0;
    if (pipe.scissor) {
        pipe.left = static_cast<int>(ld32(c.mem, ptr));
        pipe.top = static_cast<int>(ld32(c.mem, ptr + 4));
        pipe.right = static_cast<int>(ld32(c.mem, ptr + 8));
        pipe.bottom = static_cast<int>(ld32(c.mem, ptr + 12));
        for (unsigned i = 0; i < 4; ++i) gs.scissor[i] = static_cast<int>(ld32(c.mem, ptr + 4 * i));
    }
    ret_stdcall(c, kSOk, 12);
}

// 0x6DE7FA clears one target to the color at [esi+0x1C]. EAX is not read.
void clear_rtv(Cpu& c) {
    const std::uint32_t view = arg(c, 1);
    const std::uint32_t color = arg(c, 2);
    if (view != 0 && color != 0 && gpu::enabled()) {
        float rgba[4];
        std::memcpy(rgba, c.mem + color, 16);
        gpu::clear_target(ld32(c.mem, view + 12), ld32(c.mem, view + 16), ld32(c.mem, view + 20), rgba);
    }
    if (view != 0 && color != 0 && !gpu::enabled()) {
        const std::uint32_t resource = ld32(c.mem, view + 12);
        auto it = stored.find(resource);
        if (it != stored.end() && !it->second.bytes.empty() && it->second.row >= 4) {
            float rgba[4]{};
            for (unsigned i = 0; i < 4; ++i) {
                std::memcpy(&rgba[i], c.mem + color + i * 4, 4);
                if (rgba[i] < 0) rgba[i] = 0;
                if (rgba[i] > 1) rgba[i] = 1;
            }
            const std::uint32_t bpp = target_bpp(it->second.format);
            if (it->second.row >= bpp) {
                for (std::size_t i = 0; i + bpp <= it->second.bytes.size(); i += bpp)
                    store_color(it->second.bytes.data() + i, it->second.format, rgba);
            }
        }
    }
    ret_stdcall(c, kSOk, 12);
}

// 0x6B1D03 begins a query and does not read EAX. 0x6B1DFD ends one the same way.
void query_begin(Cpu& c) { ret_stdcall(c, kSOk, 8); }

// 0x6B89F6. GetData. A non-zero HRESULT skips the copy, and the success path
// reads the buffer (0x58 bytes of pipeline statistics, or 8 or 16). Zeros are
// an empty result. 0x6B8AE4 then requires the disjoint flag to be clear.
//
// The game times each frame on the GPU (a TIMESTAMP query at its start and
// end, a TIMESTAMP_DISJOINT for the frequency) and its dynamic resolution
// (0x4DC250, GameData/config.ini DYNAMIC_RESOLUTION_MIN/MAX) steps the scene
// width down past 16.7 ms and up below 14.6 ms. A timestamp is
// the GPU start or end of the frame's command buffer, in microseconds, so a
// frame's two differ by its GPU time. Zeros (frequency 0) left the width at
// its maximum on a phone that needs about 30 ms of GPU a frame.
void query_get_data(Cpu& c) {
    const std::uint32_t query = arg(c, 1);
    const std::uint32_t data = arg(c, 2);
    const std::uint32_t size = arg(c, 3);
    if (data != 0 && size != 0 && size <= 0x100u)
        for (std::uint32_t i = 0; i < size; ++i) st8(c.mem, data + i, 0);
    // DCUSA_GPU_QUERIES=0: zeros for every query, as before (no dynamic resolution).
    static const bool queries = [] {
        const char* v = std::getenv("DCUSA_GPU_QUERIES");
        return v == nullptr || std::strcmp(v, "0") != 0;
    }();
    if (queries && data != 0 && query != 0 && gpu::enabled()) {
        const std::uint32_t type = ld32(c.mem, query + 12);
        if (type == 3 && size >= 12) {  // D3D11_QUERY_DATA_TIMESTAMP_DISJOINT: Frequency, Disjoint
            st32(c.mem, data, 1000000u);
            st32(c.mem, data + 4, 0);
            st32(c.mem, data + 8, 0);
        } else if (type == 2 && size >= 8) {  // TIMESTAMP
            // The GPU time at that point of the stream: the start of its frame's
            // command buffer when no draw came first, else the end. Not there
            // yet (S_FALSE, 1) until that command buffer has run.
            const auto it = query_marks.find(query);
            std::uint64_t start = 0, end = 0;
            // DCUSA_GPU_QUERY_LAG=<k>: a frame's times are not there until k
            // presents after it, as on a slower GPU (an iPhone's lag 2 to 3).
            static const std::uint64_t lag = [] {
                const char* v = std::getenv("DCUSA_GPU_QUERY_LAG");
                return v != nullptr ? static_cast<std::uint64_t>(std::atoi(v)) : 0u;
            }();
            if (it != query_marks.end() && (g_frames_presented < it->second.frame + lag ||
                                            !gpu::frame_gpu_times(it->second.frame, &start, &end))) {
                ret_stdcall(c, 1u, 20);
                return;
            }
            std::uint64_t t = it == query_marks.end() ? 0 : it->second.after_draws ? end : start;
            // DCUSA_GPU_TIME_SCALE=<k>: GPU times k times longer, so a Mac sees what a
            // phone's GPU makes the game do (dynamic resolution and bake frequency).
            static const double scale = std::getenv("DCUSA_GPU_TIME_SCALE") ? std::atof(std::getenv("DCUSA_GPU_TIME_SCALE")) : 1.0;
            if (scale != 1.0) t = static_cast<std::uint64_t>(static_cast<double>(t) * scale);
            st32(c.mem, data, static_cast<std::uint32_t>(t));
            st32(c.mem, data + 4, static_cast<std::uint32_t>(t >> 32));
        }
    }
    ret_stdcall(c, kSOk, 20);
}

// 0x6B1FB1. ClearState (vtable index 110). The caller ignores EAX.
void clear_state(Cpu& c) {
    gs = gpu::DrawState{};
    g_last_blend = g_last_depth = g_last_raster = ~0u;
    bound_shaders = ShaderObjects{};
    ret_stdcall(c, kSOk, 4);
}

// 0x6C8479. GenerateMips. 0x6C847F overwrites EAX before the function returns.
void generate_mips(Cpu& c) {
    if (arg(c, 1) != 0) gpu::generate_mips(ld32(c.mem, arg(c, 1) + 12));
    ret_stdcall(c, kSOk, 8);
}

// The textures behind `count` views from `views`, from slot `start`, and the
// mip range each view covers.
void set_views(Cpu& c, std::uint32_t* slots, std::uint8_t* first, std::uint8_t* levels, std::uint32_t start,
               std::uint32_t count, std::uint32_t views) {
    for (std::uint32_t i = 0; i < count && start + i < gpu::kSlots; ++i) {
        const std::uint32_t view = views != 0 ? ld32(c.mem, views + 4 * i) : 0u;
        slots[start + i] = view != 0 ? ld32(c.mem, view + 12) : 0u;
        first[start + i] = static_cast<std::uint8_t>(view != 0 ? ld32(c.mem, view + 16) : 0u);
        levels[start + i] = static_cast<std::uint8_t>(view != 0 ? ld32(c.mem, view + 20) : 0u);
    }
}

void set_objects(Cpu& c, std::uint32_t* slots, unsigned cap, std::uint32_t start, std::uint32_t count, std::uint32_t list) {
    for (std::uint32_t i = 0; i < count && start + i < cap; ++i) slots[start + i] = list != 0 ? ld32(c.mem, list + 4 * i) : 0u;
}

// PSSetShaderResources at +0x20. Callers at 0x6BBA27 and 0x6C90C0 pass a slot, a count and a view list.
void ps_set_resources(Cpu& c) {
    const std::uint32_t count = arg(c, 2);
    const std::uint32_t views = arg(c, 3);
    set_views(c, gs.ps_srv, gs.ps_srv_mip, gs.ps_srv_mips, arg(c, 1), count, views);
    ++g_binds;
    if (views != 0 && count != 0 && count <= 128u) {
        std::uint32_t chosen = 0;
        std::uint32_t color = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t view = ld32(c.mem, views + i * 4u);
            if (view == 0) continue;
            const std::uint32_t res = ld32(c.mem, view + 12);
            if (chosen == 0) chosen = res;
            const auto image = stored.find(res);
            const std::uint32_t format = image == stored.end() ? 0u : image->second.format;
            const std::size_t nbytes = image == stored.end() ? 0u : image->second.bytes.size();
            if ((format == 28 || format == 29 || format == 71 || format == 77 || format == 87) && nbytes > 256u)
                color = res;
            static int color_logs = 0;
            if (stats_on() && color_logs < 8 && color == res && nbytes > 256u) {
                ++color_logs;
                std::fprintf(stderr, "color bind %u slot %u bytes %zu fmt %u\n", g_binds, i, nbytes, format);
            }
        }
        pipe.texture = color != 0 ? color : chosen;
    }
    ret_stdcall(c, kSOk, 16);
}

// 0x6D0114. VSSetShaderResources. The next instruction overwrites EAX.
void vs_set_resources(Cpu& c) {
    set_views(c, gs.vs_srv, gs.vs_srv_mip, gs.vs_srv_mips, arg(c, 1), arg(c, 2), arg(c, 3));
    ret_stdcall(c, kSOk, 16);
}

// 0x6DE865. ClearDepthStencilView. The caller returns without reading EAX.
// ClearDepthStencilView(view, flags, depth, stencil).
void clear_dsv(Cpu& c) {
    const std::uint32_t view = arg(c, 1);
    float depth = 1;
    const std::uint32_t bits = arg(c, 3);
    std::memcpy(&depth, &bits, 4);
    if (view != 0) gpu::clear_depth(ld32(c.mem, view + 12), depth, static_cast<std::uint8_t>(arg(c, 4)), arg(c, 2));
    ret_stdcall(c, kSOk, 20);
}

// 0x6D64B9. Map. A negative HRESULT skips the copy; success returns pData to the caller.
void map_resource(Cpu& c) {
    const std::uint32_t resource = arg(c, 1);
    const std::uint32_t type = arg(c, 3);
    const std::uint32_t out = arg(c, 5);
    static bool committed = false;  // once: a system call per Map was 3% of a race frame
    if (!committed) {
        const std::uint64_t page = GuestMemory::PageSize;
        const std::uint64_t lo = kMapScratch & ~(page - 1);
        const std::uint64_t hi = (kMapScratch + kMapBytes + page - 1) & ~(page - 1);
        if (mprotect(c.mem + lo, static_cast<std::size_t>(hi - lo), PROT_READ | PROT_WRITE) != 0)
            fail_closed(Unported::Service, kMapScratch, "D3D11 map memory");
        committed = true;
    }
    std::uint32_t size = 0;
    std::uint32_t pitch = 0;
    auto it = stored.find(resource);
    if (it != stored.end()) {
        size = static_cast<std::uint32_t>(it->second.bytes.size());
        pitch = it->second.row;
    } else if (resource >= kPool && resource < kPool + kPoolCap * 0x40u) {
        pitch = ld32(c.mem, resource + 12);
    }
    static const bool homes = [] {
        const char* e = std::getenv("DCUSA_BUFFER_HOMES");
        return e == nullptr || std::atoi(e) != 0;
    }();
    if (homes && size != 0 && it != stored.end() && gpu::enabled() && ld32(c.mem, resource + 4) != Texture) {
        std::uint32_t home = 0;
        if (auto h = buffer_home.find(resource); h != buffer_home.end()) {
            home = h->second;
        } else {
            const std::uint32_t need = (size + 63u) & ~63u;
            if (auto f = free_homes.find(need); f != free_homes.end()) {
                home = f->second;
                free_homes.erase(f);
            } else if (buffer_home_used + need <= kBufferHomeBytes) {
                if (buffer_home_used == 0 && mprotect(c.mem + kBufferHomes, kBufferHomeBytes, PROT_READ | PROT_WRITE) != 0)
                    fail_closed(Unported::Service, kBufferHomes, "D3D11 buffer homes");
                home = kBufferHomes + buffer_home_used;
                buffer_home_used += need;
                home_size[home] = need;
            }
            if (home != 0) {
                std::memcpy(c.mem + home, it->second.bytes.data(), size);
                buffer_home[resource] = home;
            }
        }
        if (home != 0) {
            live_maps.push_back(LiveMap{resource, home, size, type != 1u, true});
            if (out != 0) {
                st32(c.mem, out, home);
                st32(c.mem, out + 4, pitch);
                st32(c.mem, out + 8, 0);
            }
            ret_stdcall(c, kSOk, 24);
            return;
        }
    }
    if (scratch_used + size > kMapBytes) scratch_used = 0;
    const std::uint32_t at = kMapScratch + scratch_used;
    if (size != 0) {
        // D3D11_MAP_WRITE_DISCARD (4) leaves the contents undefined, and the game
        // rewrites them, so only the other kinds copy the current bytes in.
        if (type != 4u) {
            if (it != stored.end()) std::memcpy(c.mem + at, it->second.bytes.data(), size);
            else std::memset(c.mem + at, 0, size);
        }
        scratch_used += (size + 15u) & ~15u;
    }
    live_maps.push_back(LiveMap{resource, at, size, type != 1u, false});
    // DCUSA_MAP_STATS=<presents>: Maps by type and bytes, every that many presents.
    static const int map_stats = std::getenv("DCUSA_MAP_STATS") ? std::atoi(std::getenv("DCUSA_MAP_STATS")) : 0;
    if (map_stats > 0) {
        static std::uint64_t counts[8]{}, bytes[8]{};
        static std::uint64_t last = 0;
        ++counts[type & 7u];
        bytes[type & 7u] += size;
        if (g_frames_presented >= last + static_cast<std::uint64_t>(map_stats)) {
            std::fprintf(stderr, "maps over %d presents: discard %llu (%llu KB) no-overwrite %llu (%llu KB) write %llu read %llu rw %llu\n",
                         map_stats, (unsigned long long)counts[4], (unsigned long long)(bytes[4] >> 10),
                         (unsigned long long)counts[5], (unsigned long long)(bytes[5] >> 10), (unsigned long long)counts[2],
                         (unsigned long long)counts[1], (unsigned long long)counts[3]);
            std::fill(std::begin(counts), std::end(counts), 0);
            std::fill(std::begin(bytes), std::end(bytes), 0);
            last = g_frames_presented;
        }
    }
    if (out != 0) {
        st32(c.mem, out, size == 0 ? kMapScratch : at);
        st32(c.mem, out + 4, pitch);
        st32(c.mem, out + 8, 0);
    }
    ret_stdcall(c, kSOk, 24);
}

// A constant buffer that starts like a projection matrix (for the 2D reference).
void note_projection(const std::uint8_t* bytes) {
    float row[4]{};
    std::memcpy(row, bytes, sizeof row);
    float next = 0;
    std::memcpy(&next, bytes + 20, 4);
    if (row[0] > 0.5f && row[0] < 8.0f && row[1] == 0 && row[2] == 0 && row[3] == 0 && next > 0.5f) {
        std::memcpy(g_proj, bytes, sizeof g_proj);
        g_have_proj = true;
    }
}

// 0x6D66EE. Unmap. Both call sites push an extra zero, and the epilog only pops saved registers.
// A write map copies the scratch back into the resource.
void unmap_resource(Cpu& c) {
    const std::uint32_t resource = arg(c, 1);
    for (auto it = live_maps.begin(); it != live_maps.end(); ++it) {
        if (it->resource != resource) continue;
        if (it->home) {
            if (it->write) {
                const std::uint8_t* bytes = c.mem + it->at;
                if (it->size >= 64 && it->size <= 1024) note_projection(bytes);
                gpu::upload_buffer(resource, bytes, it->size);
            }
            live_maps.erase(it);
            break;
        }
        auto stored_it = stored.find(resource);
        if (it->write && stored_it != stored.end() && it->size != 0) {
            std::memcpy(stored_it->second.bytes.data(), c.mem + it->at, it->size);
            if (stored_it->second.format == 0 && it->size >= 64 && it->size <= 1024 &&
                stored_it->second.bytes.size() >= 64)
                note_projection(stored_it->second.bytes.data());
            if (stats_on() && stored_it->second.format == 0 && it->size >= 64 &&
                it->size <= 8192) {
                static int shown = 0;
                float head = 0;
                std::memcpy(&head, stored_it->second.bytes.data(), 4);
                if (shown < 3 && head > 0.2f && head < 20.0f) {
                    ++shown;
                    const int count = it->size >= 400 ? 40 : 16;
                    std::fprintf(stderr, "unmap %u", it->size);
                    for (int i = 0; i < count; ++i) {
                        float v = 0;
                        std::memcpy(&v, stored_it->second.bytes.data() + static_cast<std::size_t>(i) * 4u, 4);
                        std::fprintf(stderr, " %.4g", v);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
        }
        if (it->write) sync_gpu(c.mem, resource);
        live_maps.erase(it);
        break;
    }
    ret_stdcall(c, kSOk, 12);
}

// Draw setup in 0x6CFF24..0x6D0792. Every one of these returns void and the
// next instruction overwrites EAX. The pop counts are the pushes at the call,
// including the extra zero 0x6D0343 puts under PSSetShader.
void vs_set_constants(Cpu& c) {                                    // +0x1C  0x6D00A6, start, count, buffers
    const std::uint32_t count = arg(c, 2);
    const std::uint32_t ptrs = arg(c, 3);
    set_objects(c, gs.vs_cb, gpu::kBuffers, arg(c, 1), count, ptrs);
    if (count != 0 && ptrs != 0) {
        pipe.vs_cb = ptrs;
        pipe.vs_cb_count = count > 8u ? 8u : count;
        for (std::uint32_t i = 0; i < pipe.vs_cb_count; ++i) pipe.vs_ids[i] = ld32(c.mem, ptrs + i * 4u);
    }
    ret_stdcall(c, kSOk, 16);
}
void ps_set_shader(Cpu& c) {                                       // +0x24  0x6D0357, shader, classes, count
    pipe.shader = arg(c, 1);
    bound_shaders.ps = arg(c, 1);
    ret_stdcall(c, kSOk, 16);
}
void ps_set_samplers(Cpu& c) {                                     // +0x28  0x6D0698
    set_objects(c, gs.ps_samp, gpu::kSlots, arg(c, 1), arg(c, 2), arg(c, 3));
    ret_stdcall(c, kSOk, 16);
}
void vs_set_shader(Cpu& c) {                                       // +0x2C  0x6D0050
    bound_shaders.vs = arg(c, 1);
    ret_stdcall(c, kSOk, 16);
}
const std::uint8_t* g_mem_for_trace = nullptr;
// The shaders bound now, as compile request keys.
void resolve_shaders() {
    gs.vs = key_of_shader(bound_shaders.vs);
    gs.ps = key_of_shader(bound_shaders.ps);
    static const bool shader_trace = std::getenv("DCUSA_SHADER_TRACE") != nullptr;
    if (shader_trace) {
        static int shown = 0;
        if (shown++ < 6) {
            const auto code = shader_code.find(bound_shaders.ps);
            const std::uint32_t at = code == shader_code.end() ? 0u : code->second;
            std::fprintf(stderr, "resolve vs %08x -> %016llx ps %08x -> %016llx code %08x head %08x %08x\n", bound_shaders.vs,
                         static_cast<unsigned long long>(gs.vs), bound_shaders.ps, static_cast<unsigned long long>(gs.ps), at,
                         at ? ld32(g_mem_for_trace, at) : 0u, at ? ld32(g_mem_for_trace, at + 4) : 0u);
        }
    }
}
// DCUSA_DRAW_STACK=<w>x<h>: for the first few draws into a render target of
// that viewport size each 600 presents, the code addresses on the guest stack
// (an approximate call chain), to find the game code behind a pass.
void draw_stack_probe(Cpu& c) {
    static const char* want = std::getenv("DCUSA_DRAW_STACK");
    if (want == nullptr) return;
    // DCUSA_DRAW_STACK=all: every draw of present DCUSA_DRAW_STACK_AT, one line each.
    if (std::strcmp(want, "all") == 0) {
        // DCUSA_DRAW_STACK_MIN=<n>: instead, the first frame after one with at least n draws.
        static const std::uint64_t at = std::getenv("DCUSA_DRAW_STACK_AT") ? std::atoll(std::getenv("DCUSA_DRAW_STACK_AT")) : 0;
        static const std::uint32_t min_draws = std::getenv("DCUSA_DRAW_STACK_MIN") ? std::atoi(std::getenv("DCUSA_DRAW_STACK_MIN")) : 0;
        static std::uint64_t chosen = ~0ull, seen_frame = ~0ull;
        static std::uint32_t last_count = 0;
        if (min_draws != 0) {
            if (g_frames_presented != seen_frame) {
                if (chosen == ~0ull && last_count >= min_draws && g_frames_presented > 3000) chosen = g_frames_presented;
                seen_frame = g_frames_presented;
            }
            last_count = g_frame_draws + 1;
            if (g_frames_presented != chosen) return;
        } else if (g_frames_presented != at) {
            return;
        }
        std::fprintf(stderr, "draw stack %gx%g:", gs.viewport[2], gs.viewport[3]);
        const std::uint32_t esp = c.r[Cpu::ESP];
        int n = 0;
        for (std::uint32_t p = esp; p < esp + 0x3000u && n < 40; p += 4) {
            const std::uint32_t v = ld32(c.mem, p);
            if (v >= 0x401000u && v < 0xD00000u) std::fprintf(stderr, " %08X", v), ++n;
        }
        std::fprintf(stderr, "\n");
        return;
    }
    char vp[32];
    std::snprintf(vp, sizeof vp, "%gx%g", gs.viewport[2], gs.viewport[3]);
    if (std::strcmp(vp, want) != 0) return;
    static std::uint64_t last_frame = ~0ull;
    static int shown = 0;
    if (g_frames_presented / 600 != last_frame) last_frame = g_frames_presented / 600, shown = 0;
    if (shown++ >= 3) return;
    std::fprintf(stderr, "draw stack %s present %llu:", vp, static_cast<unsigned long long>(g_frames_presented));
    const std::uint32_t esp = c.r[Cpu::ESP];
    int n = 0;
    for (std::uint32_t p = esp; p < esp + 0x2000u && n < 28; p += 4) {
        const std::uint32_t v = ld32(c.mem, p);
        if (v >= 0x401000u && v < 0xD00000u) std::fprintf(stderr, " %08X", v), ++n;
    }
    std::fprintf(stderr, "\n");
}

void draw_indexed(Cpu& c) {                                        // +0x30  0x6D0792
    g_mem_for_trace = c.mem;
    draw_stack_probe(c);
    if (gpu::enabled()) {
        resolve_shaders();
        ++g_frame_draws;
        gpu::draw_indexed(gs, arg(c, 1), arg(c, 2), static_cast<std::int32_t>(arg(c, 3)));
    } else {
        draw_triangles(arg(c, 1), arg(c, 2), static_cast<std::int32_t>(arg(c, 3)));
    }
    ret_stdcall(c, kSOk, 16);
}
void draw_plain(Cpu& c) {                                          // +0x34  Draw(count, start)
    if (!gpu::enabled()) fail_closed(Unported::Service, 0x34, "ID3D11DeviceContext+34 (Draw) without Metal");
    resolve_shaders();
    ++g_frame_draws;
    gpu::draw(gs, arg(c, 1), arg(c, 2));
    ret_stdcall(c, kSOk, 12);
}
void ps_set_constants(Cpu& c) {                                    // +0x40  0x6D03CE
    set_objects(c, gs.ps_cb, gpu::kBuffers, arg(c, 1), arg(c, 2), arg(c, 3));
    ret_stdcall(c, kSOk, 16);
}
void ia_set_layout(Cpu& c) {                                      // +0x44  0x6D0723
    pipe.layout = arg(c, 1);
    if (const auto it = gpu_layouts.find(arg(c, 1)); it != gpu_layouts.end()) {
        gs.layout = it->second.elements;
        gs.layout_serial = it->second.serial;
    } else {
        gs.layout = nullptr;
        gs.layout_serial = 0;
    }
    ret_stdcall(c, kSOk, 8);
}
void ia_set_vertices(Cpu& c) {                                     // +0x48  0x6CFF35
    const std::uint32_t start = arg(c, 1);
    const std::uint32_t count = arg(c, 2);
    const std::uint32_t bufs = arg(c, 3);
    const std::uint32_t strides = arg(c, 4);
    const std::uint32_t offs = arg(c, 5);
    for (std::uint32_t i = 0; i < count && start + i < 8u; ++i) {
        pipe.vb[start + i] = bufs == 0 ? 0u : ld32(c.mem, bufs + i * 4u);
        pipe.stride[start + i] = strides == 0 ? 0u : ld32(c.mem, strides + i * 4u);
        pipe.voff[start + i] = offs == 0 ? 0u : ld32(c.mem, offs + i * 4u);
        gs.vb[start + i] = pipe.vb[start + i];
        gs.stride[start + i] = pipe.stride[start + i];
        gs.voff[start + i] = pipe.voff[start + i];
    }
    ret_stdcall(c, kSOk, 24);
}
void ia_set_index(Cpu& c) {                                        // +0x4C  0x6CFF66
    pipe.ib = arg(c, 1);
    pipe.ib_format = arg(c, 2);
    pipe.ib_off = arg(c, 3);
    gs.ib = pipe.ib;
    gs.ib_format = pipe.ib_format;
    gs.ib_off = pipe.ib_off;
    ret_stdcall(c, kSOk, 16);
}
void ia_set_topology(Cpu& c) {                                     // +0x60  0x6D0741
    pipe.topology = arg(c, 1);
    gs.topology = arg(c, 1);
    ret_stdcall(c, kSOk, 8);
}
void vs_set_samplers(Cpu& c) {                                     // +0x68  0x6D031C
    set_objects(c, gs.vs_samp, gpu::kSlots, arg(c, 1), arg(c, 2), arg(c, 3));
    ret_stdcall(c, kSOk, 16);
}
void om_set_blend(Cpu& c) {                                        // +0x8C  0x6D06E6, state, factor, mask
    if (arg(c, 1) != g_last_blend) {
        g_last_blend = arg(c, 1);
        const auto it = blend_states.find(arg(c, 1));
        for (unsigned i = 0; i < gpu::kTargets; ++i) gs.blend[i] = it != blend_states.end() ? it->second[i] : gpu::BlendTarget{};
        gs.alpha_to_coverage = alpha_to_coverage_states.count(arg(c, 1)) != 0;
    }
    if (arg(c, 2) != 0) std::memcpy(gs.blend_factor, c.mem + arg(c, 2), 16);
    else for (float& f : gs.blend_factor) f = 1;
    ret_stdcall(c, kSOk, 16);
}
void om_set_depth(Cpu& c) {                                        // +0x90  0x6D0012, state, stencil ref
    if (arg(c, 1) != g_last_depth) {
        g_last_depth = arg(c, 1);
        const auto it = depth_states.find(arg(c, 1));
        gs.depth = it != depth_states.end() ? it->second : gpu::DepthDesc{};
        // Without the depth pre-pass, a depth-equal test with no write (the main
        // pass over the pre-pass depth) tests less-equal and writes instead.
        if (skip_velocity_pass() && gs.depth.enable && !gs.depth.write && gs.depth.func == 3u) {
            gs.depth.func = 4u;  // D3D11_COMPARISON_LESS_EQUAL
            gs.depth.write = true;
        }
    }
    ret_stdcall(c, kSOk, 12);
}
void rs_set_state(Cpu& c) {                                        // +0xAC  0x6CFFA5
    if (arg(c, 1) != g_last_raster) {
        g_last_raster = arg(c, 1);
        const auto it = raster_states.find(arg(c, 1));
        gs.raster = it != raster_states.end() ? it->second : gpu::RasterDesc{};
    }
    ret_stdcall(c, kSOk, 8);
}
void query_end(Cpu& c) {
    // End(query): a TIMESTAMP query takes the GPU time completed so far (query_get_data).
    const std::uint32_t query = arg(c, 1);
    if (query != 0 && gpu::enabled() && ld32(c.mem, query + 12) == 2) {
        query_marks[query] = QueryMark{g_frames_presented, g_frame_draws != 0};
        static const bool trace = std::getenv("DCUSA_QUERY_TRACE") != nullptr;
        if (trace) std::fprintf(stderr, "timestamp %08x frame %llu after %u draws\n", query,
                                static_cast<unsigned long long>(g_frames_presented), g_frame_draws);
    }
    ret_stdcall(c, kSOk, 8);
}

void device_call(Cpu& c, std::uint32_t slot) {
    switch (slot) {
    case 0: query(c, Device); break;
    case 4: addref(c); break;
    case 8: release(c); break;
    case 0x0C: create_buffer(c); break;
    case 0x14: create_texture(c); break;
    case 0x1C: create_srv(c); break;
    case 0x24: create_rtv(c); break;
    case 0x28: create_dsv(c); break;
    case 0x2C: create_layout(c); break;
    case 0x50: create_blend(c); break;
    case 0x54: create_depth_state(c); break;
    case 0x58: create_rasterizer(c); break;
    case 0x30: create_shader(c); break;
    case 0x3C: create_shader(c); break;
    case 0x48: create_shader(c); break;
    case 0x5C: create_sampler(c); break;
    case 0x60: create_query(c); break;
    case 0x78: check_msaa(c); break;
    case 0x84: check_feature(c); break;
    default: refuse(Device, slot);
    }
}

constexpr std::uint32_t kSoundScratch = 0x36000000u;
constexpr std::uint32_t kSoundScratchBytes = 1u << 20;

// DirectInput: the cabinet's SEGA IO board, the one device the game reads.
//
// 0x5DEBA0 enumerates game controllers (callback 0x5DE930) and keeps a device
// only when its instance name is "SEGA IO Controller"; it then enumerates the
// axes (callback 0x5DE8B0 sets each to -1000..1000), sets c_dfDIJoystick and
// acquires it. Each frame 0x47CDF0 polls it with GetDeviceState and maps:
//   lX  steering: (lX + 1000) / 2000 * 255, then the test-menu calibration
//   lRx accelerator, lZ brake: -1000 released .. +1000 full
//   buttons through the profile at 0x116D490 + 4 * 0xE0 (button -> action):
//     0 start (2), 8..11 VR / view 1..4 (3..6), 3 shift up (14), 4 shift down
//     (13), 1 2 6 7 gear 1..4 (7..10).
// The game counts a shift once per press (0x5E2F96) and keeps the gear in
// 1..4 itself. Coin is not on the board: the shell sends credits
// (shell_insert_coin). A second board (the H-pattern lever, player 2) is not
// reported, which reads as the lever in neutral.
constexpr char kIoBoardName[] = "SEGA IO Controller";
// Axis instances in c_dfDIJoystick: lX 0, lY 1, lZ 2, lRx 3.
constexpr unsigned kAxes = 4;
std::int32_t axis_min[kAxes] = {0, 0, 0, 0}, axis_max[kAxes] = {65535, 65535, 65535, 65535};
bool coin_held = false;

void dinput_create_device(Cpu& c) {
    const std::uint32_t out = arg(c, 2);
    if (out != 0) st32(c.mem, out, object(DInputDev));
    ret_stdcall(c, kSOk, 16);
}

void put_name(std::uint8_t* mem, std::uint32_t at, const char* name) {
    for (std::uint32_t i = 0;; ++i) {
        st8(mem, at + i, static_cast<std::uint8_t>(name[i]));
        if (name[i] == 0) break;
    }
}

// EnumDevices(this, dwDevType, callback, pvRef, dwFlags): one DIDEVICEINSTANCEA.
void dinput_enum(Cpu& c) {
    const std::uint32_t type = arg(c, 1), callback = arg(c, 2), ref = arg(c, 3);
    // DI8DEVCLASS_ALL (0) or DI8DEVCLASS_GAMECTRL (4).
    if (callback != 0 && (type == 0 || type == 4)) {
        const std::uint32_t saved = c.r[Cpu::ESP];
        c.r[Cpu::ESP] = (saved - 0x300u) & ~15u;
        const std::uint32_t inst = c.r[Cpu::ESP];
        std::memset(c.mem + inst, 0, 0x244);
        st32(c.mem, inst, 0x244);              // dwSize
        st32(c.mem, inst + 0x04, 0x5E6A10D0);  // guidInstance (any stable value)
        st32(c.mem, inst + 0x14, 0x5E6A10D1);  // guidProduct
        st32(c.mem, inst + 0x24, 0x00000116);  // dwDevType: DI8DEVTYPE_DRIVING, first person
        put_name(c.mem, inst + 0x28, kIoBoardName);   // tszInstanceName
        put_name(c.mem, inst + 0x12C, kIoBoardName);  // tszProductName
        call_guest(c, callback, {inst, ref});
        c.r[Cpu::ESP] = saved;
    }
    ret_stdcall(c, kSOk, 20);
}

// EnumObjects(this, callback, pvRef, dwFlags): the four absolute axes.
void dinput_enum_objects(Cpu& c) {
    const std::uint32_t callback = arg(c, 1), ref = arg(c, 2), flags = arg(c, 3);
    // DIDFT_ALL (0), or a mask that includes DIDFT_ABSAXIS (2).
    if (callback != 0 && (flags == 0 || (flags & 2u) != 0)) {
        const std::uint32_t saved = c.r[Cpu::ESP];
        c.r[Cpu::ESP] = (saved - 0x200u) & ~15u;
        const std::uint32_t obj = c.r[Cpu::ESP];
        static const char* names[kAxes] = {"X Axis", "Y Axis", "Z Axis", "X Rotation"};
        static const std::uint32_t offsets[kAxes] = {0, 4, 8, 12};
        for (unsigned i = 0; i < kAxes; ++i) {
            std::memset(c.mem + obj, 0, 0x13C);
            st32(c.mem, obj, 0x13C);                  // dwSize
            st32(c.mem, obj + 0x14, offsets[i]);      // dwOfs
            st32(c.mem, obj + 0x18, 2u | (i << 8));   // dwType: DIDFT_ABSAXIS | instance
            put_name(c.mem, obj + 0x20, names[i]);
            if (call_guest(c, callback, {obj, ref}) == 0) break;  // DIENUM_STOP
        }
        c.r[Cpu::ESP] = saved;
    }
    ret_stdcall(c, kSOk, 16);
}

void dinput_caps(Cpu& c) {
    const std::uint32_t caps = arg(c, 1);
    if (caps != 0) {
        const std::uint32_t n = ld32(c.mem, caps);
        if (n >= 24 && n <= 64) {
            st32(c.mem, caps + 4, 0x5u);     // DIDC_ATTACHED | DIDC_POLLEDDEVICE
            st32(c.mem, caps + 8, 0x116u);   // dwDevType
            st32(c.mem, caps + 12, kAxes);   // dwAxes
            st32(c.mem, caps + 16, 12u);     // dwButtons
            st32(c.mem, caps + 20, 0u);      // dwPOVs
        }
    }
    ret_stdcall(c, kSOk, 8);
}

// DIPROP_RANGE (4): DIPROPHEADER {dwSize, dwHeaderSize, dwObj, dwHow} then lMin, lMax.
void dinput_get_prop(Cpu& c) {
    const std::uint32_t prop = arg(c, 1);
    const std::uint32_t hdr = arg(c, 2);
    if (prop == 4 && hdr != 0 && ld32(c.mem, hdr) >= 24) {
        const unsigned i = ld32(c.mem, hdr + 12) == 2 ? (ld32(c.mem, hdr + 8) >> 8) & 0xFFFFu : 0u;
        st32(c.mem, hdr + 16, static_cast<std::uint32_t>(axis_min[i < kAxes ? i : 0]));
        st32(c.mem, hdr + 20, static_cast<std::uint32_t>(axis_max[i < kAxes ? i : 0]));
    }
    ret_stdcall(c, kSOk, 12);
}

void dinput_set_prop(Cpu& c) {
    const std::uint32_t prop = arg(c, 1);
    const std::uint32_t hdr = arg(c, 2);
    if (prop == 4 && hdr != 0 && ld32(c.mem, hdr) >= 24) {
        const auto lo = static_cast<std::int32_t>(ld32(c.mem, hdr + 16));
        const auto hi = static_cast<std::int32_t>(ld32(c.mem, hdr + 20));
        const std::uint32_t how = ld32(c.mem, hdr + 12);
        for (unsigned i = 0; i < kAxes && lo < hi; ++i) {
            // DIPH_DEVICE (0) sets every axis; DIPH_BYID (2) the one named by dwObj.
            if (how == 0 || (how == 2 && ((ld32(c.mem, hdr + 8) >> 8) & 0xFFFFu) == i)) {
                axis_min[i] = lo;
                axis_max[i] = hi;
            }
        }
    }
    ret_stdcall(c, kSOk, 12);
}

void dinput_acquire(Cpu& c) { ret_stdcall(c, kSOk, 4); }
void dinput_format(Cpu& c) { ret_stdcall(c, kSOk, 8); }
void dinput_coop(Cpu& c) { ret_stdcall(c, kSOk, 12); }

// A value in 0..1 (or -1..1 when `centred`) as a position in the axis range.
std::int32_t axis_value(unsigned axis, float v, bool centred) {
    const float t = centred ? (std::clamp(v, -1.0f, 1.0f) + 1.0f) * 0.5f : std::clamp(v, 0.0f, 1.0f);
    const double span = static_cast<double>(axis_max[axis]) - axis_min[axis];
    return static_cast<std::int32_t>(std::lround(axis_min[axis] + t * span));
}

// GetDeviceState(this, cbData, lpvData) with c_dfDIJoystick (80 bytes).
void dinput_state(Cpu& c) {
    const std::uint32_t cb = arg(c, 1);
    const std::uint32_t data = arg(c, 2);
    const dcusa::PadSnapshot& pad = current_pad();
    if (data != 0 && cb >= 80 && cb <= 512) {
        std::memset(c.mem + data, 0, cb);
        const bool on = pad.connected;
        st32(c.mem, data + 0, static_cast<std::uint32_t>(axis_value(0, on ? pad.steering : 0.0f, true)));  // lX
        st32(c.mem, data + 4, static_cast<std::uint32_t>(axis_value(1, 0.0f, true)));                        // lY
        st32(c.mem, data + 8, static_cast<std::uint32_t>(axis_value(2, on ? pad.brake : 0.0f, false)));     // lZ
        st32(c.mem, data + 12, static_cast<std::uint32_t>(axis_value(3, on ? pad.accelerator : 0.0f, false)));  // lRx
        for (std::uint32_t i = 0; i < 4; ++i) st32(c.mem, data + 32 + i * 4, 0xFFFFFFFFu);  // POVs centred
        static const struct {
            unsigned button;
            std::uint32_t bit;
        } board[] = {
            {0, dcusa::PadSnapshot::Start},     {8, dcusa::PadSnapshot::View1},     {9, dcusa::PadSnapshot::View2},
            {10, dcusa::PadSnapshot::View3},    {11, dcusa::PadSnapshot::View4},    {3, dcusa::PadSnapshot::ShiftUp},
            {4, dcusa::PadSnapshot::ShiftDown}, {1, dcusa::PadSnapshot::Gear1},     {2, dcusa::PadSnapshot::Gear2},
            {6, dcusa::PadSnapshot::Gear3},     {7, dcusa::PadSnapshot::Gear4},
        };
        if (on)
            for (const auto& b : board)
                if ((pad.buttons & b.bit) != 0) c.mem[data + 48 + b.button] = 0x80;
    }
    // One credit per press of the coin button.
    const bool coin = pad.connected && (pad.buttons & dcusa::PadSnapshot::Coin) != 0;
    if (coin && !coin_held) shell_insert_coin(c.mem);
    coin_held = coin;
    ret_stdcall(c, kSOk, 12);
}

// Each buffer's WAVEFORMATEX (18 bytes): from DSBUFFERDESC.lpwfxFormat (+16),
// or SetFormat on the primary buffer, whose default is 44.1 kHz stereo 16-bit.
std::unordered_map<std::uint32_t, std::array<std::uint8_t, 18>> sound_formats;

std::array<std::uint8_t, 18> default_wave_format() {
    std::array<std::uint8_t, 18> f{};
    auto put16 = [&](int at, std::uint16_t v) { f[static_cast<std::size_t>(at)] = static_cast<std::uint8_t>(v); f[static_cast<std::size_t>(at) + 1] = static_cast<std::uint8_t>(v >> 8); };
    auto put32 = [&](int at, std::uint32_t v) { put16(at, static_cast<std::uint16_t>(v)); put16(at + 2, static_cast<std::uint16_t>(v >> 16)); };
    put16(0, 1);           // WAVE_FORMAT_PCM
    put16(2, 2);           // channels
    put32(4, 44100);       // samples per second
    put32(8, 44100 * 4);   // bytes per second
    put16(12, 4);          // block align
    put16(14, 16);         // bits per sample
    return f;
}

// Sound buffer memory: guest bytes Lock hands out, carved from kSoundScratch.
// A released buffer's bytes go back to a free list by size (the game makes
// three buffers at start-up; CRI may make and drop more).
std::uint32_t sound_used = 0;
std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> sound_free;

std::uint32_t sound_alloc(std::uint8_t* mem, std::uint32_t bytes) {
    bytes = (bytes + 63u) & ~63u;
    if (auto it = sound_free.find(bytes); it != sound_free.end() && !it->second.empty()) {
        const std::uint32_t at = it->second.back();
        it->second.pop_back();
        std::memset(mem + at, 0, bytes);
        return at;
    }
    if (sound_used == 0 && mprotect(mem + kSoundScratch, kSoundScratchBytes, PROT_READ | PROT_WRITE) != 0)
        fail_closed(Unported::Service, kSoundScratch, "DirectSound buffer memory");
    if (sound_used + bytes > kSoundScratchBytes)
        fail_closed(Unported::Service, kSoundScratch, "DirectSound buffer memory is full");
    const std::uint32_t at = kSoundScratch + sound_used;
    sound_used += bytes;
    return at;
}

audio::WaveFormat wave_format(const std::array<std::uint8_t, 18>& f) {
    audio::WaveFormat w;
    w.tag = static_cast<std::uint16_t>(f[0] | f[1] << 8);
    w.channels = static_cast<std::uint16_t>(f[2] | f[3] << 8);
    w.rate = static_cast<std::uint32_t>(f[4] | f[5] << 8 | f[6] << 16 | f[7] << 24);
    w.block = static_cast<std::uint16_t>(f[12] | f[13] << 8);
    w.bits = static_cast<std::uint16_t>(f[14] | f[15] << 8);
    return w;
}

// CreateSoundBuffer(desc, out, outer). DSBUFFERDESC: dwSize, dwFlags,
// dwBufferBytes, dwReserved, lpwfxFormat. The primary buffer (DSBCAPS_PRIMARYBUFFER,
// 1) only carries the output format; secondary buffers get memory and a voice.
// The object keeps its size at +12 and its memory at +24.
void ds_create_buffer(Cpu& c) {
    const std::uint32_t desc = arg(c, 1);
    const std::uint32_t out = arg(c, 2);
    const std::uint32_t obj = spawn(c.mem, DSoundBuf);
    std::array<std::uint8_t, 18> format = default_wave_format();
    if (desc != 0 && ld32(c.mem, desc + 16) != 0) std::memcpy(format.data(), c.mem + ld32(c.mem, desc + 16), 18);
    sound_formats[obj] = format;
    const bool primary = desc != 0 && (ld32(c.mem, desc + 4) & 1u) != 0;
    const std::uint32_t bytes = desc == 0 || primary ? 0u : ld32(c.mem, desc + 8);
    st32(c.mem, obj + 12, bytes);
    if (bytes != 0) {
        const std::uint32_t at = sound_alloc(c.mem, bytes);
        st32(c.mem, obj + 24, at);
        sound_mixer().create(obj, wave_format(format), c.mem + at, bytes);
    }
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 16);
}

void ds_coop(Cpu& c) { ret_stdcall(c, kSOk, 12); }

// DSSPEAKER_STEREO. 0xB54A90 maps the low byte 4 onto mode 2, which SetupSound accepts.
void ds_speaker(Cpu& c) {
    const std::uint32_t out = arg(c, 1);
    if (out != 0) st32(c.mem, out, 4);
    ret_stdcall(c, kSOk, 8);
}

void ds_caps(Cpu& c) {
    const std::uint32_t caps = arg(c, 1);
    if (caps != 0) {
        const std::uint32_t n = ld32(c.mem, caps);
        if (n > 4 && n <= 256) std::memset(c.mem + caps + 4, 0, n - 4);
        // DSBCAPS: dwSize, dwFlags, dwBufferBytes. The game's streaming code
        // reads the size back.
        if (n >= 12 && ld32(c.mem, arg(c, 0) + 4) == DSoundBuf) st32(c.mem, caps + 8, ld32(c.mem, arg(c, 0) + 12));
    }
    ret_stdcall(c, kSOk, 8);
}

// GetCurrentPosition(play, write): where the output has got to, in bytes.
void ds_position(Cpu& c) {
    std::uint32_t play = 0, write = 0;
    sound_mixer().position(arg(c, 0), &play, &write);
    if (arg(c, 1) != 0) st32(c.mem, arg(c, 1), play);
    if (arg(c, 2) != 0) st32(c.mem, arg(c, 2), write);
    ret_stdcall(c, kSOk, 12);
}

// Lock(offset, bytes, *p1, *n1, *p2, *n2, flags): the range in the buffer's
// memory, split in two where it wraps. DSBLOCK_FROMWRITECURSOR (1) starts at
// the write cursor; DSBLOCK_ENTIREBUFFER (2) takes all of it.
void ds_lock(Cpu& c) {
    const std::uint32_t self = arg(c, 0);
    const std::uint32_t size = ld32(c.mem, self + 12);
    const std::uint32_t base = ld32(c.mem, self + 24);
    const std::uint32_t p1 = arg(c, 3), n1 = arg(c, 4), p2 = arg(c, 5), n2 = arg(c, 6), flags = arg(c, 7);
    if (size == 0 || base == 0) {
        ret_stdcall(c, 0x80070057u, 32);  // E_INVALIDARG: the primary buffer has no memory
        return;
    }
    std::uint32_t offset = arg(c, 1), bytes = arg(c, 2);
    if ((flags & 1u) != 0) {
        std::uint32_t play = 0;
        sound_mixer().position(self, &play, &offset);
    }
    if ((flags & 2u) != 0) bytes = size;
    offset %= size;
    bytes = std::min(bytes, size);
    const std::uint32_t first = std::min(bytes, size - offset);
    if (p1 != 0) st32(c.mem, p1, base + offset);
    if (n1 != 0) st32(c.mem, n1, first);
    if (p2 != 0) st32(c.mem, p2, bytes > first ? base : 0u);
    if (n2 != 0) st32(c.mem, n2, bytes - first);
    ret_stdcall(c, kSOk, 32);
}

// Play(reserved, priority, flags). DSBPLAY_LOOPING is 1.
void ds_play(Cpu& c) {
    sound_mixer().play(arg(c, 0), (arg(c, 3) & 1u) != 0);
    ret_stdcall(c, kSOk, 16);
}

void ds_set_position(Cpu& c) {
    sound_mixer().set_position(arg(c, 0), arg(c, 1));
    ret_stdcall(c, kSOk, 8);
}

// GetFormat(format, size, *written): the WAVEFORMATEX the buffer was made with.
void ds_get_format(Cpu& c) {
    const auto it = sound_formats.find(arg(c, 0));
    const std::array<std::uint8_t, 18> f = it != sound_formats.end() ? it->second : default_wave_format();
    const std::uint32_t out = arg(c, 1), cap = arg(c, 2), written = arg(c, 3);
    const std::uint32_t n = out == 0 ? 0u : std::min<std::uint32_t>(cap, 18u);
    if (n != 0) std::memcpy(c.mem + out, f.data(), n);
    if (written != 0) st32(c.mem, written, out == 0 ? 18u : n);
    ret_stdcall(c, kSOk, 16);
}

// SetFormat(format): the primary buffer's output format.
void ds_set_format(Cpu& c) {
    if (arg(c, 1) != 0) std::memcpy(sound_formats[arg(c, 0)].data(), c.mem + arg(c, 1), 18);
    ret_stdcall(c, kSOk, 8);
}

// GetVolume (+0x18), GetPan (+0x1C), GetFrequency (+0x20).
void ds_get_level(Cpu& c, std::uint32_t slot) {
    const std::uint32_t self = arg(c, 0), out = arg(c, 1);
    if (out != 0) {
        audio::DsMixer& m = sound_mixer();
        st32(c.mem, out, slot == 0x18 ? static_cast<std::uint32_t>(m.volume(self))
                         : slot == 0x1C ? static_cast<std::uint32_t>(m.pan(self)) : m.frequency(self));
    }
    ret_stdcall(c, kSOk, 8);
}

void ds_stop(Cpu& c) {
    sound_mixer().stop(arg(c, 0));
    ret_stdcall(c, kSOk, 4);
}

void ds_status(Cpu& c) {
    const std::uint32_t out = arg(c, 1);
    if (out != 0) st32(c.mem, out, sound_mixer().status(arg(c, 0)));
    ret_stdcall(c, kSOk, 8);
}

// DCUSA_DSOUND_TRACE=1: the first calls of each kind per buffer, with their
// arguments and caller, and every CreateSoundBuffer in full.
void ds_trace(Cpu& c, bool device, std::uint32_t slot) {
    static const bool on = std::getenv("DCUSA_DSOUND_TRACE") != nullptr;
    if (!on) return;
    static std::unordered_map<std::uint64_t, unsigned> seen;
    const std::uint32_t self = arg(c, 0);
    const unsigned n = ++seen[(std::uint64_t(self) << 16) | slot];
    if (device && slot == 0x0C) {
        const std::uint32_t desc = arg(c, 1);
        const std::uint32_t fmt = desc ? ld32(c.mem, desc + 16) : 0;
        std::fprintf(stderr, "ds create flags %08x bytes %u fmt %u ch %u rate %u bits %u (caller %08x)\n",
                     desc ? ld32(c.mem, desc + 4) : 0, desc ? ld32(c.mem, desc + 8) : 0, fmt ? ld16(c.mem, fmt) : 0,
                     fmt ? ld16(c.mem, fmt + 2) : 0, fmt ? ld32(c.mem, fmt + 4) : 0, fmt ? ld16(c.mem, fmt + 14) : 0,
                     ld32(c.mem, c.r[Cpu::ESP]));
        if (fmt != 0) {
            std::fprintf(stderr, "   format bytes:");
            for (unsigned i = 0; i < 40; ++i) std::fprintf(stderr, " %02x", c.mem[fmt + i]);
            std::fprintf(stderr, "\n");
        }
        return;
    }
    if (n > 6 && n != 100 && n != 1000 && n != 10000) return;
    std::fprintf(stderr, "ds %s %08x slot %02x #%u args %08x %08x %08x %08x %08x (caller %08x)\n", device ? "dev" : "buf",
                 self, slot, n, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), ld32(c.mem, c.r[Cpu::ESP]));
}

// SetVolume (+0x3C), SetPan (+0x40), SetFrequency (+0x44).
void ds_set_level(Cpu& c, std::uint32_t slot) {
    const std::uint32_t self = arg(c, 0), v = arg(c, 1);
    audio::DsMixer& m = sound_mixer();
    if (slot == 0x3C) m.set_volume(self, static_cast<std::int32_t>(v));
    else if (slot == 0x40) m.set_pan(self, static_cast<std::int32_t>(v));
    else m.set_frequency(self, v);
    ret_stdcall(c, kSOk, 8);
}
void ds_unlock(Cpu& c) { ret_stdcall(c, kSOk, 20); }
void ds_restore(Cpu& c) { ret_stdcall(c, kSOk, 4); }

struct ShaderBind {
    char name[48]{};
    std::uint32_t type = 0;
    std::uint32_t slot = 0;
    std::uint32_t dimension = 0;
};

std::unordered_map<std::uint32_t, std::vector<ShaderBind>> shader_binds;

// A translated module's constant buffers, from its .refl (tools/shaderprep).
// Offsets are the MSL's, so the bytes the game writes are where Metal reads them.
struct PreparedVar {
    std::string name, type;
    std::uint32_t offset = 0, size = 0;
    std::uint32_t elements = 0;  // array length; 0 for a single value
    char major = '-';            // 'c' column-major matrix (the HLSL default), 'r' row-major
};
struct PreparedBuffer {
    std::string name;
    std::uint32_t slot = 0, size = 0;
    std::vector<PreparedVar> vars;
};
std::unordered_map<std::uint32_t, std::vector<PreparedBuffer>> shader_buffers;  // bytecode -> buffers

// Reflection objects handed out for (bytecode, buffer, variable), so repeated
// lookups return the same pointer. kNoIndex marks the buffer object itself, and
// the variable index kNoIndex on a buffer index kNoIndex is D3D's "invalid"
// object, whose GetDesc fails.
constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;
struct ReflRef {
    std::uint32_t code = 0, buffer = kNoIndex, var = kNoIndex;
};
std::unordered_map<std::uint32_t, ReflRef> refl_objects;     // object -> what it describes
std::unordered_map<std::uint64_t, std::uint32_t> refl_cache;  // (kind, code, buffer, var) -> object
std::unordered_map<std::uint32_t, std::uint32_t> reflector_source;

bool ident_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

std::vector<ShaderBind> parse_shader(const std::uint8_t* mem, std::uint32_t src, std::uint32_t size) {
    std::vector<ShaderBind> found;
    if (src == 0 || size < 8) return found;
    if (size > (1u << 20)) size = 1u << 20;
    std::string text;
    text.resize(size);
    std::memcpy(text.data(), mem + src, size);
    struct Key {
        const char* word;
        std::uint32_t type;
        std::uint32_t dimension;
    };
    const Key keys[] = {
        {"Texture2DArray", 2, 5}, {"Texture2DMS", 2, 6}, {"TextureCube", 2, 9}, {"Texture3D", 2, 8},
        {"Texture2D", 2, 4},      {"Texture1D", 2, 2},   {"SamplerComparisonState", 3, 0}, {"SamplerState", 3, 0},
    };
    std::uint32_t next_tex = 0;
    std::uint32_t next_samp = 0;
    for (std::size_t i = 0; i < text.size() && found.size() < 32;) {
        if (text[i] == '#') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
            if (i < text.size()) ++i;
            if (i < text.size()) ++i;
            continue;
        }
        if (i > 0 && ident_char(text[i - 1])) {
            ++i;
            continue;
        }
        const Key* key = nullptr;
        for (const Key& candidate : keys) {
            const std::size_t n = std::strlen(candidate.word);
            if (i + n <= text.size() && std::memcmp(text.data() + i, candidate.word, n) == 0 &&
                (i + n == text.size() || !ident_char(text[i + n]))) {
                key = &candidate;
                i += n;
                break;
            }
        }
        if (key == nullptr) {
            ++i;
            continue;
        }
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
        if (i < text.size() && text[i] == '<') {
            int depth = 0;
            for (; i < text.size(); ++i) {
                if (text[i] == '<') ++depth;
                else if (text[i] == '>' && --depth == 0) {
                    ++i;
                    break;
                }
            }
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        }
        if (i >= text.size() || !ident_char(text[i]) || (text[i] >= '0' && text[i] <= '9')) {
            if (i < text.size()) ++i;
            continue;
        }
        ShaderBind bind;
        std::size_t n = 0;
        while (i < text.size() && ident_char(text[i]) && n + 1 < sizeof bind.name) bind.name[n++] = text[i++];
        bind.name[n] = 0;
        const char* ignored[] = {"float", "half", "int", "uint", "void", "sampler2D", "samplerCube", "return"};
        bool skip = false;
        for (const char* bad : ignored)
            if (std::strcmp(bind.name, bad) == 0) skip = true;
        if (skip) continue;
        bind.type = key->type;
        bind.dimension = key->dimension;
        bind.slot = key->type == 3 ? next_samp++ : next_tex++;
        found.push_back(bind);
    }
    return found;
}

void note_shader(std::uint32_t shader) {
    if (shader == 0 || !stats_on()) return;
    const auto code = shader_code.find(shader);
    if (code == shader_code.end()) return;
    const auto found = shader_binds.find(code->second);
    if (found == shader_binds.end()) {
        std::fprintf(stderr, "  shader bytes %08x no binds\n", code->second);
        return;
    }
    std::fprintf(stderr, "  shader");
    for (const ShaderBind& bind : found->second) {
        if (bind.type != 2) continue;
        std::fprintf(stderr, " t%u=%s", bind.slot, bind.name);
    }
    std::fprintf(stderr, "\n");
}

const std::vector<ShaderBind>* binds_of(std::uint32_t reflector) {
    const auto src = reflector_source.find(reflector);
    if (src == reflector_source.end()) return nullptr;
    const auto found = shader_binds.find(src->second);
    if (found == shader_binds.end() || found->second.empty()) return nullptr;
    return &found->second;
}

void reflect_output(Cpu& c) { ret_stdcall(c, 0x80004005u, 12); }

void reflect_binding(Cpu& c) {
    const std::uint32_t self = arg(c, 0);
    const std::uint32_t index = arg(c, 1);
    const std::uint32_t desc = arg(c, 2);
    const std::vector<ShaderBind>* binds = binds_of(self);
    if (desc != 0 && binds != nullptr && index < binds->size()) {
        const ShaderBind& bind = (*binds)[index];
        std::memset(c.mem + desc, 0, 32);
        st32(c.mem, desc, intern(c.mem, bind.name));
        st32(c.mem, desc + 4, bind.type);
        st32(c.mem, desc + 8, bind.slot);
        st32(c.mem, desc + 12, 1);
        st32(c.mem, desc + 24, bind.dimension);
    }
    ret_stdcall(c, kSOk, 12);
}

std::uint32_t code_of(std::uint32_t reflector) {
    const auto src = reflector_source.find(reflector);
    return src == reflector_source.end() ? 0u : src->second;
}

const std::vector<PreparedBuffer>* buffers_of_code(std::uint32_t code) {
    const auto found = shader_buffers.find(code);
    return found == shader_buffers.end() ? nullptr : &found->second;
}

const std::vector<PreparedBuffer>* buffers_of(std::uint32_t reflector) { return buffers_of_code(code_of(reflector)); }

std::uint32_t refl_object(std::uint8_t* mem, Kind kind, std::uint32_t code, std::uint32_t buffer, std::uint32_t var) {
    const std::uint64_t key = (static_cast<std::uint64_t>(kind) << 56) ^ (static_cast<std::uint64_t>(code) << 24) ^
                              (static_cast<std::uint64_t>(buffer & 0xFFFu) << 12) ^ (var & 0xFFFu);
    const auto hit = refl_cache.find(key);
    if (hit != refl_cache.end()) return hit->second;
    const std::uint32_t obj = spawn(mem, kind);
    refl_objects[obj] = ReflRef{code, buffer, var};
    refl_cache.emplace(key, obj);
    return obj;
}

const PreparedBuffer* refl_buffer(const ReflRef& r) {
    const std::vector<PreparedBuffer>* buffers = buffers_of_code(r.code);
    return buffers != nullptr && r.buffer < buffers->size() ? &(*buffers)[r.buffer] : nullptr;
}

const PreparedVar* refl_var(const ReflRef& r) {
    const PreparedBuffer* b = refl_buffer(r);
    return b != nullptr && r.var < b->vars.size() ? &b->vars[r.var] : nullptr;
}

ReflRef ref_of(std::uint32_t obj) {
    const auto it = refl_objects.find(obj);
    return it == refl_objects.end() ? ReflRef{} : it->second;
}

// ID3D11ShaderReflection::GetConstantBufferByIndex / ByName return the object,
// not an HRESULT. Out of range is D3D's invalid object, whose GetDesc fails.
void reflect_buffer_by_index(Cpu& c) {
    const std::uint32_t code = code_of(arg(c, 0));
    const std::uint32_t index = arg(c, 1);
    const std::vector<PreparedBuffer>* buffers = buffers_of_code(code);
    const bool ok = buffers != nullptr && index < buffers->size();
    ret_stdcall(c, refl_object(c.mem, ReflBuffer, code, ok ? index : kNoIndex, kNoIndex), 8);
}

void reflect_buffer_by_name(Cpu& c) {
    const std::uint32_t code = code_of(arg(c, 0));
    const std::string name = guest_cstr(c.mem, arg(c, 1), 256);
    std::uint32_t index = kNoIndex;
    if (const std::vector<PreparedBuffer>* buffers = buffers_of_code(code))
        for (std::uint32_t i = 0; i < buffers->size(); ++i)
            if ((*buffers)[i].name == name) index = i;
    ret_stdcall(c, refl_object(c.mem, ReflBuffer, code, index, kNoIndex), 8);
}

// ID3D11ShaderReflection::GetVariableByName searches every constant buffer.
void reflect_variable_by_name(Cpu& c) {
    const std::uint32_t code = code_of(arg(c, 0));
    const std::string name = guest_cstr(c.mem, arg(c, 1), 256);
    std::uint32_t bi = kNoIndex, vi = kNoIndex;
    if (const std::vector<PreparedBuffer>* buffers = buffers_of_code(code))
        for (std::uint32_t b = 0; b < buffers->size() && bi == kNoIndex; ++b)
            for (std::uint32_t v = 0; v < (*buffers)[b].vars.size(); ++v)
                if ((*buffers)[b].vars[v].name == name) {
                    bi = b;
                    vi = v;
                    break;
                }
    ret_stdcall(c, refl_object(c.mem, ReflVariable, code, bi, vi), 8);
}

void reflect_binding_by_name(Cpu& c) {
    const std::uint32_t self = arg(c, 0);
    const std::string name = guest_cstr(c.mem, arg(c, 1), 256);
    const std::vector<ShaderBind>* binds = binds_of(self);
    if (binds != nullptr) {
        for (std::uint32_t i = 0; i < binds->size(); ++i) {
            if (name != (*binds)[i].name) continue;
            // Same layout as GetResourceBindingDesc; reuse it with the index.
            st32(c.mem, c.r[Cpu::ESP] + 8, i);
            return reflect_binding(c);
        }
    }
    ret_stdcall(c, 0x80070057u, 12);  // E_INVALIDARG
}

// D3D11_SHADER_BUFFER_DESC: Name, Type (D3D_CT_CBUFFER), Variables, Size, uFlags.
void buffer_desc(Cpu& c) {
    const PreparedBuffer* b = refl_buffer(ref_of(arg(c, 0)));
    const std::uint32_t desc = arg(c, 1);
    if (b == nullptr || desc == 0) return ret_stdcall(c, 0x80004005u, 8);
    ++g_refl_buffers;
    st32(c.mem, desc, intern(c.mem, b->name));
    st32(c.mem, desc + 4, 0);
    st32(c.mem, desc + 8, static_cast<std::uint32_t>(b->vars.size()));
    st32(c.mem, desc + 12, b->size);
    st32(c.mem, desc + 16, 0);
    ret_stdcall(c, kSOk, 8);
}

void buffer_variable_by_index(Cpu& c) {
    const ReflRef r = ref_of(arg(c, 0));
    const PreparedBuffer* b = refl_buffer(r);
    const std::uint32_t index = arg(c, 1);
    const bool ok = b != nullptr && index < b->vars.size();
    ret_stdcall(c, refl_object(c.mem, ReflVariable, r.code, ok ? r.buffer : kNoIndex, ok ? index : kNoIndex), 8);
}

void buffer_variable_by_name(Cpu& c) {
    const ReflRef r = ref_of(arg(c, 0));
    const PreparedBuffer* b = refl_buffer(r);
    const std::string name = guest_cstr(c.mem, arg(c, 1), 256);
    std::uint32_t vi = kNoIndex;
    if (b != nullptr)
        for (std::uint32_t v = 0; v < b->vars.size(); ++v)
            if (b->vars[v].name == name) vi = v;
    ret_stdcall(c, refl_object(c.mem, ReflVariable, r.code, vi == kNoIndex ? kNoIndex : r.buffer, vi), 8);
}

// D3D11_SHADER_VARIABLE_DESC: Name, StartOffset, Size, uFlags (D3D_SVF_USED),
// DefaultValue, StartTexture, TextureSize, StartSampler, SamplerSize.
void variable_desc(Cpu& c) {
    const PreparedVar* v = refl_var(ref_of(arg(c, 0)));
    const std::uint32_t desc = arg(c, 1);
    if (v == nullptr || desc == 0) return ret_stdcall(c, 0x80004005u, 8);
    ++g_refl_vars;
    st32(c.mem, desc, intern(c.mem, v->name));
    st32(c.mem, desc + 4, v->offset);
    st32(c.mem, desc + 8, v->size);
    st32(c.mem, desc + 12, 2);
    st32(c.mem, desc + 16, 0);
    st32(c.mem, desc + 20, 0xFFFFFFFFu);
    st32(c.mem, desc + 24, 0);
    st32(c.mem, desc + 28, 0xFFFFFFFFu);
    st32(c.mem, desc + 32, 0);
    ret_stdcall(c, kSOk, 8);
}

void variable_type(Cpu& c) {
    const ReflRef r = ref_of(arg(c, 0));
    ret_stdcall(c, refl_object(c.mem, ReflType, r.code, r.buffer, r.var), 4);
}

void variable_buffer(Cpu& c) {
    const ReflRef r = ref_of(arg(c, 0));
    ret_stdcall(c, refl_object(c.mem, ReflBuffer, r.code, r.buffer, kNoIndex), 4);
}

// D3D11_SHADER_TYPE_DESC: Class, Type, Rows, Columns, Elements, Members, Offset,
// Name. The SPIR-V type name (float, vec3, ivec4, mat4) gives the shape; HLSL
// matrices are column-major unless declared otherwise.
void type_desc(Cpu& c) {
    const PreparedVar* v = refl_var(ref_of(arg(c, 0)));
    const std::uint32_t desc = arg(c, 1);
    if (v == nullptr || desc == 0) return ret_stdcall(c, 0x80004005u, 8);
    std::uint32_t cls = 0, type = 3, rows = 1, cols = 1;  // D3D_SVC_SCALAR, D3D_SVT_FLOAT
    std::string base = "float";
    std::string t = v->type;
    if (t.rfind("ivec", 0) == 0 || t == "int") {
        type = 2;
        base = "int";
    } else if (t.rfind("uvec", 0) == 0 || t == "uint") {
        type = 19;
        base = "uint";
    } else if (t.rfind("bvec", 0) == 0 || t == "bool") {
        type = 1;
        base = "bool";
    }
    std::string name = base;
    if (const auto at = t.find("vec"); at != std::string::npos) {
        cls = 1;
        cols = static_cast<std::uint32_t>(std::atoi(t.c_str() + at + 3));
        name = base + std::to_string(cols);
    } else if (t.rfind("mat", 0) == 0) {
        cls = v->major == 'r' ? 2u : 3u;  // D3D_SVC_MATRIX_ROWS / _COLUMNS
        cols = static_cast<std::uint32_t>(std::atoi(t.c_str() + 3));
        const auto x = t.find('x');
        rows = x == std::string::npos ? cols : static_cast<std::uint32_t>(std::atoi(t.c_str() + x + 1));
        name = base + std::to_string(rows) + "x" + std::to_string(cols);
    } else if (t != base) {
        fail_closed(Unported::Service, v->offset, ("reflection type " + t).c_str());
    }
    ++g_refl_types;
    st32(c.mem, desc, cls);
    st32(c.mem, desc + 4, type);
    st32(c.mem, desc + 8, rows);
    st32(c.mem, desc + 12, cols);
    st32(c.mem, desc + 16, v->elements);
    st32(c.mem, desc + 20, 0);
    st32(c.mem, desc + 24, 0);
    st32(c.mem, desc + 28, intern(c.mem, name));
    ret_stdcall(c, kSOk, 8);
}

// The last reference went: forget everything kept for the object, release
// what a view held, and queue the slot for reuse.
void destroy(std::uint8_t* mem, std::uint32_t obj) {
    const std::uint32_t kind = ld32(mem, obj + 4);
    if (kind == Srv || kind == Rtv || kind == Dsv) drop_ref(mem, ld32(mem, obj + 12));
    stored.erase(obj);
    if (auto h = buffer_home.find(obj); h != buffer_home.end()) {
        free_homes.emplace(home_size[h->second], h->second);
        buffer_home.erase(h);
    }
    for (auto it = live_maps.begin(); it != live_maps.end();) it = it->resource == obj ? live_maps.erase(it) : it + 1;
    layouts.erase(obj);
    gpu_layouts.erase(obj);
    blend_states.erase(obj);
    if (obj == g_last_blend || obj == g_last_depth || obj == g_last_raster) g_last_blend = g_last_depth = g_last_raster = ~0u;
    alpha_to_coverage_states.erase(obj);
    depth_states.erase(obj);
    raster_states.erase(obj);
    shader_code.erase(obj);
    shader_obj_key.erase(obj);
    reflector_source.erase(obj);
    gpu::destroy(obj);
    if (kind == DSoundBuf) {
        sound_mixer().remove(obj);
        sound_formats.erase(obj);
        if (const std::uint32_t at = ld32(mem, obj + 24); at != 0)
            sound_free[(ld32(mem, obj + 12) + 63u) & ~63u].push_back(at);
    }
    if (obj == pipe.backbuffer) pipe.backbuffer = 0;
    st32(mem, obj, 0);  // a call through a stale pointer now fails closed (no vtable)
    free_slots.push_back(obj);
}

// ID3D11Buffer::GetDesc (+0x28).
void buffer_get_desc(Cpu& c) {
    const std::uint32_t self = arg(c, 0);
    const std::uint32_t desc = arg(c, 1);
    if (desc != 0)
        for (std::uint32_t i = 0; i < 6; ++i) st32(c.mem, desc + 4 * i, ld32(c.mem, self + 12 + 4 * i));
    ret_stdcall(c, 0, 8);
}

void plain_call(Cpu& c, Kind kind, std::uint32_t slot) {
    switch (slot) {
    case 0: query(c, kind); break;
    case 4: addref(c); break;
    case 8: release(c); break;
    default: refuse(kind, slot);
    }
}

}  // namespace

void set_presenter(dcusa::Presenter* presenter) { g_presenter = presenter; }

bool presented_frame(std::uint32_t* width, std::uint32_t* height, const std::uint8_t** rgba) {
    if (g_frame.empty() || g_frame_w == 0) return false;
    if (width != nullptr) *width = g_frame_w;
    if (height != nullptr) *height = g_frame_h;
    if (rgba != nullptr) *rgba = g_frame.data();
    return true;
}

void blob_pointer(Cpu& c) {
    const std::uint32_t self = arg(c, 0);
    ret_stdcall(c, ld32(c.mem, self + 12), 4);
}

void blob_size(Cpu& c) {
    const std::uint32_t self = arg(c, 0);
    ret_stdcall(c, ld32(c.mem, self + 16), 4);
}

// 0x6DC817. BoundResources is the dword at offset 16. Zero skips the binding walk at 0x6DC860.
void shader_desc(Cpu& c) {
    const std::uint32_t desc = arg(c, 1);
    if (desc != 0) {
        std::memset(c.mem + desc, 0, 160);
        if (const std::vector<ShaderBind>* binds = binds_of(arg(c, 0)))
            st32(c.mem, desc + 16, static_cast<std::uint32_t>(binds->size()));
        if (const std::vector<PreparedBuffer>* buffers = buffers_of(arg(c, 0)))
            st32(c.mem, desc + 12, static_cast<std::uint32_t>(buffers->size()));
    }
    ret_stdcall(c, kSOk, 8);
}

void DirectInput8Create(Cpu& c) {
    publish(c.mem);
    const std::uint32_t out = arg(c, 3);
    if (out != 0) st32(c.mem, out, object(DInput));
    ret_stdcall(c, kSOk, 20);
}

void DirectSoundCreate8(Cpu& c) {
    publish(c.mem);
    const std::uint32_t out = arg(c, 1);
    if (out != 0) st32(c.mem, out, object(DSound));
    ret_stdcall(c, kSOk, 12);
}

void D3D11CreateDevice(Cpu& c) {
    publish(c.mem);
    const std::uint32_t device = arg(c, 7);
    const std::uint32_t level = arg(c, 8);
    const std::uint32_t context = arg(c, 9);
    if (device != 0) st32(c.mem, device, object(Device));
    if (context != 0) st32(c.mem, context, object(Context));
    // 11_0 is in the list passed at 0x6B964E, and 0x6B9738 accepts it.
    if (level != 0) st32(c.mem, level, 0xB000);
    ret_stdcall(c, kSOk, 40);
}

// 0x6DB5CF. A negative HRESULT makes the shader load fail. The bytes are not HLSL;
// CreateVertexShader, CreatePixelShader and CreateComputeShader accept the pointer as-is.
// D3DCompile(source, size, name, defines, include, entry, target, flags1, flags2,
// *code, *errors). The request is identified by a hash of the source, the
// macro list, the entry point and the profile. That is the key the prepared MSL
// is stored under.

struct CompileRequest {
    std::string source, entry, target, name;
    std::vector<std::pair<std::string, std::string>> defines;
    std::uint64_t key = 0;
};

CompileRequest compile_request(Cpu& c) {
    CompileRequest r;
    const std::uint32_t src = arg(c, 0);
    std::uint32_t size = arg(c, 1);
    if (size > (4u << 20)) size = 4u << 20;
    if (src != 0) r.source.assign(reinterpret_cast<const char*>(c.mem + src), size);
    while (!r.source.empty() && r.source.back() == '\0') r.source.pop_back();
    r.name = guest_cstr(c.mem, arg(c, 2), 260);
    for (std::uint32_t m = arg(c, 3); m != 0 && ld32(c.mem, m) != 0 && r.defines.size() < 256; m += 8)
        r.defines.emplace_back(guest_cstr(c.mem, ld32(c.mem, m), 256), guest_cstr(c.mem, ld32(c.mem, m + 4), 1024));
    r.entry = guest_cstr(c.mem, arg(c, 5), 256);
    r.target = guest_cstr(c.mem, arg(c, 6), 64);
    std::uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](const std::string& t) {
        for (char ch : t) h = (h ^ static_cast<unsigned char>(ch)) * 0x100000001B3ull;
        h = (h ^ 0xFFu) * 0x100000001B3ull;
    };
    mix(r.source);
    for (const auto& d : r.defines) {
        mix(d.first);
        mix(d.second);
    }
    mix(r.entry);
    mix(r.target);
    r.key = h;
    return r;
}

std::string json_escape(const std::string& t) {
    std::string out;
    for (char sc : t) {
        const auto ch = static_cast<unsigned char>(sc);
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", ch);
            out += buf;
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

// DCUSA_SHADERS=<dir> is the prepared shader folder (tools/shaderprep):
// src/<key>.hlsl and src/<key>.json are written the first time a request is
// seen, and msl/<key>.metal / msl/<key>.refl are what translate_shaders.py
// made of them. The files hold the player's shader source and stay out of the
// repository.
const char* shader_dir() { return std::getenv("DCUSA_SHADERS"); }

std::string key_text(std::uint64_t key) {
    char text[24];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(key));
    return text;
}

void dump_compile(const CompileRequest& r) {
    const char* dir = shader_dir();
    if (dir == nullptr) return;
    const std::string key = key_text(r.key);
    std::error_code ec;
    std::filesystem::create_directories(std::string(dir) + "/src", ec);
    const std::string base = std::string(dir) + "/src/" + key;
    if (std::ifstream(base + ".json").good()) return;
    std::ofstream(base + ".hlsl", std::ios::binary) << r.source;
    std::ofstream meta(base + ".json");
    meta << "{\"key\": \"" << key << "\", \"entry\": \"" << json_escape(r.entry) << "\", \"target\": \""
         << json_escape(r.target) << "\", \"name\": \"" << json_escape(r.name) << "\", \"defines\": [";
    for (std::size_t i = 0; i < r.defines.size(); ++i)
        meta << (i ? ", " : "") << "[\"" << json_escape(r.defines[i].first) << "\", \"" << json_escape(r.defines[i].second)
             << "\"]";
    meta << "]}\n";
}

// The bindings translate_shaders.py recorded for this request: the slots the
// MSL reads. Empty when the request has not been translated.
bool prepared_binds(std::uint64_t key, std::vector<ShaderBind>* out, std::vector<PreparedBuffer>* buffers) {
    const char* dir = shader_dir();
    if (dir == nullptr) return false;
    std::ifstream in(std::string(dir) + "/msl/" + key_text(key) + ".refl");
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kind;
        if (!(ls >> kind)) continue;
        ShaderBind b;
        std::string name;
        std::uint32_t a = 0, d = 0;
        if (kind == "stage" || kind == "input") continue;  // the Metal device reads the vertex inputs
        if (kind == "member") {
            PreparedVar v;
            ls >> v.offset >> v.size >> v.type >> v.elements >> v.major >> v.name;
            if (buffers->empty()) return false;
            buffers->back().vars.push_back(v);
            continue;
        }
        ls >> a >> d >> name;
        if (kind == "texture") {
            b.type = 2;
            b.dimension = d;
        } else if (kind == "sampler") {
            b.type = 3;
        } else if (kind == "cbuffer") {
            b.type = 0;
            PreparedBuffer pb;
            pb.name = name;
            pb.slot = a;
            pb.size = d;
            buffers->push_back(pb);
        } else {
            return false;
        }
        b.slot = a;
        std::snprintf(b.name, sizeof b.name, "%s", name.c_str());
        out->push_back(b);
    }
    return true;
}


void D3DCompile(Cpu& c) {
    publish(c.mem);
    const CompileRequest request = compile_request(c);
    dump_compile(request);
    const std::uint32_t out = arg(c, 9);
    const std::uint32_t obj = spawn(c.mem, Blob);
    st32(c.mem, obj + 12, obj + 32);
    st32(c.mem, obj + 16, 16);
    st32(c.mem, obj + 32, kDxbc);
    st32(c.mem, obj + 36, kBlobMarker);
    st32(c.mem, obj + 40, static_cast<std::uint32_t>(request.key));
    st32(c.mem, obj + 44, static_cast<std::uint32_t>(request.key >> 32));
    std::vector<ShaderBind> binds;
    std::vector<PreparedBuffer> buffers;
    bool prepared = prepared_binds(request.key, &binds, &buffers);
    // DCUSA_TRANSLATE=1: a request with no translation yet is translated now,
    // so a run that reaches a new scene does not need a restart.
#if !TARGET_OS_IPHONE  // iOS has no Python and no processes: it ships the shaders translated
    if (!prepared && shader_dir() != nullptr && std::getenv("DCUSA_TRANSLATE") != nullptr) {
        const std::string cmd = std::string("python3 '") + DCUSA_SOURCE_DIR "/tools/shaderprep/translate_shaders.py' '" +
                                shader_dir() + "' --quiet --only " + key_text(request.key);
        if (std::system(cmd.c_str()) == 0) prepared = prepared_binds(request.key, &binds, &buffers);
    }
#endif
    if (prepared) {
        shader_buffers[obj + 32] = buffers;
        ++g_prepared;
    } else {
        binds = parse_shader(c.mem, arg(c, 0), arg(c, 1));
        ++g_unprepared;
        if (shader_dir() != nullptr && stats_on())
            std::fprintf(stderr, "shader %s (%s %s) not translated yet\n", key_text(request.key).c_str(),
                         request.name.c_str(), request.target.c_str());
    }
    shader_key[obj + 32] = request.key;
    if (!binds.empty()) {
        shader_binds[obj + 32] = binds;
        static int shown = 0;
        if (stats_on() && shown < 6) {
            ++shown;
            std::fprintf(stderr, "shader %d binds %zu", shown, binds.size());
            for (std::size_t i = 0; i < binds.size() && i < 6; ++i)
                std::fprintf(stderr, " %s:%u", binds[i].name, binds[i].slot);
            std::fprintf(stderr, "\n");
        }
    }
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 44);
}

// 0x6DC802. The caller dereferences the reflector without testing the HRESULT.
void D3DReflect(Cpu& c) {
    publish(c.mem);
    const std::uint32_t iid = arg(c, 2);
    const std::uint32_t out = arg(c, 3);
    if (!guid_is(c.mem, iid, 0x8D536CA1u, 0x0CCA, 0x4956, 0xA837786963755584ull)) refuse_iid(c.mem, iid);
    const std::uint32_t obj = spawn(c.mem, Reflector);
    reflector_source[obj] = arg(c, 0);
    // Bytecode from the game's shader cache never went through D3DCompile here:
    // its embedded key finds the translation's bindings.
    std::uint64_t key = 0;
    if (blob_key(c.mem, arg(c, 0), &key)) {
        std::vector<ShaderBind> binds;
        std::vector<PreparedBuffer> buffers;
        if (prepared_binds(key, &binds, &buffers)) {
            shader_binds[arg(c, 0)] = binds;
            shader_buffers[arg(c, 0)] = buffers;
        }
    }
    if (out != 0) st32(c.mem, out, obj);
    ret_stdcall(c, kSOk, 16);
}

void call_com(Cpu& c, std::uint32_t target) {
    const std::uint32_t rel = target - kComCall;
    const auto kind = static_cast<Kind>(rel >> 12);
    const std::uint32_t slot = rel & 0xFFFu;
    if (kind == Device) return device_call(c, slot);
    if (kind == Context) {  // the per-draw calls: a jump table, not a chain of compares
        switch (slot) {
        case 0x6C: return query_begin(c);
        case 0x74: return query_get_data(c);
        case 0x1B8: return clear_state(c);
        case 0x70: return query_end(c);
        case 0x84: return om_set_targets(c);
        case 0xB0: return rs_set_viewports(c);
        case 0xB4: return rs_set_scissors(c);
        case 0xC8: return clear_rtv(c);
        case 0xD4: return clear_dsv(c);
        case 0x38: return map_resource(c);
        case 0x3C: return unmap_resource(c);
        case 0xD8: return generate_mips(c);
        case 0x1C: return vs_set_constants(c);
        case 0x20: return ps_set_resources(c);
        case 0x24: return ps_set_shader(c);
        case 0x28: return ps_set_samplers(c);
        case 0x2C: return vs_set_shader(c);
        case 0x30: return draw_indexed(c);
        case 0x34: return draw_plain(c);
        case 0x40: return ps_set_constants(c);
        case 0x44: return ia_set_layout(c);
        case 0x48: return ia_set_vertices(c);
        case 0x4C: return ia_set_index(c);
        case 0x60: return ia_set_topology(c);
        case 0x68: return vs_set_samplers(c);
        case 0x8C: return om_set_blend(c);
        case 0x90: return om_set_depth(c);
        case 0xAC: return rs_set_state(c);
        case 0x64: return vs_set_resources(c);
        case 0xC0: return update_subresource(c);
        default: return plain_call(c, kind, slot);  // nothing below handles Context
        }
    }
    if (kind == Buffer && slot == 0x28) return buffer_get_desc(c);
    if (kind == Dxgi && slot == 0x1C) return get_adapter(c);
    if (kind == Adapter && slot == 0x18) return get_parent(c);
    if (kind == Adapter && slot == 0x1C) return enum_outputs(c);
    if (kind == Adapter && slot == 0x20) return get_desc(c);
    if (kind == Output && slot == 0x20) return mode_list(c);
    if (kind == Factory && slot == 0x20) return associate(c);
    if (kind == Factory && slot == 0x28) return create_swap(c);
    if (kind == Swap && slot == 0x24) return get_buffer(c);
    if (kind == Swap && slot == 0x28) return set_fullscreen(c);
    if (kind == Swap && slot == 0x20) return present(c);
    if (kind == Swap && slot == 0x2C) return get_fullscreen(c);
    if (kind == Swap && slot == 0x30) return swap_desc(c);
    if (kind == Swap && slot == 0x3C) return get_output(c);
    if (kind == Output && slot == 0x28) return wait_vblank(c);
    if (kind == Texture && slot == 0x14) return set_private(c);
    if (kind == Texture && slot == 0x28) return texture_desc(c);
    if (kind == Blob && slot == 0x0C) return blob_pointer(c);
    if (kind == Blob && slot == 0x10) return blob_size(c);
    if (kind == Reflector && slot == 0x0C) return shader_desc(c);
    if (kind == Reflector && slot == 0x10) return reflect_buffer_by_index(c);
    if (kind == Reflector && slot == 0x14) return reflect_buffer_by_name(c);
    if (kind == Reflector && slot == 0x18) return reflect_binding(c);
    if (kind == Reflector && slot == 0x28) return reflect_variable_by_name(c);
    if (kind == Reflector && slot == 0x2C) return reflect_binding_by_name(c);
    // The reflection sub-interfaces have no IUnknown: GetDesc is slot 0.
    if (kind == ReflBuffer && slot == 0x00) return buffer_desc(c);
    if (kind == ReflBuffer && slot == 0x04) return buffer_variable_by_index(c);
    if (kind == ReflBuffer && slot == 0x08) return buffer_variable_by_name(c);
    if (kind == ReflVariable && slot == 0x00) return variable_desc(c);
    if (kind == ReflVariable && slot == 0x04) return variable_type(c);
    if (kind == ReflVariable && slot == 0x08) return variable_buffer(c);
    if (kind == ReflType && slot == 0x00) return type_desc(c);
    if (kind == ReflBuffer || kind == ReflVariable || kind == ReflType) return refuse(kind, slot);
    if (kind == Reflector && slot == 0x20) return reflect_output(c);
    if (kind == DInput && slot == 0x0C) return dinput_create_device(c);
    if (kind == DInput && slot == 0x10) return dinput_enum(c);
    if (kind == DInputDev && slot == 0x0C) return dinput_caps(c);
    if (kind == DInputDev && slot == 0x10) return dinput_enum_objects(c);
    if (kind == DInputDev && slot == 0x14) return dinput_get_prop(c);
    if (kind == DInputDev && slot == 0x18) return dinput_set_prop(c);
    if (kind == DInputDev && slot == 0x1C) return dinput_acquire(c);
    if (kind == DInputDev && slot == 0x20) return dinput_acquire(c);
    if (kind == DInputDev && slot == 0x24) return dinput_state(c);
    if (kind == DInputDev && slot == 0x2C) return dinput_format(c);
    if (kind == DInputDev && slot == 0x34) return dinput_coop(c);
    if (kind == DInputDev && slot == 0x64) return dinput_acquire(c);
    if (kind == DSound || kind == DSoundBuf) ds_trace(c, kind == DSound, slot);
    if (kind == DSound && slot == 0x0C) return ds_create_buffer(c);
    if (kind == DSound && slot == 0x10) return ds_caps(c);
    if (kind == DSound && slot == 0x18) return ds_coop(c);
    if (kind == DSound && slot == 0x20) return ds_speaker(c);
    // IDirectSoundBuffer, in dsound.h order after IUnknown.
    if (kind == DSoundBuf && slot == 0x0C) return ds_caps(c);
    if (kind == DSoundBuf && slot == 0x10) return ds_position(c);
    if (kind == DSoundBuf && slot == 0x14) return ds_get_format(c);
    if (kind == DSoundBuf && slot >= 0x18 && slot <= 0x20) return ds_get_level(c, slot);
    if (kind == DSoundBuf && slot == 0x24) return ds_status(c);
    if (kind == DSoundBuf && slot == 0x2C) return ds_lock(c);
    if (kind == DSoundBuf && slot == 0x30) return ds_play(c);
    if (kind == DSoundBuf && slot == 0x34) return ds_set_position(c);
    if (kind == DSoundBuf && slot == 0x38) return ds_set_format(c);
    if (kind == DSoundBuf && slot >= 0x3C && slot <= 0x44) return ds_set_level(c, slot);
    if (kind == DSoundBuf && slot == 0x48) return ds_stop(c);
    if (kind == DSoundBuf && slot == 0x4C) return ds_unlock(c);
    if (kind == DSoundBuf && slot == 0x50) return ds_restore(c);
    if (kind < KindCount) return plain_call(c, kind, slot);
    refuse(kind, slot);
}

bool frame_skip_render() {
    const int mode = frame_skip_mode();
    if (mode == 0 || g_boot_loading) return false;
    bool skip = false;
    if (mode == 2) skip = g_skip_run == 0;
    else skip = g_skip_run < kMaxSkips && g_pace_next.time_since_epoch().count() != 0 &&
                std::chrono::steady_clock::now() > g_pace_next;
    if (skip) {
        ++g_skip_run;
        ++g_skipped_total;
        g_render_skipped = true;
    } else {
        g_skip_run = 0;
    }
    return skip;
}

void boot_progress(std::uint32_t percent) { g_boot_percent = static_cast<int>(std::min<std::uint32_t>(percent, 100)); }

std::uint32_t cube_faces_cap() {
    static const std::uint32_t cap = [] {
        const char* e = std::getenv("DCUSA_CUBE_FACES");
        const int n = e != nullptr ? std::atoi(e) : (TARGET_OS_IPHONE ? 1 : 6);
        return static_cast<std::uint32_t>(n < 0 ? 0 : n);
    }();
    return cap;
}

bool shadow_cascade_skipped(std::uint32_t index) {
    static const std::uint32_t drawn = [] {
        const char* e = std::getenv("DCUSA_SHADOW_CASCADES");
        // All three everywhere: with two on iOS, trees past the second cascade
        // lost their shadow and turned bright (Three Seven Speedway's forest),
        // and the third costs an iPhone little since the native glow.
        const int n = e != nullptr ? std::atoi(e) : 3;
        return static_cast<std::uint32_t>(n < 0 ? 0 : n);
    }();
    return index >= drawn;
}

bool skip_velocity_pass() {
    static const bool skip = [] {
        const char* e = std::getenv("DCUSA_VELOCITY");
        if (e != nullptr) return std::atoi(e) == 0;
        return TARGET_OS_IPHONE != 0;
    }();
    return skip;
}

}

namespace dcusa {
void toggle_fps_overlay() {
    int v = g_fps_overlay.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = std::getenv("DCUSA_SHOW_FPS");
        v = e != nullptr && std::atoi(e) != 0 ? 1 : 0;
    }
    g_fps_overlay.store(v == 1 ? 0 : 1, std::memory_order_relaxed);
}
}  // namespace dcusa

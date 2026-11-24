#include "gpu/metal.hpp"
#include "gpu/metal_direct.hpp"
#include "runtime/hitch.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace dcusa::gpu {
namespace {

// Metal buffer indices: constant buffers keep their b# slot (0-13), vertex
// streams follow.
constexpr NSUInteger kStreamBase = 16;
constexpr NSUInteger kArenaSize = 8u << 20;  // upload arena; larger uploads get a buffer of their own

struct Texture {
    id<MTLTexture> tex = nil;
    TextureDesc desc;
    MTLPixelFormat format = MTLPixelFormatInvalid;
    bool depth = false;
    bool cube = false;  // a cube map (D3D11_RESOURCE_MISC_TEXTURECUBE): six faces per element
    // MSAA (DCUSA_MSAA): the multisampled surface passes render into; `tex` is
    // its resolve, which everything that reads the texture sees.
    id<MTLTexture> ms = nil;
    std::uint32_t samples = 1;
    // A depth texture the shaders read as Texture2D: an R32Float copy, taken
    // again after each pass or clear that may have changed the depth.
    std::uint64_t written = 0, copied = ~0ull;
    id<MTLTexture> as_color = nil;
    // Views over a mip range of `tex` (key: first level << 8 | count), made when
    // a shader resource view does not start at level 0 or stops early.
    id<MTLTexture> views_of = nil;
    std::unordered_map<std::uint32_t, id<MTLTexture>> views;
    // Clears not encoded yet (deferred_clears): one bit per colour mip, and
    // the depth / stencil clear flags (1 depth, 2 stencil).
    std::uint32_t clear_mips = 0;
    float clear_rgba[16][4] = {};
    std::uint32_t clear_ds = 0;
    float clear_depth = 0;
    std::uint8_t clear_stencil = 0;
    id<MTLBuffer> depth_staging = nil;  // depth_as_color's, reused
};

struct Program {
    id<MTLFunction> fn = nil;
    struct Input {
        std::uint32_t location;
        std::string semantic;
        std::uint32_t index;
    };
    std::vector<Input> inputs;
    bool failed = false;
};

// What a pipeline's functions read, from Metal's reflection, so a slot the game
// left empty gets a zero stand-in (D3D reads zeros there; Metal requires a binding).
struct StageUse {
    std::uint32_t buffers = 0;   // bit per constant buffer index
    std::uint32_t textures = 0;  // bit per texture index
    std::uint32_t depth = 0;     // of those, the depth2d ones
    std::uint32_t cube = 0;      // and the texturecube ones
    std::uint32_t ms = 0;        // and the texture2d_ms ones
    std::uint32_t samplers = 0;
    // Bytes the function reads from each constant buffer.
    std::uint32_t buffer_size[kBuffers] = {};
};
struct Pipeline {
    id<MTLRenderPipelineState> state = nil;
    StageUse vs, ps;
};

// A buffer's current bytes: a range of an upload arena, or a buffer of its own
// for large uploads.
struct BufferRef {
    id<MTLBuffer> buf = nil;
    NSUInteger off = 0, len = 0;
    const void* contents() const { return static_cast<const std::uint8_t*>(buf.contents) + off; }
};

struct Device {
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLCommandBuffer> cmd = nil;
    id<MTLRenderCommandEncoder> enc = nil;
    std::uint32_t enc_rtv[kTargets]{};
    // What is bound on the current encoder, so a draw only sets what changed.
    // Cleared whenever a new encoder starts.
    struct Bound {
        void* pipeline = nullptr;
        void* depth = nullptr;
        double viewport[6] = {-1, -1, -1, -1, -1, -1};
        NSUInteger scissor[4] = {~0ul, ~0ul, ~0ul, ~0ul};
        int cull = -1, winding = -1;
        float blend[4] = {-1, -1, -1, -1};
        void* buf[2][kStreamBase + kStreams] = {};
        NSUInteger off[2][kStreamBase + kStreams] = {};
        void* tex[2][kSlots] = {};
        void* samp[2][kSlots] = {};
    } bound;
    std::uint8_t enc_mip[kTargets]{};
    std::uint8_t enc_slice[kTargets]{};
    std::uint32_t enc_dsv = 0;
    std::string shader_dir;
    std::unordered_map<std::uint32_t, Texture> textures;
    std::unordered_map<std::uint32_t, BufferRef> buffers;
    // Upload arenas: buffer updates are copied into the current arena, so a
    // draw already encoded keeps the bytes it was given. A full arena waits in
    // `retired` until the command buffer that read it completes, then is reused.
    id<MTLBuffer> arena = nil;
    NSUInteger arena_used = 0;
    std::vector<id<MTLBuffer>> retired, spare_arenas;
    // Per arena, how many buffers' current bytes are in it. An arena the GPU is
    // done with is reused only at zero; until then it waits in `parked`.
    std::unordered_map<void*, unsigned> arena_live;
    std::vector<id<MTLBuffer>> parked;
    // Frames committed without waiting, each with the arenas it retired.
    std::deque<std::pair<id<MTLCommandBuffer>, std::vector<id<MTLBuffer>>>> in_flight;
    std::unordered_map<std::uint32_t, id<MTLSamplerState>> samplers;
    std::unordered_map<std::uint64_t, Program> programs;
    std::unordered_map<std::string, Pipeline> pipelines;
    id<MTLBuffer> zero_buffer = nil;
    id<MTLTexture> zero_texture = nil, zero_depth = nil, zero_cube = nil, zero_ms = nil;
    id<MTLSamplerState> default_sampler = nil;
    std::unordered_map<std::uint32_t, id<MTLDepthStencilState>> depth_states;
    std::unordered_map<std::string, bool> reported;
    Stats stats;
};

Device* g_dev = nullptr;
bool g_checked = false;
bool g_trace = false;  // set_trace(): one line per draw

void trace_draw(const DrawState& s, const char* kind, std::uint32_t count, const char* result) {
    if (!g_trace) return;
    std::fprintf(stderr, "draw %s n %u vs %016llx ps %016llx rt %06x/%06x ds %06x vp %.0f,%.0f %.0fx%.0f z %.2f-%.2f topo %u "
                 "blend %d mask %x cull %u ccw %d scissor %d depth %d/%d/%u vb %06x stride %u off %u ib %06x fmt %u srv",
                 kind, count, static_cast<unsigned long long>(s.vs), static_cast<unsigned long long>(s.ps), s.rtv[0],
                 s.rtv[1], s.dsv, s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], s.viewport[4], s.viewport[5],
                 s.topology, s.blend[0].enable, s.blend[0].write_mask, s.raster.cull, s.raster.front_ccw, s.raster.scissor,
                 s.depth.enable, s.depth.write, s.depth.func, s.vb[0], s.stride[0], s.voff[0], s.ib, s.ib_format);
    for (unsigned i = 0; i < 6; ++i) std::fprintf(stderr, " %06x", s.ps_srv[i]);
    std::fprintf(stderr, " cb %06x %06x: %s", s.ps_cb[0], s.vs_cb[0], result);
    auto b = g_dev->buffers.find(s.ps_cb[0]);
    if (s.ps_cb[0] && b != g_dev->buffers.end()) {
        const float* f = static_cast<const float*>(b->second.contents());
        std::fprintf(stderr, " ps_cb[%lu]", static_cast<unsigned long>(b->second.len));
        for (NSUInteger i = 0; i < std::min<NSUInteger>(24, b->second.len / 4); ++i) std::fprintf(stderr, " %.3g", f[i]);
    } else if (s.ps_cb[0]) {
        std::fprintf(stderr, " ps_cb never uploaded");
    }
    std::fprintf(stderr, "\n");
    if (std::getenv("DCUSA_METAL_TRACE_CB") != nullptr) {
        for (unsigned slot = 1; slot < kBuffers; ++slot) {
            auto cb = g_dev->buffers.find(s.ps_cb[slot]);
            if (!s.ps_cb[slot] || cb == g_dev->buffers.end()) continue;
            const float* f = static_cast<const float*>(cb->second.contents());
            std::fprintf(stderr, "    ps_cb%u[%lu]", slot, static_cast<unsigned long>(cb->second.len));
            for (NSUInteger i = 0; i < std::min<NSUInteger>(128, cb->second.len / 4); ++i) std::fprintf(stderr, " %.3g", f[i]);
            std::fprintf(stderr, "\n");
        }
    }
}

bool verbose() {
    static const bool on = std::getenv("DCUSA_STATS") != nullptr;
    return on;
}

void problem(const std::string& what) {
    Device& d = *g_dev;
    d.stats.last_problem = what;
    if (d.reported.emplace(what, true).second && verbose()) std::fprintf(stderr, "metal: %s\n", what.c_str());
}

MTLPixelFormat pixel_format(std::uint32_t dxgi, bool depth, bool* is_depth) {
    *is_depth = false;
    if (depth) {
        *is_depth = true;
        switch (dxgi) {
        case 19: case 20: case 44: case 45: return MTLPixelFormatDepth32Float_Stencil8;
        default: return MTLPixelFormatDepth32Float;  // 39, 40, 53, 55
        }
    }
    switch (dxgi) {
    case 1: case 2: return MTLPixelFormatRGBA32Float;
    case 9: case 10: return MTLPixelFormatRGBA16Float;
    case 11: return MTLPixelFormatRGBA16Unorm;
    case 12: return MTLPixelFormatRGBA16Uint;
    case 13: return MTLPixelFormatRGBA16Snorm;
    case 15: case 16: return MTLPixelFormatRG32Float;
    case 23: case 24: return MTLPixelFormatRGB10A2Unorm;
    case 26: return MTLPixelFormatRG11B10Float;
    case 27: case 28: return MTLPixelFormatRGBA8Unorm;
    case 29: return MTLPixelFormatRGBA8Unorm_sRGB;
    case 30: return MTLPixelFormatRGBA8Uint;
    case 31: return MTLPixelFormatRGBA8Snorm;
    case 33: case 34: return MTLPixelFormatRG16Float;
    case 35: return MTLPixelFormatRG16Unorm;
    case 39: case 41: return MTLPixelFormatR32Float;
    case 42: return MTLPixelFormatR32Uint;
    case 48: case 49: return MTLPixelFormatRG8Unorm;
    case 53: case 54: return MTLPixelFormatR16Float;
    case 56: return MTLPixelFormatR16Unorm;
    case 60: case 61: return MTLPixelFormatR8Unorm;
    case 65: return MTLPixelFormatA8Unorm;
    case 70: case 71: return MTLPixelFormatBC1_RGBA;
    case 72: return MTLPixelFormatBC1_RGBA_sRGB;
    case 73: case 74: return MTLPixelFormatBC2_RGBA;
    case 75: return MTLPixelFormatBC2_RGBA_sRGB;
    case 76: case 77: return MTLPixelFormatBC3_RGBA;
    case 78: return MTLPixelFormatBC3_RGBA_sRGB;
    case 79: case 80: return MTLPixelFormatBC4_RUnorm;
    case 82: case 83: return MTLPixelFormatBC5_RGUnorm;
    case 87: case 88: case 90: return MTLPixelFormatBGRA8Unorm;
    case 91: return MTLPixelFormatBGRA8Unorm_sRGB;
    case 94: case 95: return MTLPixelFormatBC6H_RGBUfloat;
    case 96: return MTLPixelFormatBC6H_RGBFloat;
    case 97: case 98: return MTLPixelFormatBC7_RGBAUnorm;
    case 99: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    default: return MTLPixelFormatInvalid;
    }
}

bool has_stencil(MTLPixelFormat f) { return f == MTLPixelFormatDepth32Float_Stencil8; }

MTLVertexFormat vertex_format(std::uint32_t dxgi) {
    switch (dxgi) {
    case 2: return MTLVertexFormatFloat4;
    case 3: return MTLVertexFormatUInt4;
    case 4: return MTLVertexFormatInt4;
    case 6: return MTLVertexFormatFloat3;
    case 7: return MTLVertexFormatUInt3;
    case 10: return MTLVertexFormatHalf4;
    case 11: return MTLVertexFormatUShort4Normalized;
    case 12: return MTLVertexFormatUShort4;
    case 13: return MTLVertexFormatShort4Normalized;
    case 14: return MTLVertexFormatShort4;
    case 16: return MTLVertexFormatFloat2;
    case 17: return MTLVertexFormatUInt2;
    case 28: return MTLVertexFormatUChar4Normalized;
    case 30: return MTLVertexFormatUChar4;
    case 31: return MTLVertexFormatChar4Normalized;
    case 32: return MTLVertexFormatChar4;
    case 34: return MTLVertexFormatHalf2;
    case 35: return MTLVertexFormatUShort2Normalized;
    case 36: return MTLVertexFormatUShort2;
    case 37: return MTLVertexFormatShort2Normalized;
    case 41: return MTLVertexFormatFloat;
    case 42: return MTLVertexFormatUInt;
    case 43: return MTLVertexFormatInt;
    case 87: return MTLVertexFormatUChar4Normalized_BGRA;
    default: return MTLVertexFormatInvalid;
    }
}

MTLBlendFactor blend_factor(std::uint32_t b) {
    switch (b) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    case 16: return MTLBlendFactorSource1Color;
    case 17: return MTLBlendFactorOneMinusSource1Color;
    case 18: return MTLBlendFactorSource1Alpha;
    case 19: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation blend_op(std::uint32_t op) {
    switch (op) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLCompareFunction compare(std::uint32_t f) {
    switch (f) {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLSamplerAddressMode address(std::uint32_t a) {
    switch (a) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

std::string key_text(std::uint64_t key) {
    char text[24];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(key));
    return text;
}

Program& program(std::uint64_t key, bool vertex) {
    Device& d = *g_dev;
    Program& p = d.programs[key];
    if (p.fn != nil || p.failed) return p;
    const std::string base = d.shader_dir + "/msl/" + key_text(key);
    NSString* path = [NSString stringWithUTF8String:(base + ".metal").c_str()];
    NSString* source = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (source == nil) {
        p.failed = true;
        problem("no translated shader " + key_text(key));
        return p;
    }
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_1;
    // Relaxed: fast arithmetic that still honours NaN and infinity, which the
    // D3D11 min / max rules in the translated shaders rely on (translate_shaders.py).
    options.mathMode = MTLMathModeRelaxed;
    NSError* error = nil;
    hitch::Timer compile_timer(hitch::Pipeline);
    id<MTLLibrary> lib = [d.dev newLibraryWithSource:source options:options error:&error];
    p.fn = lib ? [lib newFunctionWithName:@"main0"] : nil;
    if (p.fn == nil) {
        p.failed = true;
        problem("shader " + key_text(key) + " does not compile: " +
                (error ? std::string(error.localizedDescription.UTF8String).substr(0, 200) : "no main0"));
        return p;
    }
    if (vertex) {
        std::ifstream in(base + ".refl");
        std::string kind;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            ls >> kind;
            if (kind != "input") continue;
            Program::Input i;
            ls >> i.location >> i.semantic >> i.index;
            p.inputs.push_back(i);
        }
    }
    return p;
}

std::string upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

// Ends the render pass, if one is open.
void end_pass() {
    Device& d = *g_dev;
    if (d.enc != nil) {
        [d.enc endEncoding];
        d.enc = nil;
    }
}

// A committed frame the GPU has not finished.
bool gpu_busy() {
    for (auto& entry : g_dev->in_flight)
        if (entry.first.status < MTLCommandBufferStatusCompleted) return true;
    return false;
}

void check_completed(id<MTLCommandBuffer> cmd) {
    if (cmd.status == MTLCommandBufferStatusError)
        problem(std::string("command buffer failed: ") + (cmd.error ? cmd.error.localizedDescription.UTF8String : "?"));
}

// `size` bytes in the current upload arena (size <= kArenaSize).
BufferRef arena_alloc(NSUInteger size) {
    Device& d = *g_dev;
    // 256 bytes: the strictest offset alignment Metal asks of a constant buffer.
    NSUInteger off = (d.arena_used + 255) & ~NSUInteger(255);
    if (d.arena == nil || off + size > kArenaSize) {
        if (d.arena != nil) d.retired.push_back(d.arena);
        if (!d.spare_arenas.empty()) {
            d.arena = d.spare_arenas.back();
            d.spare_arenas.pop_back();
        } else {
            d.arena = [d.dev newBufferWithLength:kArenaSize options:MTLResourceStorageModeShared];
            d.arena_live[(__bridge void*)d.arena] = 0;
        }
        off = 0;
    }
    d.arena_used = off + size;
    return {d.arena, off, size};
}

// A constant buffer as a function reads it. D3D11 reads zeros past the end of a
// bound constant buffer; a shorter buffer gets a zero-padded copy, since past
// its end in an arena is the next upload's data.
BufferRef constant_view(const BufferRef& b, std::uint32_t wanted) {
    if (wanted <= b.len || wanted > kArenaSize) return b;
    BufferRef r = arena_alloc(wanted);
    auto* dst = static_cast<std::uint8_t*>(const_cast<void*>(r.contents()));
    std::memcpy(dst, b.contents(), b.len);
    std::memset(dst + b.len, 0, wanted - b.len);
    return r;
}

// The GPU is done with these arenas: reuse the ones no buffer still lives in.
void release_arenas(const std::vector<id<MTLBuffer>>& arenas) {
    Device& d = *g_dev;
    for (id<MTLBuffer> a : arenas) {
        if (d.arena_live[(__bridge void*)a] == 0) d.spare_arenas.push_back(a);
        else d.parked.push_back(a);
    }
}

// Parked arenas whose last buffer has moved on.
void unpark_arenas() {
    Device& d = *g_dev;
    auto keep = d.parked.begin();
    for (id<MTLBuffer> a : d.parked) {
        if (d.arena_live[(__bridge void*)a] == 0) d.spare_arenas.push_back(a);
        else *keep++ = a;
    }
    d.parked.erase(keep, d.parked.end());
}

// A buffer's bytes are now `r` (or gone, when r.buf is nil).
void set_buffer(std::uint32_t key, const BufferRef& r) {
    Device& d = *g_dev;
    auto it = d.buffers.find(key);
    if (it != d.buffers.end()) {
        if (auto live = d.arena_live.find((__bridge void*)it->second.buf); live != d.arena_live.end()) --live->second;
        if (r.buf == nil) d.buffers.erase(it);
    }
    if (r.buf == nil) return;
    if (auto live = d.arena_live.find((__bridge void*)r.buf); live != d.arena_live.end()) ++live->second;
    d.buffers[key] = r;
}

// Every command buffer committed so far has completed (one queue completes in
// order): every retired arena the GPU read is free of draws.
void recycle_arenas() {
    Device& d = *g_dev;
    for (auto& [cmd, arenas] : d.in_flight) {
        check_completed(cmd);
        release_arenas(arenas);
    }
    d.in_flight.clear();
    release_arenas(d.retired);
    d.retired.clear();
    unpark_arenas();
}

id<MTLCommandBuffer> command_buffer() {
    Device& d = *g_dev;
    if (d.cmd == nil) d.cmd = [d.queue commandBuffer];
    return d.cmd;
}

// DCUSA_GPU_PASSES=<n>[,<n>...]: GPU time per pass for those frames (counting
// command buffers that drew anything; DCUSA_GPU_PASSES_LOG=1 prints each one's
// draw count). DCUSA_GPU_PASSES=d<draws>: frames with at least that many draws
// that follow one that had them, at most one in 300. Every render and blit pass of the frame gets
// a timestamp at its start and end, printed as a table once the GPU has run it.
// Passes overlap on the GPU (one's vertex work runs during the last one's
// fragment work), so the column does not sum to the frame.
struct PassProfile {
    std::vector<unsigned> frames;  // sorted
    unsigned frame = 0, frame_draws = 0, min_draws = 0, last_print = 0;
    bool on = false, active = false;
    id<MTLCounterSampleBuffer> samples = nil;
    struct Row { std::string label; unsigned draws = 0; std::unordered_map<std::uint64_t, unsigned> ps; };
    std::vector<Row> rows;
    int open = -1;  // the row of the open render pass
} g_pp;
constexpr unsigned kProfilePasses = 2048;

void profile_init() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* v = std::getenv("DCUSA_GPU_PASSES");
    if (v == nullptr) return;
    id<MTLDevice> dev = g_dev->dev;
    if (![dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
        std::fprintf(stderr, "metal: no stage-boundary counters; DCUSA_GPU_PASSES ignored\n");
        return;
    }
    for (id<MTLCounterSet> set in dev.counterSets) {
        if (![set.name isEqualToString:MTLCommonCounterSetTimestamp]) continue;
        MTLCounterSampleBufferDescriptor* sd = [MTLCounterSampleBufferDescriptor new];
        sd.counterSet = set;
        sd.storageMode = MTLStorageModeShared;
        sd.sampleCount = 2 * kProfilePasses;
        NSError* err = nil;
        g_pp.samples = [dev newCounterSampleBufferWithDescriptor:sd error:&err];
    }
    if (g_pp.samples == nil) {
        std::fprintf(stderr, "metal: no timestamp counter set; DCUSA_GPU_PASSES ignored\n");
        return;
    }
    if (*v == 'd') g_pp.min_draws = static_cast<unsigned>(std::atoi(v + 1));
    for (const char* p = v; *p && *v != 'd';) {
        g_pp.frames.push_back(static_cast<unsigned>(std::strtoul(p, const_cast<char**>(&p), 10)));
        while (*p == ',' || *p == ' ') ++p;
    }
    std::sort(g_pp.frames.begin(), g_pp.frames.end());
    g_pp.on = true;
    g_pp.active = !g_pp.frames.empty() && g_pp.frames.front() == 1;
}

// Puts timestamps on a render pass of the profiled frame.
void profile_pass(MTLRenderPassDescriptor* pass, std::string label) {
    g_pp.open = -1;
    if (!g_pp.active || g_pp.rows.size() >= kProfilePasses) return;
    const NSUInteger i = g_pp.rows.size();
    pass.sampleBufferAttachments[0].sampleBuffer = g_pp.samples;
    pass.sampleBufferAttachments[0].startOfVertexSampleIndex = 2 * i;
    pass.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
    pass.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
    pass.sampleBufferAttachments[0].endOfFragmentSampleIndex = 2 * i + 1;
    g_pp.rows.push_back({std::move(label)});
    g_pp.open = static_cast<int>(i);
}

void profile_draw(std::uint64_t ps) {
    ++g_pp.frame_draws;
    if (g_pp.open < 0) return;
    PassProfile::Row& r = g_pp.rows[g_pp.open];
    ++r.draws;
    ++r.ps[ps];
}

id<MTLBlitCommandEncoder> blit_encoder(const char* label) {
    g_pp.open = -1;
    if (!g_pp.active || g_pp.rows.size() >= kProfilePasses) return [command_buffer() blitCommandEncoder];
    const NSUInteger i = g_pp.rows.size();
    MTLBlitPassDescriptor* pass = [MTLBlitPassDescriptor blitPassDescriptor];
    pass.sampleBufferAttachments[0].sampleBuffer = g_pp.samples;
    pass.sampleBufferAttachments[0].startOfEncoderSampleIndex = 2 * i;
    pass.sampleBufferAttachments[0].endOfEncoderSampleIndex = 2 * i + 1;
    g_pp.rows.push_back({label});
    return [command_buffer() blitCommandEncoderWithDescriptor:pass];
}

std::string target_label(const char* kind, Texture* t, std::uint32_t mip) {
    char b[96];
    if (t == nullptr) return kind;
    std::snprintf(b, sizeof b, "%s %ux%u fmt %u", kind, std::max(1u, t->desc.width >> mip), std::max(1u, t->desc.height >> mip),
                  t->desc.format);
    return b;
}

// After the profiled frame's commit: waits for it and prints the table. Then
// decides whether the next frame is profiled.
void profile_submitted(id<MTLCommandBuffer> cmd) {
    if (!g_pp.on || g_pp.frame_draws == 0) return;
    ++g_pp.frame;
    if (std::getenv("DCUSA_GPU_PASSES_LOG")) std::fprintf(stderr, "gpu frame %u draws %u\n", g_pp.frame, g_pp.frame_draws);
    if (g_pp.active && g_pp.min_draws && g_pp.frame_draws < g_pp.min_draws) g_pp.active = false;
    if (g_pp.active) {
        g_pp.last_print = g_pp.frame;
        [cmd waitUntilCompleted];
        const NSUInteger n = g_pp.rows.size();
        NSData* data = [g_pp.samples resolveCounterRange:NSMakeRange(0, 2 * n)];
        const auto* ts = data ? static_cast<const MTLCounterResultTimestamp*>(data.bytes) : nullptr;
        std::uint64_t lo = ~0ull, hi = 0;
        for (NSUInteger k = 0; ts && k < 2 * n; ++k) {
            const std::uint64_t t = ts[k].timestamp;
            if (t == MTLCounterErrorValue || t == 0) continue;
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
        // Timestamps are GPU ticks; the command buffer's span gives the scale.
        const double span = cmd.GPUEndTime - cmd.GPUStartTime;
        const double us = hi > lo ? span * 1e6 / static_cast<double>(hi - lo) : 0;
        std::fprintf(stderr, "gpu passes: frame %u, %lu passes, %u draws, command buffer %.2f ms\n", g_pp.frame,
                     static_cast<unsigned long>(n), g_pp.frame_draws, span * 1e3);
        for (NSUInteger k = 0; ts && k < n; ++k) {
            const std::uint64_t a = ts[2 * k].timestamp, b = ts[2 * k + 1].timestamp;
            const bool ok = a != MTLCounterErrorValue && b != MTLCounterErrorValue && b >= a && a != 0;
            const PassProfile::Row& r = g_pp.rows[k];
            std::uint64_t top = 0;
            unsigned top_n = 0;
            for (const auto& [key, c] : r.ps)
                if (c > top_n) top = key, top_n = c;
            std::fprintf(stderr, "  %4lu %8.1f us  start %8.1f  %-34s draws %4u ps %zu", static_cast<unsigned long>(k),
                         ok ? (b - a) * us : -1.0, a > lo && a != MTLCounterErrorValue ? (a - lo) * us : 0.0,
                         r.label.c_str(), r.draws, r.ps.size());
            if (top_n) std::fprintf(stderr, " top %016llx x%u", static_cast<unsigned long long>(top), top_n);
            std::fprintf(stderr, "\n");
        }
        g_pp.rows.clear();
    }
    g_pp.rows.clear();
    g_pp.active = g_pp.min_draws ? g_pp.frame_draws >= g_pp.min_draws && (g_pp.last_print == 0 || g_pp.frame - g_pp.last_print >= 300)
                                 : std::binary_search(g_pp.frames.begin(), g_pp.frames.end(), g_pp.frame + 1);
    g_pp.frame_draws = 0;
    g_pp.open = -1;
}

Texture* texture(std::uint32_t key) {
    auto it = g_dev->textures.find(key);
    return it == g_dev->textures.end() || it->second.tex == nil ? nullptr : &it->second;
}

// Deferred clears. A D3D clear encoded as its own pass makes a tile-based GPU
// write the whole surface out, and the next pass read it back in. Instead the
// clear waits on the texture and becomes the load action of the next pass that
// renders to it (begin_pass). Anything else that reads the texture first
// encodes it as a pass of its own here.
void flush_clears(Texture& t) {
    if (t.clear_mips == 0 && t.clear_ds == 0) return;
    end_pass();
    for (std::uint32_t mip = 0; mip < 16; ++mip) {
        if (!(t.clear_mips & (1u << mip))) continue;
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = t.tex;
        pass.colorAttachments[0].level = mip;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (t.ms != nil && mip == 0) {  // the multisampled surface too, resolved into tex
            pass.colorAttachments[0].texture = t.ms;
            pass.colorAttachments[0].level = 0;
            pass.colorAttachments[0].resolveTexture = t.tex;
            pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        }
        const float* c = t.clear_rgba[mip];
        pass.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        profile_pass(pass, target_label("clear", &t, mip));
        [[command_buffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
    }
    if (t.clear_ds) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.depthAttachment.texture = t.tex;
        pass.depthAttachment.loadAction = (t.clear_ds & 1u) ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = t.clear_depth;
        if (t.ms != nil) {
            pass.depthAttachment.texture = t.ms;
            pass.depthAttachment.resolveTexture = t.tex;
            pass.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
            pass.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        }
        if (has_stencil(t.format)) {
            pass.stencilAttachment.texture = t.tex;
            pass.stencilAttachment.loadAction = (t.clear_ds & 2u) ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
            pass.stencilAttachment.clearStencil = t.clear_stencil;
        }
        profile_pass(pass, target_label("clear depth", &t, 0));
        [[command_buffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
    }
    t.clear_mips = 0;
    t.clear_ds = 0;
}

// The texture behind `key`, with any clear it is owed encoded.
Texture* used(std::uint32_t key) {
    Texture* t = texture(key);
    if (t != nullptr) flush_clears(*t);
    return t;
}

// The open pass renders to `key`.
bool attached(std::uint32_t key) {
    const Device& d = *g_dev;
    if (d.enc == nil) return false;
    if (d.enc_dsv == key) return true;
    for (std::uint32_t r : d.enc_rtv)
        if (r == key) return true;
    return false;
}

bool is_cube(const TextureDesc& desc) {
    return (desc.misc & 0x4u) != 0 && desc.array >= 6 && desc.array % 6 == 0 && desc.width == desc.height;
}

id<MTLTexture> new_texture(const TextureDesc& desc, MTLPixelFormat format, bool depth, std::uint32_t mips) {
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:desc.width
                                                                                height:desc.height
                                                                             mipmapped:NO];
    if (is_cube(desc)) {
        td.textureType = desc.array == 6 ? MTLTextureTypeCube : MTLTextureTypeCubeArray;
        td.arrayLength = desc.array / 6;
    }
    td.mipmapLevelCount = mips;
    MTLTextureUsage usage = MTLTextureUsageShaderRead;
    if ((desc.bind & 0x60u) != 0 || depth) usage |= MTLTextureUsageRenderTarget;
    td.usage = usage;
    td.storageMode = depth || (desc.bind & 0x20u) != 0 ? MTLStorageModePrivate : MTLStorageModeShared;
    return [g_dev->dev newTextureWithDescriptor:td];
}

// A render pass on exactly these targets, opened with `load` (or clears).
// The levels a shader resource view covers: the texture itself, or a view of
// levels first .. first + count - 1 (count 0: to the last level). The bloom
// chain reads one level of a texture while rendering the next; reading the whole
// texture there would read the level being written.
id<MTLTexture> mip_view(Texture& t, std::uint32_t first, std::uint32_t count) {
    const NSUInteger levels = t.tex.mipmapLevelCount;
    if (first >= levels) first = static_cast<std::uint32_t>(levels - 1);
    const NSUInteger n = count == 0 ? levels - first : std::min<NSUInteger>(count, levels - first);
    if (first == 0 && n == levels) return t.tex;
    if (t.views_of != t.tex) {
        t.views.clear();
        t.views_of = t.tex;
    }
    const std::uint32_t key = first << 8 | static_cast<std::uint32_t>(n);
    if (auto it = t.views.find(key); it != t.views.end()) return it->second;
    const NSUInteger slices = t.tex.arrayLength * (t.cube ? 6 : 1);
    id<MTLTexture> v = [t.tex newTextureViewWithPixelFormat:t.tex.pixelFormat textureType:t.tex.textureType
                                                     levels:NSMakeRange(first, n) slices:NSMakeRange(0, slices)];
    t.views[key] = v;
    return v;
}

// The sample count a pass over these targets renders with: the targets'
// multisampled surfaces when every one has them with the same count, else 1.
std::uint32_t pass_samples(const std::uint32_t rtv[kTargets], std::uint32_t dsv) {
    std::uint32_t n = 0;
    bool mixed = false;
    auto note = [&](Texture* t) {
        if (t == nullptr) return;
        const std::uint32_t c = t->ms != nil ? t->samples : 1;
        if (n == 0) n = c;
        else if (n != c) mixed = true;
    };
    for (unsigned i = 0; i < kTargets; ++i) note(rtv[i] ? texture(rtv[i]) : nullptr);
    note(dsv ? texture(dsv) : nullptr);
    if (mixed) {
        problem("pass mixes multisampled and single-sampled targets; drawn single-sampled");
        return 1;
    }
    return n == 0 ? 1 : n;
}

bool begin_pass(const std::uint32_t rtv[kTargets], const std::uint8_t mip[kTargets], const std::uint8_t slice[kTargets],
                std::uint32_t dsv) {
    Device& d = *g_dev;
    if (d.enc != nil && std::memcmp(d.enc_rtv, rtv, sizeof d.enc_rtv) == 0 &&
        std::memcmp(d.enc_mip, mip, sizeof d.enc_mip) == 0 && std::memcmp(d.enc_slice, slice, sizeof d.enc_slice) == 0 &&
        d.enc_dsv == dsv)
        return true;
    end_pass();
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    bool any = false;
    // MSAA: draw into the multisampled surfaces, keep them (a later pass may
    // load them) and resolve into the textures everything else reads.
    const bool msaa = pass_samples(rtv, dsv) > 1;
    for (unsigned i = 0; i < kTargets; ++i) {
        Texture* t = rtv[i] ? texture(rtv[i]) : nullptr;
        if (t == nullptr || t->depth) continue;
        const NSUInteger level = std::min<NSUInteger>(mip[i], t->tex.mipmapLevelCount - 1);
        pass.colorAttachments[i].texture = msaa ? t->ms : t->tex;
        pass.colorAttachments[i].level = msaa ? 0 : level;
        pass.colorAttachments[i].slice = t->cube ? std::min<NSUInteger>(slice[i], 6 * t->tex.arrayLength - 1) : 0;
        pass.colorAttachments[i].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[i].storeAction = msaa ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore;
        if (msaa) {
            pass.colorAttachments[i].resolveTexture = t->tex;
            pass.colorAttachments[i].resolveLevel = level;
        }
        if (level < 16 && slice[i] == 0 && (t->clear_mips & (1u << level))) {
            const float* c = t->clear_rgba[level];
            pass.colorAttachments[i].loadAction = MTLLoadActionClear;
            pass.colorAttachments[i].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
            t->clear_mips &= ~(1u << level);
        }
        if (t->clear_mips) flush_clears(*t);  // other mips: not this pass's
        any = true;
    }
    if (Texture* t = dsv ? texture(dsv) : nullptr; t != nullptr && t->depth) {
        ++t->written;
        pass.depthAttachment.texture = msaa ? t->ms : t->tex;
        pass.depthAttachment.loadAction = (t->clear_ds & 1u) ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.depthAttachment.storeAction = msaa ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore;
        pass.depthAttachment.clearDepth = t->clear_depth;
        if (msaa) {
            pass.depthAttachment.resolveTexture = t->tex;
            pass.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        }
        if (has_stencil(t->format)) {
            pass.stencilAttachment.texture = msaa ? t->ms : t->tex;
            pass.stencilAttachment.loadAction = (t->clear_ds & 2u) ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = msaa ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore;
            pass.stencilAttachment.clearStencil = t->clear_stencil;
            if (msaa) pass.stencilAttachment.resolveTexture = t->tex;
        }
        t->clear_ds = 0;
        any = true;
    }
    if (!any) return false;
    if (g_pp.active) {
        Texture* t0 = nullptr;
        unsigned n = 0;
        for (unsigned i = 0; i < kTargets; ++i)
            if (Texture* t = rtv[i] ? texture(rtv[i]) : nullptr; t != nullptr && !t->depth) {
                if (t0 == nullptr) t0 = t;
                ++n;
            }
        std::string label = t0 ? target_label("pass", t0, mip[0]) : std::string("pass depth-only");
        if (n > 1) label += " +" + std::to_string(n - 1);
        if (dsv) label += " ds";
        profile_pass(pass, label);
    } else {
        g_pp.open = -1;
    }
    d.enc = [command_buffer() renderCommandEncoderWithDescriptor:pass];
    d.bound = Device::Bound{};
    std::memcpy(d.enc_rtv, rtv, sizeof d.enc_rtv);
    std::memcpy(d.enc_mip, mip, sizeof d.enc_mip);
    std::memcpy(d.enc_slice, slice, sizeof d.enc_slice);
    d.enc_dsv = dsv;
    return true;
}

void note_use(NSArray<id<MTLBinding>>* bindings, StageUse* use) {
    for (id<MTLBinding> b in bindings) {
        if (!b.used || b.index >= 32) continue;
        const std::uint32_t bit = 1u << b.index;
        if (b.type == MTLBindingTypeBuffer && b.index < kBuffers) {
            use->buffers |= bit;
            use->buffer_size[b.index] = static_cast<std::uint32_t>(static_cast<id<MTLBufferBinding>>(b).bufferDataSize);
        }
        if (b.type == MTLBindingTypeTexture) {
            use->textures |= bit;
            const id<MTLTextureBinding> tb = static_cast<id<MTLTextureBinding>>(b);
            if (tb.isDepthTexture) use->depth |= bit;
            if (tb.textureType == MTLTextureTypeCube || tb.textureType == MTLTextureTypeCubeArray) use->cube |= bit;
            if (tb.textureType == MTLTextureType2DMultisample) use->ms |= bit;
        }
        if (b.type == MTLBindingTypeSampler) use->samplers |= bit;
    }
}

const Pipeline* pipeline(const DrawState& s, Program& vs, Program& ps) {
    Device& d = *g_dev;
    // The key covers everything the descriptor reads, as raw bytes: this runs
    // on every draw, and formatting it as text was most of the draw's cost.
    static std::string k;
    k.clear();
    auto put = [](std::uint64_t v) { k.append(reinterpret_cast<const char*>(&v), sizeof v); };
    put(s.vs);
    put(s.ps);
    for (const VertexElement& e : s.layout) {
        k.append(e.semantic);
        k.push_back('\0');
        put(std::uint64_t(e.index) | std::uint64_t(e.format) << 32);
        put(std::uint64_t(e.slot) | std::uint64_t(e.offset) << 32);
        put(std::uint64_t(e.instance) | std::uint64_t(e.step) << 32);
    }
    k.push_back('\1');
    for (unsigned i = 0; i < kStreams; ++i) put(s.stride[i]);
    for (unsigned i = 0; i < kTargets; ++i) {
        Texture* t = s.rtv[i] ? texture(s.rtv[i]) : nullptr;
        put(t ? static_cast<std::uint64_t>(t->format) : 0u);
        const BlendTarget& b = s.blend[i];
        put(std::uint64_t(b.enable) | std::uint64_t(b.src) << 8 | std::uint64_t(b.dst) << 16 | std::uint64_t(b.op) << 24 |
            std::uint64_t(b.src_alpha) << 32 | std::uint64_t(b.dst_alpha) << 40 | std::uint64_t(b.op_alpha) << 48 |
            std::uint64_t(b.write_mask) << 56);
    }
    Texture* dt = s.dsv ? texture(s.dsv) : nullptr;
    put(dt ? static_cast<std::uint64_t>(dt->format) : 0u);
    put(s.alpha_to_coverage ? 1u : 0u);
    const std::uint32_t samples = pass_samples(s.rtv, s.dsv);
    put(samples);
    if (auto it = d.pipelines.find(k); it != d.pipelines.end()) return it->second.state ? &it->second : nullptr;

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs.fn;
    pd.fragmentFunction = ps.fn;
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    bool used[kStreams]{};
    for (const Program::Input& in : vs.inputs) {
        const VertexElement* found = nullptr;
        for (const VertexElement& e : s.layout)
            if (upper(e.semantic) == in.semantic && e.index == in.index) found = &e;
        if (found == nullptr || found->slot >= kStreams) {
            problem("input layout has no " + in.semantic + std::to_string(in.index) + " for VS " + key_text(s.vs));
            d.pipelines[k] = Pipeline{};
            return nullptr;
        }
        const MTLVertexFormat vf = vertex_format(found->format);
        if (vf == MTLVertexFormatInvalid) {
            problem("vertex format " + std::to_string(found->format) + " not mapped");
            d.pipelines[k] = Pipeline{};
            return nullptr;
        }
        vd.attributes[in.location].format = vf;
        vd.attributes[in.location].offset = found->offset;
        vd.attributes[in.location].bufferIndex = kStreamBase + found->slot;
        used[found->slot] = true;
        vd.layouts[kStreamBase + found->slot].stride = s.stride[found->slot] ? s.stride[found->slot] : 4;
        vd.layouts[kStreamBase + found->slot].stepFunction =
            found->instance ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
        vd.layouts[kStreamBase + found->slot].stepRate = found->instance ? (found->step ? found->step : 1) : 1;
    }
    (void)used;
    pd.vertexDescriptor = vd;
    for (unsigned i = 0; i < kTargets; ++i) {
        Texture* t = s.rtv[i] ? texture(s.rtv[i]) : nullptr;
        if (t == nullptr || t->depth) continue;
        MTLRenderPipelineColorAttachmentDescriptor* ca = pd.colorAttachments[i];
        ca.pixelFormat = t->format;
        const BlendTarget& b = s.blend[i];
        ca.blendingEnabled = b.enable;
        ca.sourceRGBBlendFactor = blend_factor(b.src);
        ca.destinationRGBBlendFactor = blend_factor(b.dst);
        ca.rgbBlendOperation = blend_op(b.op);
        ca.sourceAlphaBlendFactor = blend_factor(b.src_alpha);
        ca.destinationAlphaBlendFactor = blend_factor(b.dst_alpha);
        ca.alphaBlendOperation = blend_op(b.op_alpha);
        MTLColorWriteMask m = MTLColorWriteMaskNone;
        if (b.write_mask & 1) m |= MTLColorWriteMaskRed;
        if (b.write_mask & 2) m |= MTLColorWriteMaskGreen;
        if (b.write_mask & 4) m |= MTLColorWriteMaskBlue;
        if (b.write_mask & 8) m |= MTLColorWriteMaskAlpha;
        ca.writeMask = m;
    }
    // With one sample a pixel, alpha to coverage is an alpha test at 0.5, as in
    // D3D11: the leaves of the trees are cut-out cards that relied on it.
    pd.alphaToCoverageEnabled = s.alpha_to_coverage;
    pd.rasterSampleCount = samples;
    if (dt != nullptr && dt->depth) {
        pd.depthAttachmentPixelFormat = dt->format;
        if (has_stencil(dt->format)) pd.stencilAttachmentPixelFormat = dt->format;
    }
    NSError* error = nil;
    MTLRenderPipelineReflection* reflection = nil;
    hitch::Timer build_timer(hitch::Pipeline);
    id<MTLRenderPipelineState> state = [d.dev newRenderPipelineStateWithDescriptor:pd
                                                                           options:MTLPipelineOptionBindingInfo
                                                                        reflection:&reflection
                                                                             error:&error];
    Pipeline made;
    made.state = state;
    if (reflection != nil) {
        note_use(reflection.vertexBindings, &made.vs);
        note_use(reflection.fragmentBindings, &made.ps);
    }
    if (state == nil) {
        ++d.stats.pipeline_failures;
        problem("pipeline " + key_text(s.vs) + "/" + key_text(s.ps) + ": " +
                (error ? std::string(error.localizedDescription.UTF8String).substr(0, 300) : "?"));
    } else {
        ++d.stats.pipelines;
    }
    d.pipelines[k] = made;
    return state ? &d.pipelines[k] : nullptr;
}

// Zero stand-ins for what a stage reads and the game did not bind.
void stand_ins() {
    Device& d = *g_dev;
    if (d.zero_buffer != nil) return;
    d.zero_buffer = [d.dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    std::memset(d.zero_buffer.contents, 0, 65536);
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:1
                                                                                height:1
                                                                             mipmapped:NO];
    d.zero_texture = [d.dev newTextureWithDescriptor:td];
    const std::uint32_t zero = 0;
    [d.zero_texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];
    MTLTextureDescriptor* dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                 width:1
                                                                                height:1
                                                                             mipmapped:NO];
    dd.storageMode = MTLStorageModePrivate;
    dd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    d.zero_depth = [d.dev newTextureWithDescriptor:dd];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = d.zero_depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0;
    [[command_buffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
    MTLTextureDescriptor* cd = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     size:1
                                                                                mipmapped:NO];
    d.zero_cube = [d.dev newTextureWithDescriptor:cd];
    for (NSUInteger face = 0; face < 6; ++face)
        [d.zero_cube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:face withBytes:&zero bytesPerRow:4
                     bytesPerImage:4];
    MTLTextureDescriptor* md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:1
                                                                                height:1
                                                                             mipmapped:NO];
    md.textureType = MTLTextureType2DMultisample;
    md.sampleCount = 4;
    md.storageMode = MTLStorageModePrivate;
    md.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    d.zero_ms = [d.dev newTextureWithDescriptor:md];
    MTLRenderPassDescriptor* mpass = [MTLRenderPassDescriptor renderPassDescriptor];
    mpass.colorAttachments[0].texture = d.zero_ms;
    mpass.colorAttachments[0].loadAction = MTLLoadActionClear;
    mpass.colorAttachments[0].storeAction = MTLStoreActionStore;
    mpass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    [[command_buffer() renderCommandEncoderWithDescriptor:mpass] endEncoding];
    d.default_sampler = [d.dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
}

id<MTLDepthStencilState> depth_state(const DepthDesc& dd) {
    Device& d = *g_dev;
    const std::uint32_t k = std::uint32_t(dd.enable) | std::uint32_t(dd.write) << 1 | dd.func << 2;
    if (auto it = d.depth_states.find(k); it != d.depth_states.end()) return it->second;
    MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
    desc.depthCompareFunction = dd.enable ? compare(dd.func) : MTLCompareFunctionAlways;
    desc.depthWriteEnabled = dd.enable && dd.write;
    id<MTLDepthStencilState> state = [d.dev newDepthStencilStateWithDescriptor:desc];
    d.depth_states[k] = state;
    return state;
}

MTLPrimitiveType primitive(std::uint32_t topology, bool* ok) {
    *ok = true;
    switch (topology) {
    case 1: return MTLPrimitiveTypePoint;
    case 2: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 4: return MTLPrimitiveTypeTriangle;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    default: *ok = false; return MTLPrimitiveTypeTriangle;
    }
}

Texture* dt_bound(const DrawState& s) {
    Texture* t = s.dsv ? texture(s.dsv) : nullptr;
    return t != nullptr && t->depth ? t : nullptr;
}

// The R32Float copy of depth texture `t`, brought up to date. Ends the open pass.
id<MTLTexture> depth_as_color(Texture& t) {
    Device& d = *g_dev;
    flush_clears(t);
    if (t.as_color != nil && t.copied == t.written) return t.as_color;
    const NSUInteger w = t.desc.width, h = t.desc.height;
    if (t.as_color == nil) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead;
        t.as_color = [d.dev newTextureWithDescriptor:td];
    }
    end_pass();
    if (t.depth_staging == nil)
        t.depth_staging = [d.dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModePrivate];
    id<MTLBuffer> staging = t.depth_staging;
    id<MTLBlitCommandEncoder> blit = blit_encoder("depth copy");
    [blit copyFromTexture:t.tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1) toBuffer:staging destinationOffset:0 destinationBytesPerRow:w * 4
    destinationBytesPerImage:w * h * 4 options:has_stencil(t.format) ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
    [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:w * 4 sourceBytesPerImage:w * h * 4
              sourceSize:MTLSizeMake(w, h, 1) toTexture:t.as_color destinationSlice:0 destinationLevel:0
       destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    t.copied = t.written;
    return t.as_color;
}

// Binds everything and returns the open encoder, or nil when the draw cannot run.
id<MTLRenderCommandEncoder> prepare(const DrawState& s, MTLPrimitiveType* prim) {
    Device& d = *g_dev;
    bool ok = false;
    *prim = primitive(s.topology, &ok);
    if (!ok) {
        problem("topology " + std::to_string(s.topology));
        return nil;
    }
    if (s.vs == 0) {
        problem("draw without a translated vertex shader");
        return nil;
    }
    // No pixel shader is a depth-only pass (shadow maps): D3D rasterizes and
    // writes depth, and so does a Metal pipeline without a fragment function.
    static Program none;
    Program& vs = program(s.vs, true);
    Program& ps = s.ps != 0 ? program(s.ps, false) : none;
    if (vs.fn == nil || (s.ps != 0 && ps.fn == nil)) return nil;
    const Pipeline* pl = pipeline(s, vs, ps);
    if (pl == nullptr) return nil;
    id<MTLRenderPipelineState> state = pl->state;
    stand_ins();
    // Depth read as colour: copies made before the pass opens (a blit cannot
    // run inside it).
    id<MTLTexture> color_of_depth[2][kSlots] = {};
    for (unsigned i = 0; i < kSlots; ++i) {
        const std::uint32_t bit = 1u << i;
        const std::uint32_t ids[2] = {s.vs_srv[i], s.ps_srv[i]};
        const StageUse* uses[2] = {&pl->vs, &pl->ps};
        for (int st = 0; st < 2; ++st) {
            Texture* t = ids[st] && (uses[st]->textures & bit) ? used(ids[st]) : nullptr;
            if (t != nullptr && t->depth && !(uses[st]->depth & bit)) {
                if (ids[st] == s.dsv) problem("depth texture bound as target and read as colour");
                else color_of_depth[st][i] = depth_as_color(*t);
            }
        }
    }
    if (!begin_pass(s.rtv, s.rtv_mip, s.rtv_slice, s.dsv)) {
        problem("draw with no Metal render target");
        return nil;
    }
    id<MTLRenderCommandEncoder> e = d.enc;
    Device::Bound& b = d.bound;
    if (b.pipeline != (__bridge void*)state) {
        [e setRenderPipelineState:state];
        b.pipeline = (__bridge void*)state;
    }
    DepthDesc dd = s.depth;
    if (dt_bound(s) == nullptr) dd.enable = false;  // no depth attachment: nothing to test or write
    if (id<MTLDepthStencilState> ds = depth_state(dd); b.depth != (__bridge void*)ds) {
        [e setDepthStencilState:ds];
        b.depth = (__bridge void*)ds;
    }
    const double vpd[6] = {s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], s.viewport[4], s.viewport[5]};
    if (std::memcmp(vpd, b.viewport, sizeof vpd) != 0) {
        [e setViewport:MTLViewport{vpd[0], vpd[1], vpd[2], vpd[3], vpd[4], vpd[5]}];
        std::memcpy(b.viewport, vpd, sizeof vpd);
    }
    // The target size bounds the scissor; without ScissorEnable it is the whole target.
    Texture* first = nullptr;
    for (unsigned i = 0; i < kTargets && first == nullptr; ++i) first = s.rtv[i] ? texture(s.rtv[i]) : nullptr;
    if (first == nullptr && s.dsv) first = texture(s.dsv);
    const int tw = first ? static_cast<int>(first->desc.width) : 1, th = first ? static_cast<int>(first->desc.height) : 1;
    int x0 = 0, y0 = 0, x1 = tw, y1 = th;
    if (s.raster.scissor) {
        x0 = std::max(0, std::min(s.scissor[0], tw));
        y0 = std::max(0, std::min(s.scissor[1], th));
        x1 = std::max(x0, std::min(s.scissor[2], tw));
        y1 = std::max(y0, std::min(s.scissor[3], th));
    }
    if (x1 <= x0 || y1 <= y0) return nil;  // nothing passes the scissor
    const NSUInteger sc[4] = {static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0), static_cast<NSUInteger>(x1 - x0),
                              static_cast<NSUInteger>(y1 - y0)};
    if (std::memcmp(sc, b.scissor, sizeof sc) != 0) {
        [e setScissorRect:MTLScissorRect{sc[0], sc[1], sc[2], sc[3]}];
        std::memcpy(b.scissor, sc, sizeof sc);
    }
    const int cull = s.raster.cull == 2 ? int(MTLCullModeFront) : s.raster.cull == 3 ? int(MTLCullModeBack) : int(MTLCullModeNone);
    if (cull != b.cull) {
        [e setCullMode:static_cast<MTLCullMode>(cull)];
        b.cull = cull;
    }
    const int winding = s.raster.front_ccw ? int(MTLWindingCounterClockwise) : int(MTLWindingClockwise);
    if (winding != b.winding) {
        [e setFrontFacingWinding:static_cast<MTLWinding>(winding)];
        b.winding = winding;
    }
    if (std::memcmp(s.blend_factor, b.blend, sizeof b.blend) != 0) {
        [e setBlendColorRed:s.blend_factor[0] green:s.blend_factor[1] blue:s.blend_factor[2] alpha:s.blend_factor[3]];
        std::memcpy(b.blend, s.blend_factor, sizeof b.blend);
    }
    // Only what the functions read is bound (the reflection's bitmasks); D3D
    // slots a shader ignores do not matter.
    auto set_buffer_at = [&](int stage, unsigned index, id<MTLBuffer> buf, NSUInteger off) {
        if (b.buf[stage][index] == (__bridge void*)buf && b.off[stage][index] == off) return;
        if (stage == 0) [e setVertexBuffer:buf offset:off atIndex:index];
        else [e setFragmentBuffer:buf offset:off atIndex:index];
        b.buf[stage][index] = (__bridge void*)buf;
        b.off[stage][index] = off;
    };
    const std::uint32_t* cbs[2] = {s.vs_cb, s.ps_cb};
    const StageUse* uses[2] = {&pl->vs, &pl->ps};
    for (int stage = 0; stage < 2; ++stage)
        for (std::uint32_t m = uses[stage]->buffers; m != 0; m &= m - 1) {
            const unsigned i = static_cast<unsigned>(__builtin_ctz(m));
            if (i >= kBuffers) break;
            auto it = cbs[stage][i] ? d.buffers.find(cbs[stage][i]) : d.buffers.end();
            if (it != d.buffers.end()) {
                const BufferRef r = constant_view(it->second, uses[stage]->buffer_size[i]);
                set_buffer_at(stage, i, r.buf, r.off);
            } else {
                set_buffer_at(stage, i, d.zero_buffer, 0);
            }
        }
    std::uint32_t streams = 0;
    for (const VertexElement& el : s.layout)
        if (el.slot < kStreams) streams |= 1u << el.slot;
    for (std::uint32_t m = streams; m != 0; m &= m - 1) {
        const unsigned i = static_cast<unsigned>(__builtin_ctz(m));
        auto it = s.vb[i] ? d.buffers.find(s.vb[i]) : d.buffers.end();
        if (it != d.buffers.end()) set_buffer_at(0, kStreamBase + i, it->second.buf, it->second.off + s.voff[i]);
    }
    // A texture the function reads as depth2d needs a depth texture, and
    // texture2d a colour one: a mismatch gets the zero stand-in of the right kind.
    auto pick = [&](std::uint32_t id_, std::uint32_t bit, const StageUse& use, id<MTLTexture> converted,
                    std::uint32_t first_mip, std::uint32_t count) -> id<MTLTexture> {
        if (converted != nil) return converted;
        const bool want_depth = (use.depth & bit) != 0;
        const bool want_cube = (use.cube & bit) != 0;
        Texture* t = id_ ? texture(id_) : nullptr;
        if (use.ms & bit) {  // texture2d_ms: the multisampled surface itself
            if (t != nullptr && t->ms != nil && !t->depth) return t->ms;
            problem("texture2d_ms read without a multisampled colour texture");
            return d.zero_ms;
        }
        if (t != nullptr && t->depth == want_depth && t->cube == want_cube) return mip_view(*t, first_mip, count);
        if (t != nullptr) problem(std::string("texture kind mismatch: ") + (t->depth ? "depth" : t->cube ? "cube" : "colour") +
                                  " bound where the shader reads " + (want_depth ? "depth" : want_cube ? "cube" : "colour"));
        return want_depth ? d.zero_depth : want_cube ? d.zero_cube : d.zero_texture;
    };
    const std::uint32_t* srvs[2] = {s.vs_srv, s.ps_srv};
    const std::uint8_t* mips[2] = {s.vs_srv_mip, s.ps_srv_mip};
    const std::uint8_t* counts[2] = {s.vs_srv_mips, s.ps_srv_mips};
    const std::uint32_t* samps[2] = {s.vs_samp, s.ps_samp};
    for (int stage = 0; stage < 2; ++stage) {
        for (std::uint32_t m = uses[stage]->textures; m != 0; m &= m - 1) {
            const unsigned i = static_cast<unsigned>(__builtin_ctz(m));
            if (i >= kSlots) break;
            id<MTLTexture> t = pick(srvs[stage][i], 1u << i, *uses[stage], color_of_depth[stage][i], mips[stage][i],
                                    counts[stage][i]);
            if (b.tex[stage][i] == (__bridge void*)t) continue;
            if (stage == 0) [e setVertexTexture:t atIndex:i];
            else [e setFragmentTexture:t atIndex:i];
            b.tex[stage][i] = (__bridge void*)t;
        }
        for (std::uint32_t m = uses[stage]->samplers; m != 0; m &= m - 1) {
            const unsigned i = static_cast<unsigned>(__builtin_ctz(m));
            if (i >= kSlots) break;
            auto it = samps[stage][i] ? d.samplers.find(samps[stage][i]) : d.samplers.end();
            id<MTLSamplerState> st = it != d.samplers.end() ? it->second : d.default_sampler;
            if (b.samp[stage][i] == (__bridge void*)st) continue;
            if (stage == 0) [e setVertexSamplerState:st atIndex:i];
            else [e setFragmentSamplerState:st atIndex:i];
            b.samp[stage][i] = (__bridge void*)st;
        }
    }
    return e;
}

}  // namespace

bool enabled() {
    if (g_checked) return g_dev != nullptr;
    g_checked = true;
    const char* on = std::getenv("DCUSA_METAL");
    const char* dir = std::getenv("DCUSA_SHADERS");
    if (on == nullptr || std::strcmp(on, "0") == 0 || dir == nullptr) return false;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev == nil) {
        std::fprintf(stderr, "metal: no device; DCUSA_METAL ignored\n");
        return false;
    }
    g_dev = new Device;
    g_dev->dev = dev;
    g_dev->queue = [dev newCommandQueue];
    profile_init();
    g_dev->shader_dir = dir;
    if (verbose())
        std::fprintf(stderr, "metal: %s, BCn %s\n", dev.name.UTF8String,
                     dev.supportsBCTextureCompression ? "yes" : "no");
    return true;
}

// The device itself. gpu_queue.mm runs these on the render thread, in the
// order the guest called them.
namespace direct {

void upload_texture_sub(std::uint32_t key, std::uint32_t slice, std::uint32_t mip, const std::uint8_t* bytes, std::size_t size,
                        std::uint32_t row_pitch);

void create_texture(std::uint32_t key, const TextureDesc& desc) {
    if (!enabled()) return;
    bool depth = false;
    const MTLPixelFormat format = pixel_format(desc.format, (desc.bind & 0x40u) != 0, &depth);
    Texture t;
    t.desc = desc;
    t.format = format;
    t.depth = depth;
    t.cube = is_cube(desc) && !depth;
    if (format == MTLPixelFormatInvalid || desc.width == 0 || desc.height == 0) {
        problem("texture format " + std::to_string(desc.format) + " not mapped");
        g_dev->textures[key] = t;
        return;
    }
    // Only mip 0 of uploaded data arrives from the guest, so an uploaded texture
    // has more levels only when the game asks Metal to generate them. A render
    // target keeps its levels: the bloom and glare chains render each level
    // while reading the one above it.
    const bool chain = desc.mips != 1 && ((desc.misc & 1u) != 0 || (desc.bind & 0x20u) != 0);
    std::uint32_t levels = 1;
    std::uint32_t full = 1;
    for (std::uint32_t w = std::max(desc.width, desc.height); w > 1; w >>= 1) ++full;
    if (chain) levels = desc.mips > 1 ? std::min(full, desc.mips) : full;
    // A texture the game fills itself (initial data, or UpdateSubresource per
    // level, which is how it loads most of them) keeps every level it declares:
    // without the smaller levels distant fences and signs shimmered and the
    // road's normal-mapped sheen broke up into speckles.
    if ((t.cube || !chain) && desc.mips != 1 && (desc.bind & 0x60u) == 0)
        levels = desc.mips == 0 ? full : std::min(full, desc.mips);
    t.tex = new_texture(desc, format, depth, levels);
    if (desc.samples > 1 && (desc.bind & 0x60u) != 0 && !t.cube && [g_dev->dev supportsTextureSampleCount:desc.samples]) {
        MTLTextureDescriptor* md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:desc.width
                                                                                     height:desc.height mipmapped:NO];
        md.textureType = MTLTextureType2DMultisample;
        md.sampleCount = desc.samples;
        md.storageMode = MTLStorageModePrivate;
        md.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        t.ms = [g_dev->dev newTextureWithDescriptor:md];
        t.samples = desc.samples;
    }
    ++g_dev->stats.textures;
    g_dev->textures[key] = t;
    // D3D11 creates resources zero-filled; Metal leaves private textures
    // undefined, and stale bits there (NaN included) reach the composite.
    if (t.tex.storageMode == MTLStorageModePrivate) {
        end_pass();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        if (depth) {
            pass.depthAttachment.texture = t.tex;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            pass.depthAttachment.clearDepth = 0;
            if (has_stencil(format)) {
                pass.stencilAttachment.texture = t.tex;
                pass.stencilAttachment.loadAction = MTLLoadActionClear;
                pass.stencilAttachment.storeAction = MTLStoreActionStore;
            }
        } else {
            pass.colorAttachments[0].texture = t.tex;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        }
        [[command_buffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
        // A cube map's other faces (slice 0 was cleared above).
        for (NSUInteger face = 1; !depth && t.cube && face < 6 * t.tex.arrayLength; ++face) {
            pass.colorAttachments[0].slice = face;
            [[command_buffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
        }
        if (t.ms != nil) {  // and the multisampled surface
            if (depth) {
                pass.depthAttachment.texture = t.ms;
                if (has_stencil(format)) pass.stencilAttachment.texture = t.ms;
            } else {
                pass.colorAttachments[0].texture = t.ms;
                pass.colorAttachments[0].slice = 0;
            }
            [[command_buffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
        }
    }
}

void upload_texture(std::uint32_t key, const std::uint8_t* bytes, std::size_t size, std::uint32_t row_pitch) {
    if (!enabled()) return;
    auto it = g_dev->textures.find(key);
    if (it == g_dev->textures.end() || it->second.tex == nil || it->second.depth || bytes == nullptr) return;
    Texture& t = it->second;
    if (t.cube) return upload_texture_sub(key, 0, 0, bytes, size, row_pitch);  // face 0, level 0
    flush_clears(t);
    const bool bc = t.desc.format >= 70 && t.desc.format <= 99 && t.desc.format != 87 && t.desc.format != 88 &&
                    t.desc.format != 90 && t.desc.format != 91;
    const std::uint32_t rows = bc ? (t.desc.height + 3) / 4 : t.desc.height;
    if (row_pitch == 0 || static_cast<std::size_t>(row_pitch) * rows > size) return;
    // A texture a pending draw may read gets a new Metal texture for the new bytes.
    if (t.tex.storageMode == MTLStorageModePrivate) {
        id<MTLBuffer> staging = [g_dev->dev newBufferWithBytes:bytes length:size options:MTLResourceStorageModeShared];
        end_pass();
        id<MTLBlitCommandEncoder> blit = blit_encoder("texture upload");
        [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:row_pitch sourceBytesPerImage:row_pitch * rows
                  sourceSize:MTLSizeMake(t.desc.width, t.desc.height, 1) toTexture:t.tex destinationSlice:0
            destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
    } else {
        // A draw still recorded or still on the GPU may read the old bytes.
        if (g_dev->cmd != nil || gpu_busy()) {
            id<MTLTexture> old = t.tex;
            t.tex = new_texture(t.desc, t.format, false, static_cast<std::uint32_t>(t.tex.mipmapLevelCount));
            // The smaller levels carry over (only level 0 is replaced below).
            if (old.mipmapLevelCount > 1) {
                end_pass();
                id<MTLBlitCommandEncoder> blit = blit_encoder("mip carry-over");
                for (NSUInteger level = 1; level < old.mipmapLevelCount; ++level)
                    [blit copyFromTexture:old sourceSlice:0 sourceLevel:level toTexture:t.tex destinationSlice:0
                         destinationLevel:level sliceCount:1 levelCount:1];
                [blit endEncoding];
            }
        }
        [t.tex replaceRegion:MTLRegionMake2D(0, 0, t.desc.width, t.desc.height) mipmapLevel:0 withBytes:bytes
                 bytesPerRow:row_pitch];
    }
    ++g_dev->stats.uploads;
}

void upload_texture_sub(std::uint32_t key, std::uint32_t slice, std::uint32_t mip, const std::uint8_t* bytes, std::size_t size,
                        std::uint32_t row_pitch) {
    if (!enabled()) return;
    Texture* t = texture(key);
    if (t == nullptr || t->depth || bytes == nullptr || mip >= t->tex.mipmapLevelCount) return;
    const NSUInteger slices = t->tex.arrayLength * (t->cube ? 6 : 1);
    if (slice >= slices) return;
    flush_clears(*t);
    const std::uint32_t w = std::max(1u, t->desc.width >> mip), h = std::max(1u, t->desc.height >> mip);
    const bool bc = t->desc.format >= 70 && t->desc.format <= 99 && t->desc.format != 87 && t->desc.format != 88 &&
                    t->desc.format != 90 && t->desc.format != 91;
    const std::uint32_t rows = bc ? (h + 3) / 4 : h;
    if (row_pitch == 0 || static_cast<std::size_t>(row_pitch) * rows > size) return;
    if (t->tex.storageMode == MTLStorageModePrivate) {
        id<MTLBuffer> staging = [g_dev->dev newBufferWithBytes:bytes length:size options:MTLResourceStorageModeShared];
        end_pass();
        id<MTLBlitCommandEncoder> blit = blit_encoder("texture upload");
        [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:row_pitch sourceBytesPerImage:row_pitch * rows
                  sourceSize:MTLSizeMake(w, h, 1) toTexture:t->tex destinationSlice:slice destinationLevel:mip
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
    } else {
        [t->tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:mip slice:slice withBytes:bytes bytesPerRow:row_pitch
                bytesPerImage:static_cast<NSUInteger>(row_pitch) * rows];
    }
    ++g_dev->stats.uploads;
}

void generate_mips(std::uint32_t key) {
    if (!enabled()) return;
    Texture* t = used(key);
    if (t == nullptr || t->tex.mipmapLevelCount < 2) return;
    end_pass();
    id<MTLBlitCommandEncoder> blit = blit_encoder("generate mips");
    [blit generateMipmapsForTexture:t->tex];
    [blit endEncoding];
}

void upload_buffer(std::uint32_t key, const std::uint8_t* bytes, std::size_t size) {
    if (!enabled() || bytes == nullptr || size == 0) return;
    // Always new storage: draws already encoded keep the bytes they were given.
    Device& d = *g_dev;
    static const bool no_arena = [] {
        const char* v = std::getenv("DCUSA_ARENA");
        return v != nullptr && std::strcmp(v, "0") == 0;
    }();
    // The first bytes a buffer gets (static meshes, index buffers, constants set
    // once) take a buffer of their own; only buffers the game updates again
    // (per-frame constants, dynamic vertices) use the arena.
    const bool first = d.buffers.find(key) == d.buffers.end();
    if (no_arena || first || size > kArenaSize / 8) {
        set_buffer(key, {[d.dev newBufferWithBytes:bytes length:size options:MTLResourceStorageModeShared], 0, size});
    } else {
        BufferRef r = arena_alloc(size);
        std::memcpy(const_cast<void*>(r.contents()), bytes, size);
        set_buffer(key, r);
    }
    ++g_dev->stats.uploads;
}

void create_sampler(std::uint32_t key, const SamplerDesc& desc) {
    if (!enabled()) return;
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    // D3D11_FILTER: bit 0 mip linear, bit 2 mag linear, bit 4 min linear,
    // 0x55 anisotropic, 0x80 comparison.
    const std::uint32_t f = desc.filter & 0x7Fu;
    sd.mipFilter = (f & 0x1u) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    sd.magFilter = (f & 0x4u) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.minFilter = (f & 0x10u) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    if ((f & 0x40u) != 0) sd.maxAnisotropy = std::max<std::uint32_t>(1, std::min<std::uint32_t>(16, desc.max_aniso));
    sd.sAddressMode = address(desc.address[0]);
    sd.tAddressMode = address(desc.address[1]);
    sd.rAddressMode = address(desc.address[2]);
    sd.lodMinClamp = std::max(0.0f, desc.min_lod);
    sd.lodMaxClamp = desc.max_lod;
    if ((desc.filter & 0x80u) != 0) sd.compareFunction = compare(desc.compare);
    const bool white = desc.border[0] > 0.5f && desc.border[3] > 0.5f;
    sd.borderColor = white ? MTLSamplerBorderColorOpaqueWhite
                           : desc.border[3] > 0.5f ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorTransparentBlack;
    g_dev->samplers[key] = [g_dev->dev newSamplerStateWithDescriptor:sd];
}

void destroy(std::uint32_t key) {
    if (!enabled()) return;
    g_dev->textures.erase(key);
    set_buffer(key, {});
    g_dev->samplers.erase(key);
}

void clear_target(std::uint32_t key, std::uint32_t mip, std::uint32_t slice, const float rgba[4]) {
    if (!enabled()) return;
    Texture* t = texture(key);
    if (g_trace) std::fprintf(stderr, "clear %06x %g %g %g %g\n", key, rgba[0], rgba[1], rgba[2], rgba[3]);
    if (t == nullptr || t->depth) return;
    if (attached(key)) end_pass();  // draws already encoded on it come first
    const std::uint32_t level = std::min<std::uint32_t>(mip, static_cast<std::uint32_t>(t->tex.mipmapLevelCount) - 1);
    if (t->cube && slice != 0) {
        // A face other than the first is cleared at once (deferred clears track slice 0 only).
        flush_clears(*t);
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = t->tex;
        pass.colorAttachments[0].level = level;
        pass.colorAttachments[0].slice = std::min<NSUInteger>(slice, 6 * t->tex.arrayLength - 1);
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
        profile_pass(pass, target_label("clear", t, mip));
        [[command_buffer() renderCommandEncoderWithDescriptor:pass] endEncoding];
        ++g_dev->stats.clears;
        return;
    }
    if (level >= 16) return;
    t->clear_mips |= 1u << level;
    std::memcpy(t->clear_rgba[level], rgba, sizeof t->clear_rgba[level]);
    ++g_dev->stats.clears;
}

void clear_depth(std::uint32_t key, float depth, std::uint8_t stencil, std::uint32_t flags) {
    if (!enabled()) return;
    Texture* t = texture(key);
    if (t == nullptr || !t->depth) return;
    if (attached(key)) end_pass();
    ++t->written;
    if (flags & 1u) t->clear_depth = depth;
    if (flags & 2u) t->clear_stencil = stencil;
    t->clear_ds |= flags & 3u;
    ++g_dev->stats.clears;
}

void draw_indexed(const DrawState& s, std::uint32_t count, std::uint32_t start, std::int32_t base) {
    if (!enabled() || count == 0) return;
    MTLPrimitiveType prim;
    id<MTLRenderCommandEncoder> e = prepare(s, &prim);
    auto ib = g_dev->buffers.find(s.ib);
    if (e == nil || ib == g_dev->buffers.end()) {
        trace_draw(s, "indexed", count, e == nil ? "skipped" : "no index buffer");
        ++g_dev->stats.skipped;
        if (e != nil) problem("indexed draw without an index buffer");
        return;
    }
    const bool wide = s.ib_format == 42;
    const NSUInteger size = wide ? 4 : 2;
    const NSUInteger offset = s.ib_off + static_cast<NSUInteger>(start) * size;
    if (offset + count * size > ib->second.len) {
        ++g_dev->stats.skipped;
        problem("index range outside the index buffer");
        return;
    }
    trace_draw(s, "indexed", count, "drawn");
    [e drawIndexedPrimitives:prim indexCount:count indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                 indexBuffer:ib->second.buf indexBufferOffset:ib->second.off + offset instanceCount:1 baseVertex:base baseInstance:0];
    ++g_dev->stats.draws;
    profile_draw(s.ps);
}

void draw(const DrawState& s, std::uint32_t count, std::uint32_t start) {
    if (!enabled() || count == 0) return;
    MTLPrimitiveType prim;
    id<MTLRenderCommandEncoder> e = prepare(s, &prim);
    if (e == nil) {
        ++g_dev->stats.skipped;
        return;
    }
    trace_draw(s, "plain", count, "drawn");
    [e drawPrimitives:prim vertexStart:start vertexCount:count];
    ++g_dev->stats.draws;
    profile_draw(s.ps);
}

namespace {

CAMetalLayer* g_layer = nil;
id<MTLRenderPipelineState> g_show_pipeline = nil;
id<MTLSamplerState> g_show_sampler = nil;

// A full-screen triangle that samples the frame; the viewport does the scaling.
constexpr const char* kShowSource = R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 pos [[position]]; float2 uv; };
vertex Out show_vs(uint id [[vertex_id]]) {
    float2 uv = float2((id << 1) & 2, id & 2);
    Out o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
fragment float4 show_fs(Out in [[stage_in]], texture2d<float> frame [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(frame.sample(s, in.uv).rgb, 1.0);
}
)";

bool make_show_pipeline() {
    if (g_show_pipeline != nil) return true;
    Device& d = *g_dev;
    NSError* error = nil;
    id<MTLLibrary> lib = [d.dev newLibraryWithSource:@(kShowSource) options:nil error:&error];
    if (lib == nil) {
        problem(std::string("show shader: ") + (error ? error.localizedDescription.UTF8String : "?"));
        return false;
    }
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"show_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"show_fs"];
    pd.colorAttachments[0].pixelFormat = g_layer.pixelFormat;
    g_show_pipeline = [d.dev newRenderPipelineStateWithDescriptor:pd error:&error];
    if (g_show_pipeline == nil) {
        problem(std::string("show pipeline: ") + (error ? error.localizedDescription.UTF8String : "?"));
        return false;
    }
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    g_show_sampler = [d.dev newSamplerStateWithDescriptor:sd];
    return true;
}

// FXAA (Lottes' FXAA 3.11 quality algorithm, written for Metal): finds edges
// by luma contrast, walks along each edge to its ends, and blends across it.
constexpr const char* kFxaaSource = R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 pos [[position]]; float2 uv; };
vertex Out fxaa_vs(uint id [[vertex_id]]) {
    float2 uv = float2((id << 1) & 2, id & 2);
    Out o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
static float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
static float L(texture2d<float> t, sampler s, float2 uv) { return luma(t.sample(s, uv, level(0)).rgb); }
fragment float4 fxaa_fs(Out in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    const float2 rcp = 1.0 / float2(t.get_width(), t.get_height());
    const float2 uv = in.uv;
    const float4 rgbaM = t.sample(s, uv, level(0));
    const float lM = luma(rgbaM.rgb);
    const float lN = L(t, s, uv + float2(0, -rcp.y)), lS = L(t, s, uv + float2(0, rcp.y));
    const float lE = L(t, s, uv + float2(rcp.x, 0)), lW = L(t, s, uv + float2(-rcp.x, 0));
    const float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    const float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    const float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) return rgbaM;
    const float lNW = L(t, s, uv + float2(-rcp.x, -rcp.y)), lNE = L(t, s, uv + float2(rcp.x, -rcp.y));
    const float lSW = L(t, s, uv + float2(-rcp.x, rcp.y)), lSE = L(t, s, uv + float2(rcp.x, rcp.y));
    // Sub-pixel aliasing: how far the centre is from its neighbourhood.
    const float avg = (2.0 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12.0;
    const float sub = smoothstep(0.0, 1.0, saturate(abs(avg - lM) / range));
    const float subOffset = sub * sub * 0.75;
    const float edgeH = abs(-2.0 * lW + lNW + lSW) + 2.0 * abs(-2.0 * lM + lN + lS) + abs(-2.0 * lE + lNE + lSE);
    const float edgeV = abs(-2.0 * lN + lNW + lNE) + 2.0 * abs(-2.0 * lM + lW + lE) + abs(-2.0 * lS + lSW + lSE);
    const bool horz = edgeH >= edgeV;
    const float l1 = horz ? lN : lW, l2 = horz ? lS : lE;
    const float g1 = l1 - lM, g2 = l2 - lM;
    const bool steep1 = abs(g1) >= abs(g2);
    const float gradScaled = 0.25 * max(abs(g1), abs(g2));
    float stepLen = horz ? rcp.y : rcp.x;
    float lLocal;
    if (steep1) { stepLen = -stepLen; lLocal = 0.5 * (l1 + lM); } else { lLocal = 0.5 * (l2 + lM); }
    float2 cur = uv;
    if (horz) cur.y += stepLen * 0.5; else cur.x += stepLen * 0.5;
    const float2 off = horz ? float2(rcp.x, 0) : float2(0, rcp.y);
    float2 uv1 = cur - off, uv2 = cur + off;
    float e1 = L(t, s, uv1) - lLocal, e2 = L(t, s, uv2) - lLocal;
    bool r1 = abs(e1) >= gradScaled, r2 = abs(e2) >= gradScaled;
    const float steps[10] = {1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
    for (int i = 0; i < 10 && !(r1 && r2); ++i) {
        if (!r1) { uv1 -= off * steps[i]; e1 = L(t, s, uv1) - lLocal; r1 = abs(e1) >= gradScaled; }
        if (!r2) { uv2 += off * steps[i]; e2 = L(t, s, uv2) - lLocal; r2 = abs(e2) >= gradScaled; }
    }
    const float d1 = horz ? uv.x - uv1.x : uv.y - uv1.y;
    const float d2 = horz ? uv2.x - uv.x : uv2.y - uv.y;
    const bool dir1 = d1 < d2;
    const float pixelOffset = -min(d1, d2) / (d1 + d2) + 0.5;
    const bool centreSmaller = lM < lLocal;
    const bool correct = ((dir1 ? e1 : e2) < 0.0) != centreSmaller;
    const float finalOffset = max(correct ? pixelOffset : 0.0, subOffset);
    float2 fuv = uv;
    if (horz) fuv.y += finalOffset * stepLen; else fuv.x += finalOffset * stepLen;
    return float4(t.sample(s, fuv, level(0)).rgb, rgbaM.a);
}
)";

struct Fxaa {
    id<MTLRenderPipelineState> state = nil;
    MTLPixelFormat format = MTLPixelFormatInvalid;
    id<MTLSamplerState> sampler = nil;
    id<MTLTexture> scratch = nil;
    bool failed = false;
} g_fxaa;

// FXAA: DCUSA_AA=1 on, 0 off, split on the right half; unset, on only when
// MSAA (DCUSA_MSAA, 4x by default) is off.
bool aa_enabled() {
    static const bool on = [] {
        if (const char* v = std::getenv("DCUSA_AA")) return std::strcmp(v, "0") != 0;
        const char* m = std::getenv("DCUSA_MSAA");
        return m != nullptr && std::atoi(m) == 0;
    }();
    return on;
}

}  // namespace

void antialias(std::uint32_t key) {
    if (!enabled() || !aa_enabled() || g_fxaa.failed) return;
    Texture* t = used(key);
    if (t == nullptr || t->tex == nil || t->depth || t->cube) return;
    Device& d = *g_dev;
    if (g_fxaa.state == nil || g_fxaa.format != t->format) {
        NSError* error = nil;
        id<MTLLibrary> lib = [d.dev newLibraryWithSource:@(kFxaaSource) options:nil error:&error];
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"fxaa_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fxaa_fs"];
        pd.colorAttachments[0].pixelFormat = t->format;
        g_fxaa.state = lib ? [d.dev newRenderPipelineStateWithDescriptor:pd error:&error] : nil;
        if (g_fxaa.state == nil) {
            problem(std::string("fxaa: ") + (error ? error.localizedDescription.UTF8String : "?"));
            g_fxaa.failed = true;
            return;
        }
        g_fxaa.format = t->format;
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        g_fxaa.sampler = [d.dev newSamplerStateWithDescriptor:sd];
        g_fxaa.scratch = nil;
    }
    const NSUInteger w = t->tex.width, h = t->tex.height;
    if (g_fxaa.scratch == nil || g_fxaa.scratch.width != w || g_fxaa.scratch.height != h ||
        g_fxaa.scratch.pixelFormat != t->format) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t->format width:w height:h
                                                                                   mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        g_fxaa.scratch = [d.dev newTextureWithDescriptor:td];
    }
    end_pass();
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = g_fxaa.scratch;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    profile_pass(pass, "fxaa");
    id<MTLRenderCommandEncoder> e = [command_buffer() renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:g_fxaa.state];
    [e setFragmentTexture:mip_view(*t, 0, 1) atIndex:0];
    [e setFragmentSamplerState:g_fxaa.sampler atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    // Back into the back buffer, so the window, read back and frame dumps all see it.
    id<MTLBlitCommandEncoder> blit = blit_encoder("fxaa copy");
    // DCUSA_AA=split: the right half only, to compare with and without.
    static const bool split = [] {
        const char* v = std::getenv("DCUSA_AA");
        return v != nullptr && std::strcmp(v, "split") == 0;
    }();
    const NSUInteger x0 = split ? w / 2 : 0;
    [blit copyFromTexture:g_fxaa.scratch sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, 0, 0)
               sourceSize:MTLSizeMake(w - x0, h, 1) toTexture:t->tex destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(x0, 0, 0)];
    [blit endEncoding];
}

void set_layer(void* layer) {
    if (!enabled()) return;
    g_layer = (__bridge CAMetalLayer*)layer;
    if (g_layer == nil) return;
    g_layer.device = g_dev->dev;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.framebufferOnly = YES;
    // The game's frame is sRGB-encoded; tag it so the display converts it.
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    g_layer.colorspace = srgb;
    CGColorSpaceRelease(srgb);
    g_show_pipeline = nil;
}

void show(std::uint32_t key) {
    if (!enabled() || g_layer == nil) return;
    Texture* t = used(key);
    if (t == nullptr || t->tex == nil || t->depth || !make_show_pipeline()) return;
    end_pass();
    // Follow the window: the drawable is the layer's size in pixels.
    const CGSize points = g_layer.bounds.size;
    const CGFloat scale = g_layer.contentsScale;
    const CGSize pixels = CGSizeMake(std::max<CGFloat>(1, points.width * scale), std::max<CGFloat>(1, points.height * scale));
    if (!CGSizeEqualToSize(g_layer.drawableSize, pixels)) g_layer.drawableSize = pixels;
    id<CAMetalDrawable> drawable = [g_layer nextDrawable];
    if (drawable == nil) return;
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> e = [command_buffer() renderCommandEncoderWithDescriptor:pass];
    // Fit the frame's aspect ratio inside the drawable, centred.
    const double fw = t->desc.width, fh = t->desc.height;
    const double s = std::min(pixels.width / fw, pixels.height / fh);
    const double w = fw * s, h = fh * s;
    [e setViewport:MTLViewport{(pixels.width - w) / 2, (pixels.height - h) / 2, w, h, 0, 1}];
    [e setRenderPipelineState:g_show_pipeline];
    [e setFragmentTexture:mip_view(*t, 0, 1) atIndex:0];
    [e setFragmentSamplerState:g_show_sampler atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [command_buffer() presentDrawable:drawable];
}

// GPU time per command buffer, accumulated from the completion handler (Metal's
// thread) and read on the guest's: an atomic sum in microseconds.
std::atomic<std::uint64_t> g_gpu_us{0};
std::atomic<unsigned> g_gpu_buffers{0};

void time_gpu(id<MTLCommandBuffer> cmd) {
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
        const double s = done.GPUEndTime - done.GPUStartTime;
        if (s > 0) g_gpu_us += static_cast<std::uint64_t>(s * 1e6);
        ++g_gpu_buffers;
    }];
}

void submit() {
    if (!enabled()) return;
    Device& d = *g_dev;
    end_pass();
    if (d.cmd == nil) return;
    time_gpu(d.cmd);
    [d.cmd commit];
    profile_submitted(d.cmd);
    d.in_flight.emplace_back(d.cmd, std::move(d.retired));
    d.retired.clear();
    d.cmd = nil;
    ++d.stats.frames;
    // At most two frames queued: the guest runs ahead of the GPU by one frame.
    while (!d.in_flight.empty()) {
        auto& [cmd, arenas] = d.in_flight.front();
        if (d.in_flight.size() > 2) [cmd waitUntilCompleted];
        else if (cmd.status < MTLCommandBufferStatusCompleted) break;
        check_completed(cmd);
        release_arenas(arenas);
        d.in_flight.pop_front();
    }
    unpark_arenas();
}

bool read_rgba8(std::uint32_t key, std::vector<std::uint8_t>* out, std::uint32_t* width, std::uint32_t* height) {
    if (!enabled()) return false;
    Texture* t = used(key);
    if (t == nullptr || (t->format != MTLPixelFormatRGBA8Unorm && t->format != MTLPixelFormatBGRA8Unorm &&
                         t->format != MTLPixelFormatRGBA8Unorm_sRGB && t->format != MTLPixelFormatBGRA8Unorm_sRGB))
        return false;
    end_pass();
    const NSUInteger w = t->desc.width, h = t->desc.height, row = w * 4;
    id<MTLBuffer> staging = [g_dev->dev newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [command_buffer() blitCommandEncoder];
    [blit copyFromTexture:t->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1) toBuffer:staging destinationOffset:0 destinationBytesPerRow:row
    destinationBytesPerImage:row * h];
    [blit endEncoding];
    id<MTLCommandBuffer> cmd = g_dev->cmd;
    g_dev->cmd = nil;
    [cmd commit];
    [cmd waitUntilCompleted];
    check_completed(cmd);
    recycle_arenas();
    out->resize(row * h);
    std::memcpy(out->data(), staging.contents, row * h);
    if (t->format == MTLPixelFormatBGRA8Unorm || t->format == MTLPixelFormatBGRA8Unorm_sRGB)
        for (std::size_t i = 0; i + 3 < out->size(); i += 4) std::swap((*out)[i], (*out)[i + 2]);
    *width = static_cast<std::uint32_t>(w);
    *height = static_cast<std::uint32_t>(h);
    ++g_dev->stats.frames;
    return true;
}

void set_trace(bool on) { g_trace = on; }

void scan_targets() {
    if (!enabled()) return;
    for (auto& entry : g_dev->textures) flush_clears(entry.second);
    end_pass();
    if (g_dev->cmd != nil) {
        [g_dev->cmd commit];
        [g_dev->cmd waitUntilCompleted];
        g_dev->cmd = nil;
        recycle_arenas();
    }
    for (auto& [key, t] : g_dev->textures) {
        if (t.tex == nil || (t.desc.bind & 0x60u) == 0 || t.depth) continue;
        const NSUInteger bpp = t.format == MTLPixelFormatRGBA16Float ? 8 : t.format == MTLPixelFormatRGBA32Float ? 16
                             : t.format == MTLPixelFormatRG16Float || t.format == MTLPixelFormatR32Float ||
                                       t.format == MTLPixelFormatRGBA8Unorm || t.format == MTLPixelFormatBGRA8Unorm
                                 ? 4
                             : t.format == MTLPixelFormatR16Float ? 2 : 0;
        if (bpp == 0) continue;
        const NSUInteger w = t.desc.width, h = t.desc.height, row = w * bpp;
        id<MTLBuffer> buf = [g_dev->dev newBufferWithLength:row * h options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cmd = [g_dev->queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit copyFromTexture:t.tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row
        destinationBytesPerImage:row * h];
        [blit endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        const std::uint8_t* p = static_cast<const std::uint8_t*>(buf.contents);
        std::size_t nonzero = 0;
        for (std::size_t i = 0; i < row * h; i += bpp)
            for (NSUInteger b = 0; b < bpp; ++b)
                if (p[i + b]) {
                    ++nonzero;
                    break;
                }
        // Per-channel maximum: bytes for 8-bit formats, values for half floats.
        float mx[4] = {};
        std::size_t nans = 0, negative = 0;
        // Where the NaNs are: texels with any NaN channel, per-channel counts, bounding box.
        std::size_t nan_texels = 0, nan_ch[4] = {};
        NSUInteger nx0 = w, ny0 = h, nx1 = 0, ny1 = 0;
        const unsigned channels = t.format == MTLPixelFormatRGBA16Float ? 4 : (bpp == 4 && t.format != MTLPixelFormatR32Float &&
                                  t.format != MTLPixelFormatRG16Float) ? 4 : 0;
        for (std::size_t i = 0; channels && i < row * h; i += bpp) {
            bool texel_nan = false;
            for (unsigned ch = 0; ch < 4; ++ch) {
                float v;
                if (t.format == MTLPixelFormatRGBA16Float) {
                    __fp16 hv;
                    std::memcpy(&hv, p + i + ch * 2, 2);
                    v = hv;
                    if (v != v) {
                        ++nans;
                        ++nan_ch[ch];
                        texel_nan = true;
                    } else if (v < 0) {
                        ++negative;
                    }
                } else {
                    v = p[i + ch];
                }
                if (v > mx[ch]) mx[ch] = v;
            }
            if (texel_nan) {
                ++nan_texels;
                const NSUInteger x = (i % row) / bpp, y = i / row;
                nx0 = std::min(nx0, x), ny0 = std::min(ny0, y), nx1 = std::max(nx1, x), ny1 = std::max(ny1, y);
            }
        }
        std::fprintf(stderr, "target %06x %4lux%-4lu dxgi %2u: %zu of %lu texels non-zero, max %.3g %.3g %.3g %.3g nan %zu neg %zu\n", key,
                     static_cast<unsigned long>(w), static_cast<unsigned long>(h), t.desc.format, nonzero,
                     static_cast<unsigned long>(w * h), mx[0], mx[1], mx[2], mx[3], nans, negative);
        if (nan_texels)
            std::fprintf(stderr, "  nan texels %zu (r %zu g %zu b %zu a %zu) in x %lu..%lu y %lu..%lu\n", nan_texels,
                         nan_ch[0], nan_ch[1], nan_ch[2], nan_ch[3], static_cast<unsigned long>(nx0),
                         static_cast<unsigned long>(nx1), static_cast<unsigned long>(ny0), static_cast<unsigned long>(ny1));
    }
}

const Stats& stats() {
    static Stats none;
    if (g_dev == nullptr) return none;
    g_dev->stats.gpu_seconds = static_cast<double>(g_gpu_us.load()) / 1e6;
    g_dev->stats.gpu_buffers = g_gpu_buffers.load();
    return g_dev->stats;
}

}  // namespace direct
}  // namespace dcusa::gpu

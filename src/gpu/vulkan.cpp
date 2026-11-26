// The Vulkan device behind the guest's D3D11 calls: gpu::direct (metal.hpp)
// for Linux (the Steam Deck) and, through MoltenVK, a Mac build that tests it.
//
// It follows the Metal backend (metal.mm) where the two APIs allow:
//  - Objects are keyed by the guest address of the D3D11 object.
//  - Buffer updates always get new storage (upload arenas), so draws already
//    recorded keep the bytes they were given.
//  - Shader resource views over a mip range get image views of that range
//    (the bloom chain reads one level while rendering the next).
//  - Slots a shader reads but the game left empty get zero stand-ins.
//  - MSAA targets render into a multisampled image resolved into the texture
//    everything else reads.
//  - Every present commits one command buffer with GPU timestamps, which the
//    game's timestamp queries (its dynamic resolution) read back.
// Unlike Metal, texture uploads are copies recorded in order with the draws,
// so a draw recorded before an upload sees the old texels without a new image.
//
// Simplifications: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full
// memory barrier separates each render pass, copy and clear from what follows.
//
// Shaders: msl/<key>.spv and msl/<key>.refl from tools/shaderprep
// (translate_shaders.py). Vertex resources are in descriptor set 0 and pixel
// resources in set 1; constant buffer b# at binding #, texture t# at 16 + #,
// sampler s# at 32 + #.
#include "gpu/metal.hpp"
#include "gpu/metal_direct.hpp"
#include "gpu/vk_functions.hpp"
#include "runtime/hitch.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "show_frag.h"
#include "show_vert.h"

namespace dcusa::gpu {
using namespace vk;
namespace {

constexpr VkDeviceSize kArenaSize = 8u << 20;  // upload arena; larger uploads get a buffer of their own
constexpr unsigned kFrameSlots = 3;
constexpr std::uint32_t kTexBase = 16, kSampBase = 32;

// ---- small helpers ----

bool verbose() {
    static const bool on = std::getenv("DCUSA_STATS") != nullptr;
    return on;
}

std::string key_text(std::uint64_t key) {
    char text[24];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(key));
    return text;
}

std::string upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

struct FormatInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t bytes = 0;  // per texel, or per 4x4 block for BC
    bool block = false;
    bool a8 = false;          // DXGI A8_UNORM: R8 with the value swizzled into alpha
};

FormatInfo texture_format(std::uint32_t dxgi) {
    switch (dxgi) {
    case 1: case 2: return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};
    case 9: case 10: return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};
    case 11: return {VK_FORMAT_R16G16B16A16_UNORM, 8};
    case 12: return {VK_FORMAT_R16G16B16A16_UINT, 8};
    case 13: return {VK_FORMAT_R16G16B16A16_SNORM, 8};
    case 15: case 16: return {VK_FORMAT_R32G32_SFLOAT, 8};
    case 23: case 24: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4};
    case 26: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4};
    case 27: case 28: return {VK_FORMAT_R8G8B8A8_UNORM, 4};
    case 29: return {VK_FORMAT_R8G8B8A8_SRGB, 4};
    case 30: return {VK_FORMAT_R8G8B8A8_UINT, 4};
    case 31: return {VK_FORMAT_R8G8B8A8_SNORM, 4};
    case 33: case 34: return {VK_FORMAT_R16G16_SFLOAT, 4};
    case 35: return {VK_FORMAT_R16G16_UNORM, 4};
    case 39: case 41: return {VK_FORMAT_R32_SFLOAT, 4};
    case 42: return {VK_FORMAT_R32_UINT, 4};
    case 48: case 49: return {VK_FORMAT_R8G8_UNORM, 2};
    case 53: case 54: return {VK_FORMAT_R16_SFLOAT, 2};
    case 56: return {VK_FORMAT_R16_UNORM, 2};
    case 60: case 61: return {VK_FORMAT_R8_UNORM, 1};
    case 65: return {VK_FORMAT_R8_UNORM, 1, false, true};
    case 70: case 71: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, true};
    case 72: return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, true};
    case 73: case 74: return {VK_FORMAT_BC2_UNORM_BLOCK, 16, true};
    case 75: return {VK_FORMAT_BC2_SRGB_BLOCK, 16, true};
    case 76: case 77: return {VK_FORMAT_BC3_UNORM_BLOCK, 16, true};
    case 78: return {VK_FORMAT_BC3_SRGB_BLOCK, 16, true};
    case 79: case 80: return {VK_FORMAT_BC4_UNORM_BLOCK, 8, true};
    case 81: return {VK_FORMAT_BC4_SNORM_BLOCK, 8, true};
    case 82: case 83: return {VK_FORMAT_BC5_UNORM_BLOCK, 16, true};
    case 84: return {VK_FORMAT_BC5_SNORM_BLOCK, 16, true};
    case 87: case 88: case 90: return {VK_FORMAT_B8G8R8A8_UNORM, 4};
    case 91: return {VK_FORMAT_B8G8R8A8_SRGB, 4};
    case 94: case 95: return {VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, true};
    case 96: return {VK_FORMAT_BC6H_SFLOAT_BLOCK, 16, true};
    case 97: case 98: return {VK_FORMAT_BC7_UNORM_BLOCK, 16, true};
    case 99: return {VK_FORMAT_BC7_SRGB_BLOCK, 16, true};
    default: return {};
    }
}

VkFormat depth_format(std::uint32_t dxgi) {
    switch (dxgi) {
    case 19: case 20: case 44: case 45: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default: return VK_FORMAT_D32_SFLOAT;  // 39, 40, 53, 55
    }
}

VkFormat vertex_format(std::uint32_t dxgi) {
    switch (dxgi) {
    case 2: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 3: return VK_FORMAT_R32G32B32A32_UINT;
    case 4: return VK_FORMAT_R32G32B32A32_SINT;
    case 6: return VK_FORMAT_R32G32B32_SFLOAT;
    case 7: return VK_FORMAT_R32G32B32_UINT;
    case 10: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 11: return VK_FORMAT_R16G16B16A16_UNORM;
    case 12: return VK_FORMAT_R16G16B16A16_UINT;
    case 13: return VK_FORMAT_R16G16B16A16_SNORM;
    case 14: return VK_FORMAT_R16G16B16A16_SINT;
    case 16: return VK_FORMAT_R32G32_SFLOAT;
    case 17: return VK_FORMAT_R32G32_UINT;
    case 28: return VK_FORMAT_R8G8B8A8_UNORM;
    case 30: return VK_FORMAT_R8G8B8A8_UINT;
    case 31: return VK_FORMAT_R8G8B8A8_SNORM;
    case 32: return VK_FORMAT_R8G8B8A8_SINT;
    case 34: return VK_FORMAT_R16G16_SFLOAT;
    case 35: return VK_FORMAT_R16G16_UNORM;
    case 36: return VK_FORMAT_R16G16_UINT;
    case 37: return VK_FORMAT_R16G16_SNORM;
    case 41: return VK_FORMAT_R32_SFLOAT;
    case 42: return VK_FORMAT_R32_UINT;
    case 43: return VK_FORMAT_R32_SINT;
    case 87: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkBlendFactor blend_factor(std::uint32_t b) {
    switch (b) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blend_op(std::uint32_t op) {
    switch (op) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp compare(std::uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkSamplerAddressMode address(std::uint32_t a, bool mirror_once) {
    switch (a) {
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return mirror_once ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

bool primitive(std::uint32_t topology, VkPrimitiveTopology* out) {
    switch (topology) {
    case 1: *out = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
    case 2: *out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
    case 3: *out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
    case 4: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
    case 5: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
    default: return false;
    }
}

bool is_integer(VkFormat f) {
    return f == VK_FORMAT_R16G16B16A16_UINT || f == VK_FORMAT_R8G8B8A8_UINT || f == VK_FORMAT_R32_UINT;
}

// ---- device state ----

// Host-visible memory: an upload arena (many buffers' bytes) or one large
// buffer's own. `live` counts the buffers whose current bytes are in it.
struct Arena {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    std::uint8_t* ptr = nullptr;
    VkDeviceSize size = 0;
    unsigned live = 0;
    bool own = false;  // one buffer's: destroyed, not reused, once free
};

struct BufferRef {
    Arena* arena = nullptr;
    VkDeviceSize off = 0, len = 0;
    std::uint8_t* contents() const { return arena->ptr + off; }
};

struct Texture {
    TextureDesc desc;
    FormatInfo info;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool depth = false, stencil = false, cube = false, valid = false;
    std::uint32_t levels = 1, layers = 1, samples = 1;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage ms = VK_NULL_HANDLE;  // the multisampled surface (DCUSA_MSAA)
    VkDeviceMemory ms_mem = VK_NULL_HANDLE;
    VkImageView ms_attach = VK_NULL_HANDLE, ms_sampled = VK_NULL_HANDLE;
    // Sampled views: first level << 16 | level count << 8 | view type.
    std::unordered_map<std::uint32_t, VkImageView> views;
    // Attachment views: level << 16 | layer.
    std::unordered_map<std::uint32_t, VkImageView> attach;
};

// Vulkan objects whose last use may still be on the GPU: freed with the
// frame they are handed to (frames complete in order).
struct Garbage {
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkDeviceMemory> mems;
    std::vector<VkSampler> samplers;
    bool empty() const { return images.empty() && views.empty() && mems.empty() && samplers.empty(); }
};

struct Program {
    VkShaderModule module = VK_NULL_HANDLE;
    bool failed = false;
    struct Input {
        std::uint32_t location;
        std::string semantic;
        std::uint32_t index;
    };
    std::vector<Input> inputs;
    struct Tex {
        std::uint32_t slot, dim;
    };
    std::vector<Tex> textures;
    std::vector<std::uint32_t> samplers;
    struct Cb {
        std::uint32_t slot, size;
    };
    std::vector<Cb> cbuffers;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    // The last set written for this program, reused while the bindings repeat.
    std::uint64_t set_frame = ~0ull;
    std::vector<std::uint64_t> set_contents;
    VkDescriptorSet set = VK_NULL_HANDLE;
};

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE, pre = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> pools;
    std::size_t pools_used = 0;
    bool recording = false, pre_recording = false, pending = false;
    std::uint64_t number = 0;   // frames committed before this one
    std::uint64_t serial = 0;   // recording serial (descriptor set reuse)
    int present_image = -1;     // the swapchain image show() drew into
    std::vector<Arena*> retired;
    Garbage garbage;
};

struct Device {
    void* lib = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mem_props{};
    double timestamp_ns = 1;  // nanoseconds per timestamp tick
    bool timestamps = false;
    bool mirror_once = false, anisotropy = false, dual_source = false;
    VkResolveModeFlags depth_resolve = 0;
    std::uint32_t buffer_memory = 0, buffer_memory_fallback = ~0u;
    std::string shader_dir;

    Frame frames[kFrameSlots];
    unsigned slot = 0;           // the frame being recorded
    std::uint64_t serial = 0;
    std::deque<unsigned> in_flight;

    // Recording state.
    bool rendering = false;
    bool need_barrier = false;
    std::uint32_t enc_rtv[kTargets]{};
    std::uint8_t enc_mip[kTargets]{}, enc_slice[kTargets]{};
    std::uint32_t enc_dsv = 0;
    struct Bound {
        VkPipeline pipeline = VK_NULL_HANDLE;
        float viewport[6] = {-1, -1, -1, -1, -1, -1};
        int scissor[4] = {-1, -1, -1, -1};
        int cull = -1, front = -1, depth_test = -1, depth_write = -1, depth_op = -1;
        float blend[4] = {-1, -1, -1, -1};
        VkDescriptorSet sets[2] = {};
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkBuffer vb[kStreams] = {};
        VkDeviceSize vb_off[kStreams] = {};
        VkBuffer ib = VK_NULL_HANDLE;
        VkDeviceSize ib_off = ~0ull;
        VkIndexType ib_type = VK_INDEX_TYPE_MAX_ENUM;
    } bound;

    std::unordered_map<std::uint32_t, Texture> textures;
    std::unordered_map<std::uint32_t, BufferRef> buffers;
    std::unordered_map<std::uint32_t, VkSampler> samplers;
    std::unordered_map<std::uint64_t, Program> programs;
    std::map<std::pair<VkDescriptorSetLayout, VkDescriptorSetLayout>, VkPipelineLayout> pipeline_layouts;
    std::unordered_map<std::string, Pipeline> pipelines;
    VkDescriptorSetLayout empty_layout = VK_NULL_HANDLE;

    // Upload arenas, as in metal.mm.
    Arena* arena = nullptr;
    VkDeviceSize arena_used = 0;
    Arena* static_arena = nullptr;
    VkDeviceSize static_used = 0;
    std::vector<Arena*> retired, spare_arenas, parked;
    std::vector<Arena*> all_arenas;
    Garbage garbage;

    // Zero stand-ins.
    Arena* zero_buffer = nullptr;
    Texture zero2d, zero_cube, zero_ms, zero3d;
    VkImageView zero2d_view = VK_NULL_HANDLE, zero2d_array_view = VK_NULL_HANDLE, zero_cube_view = VK_NULL_HANDLE,
                zero_ms_view = VK_NULL_HANDLE, zero3d_view = VK_NULL_HANDLE;
    VkSampler default_sampler = VK_NULL_HANDLE;

    std::unordered_map<std::string, bool> reported;
    Stats stats;
};

Device* g_dev = nullptr;
bool g_checked = false;
bool g_trace = false;
double g_rt_gpu_wait_ms = 0, g_rt_drawable_ms = 0;

// The window and its swapchain (set_layer, show).
struct Swap {
    SDL_Window* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> done;  // per image: rendering finished, for the present
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline show = VK_NULL_HANDLE, overlay = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    bool stale = false;
} g_swap;
std::atomic<std::uint32_t> g_overlay_key{0};

// GPU time, as in metal.mm.
std::atomic<std::uint64_t> g_gpu_us{0};
std::atomic<unsigned> g_gpu_buffers{0};
struct FrameTimes {
    std::atomic<std::uint64_t> frame{~0ull}, start_us{0}, end_us{0};
};
constexpr std::size_t kFrameTimes = 64;
FrameTimes g_frame_times[kFrameTimes];
std::uint64_t g_frames_committed = 0;

void problem(const std::string& what) {
    Device& d = *g_dev;
    d.stats.last_problem = what;
    if (d.reported.emplace(what, true).second && verbose()) std::fprintf(stderr, "vulkan: %s\n", what.c_str());
}

bool check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    if (g_dev != nullptr) problem(std::string(what) + " failed (" + std::to_string(static_cast<int>(r)) + ")");
    else std::fprintf(stderr, "vulkan: %s failed (%d)\n", what, static_cast<int>(r));
    return false;
}

std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) {
    const VkPhysicalDeviceMemoryProperties& m = g_dev->mem_props;
    for (std::uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & want) == want && (m.memoryTypes[i].propertyFlags & avoid) == 0)
            return i;
    return ~0u;
}

// ---- frames and command buffers ----

void complete(Frame& f, bool wait);

// The frame being recorded, begun if it is not yet.
Frame& frame() {
    Device& d = *g_dev;
    Frame& f = d.frames[d.slot];
    if (f.recording) return f;
    if (f.pending) complete(f, true);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(f.cmd, 0);
    vkBeginCommandBuffer(f.cmd, &bi);
    if (d.timestamps) {
        vkCmdResetQueryPool(f.cmd, f.queries, 0, 2);
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.queries, 0);
    }
    for (std::size_t i = 0; i < f.pools_used; ++i) vkResetDescriptorPool(d.dev, f.pools[i], 0);
    f.pools_used = 0;
    f.serial = ++d.serial;
    f.recording = true;
    f.present_image = -1;
    d.bound = Device::Bound{};
    return f;
}

VkCommandBuffer cmd() { return frame().cmd; }

// The frame's setup command buffer, run before its main one: image layout
// transitions and zero clears for new images, which so need not end a pass.
VkCommandBuffer pre() {
    Frame& f = frame();
    if (!f.pre_recording) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(f.pre, 0);
        vkBeginCommandBuffer(f.pre, &bi);
        f.pre_recording = true;
    }
    return f.pre;
}

void full_barrier(VkCommandBuffer c) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

// What was written since the last barrier is made visible to what follows.
void barrier() {
    Device& d = *g_dev;
    if (!d.need_barrier) return;
    full_barrier(cmd());
    d.need_barrier = false;
}

void end_pass() {
    Device& d = *g_dev;
    if (!d.rendering) return;
    vkCmdEndRendering(cmd());
    d.rendering = false;
    d.need_barrier = true;
    std::memset(d.enc_rtv, 0, sizeof d.enc_rtv);
    d.enc_dsv = 0;
}

VkDescriptorSet allocate_set(VkDescriptorSetLayout layout) {
    Device& d = *g_dev;
    Frame& f = frame();
    for (;;) {
        if (f.pools_used == f.pools.size()) {
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192},
                                                  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192},
                                                  {VK_DESCRIPTOR_TYPE_SAMPLER, 8192},
                                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 2048;
            pi.poolSizeCount = 4;
            pi.pPoolSizes = sizes;
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (!check(vkCreateDescriptorPool(d.dev, &pi, nullptr, &p), "vkCreateDescriptorPool")) return VK_NULL_HANDLE;
            f.pools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.pools[f.pools_used];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = vkAllocateDescriptorSets(d.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            check(r, "vkAllocateDescriptorSets");
            return VK_NULL_HANDLE;
        }
        ++f.pools_used;  // full: the next pool
    }
}

// ---- arenas (as metal.mm) ----

Arena* new_arena(VkDeviceSize size, bool own) {
    Device& d = *g_dev;
    auto* a = new Arena;
    a->size = size;
    a->own = own;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(d.dev, &bi, nullptr, &a->buf), "vkCreateBuffer")) {
        delete a;
        return nullptr;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d.dev, a->buf, &req);
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkResult r = mi.memoryTypeIndex != ~0u ? vkAllocateMemory(d.dev, &mi, nullptr, &a->mem) : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (r != VK_SUCCESS) {  // no (or no more) device-local host-visible memory
        mi.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        r = vkAllocateMemory(d.dev, &mi, nullptr, &a->mem);
    }
    if (!check(r, "vkAllocateMemory (buffer)")) {
        vkDestroyBuffer(d.dev, a->buf, nullptr);
        delete a;
        return nullptr;
    }
    vkBindBufferMemory(d.dev, a->buf, a->mem, 0);
    void* p = nullptr;
    vkMapMemory(d.dev, a->mem, 0, VK_WHOLE_SIZE, 0, &p);
    a->ptr = static_cast<std::uint8_t*>(p);
    d.all_arenas.push_back(a);
    return a;
}

void free_arena(Arena* a) {
    Device& d = *g_dev;
    vkDestroyBuffer(d.dev, a->buf, nullptr);
    vkFreeMemory(d.dev, a->mem, nullptr);
    d.all_arenas.erase(std::find(d.all_arenas.begin(), d.all_arenas.end(), a));
    delete a;
}

Arena* take_arena() {
    Device& d = *g_dev;
    if (!d.spare_arenas.empty()) {
        Arena* a = d.spare_arenas.back();
        d.spare_arenas.pop_back();
        return a;
    }
    return new_arena(kArenaSize, false);
}

BufferRef bump_alloc(Arena*& arena, VkDeviceSize& used, VkDeviceSize size) {
    VkDeviceSize off = (used + 255) & ~VkDeviceSize(255);
    if (arena == nullptr || off + size > kArenaSize) {
        if (arena != nullptr) g_dev->retired.push_back(arena);
        arena = take_arena();
        off = 0;
    }
    used = off + size;
    return {arena, off, size};
}

BufferRef arena_alloc(VkDeviceSize size) { return bump_alloc(g_dev->arena, g_dev->arena_used, size); }
BufferRef static_alloc(VkDeviceSize size) { return bump_alloc(g_dev->static_arena, g_dev->static_used, size); }

// `size` bytes for a copy to the GPU this frame: arena space, or a buffer of
// its own that is freed with the frame.
BufferRef staging_alloc(VkDeviceSize size) {
    if (size <= kArenaSize / 8) return arena_alloc(size);
    Arena* a = new_arena(size, true);
    if (a == nullptr) return {};
    g_dev->retired.push_back(a);
    return {a, 0, size};
}

void release_arenas(const std::vector<Arena*>& arenas) {
    Device& d = *g_dev;
    for (Arena* a : arenas) {
        if (a->live != 0) d.parked.push_back(a);
        else if (a->own) free_arena(a);
        else d.spare_arenas.push_back(a);
    }
}

void set_buffer(std::uint32_t key, const BufferRef& r) {
    Device& d = *g_dev;
    auto it = d.buffers.find(key);
    if (it != d.buffers.end()) {
        Arena* old = it->second.arena;
        if (--old->live == 0 && old->own) d.retired.push_back(old);  // freed once the GPU is done with it
        if (r.arena == nullptr) d.buffers.erase(it);
    }
    if (r.arena == nullptr) return;
    ++r.arena->live;
    d.buffers[key] = r;
}

void unpark_arenas() {
    Device& d = *g_dev;
    auto keep = d.parked.begin();
    for (Arena* a : d.parked) {
        if (a->live == 0) {
            if (!a->own) d.spare_arenas.push_back(a);
            // an own buffer reaching zero was put on `retired` by set_buffer
        } else {
            *keep++ = a;
        }
    }
    d.parked.erase(keep, d.parked.end());
}

void compact_parked() {
    Device& d = *g_dev;
    if (d.parked.size() < 4) return;
    static unsigned calls = 0;
    if (++calls % 30 != 0) return;
    std::unordered_map<Arena*, VkDeviceSize> live;
    for (Arena* a : d.parked)
        if (!a->own) live[a] = 0;
    for (const auto& [key, b] : d.buffers)
        if (auto it = live.find(b.arena); it != live.end()) it->second += b.len;
    std::vector<std::pair<std::uint32_t, BufferRef>> moving;
    for (const auto& [key, b] : d.buffers)
        if (auto it = live.find(b.arena); it != live.end() && it->second < kArenaSize * 3 / 4) moving.emplace_back(key, b);
    for (const auto& [key, b] : moving) {
        BufferRef r = static_alloc(b.len);
        std::memcpy(r.contents(), b.contents(), b.len);
        set_buffer(key, r);
    }
    auto keep = d.parked.begin();
    for (Arena* a : d.parked) {
        if (a->live == 0 && !a->own) d.retired.push_back(a);
        else *keep++ = a;
    }
    d.parked.erase(keep, d.parked.end());
}

void free_garbage(Garbage& g) {
    Device& d = *g_dev;
    for (VkImageView v : g.views) vkDestroyImageView(d.dev, v, nullptr);
    for (VkImage i : g.images) vkDestroyImage(d.dev, i, nullptr);
    for (VkDeviceMemory m : g.mems) vkFreeMemory(d.dev, m, nullptr);
    for (VkSampler s : g.samplers) vkDestroySampler(d.dev, s, nullptr);
    g = Garbage{};
}

// A committed frame is done (waiting for it when `wait`): its timestamps are
// read and what it held is released.
void complete(Frame& f, bool wait) {
    Device& d = *g_dev;
    if (!f.pending) return;
    if (wait) {
        const auto w0 = std::chrono::steady_clock::now();
        vkWaitForFences(d.dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
        g_rt_gpu_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
    } else if (vkGetFenceStatus(d.dev, f.fence) != VK_SUCCESS) {
        return;
    }
    f.pending = false;
    if (d.timestamps) {
        std::uint64_t t[2] = {};
        if (vkGetQueryPoolResults(d.dev, f.queries, 0, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double start = static_cast<double>(t[0]) * d.timestamp_ns / 1000.0;
            const double end = static_cast<double>(t[1]) * d.timestamp_ns / 1000.0;
            if (end > start) g_gpu_us += static_cast<std::uint64_t>(end - start);
            FrameTimes& ft = g_frame_times[f.number % kFrameTimes];
            ft.start_us.store(static_cast<std::uint64_t>(start), std::memory_order_relaxed);
            ft.end_us.store(static_cast<std::uint64_t>(end), std::memory_order_relaxed);
            ft.frame.store(f.number, std::memory_order_release);
        }
    }
    ++g_gpu_buffers;
    release_arenas(f.retired);
    f.retired.clear();
    free_garbage(f.garbage);
    if (!d.in_flight.empty() && &d.frames[d.in_flight.front()] == &f) d.in_flight.pop_front();
}

// Commits the frame being recorded. `wait`: until the GPU has run it.
void commit(bool wait) {
    Device& d = *g_dev;
    end_pass();
    Frame& f = frame();
    if (d.timestamps) vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.queries, 1);
    vkEndCommandBuffer(f.cmd);
    VkCommandBuffer bufs[2];
    std::uint32_t n = 0;
    if (f.pre_recording) {
        full_barrier(f.pre);
        vkEndCommandBuffer(f.pre);
        bufs[n++] = f.pre;
        f.pre_recording = false;
    }
    bufs[n++] = f.cmd;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = n;
    si.pCommandBuffers = bufs;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const bool presenting = f.present_image >= 0;
    if (presenting) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &g_swap.done[static_cast<std::size_t>(f.present_image)];
    }
    vkResetFences(d.dev, 1, &f.fence);
    check(vkQueueSubmit(d.queue, 1, &si, f.fence), "vkQueueSubmit");
    if (presenting) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &g_swap.done[static_cast<std::size_t>(f.present_image)];
        pi.swapchainCount = 1;
        pi.pSwapchains = &g_swap.swapchain;
        const std::uint32_t index = static_cast<std::uint32_t>(f.present_image);
        pi.pImageIndices = &index;
        const VkResult r = vkQueuePresentKHR(d.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_swap.stale = true;
    }
    f.recording = false;
    f.pending = true;
    f.number = g_frames_committed++;
    f.retired = std::move(d.retired);
    d.retired.clear();
    f.garbage = std::move(d.garbage);
    d.garbage = Garbage{};
    d.in_flight.push_back(d.slot);
    d.slot = (d.slot + 1) % kFrameSlots;
    ++d.stats.frames;
    if (wait) {
        while (!d.in_flight.empty()) complete(d.frames[d.in_flight.front()], true);
    } else {
        static const std::size_t limit = [] {
            const char* v = std::getenv("DCUSA_FRAMES_IN_FLIGHT");
            const int n2 = v ? std::atoi(v) : 2;
            return static_cast<std::size_t>(n2 < 1 ? 1 : n2 > 2 ? 2 : n2);
        }();
        while (!d.in_flight.empty()) {
            Frame& oldest = d.frames[d.in_flight.front()];
            if (d.in_flight.size() > limit) complete(oldest, true);
            else if (vkGetFenceStatus(d.dev, oldest.fence) == VK_SUCCESS) complete(oldest, false);
            else break;
        }
    }
    unpark_arenas();
    compact_parked();
}

// ---- textures ----

Texture* texture(std::uint32_t key) {
    auto it = g_dev->textures.find(key);
    return it == g_dev->textures.end() || !it->second.valid ? nullptr : &it->second;
}

bool is_cube(const TextureDesc& desc) {
    return (desc.misc & 0x4u) != 0 && desc.array >= 6 && desc.array % 6 == 0 && desc.width == desc.height;
}

VkImageAspectFlags aspect(const Texture& t, bool sampled) {
    if (!t.depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    if (sampled || !t.stencil) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
}

VkImage make_image(const Texture& t, std::uint32_t samples, VkDeviceMemory* mem) {
    Device& d = *g_dev;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = t.format;
    ci.extent = {t.desc.width, t.desc.height, 1};
    ci.mipLevels = samples > 1 ? 1 : t.levels;
    ci.arrayLayers = samples > 1 ? 1 : t.layers;
    ci.samples = static_cast<VkSampleCountFlagBits>(samples);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (t.depth) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if ((t.desc.bind & 0x20u) != 0 || samples > 1) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (t.cube && samples == 1) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (!check(vkCreateImage(d.dev, &ci, nullptr, &image), "vkCreateImage")) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d.dev, image, &req);
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mi.allocationSize = req.size;
    mi.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mi.memoryTypeIndex == ~0u) mi.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
    if (!check(vkAllocateMemory(d.dev, &mi, nullptr, mem), "vkAllocateMemory (image)")) {
        vkDestroyImage(d.dev, image, nullptr);
        return VK_NULL_HANDLE;
    }
    vkBindImageMemory(d.dev, image, *mem, 0);
    return image;
}

// A new image goes to GENERAL in the frame's setup buffer and, if `clear`, is
// zero-filled there (D3D11 creates resources zeroed).
void init_image(const Texture& t, VkImage image, std::uint32_t samples, bool clear) {
    VkCommandBuffer c = pre();
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = 0;
    ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = image;
    ib.subresourceRange = {aspect(t, false), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &ib);
    if (!clear) return;
    if (t.depth) {
        const VkClearDepthStencilValue v{0.0f, 0};
        vkCmdClearDepthStencilImage(c, image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &ib.subresourceRange);
    } else if (!t.info.block) {
        const VkClearColorValue v{};
        vkCmdClearColorImage(c, image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &ib.subresourceRange);
    }
    (void)samples;
}

VkImageView make_view(VkImage image, VkImageViewType type, VkFormat format, VkImageAspectFlags asp, std::uint32_t first_mip,
                      std::uint32_t mips, std::uint32_t first_layer, std::uint32_t layers, bool a8) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = type;
    vi.format = format;
    if (a8) vi.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
    vi.subresourceRange = {asp, first_mip, mips, first_layer, layers};
    VkImageView view = VK_NULL_HANDLE;
    check(vkCreateImageView(g_dev->dev, &vi, nullptr, &view), "vkCreateImageView");
    return view;
}

// The view a shader of dimension `dim` (D3D_SRV_DIMENSION: 4 2D, 5 2D array,
// 6 2D MS, 8 3D, 9 cube, 10 cube array) reads over levels first .. first +
// count - 1 (count 0: to the last); VK_NULL_HANDLE when the texture is not of
// that kind.
VkImageView sampled_view(Texture& t, std::uint32_t dim, std::uint32_t first, std::uint32_t count) {
    if (dim == 6) return t.ms_sampled;
    if (dim == 8) return VK_NULL_HANDLE;
    const bool want_cube = dim == 9 || dim == 10;
    if (want_cube != t.cube) return VK_NULL_HANDLE;
    if (first >= t.levels) first = t.levels - 1;
    const std::uint32_t n = count == 0 ? t.levels - first : std::min(count, t.levels - first);
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    std::uint32_t layers = 1;
    if (dim == 5) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY, layers = t.layers;
    if (dim == 9) type = VK_IMAGE_VIEW_TYPE_CUBE, layers = 6;
    if (dim == 10) type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, layers = t.layers;
    const std::uint32_t key = first << 16 | n << 8 | static_cast<std::uint32_t>(type);
    if (auto it = t.views.find(key); it != t.views.end()) return it->second;
    VkImageView v = make_view(t.image, type, t.format, aspect(t, true), first, n, 0, layers, t.info.a8);
    t.views[key] = v;
    return v;
}

VkImageView attach_view(Texture& t, std::uint32_t mip, std::uint32_t layer) {
    const std::uint32_t key = mip << 16 | layer;
    if (auto it = t.attach.find(key); it != t.attach.end()) return it->second;
    VkImageView v = make_view(t.image, VK_IMAGE_VIEW_TYPE_2D, t.format, aspect(t, false), mip, 1, layer, 1, false);
    t.attach[key] = v;
    return v;
}

void drop_texture(Texture& t) {
    Garbage& g = g_dev->garbage;
    for (auto& [k, v] : t.views) g.views.push_back(v);
    for (auto& [k, v] : t.attach) g.views.push_back(v);
    if (t.ms_attach) g.views.push_back(t.ms_attach);
    if (t.ms_sampled) g.views.push_back(t.ms_sampled);
    if (t.image) g.images.push_back(t.image);
    if (t.mem) g.mems.push_back(t.mem);
    if (t.ms) g.images.push_back(t.ms);
    if (t.ms_mem) g.mems.push_back(t.ms_mem);
    t = Texture{};
}

// ---- programs and pipelines ----

Program& program(std::uint64_t key, bool vertex) {
    Device& d = *g_dev;
    Program& p = d.programs[key];
    if (p.module != VK_NULL_HANDLE || p.failed) return p;
    const std::string base = d.shader_dir + "/msl/" + key_text(key);
    std::ifstream code(base + ".spv", std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(code)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() % 4 != 0) {
        p.failed = true;
        problem("no Vulkan shader " + key_text(key) + " (msl/" + key_text(key) + ".spv: run tools/shaderprep/translate_shaders.py)");
        return p;
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes.size();
    std::vector<std::uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    mi.pCode = words.data();
    if (!check(vkCreateShaderModule(d.dev, &mi, nullptr, &p.module), "vkCreateShaderModule")) {
        p.failed = true;
        return p;
    }
    std::ifstream in(base + ".refl");
    std::string line, kind;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        ls >> kind;
        if (kind == "input" && vertex) {
            Program::Input i;
            ls >> i.location >> i.semantic >> i.index;
            p.inputs.push_back(i);
        } else if (kind == "texture") {
            Program::Tex t{};
            ls >> t.slot >> t.dim;
            p.textures.push_back(t);
        } else if (kind == "sampler") {
            std::uint32_t slot = 0;
            ls >> slot;
            p.samplers.push_back(slot);
        } else if (kind == "cbuffer") {
            Program::Cb cb{};
            ls >> cb.slot >> cb.size;
            p.cbuffers.push_back(cb);
        }
    }
    std::vector<VkDescriptorSetLayoutBinding> b;
    const VkShaderStageFlags stage = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    for (const auto& cb : p.cbuffers) b.push_back({cb.slot, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stage, nullptr});
    for (const auto& t : p.textures) b.push_back({kTexBase + t.slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stage, nullptr});
    for (std::uint32_t s : p.samplers) b.push_back({kSampBase + s, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stage, nullptr});
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<std::uint32_t>(b.size());
    li.pBindings = b.data();
    check(vkCreateDescriptorSetLayout(d.dev, &li, nullptr, &p.layout), "vkCreateDescriptorSetLayout");
    return p;
}

VkPipelineLayout pipeline_layout(VkDescriptorSetLayout vs, VkDescriptorSetLayout ps) {
    Device& d = *g_dev;
    auto& slot = d.pipeline_layouts[{vs, ps}];
    if (slot != VK_NULL_HANDLE) return slot;
    const VkDescriptorSetLayout sets[2] = {vs ? vs : d.empty_layout, ps ? ps : d.empty_layout};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = sets;
    check(vkCreatePipelineLayout(d.dev, &li, nullptr, &slot), "vkCreatePipelineLayout");
    return slot;
}

// The sample count a pass over these targets renders with (as metal.mm).
std::uint32_t pass_samples(const std::uint32_t rtv[kTargets], std::uint32_t dsv) {
    std::uint32_t n = 0;
    bool mixed = false;
    auto note = [&](Texture* t) {
        if (t == nullptr) return;
        const std::uint32_t c = t->ms != VK_NULL_HANDLE ? t->samples : 1;
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

Texture* dt_bound(const DrawState& s) {
    Texture* t = s.dsv ? texture(s.dsv) : nullptr;
    return t != nullptr && t->depth ? t : nullptr;
}

const Pipeline* pipeline(const DrawState& s, Program& vs, Program* ps, VkPrimitiveTopology topology) {
    Device& d = *g_dev;
    static std::string k;
    k.clear();
    auto put = [](std::uint64_t v) { k.append(reinterpret_cast<const char*>(&v), sizeof v); };
    put(s.vs);
    put(s.ps);
    put(s.layout_serial);
    put(static_cast<std::uint64_t>(topology));
    for (unsigned i = 0; i < kStreams; ++i) put(s.stride[i]);
    for (unsigned i = 0; i < kTargets; ++i) {
        Texture* t = s.rtv[i] ? texture(s.rtv[i]) : nullptr;
        put(t && !t->depth ? static_cast<std::uint64_t>(t->format) : 0u);
        const BlendTarget& b = s.blend[i];
        put(std::uint64_t(b.enable) | std::uint64_t(b.src) << 8 | std::uint64_t(b.dst) << 16 | std::uint64_t(b.op) << 24 |
            std::uint64_t(b.src_alpha) << 32 | std::uint64_t(b.dst_alpha) << 40 | std::uint64_t(b.op_alpha) << 48 |
            std::uint64_t(b.write_mask) << 56);
    }
    Texture* dt = dt_bound(s);
    put(dt ? static_cast<std::uint64_t>(dt->format) : 0u);
    put(s.alpha_to_coverage ? 1u : 0u);
    const std::uint32_t samples = pass_samples(s.rtv, s.dsv);
    put(samples);
    if (auto it = d.pipelines.find(k); it != d.pipelines.end()) return it->second.pipeline ? &it->second : nullptr;

    Pipeline made;
    made.layout = pipeline_layout(vs.layout, ps ? ps->layout : VK_NULL_HANDLE);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs.module;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps ? ps->module : VK_NULL_HANDLE;
    stages[1].pName = "main";

    std::vector<VkVertexInputAttributeDescription> attrs;
    std::vector<VkVertexInputBindingDescription> binds;
    bool bound_slot[kStreams]{};
    for (const Program::Input& in : vs.inputs) {
        const VertexElement* found = nullptr;
        if (s.layout != nullptr)
            for (const VertexElement& e : *s.layout)
                if (upper(e.semantic) == in.semantic && e.index == in.index) found = &e;
        if (found == nullptr || found->slot >= kStreams) {
            problem("input layout has no " + in.semantic + std::to_string(in.index) + " for VS " + key_text(s.vs));
            d.pipelines[k] = Pipeline{};
            return nullptr;
        }
        const VkFormat vf = vertex_format(found->format);
        if (vf == VK_FORMAT_UNDEFINED) {
            problem("vertex format " + std::to_string(found->format) + " not mapped");
            d.pipelines[k] = Pipeline{};
            return nullptr;
        }
        attrs.push_back({in.location, found->slot, vf, found->offset});
        if (!bound_slot[found->slot]) {
            bound_slot[found->slot] = true;
            binds.push_back({found->slot, s.stride[found->slot] ? s.stride[found->slot] : 4,
                             found->instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = static_cast<std::uint32_t>(binds.size());
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topology;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(samples);
    ms.alphaToCoverageEnable = s.alpha_to_coverage ? VK_TRUE : VK_FALSE;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    std::vector<VkFormat> formats;
    for (unsigned i = 0; i < kTargets; ++i) {
        Texture* t = s.rtv[i] ? texture(s.rtv[i]) : nullptr;
        if (t == nullptr || t->depth) continue;
        while (formats.size() < i) {
            formats.push_back(VK_FORMAT_UNDEFINED);
            blends.push_back(VkPipelineColorBlendAttachmentState{});
        }
        formats.push_back(t->format);
        const BlendTarget& b = s.blend[i];
        VkPipelineColorBlendAttachmentState a{};
        a.blendEnable = b.enable && !is_integer(t->format) ? VK_TRUE : VK_FALSE;
        a.srcColorBlendFactor = blend_factor(b.src);
        a.dstColorBlendFactor = blend_factor(b.dst);
        a.colorBlendOp = blend_op(b.op);
        a.srcAlphaBlendFactor = blend_factor(b.src_alpha);
        a.dstAlphaBlendFactor = blend_factor(b.dst_alpha);
        a.alphaBlendOp = blend_op(b.op_alpha);
        a.colorWriteMask = b.write_mask & 0xFu;
        if (!d.dual_source) {
            auto plain = [](VkBlendFactor f) {
                switch (f) {
                case VK_BLEND_FACTOR_SRC1_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
                case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case VK_BLEND_FACTOR_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
                case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                default: return f;
                }
            };
            a.srcColorBlendFactor = plain(a.srcColorBlendFactor);
            a.dstColorBlendFactor = plain(a.dstColorBlendFactor);
            a.srcAlphaBlendFactor = plain(a.srcAlphaBlendFactor);
            a.dstAlphaBlendFactor = plain(a.dstAlphaBlendFactor);
        }
        blends.push_back(a);
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = static_cast<std::uint32_t>(blends.size());
    cb.pAttachments = blends.data();
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,          VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_CULL_MODE,         VK_DYNAMIC_STATE_FRONT_FACE,       VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                  VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 8;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = static_cast<std::uint32_t>(formats.size());
    ri.pColorAttachmentFormats = formats.data();
    if (dt != nullptr) {
        ri.depthAttachmentFormat = dt->format;
        if (dt->stencil) ri.stencilAttachmentFormat = dt->format;
    }
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri;
    pi.stageCount = ps ? 2 : 1;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = made.layout;
    hitch::Timer build_timer(hitch::Pipeline);
    const VkResult r = vkCreateGraphicsPipelines(d.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &made.pipeline);
    if (r != VK_SUCCESS) {
        ++d.stats.pipeline_failures;
        problem("pipeline " + key_text(s.vs) + "/" + key_text(s.ps) + " failed (" + std::to_string(static_cast<int>(r)) + ")");
        made.pipeline = VK_NULL_HANDLE;
    } else {
        ++d.stats.pipelines;
    }
    d.pipelines[k] = made;
    return made.pipeline ? &d.pipelines[k] : nullptr;
}

// ---- passes ----

bool begin_pass(const std::uint32_t rtv[kTargets], const std::uint8_t mip[kTargets], const std::uint8_t slice[kTargets],
                std::uint32_t dsv) {
    Device& d = *g_dev;
    if (d.rendering && std::memcmp(d.enc_rtv, rtv, sizeof d.enc_rtv) == 0 && std::memcmp(d.enc_mip, mip, sizeof d.enc_mip) == 0 &&
        std::memcmp(d.enc_slice, slice, sizeof d.enc_slice) == 0 && d.enc_dsv == dsv)
        return true;
    end_pass();
    const bool msaa = pass_samples(rtv, dsv) > 1;
    VkRenderingAttachmentInfo colors[kTargets]{};
    std::uint32_t count = 0;
    std::uint32_t w = ~0u, h = ~0u;
    for (unsigned i = 0; i < kTargets; ++i) {
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        Texture* t = rtv[i] ? texture(rtv[i]) : nullptr;
        if (t == nullptr || t->depth) continue;
        const std::uint32_t level = std::min<std::uint32_t>(mip[i], t->levels - 1);
        const std::uint32_t layer = t->cube ? std::min<std::uint32_t>(slice[i], t->layers - 1) : 0;
        VkImageView view = attach_view(*t, level, layer);
        colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (msaa) {
            colors[i].imageView = t->ms_attach;
            colors[i].resolveMode = is_integer(t->format) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_AVERAGE_BIT;
            colors[i].resolveImageView = view;
            colors[i].resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        } else {
            colors[i].imageView = view;
        }
        w = std::min(w, std::max(1u, t->desc.width >> level));
        h = std::min(h, std::max(1u, t->desc.height >> level));
        count = i + 1;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    Texture* dt = dsv ? texture(dsv) : nullptr;
    if (dt != nullptr && !dt->depth) dt = nullptr;
    if (dt != nullptr) {
        depth.imageView = msaa ? dt->ms_attach : attach_view(*dt, 0, 0);
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (msaa && (d.depth_resolve & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)) {
            depth.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            depth.resolveImageView = attach_view(*dt, 0, 0);
            depth.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        if (dt->stencil) stencil = depth;
        w = std::min(w, dt->desc.width);
        h = std::min(h, dt->desc.height);
    }
    if (count == 0 && dt == nullptr) return false;
    barrier();
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {w, h}};
    info.layerCount = 1;
    info.colorAttachmentCount = count;
    info.pColorAttachments = colors;
    if (dt != nullptr) info.pDepthAttachment = &depth;
    if (dt != nullptr && dt->stencil) info.pStencilAttachment = &stencil;
    vkCmdBeginRendering(cmd(), &info);
    d.rendering = true;
    d.bound = Device::Bound{};
    std::memcpy(d.enc_rtv, rtv, sizeof d.enc_rtv);
    std::memcpy(d.enc_mip, mip, sizeof d.enc_mip);
    std::memcpy(d.enc_slice, slice, sizeof d.enc_slice);
    d.enc_dsv = dsv;
    return true;
}

bool attached(std::uint32_t key) {
    const Device& d = *g_dev;
    if (!d.rendering) return false;
    if (d.enc_dsv == key) return true;
    for (std::uint32_t r : d.enc_rtv)
        if (r == key) return true;
    return false;
}

void make_stand_ins() {
    Device& d = *g_dev;
    if (d.zero_buffer != nullptr) return;
    d.zero_buffer = new_arena(65536, false);
    std::memset(d.zero_buffer->ptr, 0, 65536);
    ++d.zero_buffer->live;  // never released
    auto make = [&](Texture& t, bool cube, std::uint32_t samples) {
        t.desc.width = t.desc.height = 1;
        t.desc.bind = 0x28;
        t.format = VK_FORMAT_R8G8B8A8_UNORM;
        t.info = texture_format(28);
        t.cube = cube;
        t.layers = cube ? 6 : 1;
        t.levels = 1;
        t.samples = samples;
        if (samples > 1) {
            t.ms = make_image(t, samples, &t.ms_mem);
            init_image(t, t.ms, samples, true);
        } else {
            t.image = make_image(t, 1, &t.mem);
            init_image(t, t.image, 1, true);
        }
        t.valid = true;
    };
    make(d.zero2d, false, 1);
    make(d.zero_cube, true, 1);
    make(d.zero_ms, false, 4);
    d.zero2d_view = make_view(d.zero2d.image, VK_IMAGE_VIEW_TYPE_2D, d.zero2d.format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, false);
    d.zero2d_array_view =
        make_view(d.zero2d.image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, d.zero2d.format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, false);
    d.zero_cube_view =
        make_view(d.zero_cube.image, VK_IMAGE_VIEW_TYPE_CUBE, d.zero_cube.format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6, false);
    d.zero_ms_view = make_view(d.zero_ms.ms, VK_IMAGE_VIEW_TYPE_2D, d.zero_ms.format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, false);
    // A 3D stand-in.
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_3D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {1, 1, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vkCreateImage(d.dev, &ci, nullptr, &d.zero3d.image);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d.dev, d.zero3d.image, &req);
        VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mi.allocationSize = req.size;
        mi.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
        vkAllocateMemory(d.dev, &mi, nullptr, &d.zero3d.mem);
        vkBindImageMemory(d.dev, d.zero3d.image, d.zero3d.mem, 0);
        d.zero3d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.zero3d.info = texture_format(28);
        init_image(d.zero3d, d.zero3d.image, 1, true);
        d.zero3d_view = make_view(d.zero3d.image, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                                  false);
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(d.dev, &si, nullptr, &d.default_sampler);
}

VkImageView stand_in(std::uint32_t dim) {
    Device& d = *g_dev;
    switch (dim) {
    case 5: return d.zero2d_array_view;
    case 6: return d.zero_ms_view;
    case 8: return d.zero3d_view;
    case 9: case 10: return d.zero_cube_view;
    default: return d.zero2d_view;
    }
}

// A constant buffer as a shader reads it: D3D11 reads zeros past the end of a
// bound buffer, so a shorter one gets a zero-padded copy.
BufferRef constant_view(const BufferRef& b, std::uint32_t wanted) {
    if (wanted <= b.len || wanted > kArenaSize) return b;
    BufferRef r = arena_alloc(wanted);
    std::memcpy(r.contents(), b.contents(), b.len);
    std::memset(r.contents() + b.len, 0, wanted - b.len);
    return r;
}

// The descriptor set for one stage's program, written (or reused).
VkDescriptorSet stage_set(Program& p, const std::uint32_t* cbs, const std::uint32_t* srvs, const std::uint8_t* mips,
                          const std::uint8_t* counts, const std::uint32_t* samps) {
    Device& d = *g_dev;
    if (p.cbuffers.empty() && p.textures.empty() && p.samplers.empty()) return VK_NULL_HANDLE;
    static std::vector<VkDescriptorBufferInfo> bufs;
    static std::vector<VkDescriptorImageInfo> images;
    static std::vector<std::uint64_t> contents;
    bufs.clear();
    images.clear();
    contents.clear();
    for (const auto& cb : p.cbuffers) {
        auto it = cb.slot < kBuffers && cbs[cb.slot] ? d.buffers.find(cbs[cb.slot]) : d.buffers.end();
        const std::uint32_t size = std::max<std::uint32_t>(16, cb.size);
        VkDescriptorBufferInfo bi{};
        if (it != d.buffers.end()) {
            const BufferRef r = constant_view(it->second, size);
            bi = {r.arena->buf, r.off, size};
        } else {
            bi = {d.zero_buffer->buf, 0, std::min<VkDeviceSize>(size, 65536)};
        }
        bufs.push_back(bi);
        contents.push_back(reinterpret_cast<std::uint64_t>(bi.buffer));
        contents.push_back(bi.offset);
    }
    for (const auto& tx : p.textures) {
        VkImageView v = VK_NULL_HANDLE;
        Texture* t = tx.slot < kSlots && srvs[tx.slot] ? texture(srvs[tx.slot]) : nullptr;
        if (t != nullptr) {
            v = sampled_view(*t, tx.dim, mips[tx.slot], counts[tx.slot]);
            if (v == VK_NULL_HANDLE)
                problem(std::string("texture kind mismatch: ") + (t->cube ? "cube" : t->depth ? "depth" : "colour") +
                        " bound where the shader reads dimension " + std::to_string(tx.dim));
        }
        if (v == VK_NULL_HANDLE) v = stand_in(tx.dim);
        images.push_back({VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL});
        contents.push_back(reinterpret_cast<std::uint64_t>(v));
    }
    for (std::uint32_t slot : p.samplers) {
        auto it = slot < kSlots && samps[slot] ? d.samplers.find(samps[slot]) : d.samplers.end();
        VkSampler s = it != d.samplers.end() ? it->second : d.default_sampler;
        images.push_back({s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
        contents.push_back(reinterpret_cast<std::uint64_t>(s));
    }
    const Frame& f = d.frames[d.slot];
    if (p.set != VK_NULL_HANDLE && p.set_frame == f.serial && p.set_contents == contents) return p.set;
    VkDescriptorSet set = allocate_set(p.layout);
    if (set == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    static std::vector<VkWriteDescriptorSet> writes;
    writes.clear();
    std::size_t bi = 0, ii = 0;
    for (const auto& cb : p.cbuffers) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = cb.slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bufs[bi++];
        writes.push_back(w);
    }
    for (const auto& tx : p.textures) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = kTexBase + tx.slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &images[ii++];
        writes.push_back(w);
    }
    for (std::uint32_t slot : p.samplers) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = kSampBase + slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = &images[ii++];
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(d.dev, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    p.set = set;
    p.set_frame = f.serial;
    p.set_contents = contents;
    return set;
}

void trace_draw(const DrawState& s, const char* kind, std::uint32_t count, const char* result) {
    if (!g_trace) return;
    std::fprintf(stderr, "draw %s n %u vs %016llx ps %016llx rt %06x ds %06x vp %.0f,%.0f %.0fx%.0f: %s\n", kind, count,
                 static_cast<unsigned long long>(s.vs), static_cast<unsigned long long>(s.ps), s.rtv[0], s.dsv, s.viewport[0],
                 s.viewport[1], s.viewport[2], s.viewport[3], result);
}

// Binds everything for a draw; false when it cannot run.
bool prepare(const DrawState& s) {
    Device& d = *g_dev;
    VkPrimitiveTopology topology;
    if (!primitive(s.topology, &topology)) {
        problem("topology " + std::to_string(s.topology));
        return false;
    }
    if (s.vs == 0) {
        problem("draw without a translated vertex shader");
        return false;
    }
    Program& vs = program(s.vs, true);
    if (vs.module == VK_NULL_HANDLE) return false;
    Program* ps = nullptr;
    if (s.ps != 0) {
        ps = &program(s.ps, false);
        if (ps->module == VK_NULL_HANDLE) return false;
    }
    if (s.viewport[2] <= 0 || s.viewport[3] <= 0) return false;
    const Pipeline* pl = pipeline(s, vs, ps, topology);
    if (pl == nullptr) return false;
    make_stand_ins();
    // A texture read here that a pass of this frame rendered: the pass must end
    // first (its writes reach the read through the barrier).
    if (d.rendering) {
        for (unsigned i = 0; i < kSlots; ++i)
            for (std::uint32_t id : {s.vs_srv[i], s.ps_srv[i]})
                if (id != 0 && attached(id) && id != s.dsv) {
                    bool target = false;
                    for (unsigned r = 0; r < kTargets; ++r) target |= s.rtv[r] == id;
                    if (!target) end_pass();
                }
    }
    if (!begin_pass(s.rtv, s.rtv_mip, s.rtv_slice, s.dsv)) {
        problem("draw with no render target");
        return false;
    }
    VkCommandBuffer c = cmd();
    Device::Bound& b = d.bound;
    if (b.pipeline != pl->pipeline) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl->pipeline);
        b.pipeline = pl->pipeline;
    }
    // D3D's viewport, flipped: Vulkan's clip space has y down.
    const float vp[6] = {s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], s.viewport[4], s.viewport[5]};
    if (std::memcmp(vp, b.viewport, sizeof vp) != 0) {
        const VkViewport v{vp[0], vp[1] + vp[3], vp[2], -vp[3], vp[4], vp[5]};
        vkCmdSetViewport(c, 0, 1, &v);
        std::memcpy(b.viewport, vp, sizeof vp);
    }
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
    if (x1 <= x0 || y1 <= y0) return false;
    const int sc[4] = {x0, y0, x1 - x0, y1 - y0};
    if (std::memcmp(sc, b.scissor, sizeof sc) != 0) {
        const VkRect2D r{{sc[0], sc[1]}, {static_cast<std::uint32_t>(sc[2]), static_cast<std::uint32_t>(sc[3])}};
        vkCmdSetScissor(c, 0, 1, &r);
        std::memcpy(b.scissor, sc, sizeof sc);
    }
    const int cull = s.raster.cull == 2 ? VK_CULL_MODE_FRONT_BIT : s.raster.cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    if (cull != b.cull) {
        vkCmdSetCullMode(c, static_cast<VkCullModeFlags>(cull));
        b.cull = cull;
    }
    const int front = s.raster.front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (front != b.front) {
        vkCmdSetFrontFace(c, static_cast<VkFrontFace>(front));
        b.front = front;
    }
    const bool has_depth = dt_bound(s) != nullptr;
    const int test = has_depth && s.depth.enable ? 1 : 0, write = has_depth && s.depth.enable && s.depth.write ? 1 : 0;
    const int op = static_cast<int>(compare(s.depth.func));
    if (test != b.depth_test) vkCmdSetDepthTestEnable(c, static_cast<VkBool32>(test)), b.depth_test = test;
    if (write != b.depth_write) vkCmdSetDepthWriteEnable(c, static_cast<VkBool32>(write)), b.depth_write = write;
    if (op != b.depth_op) vkCmdSetDepthCompareOp(c, static_cast<VkCompareOp>(op)), b.depth_op = op;
    if (std::memcmp(s.blend_factor, b.blend, sizeof b.blend) != 0) {
        vkCmdSetBlendConstants(c, s.blend_factor);
        std::memcpy(b.blend, s.blend_factor, sizeof b.blend);
    }
    VkDescriptorSet sets[2] = {stage_set(vs, s.vs_cb, s.vs_srv, s.vs_srv_mip, s.vs_srv_mips, s.vs_samp),
                               ps ? stage_set(*ps, s.ps_cb, s.ps_srv, s.ps_srv_mip, s.ps_srv_mips, s.ps_samp) : VK_NULL_HANDLE};
    for (std::uint32_t i = 0; i < 2; ++i) {
        if (sets[i] == VK_NULL_HANDLE) continue;
        if (b.sets[i] == sets[i] && b.layout == pl->layout) continue;
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl->layout, i, 1, &sets[i], 0, nullptr);
        b.sets[i] = sets[i];
    }
    b.layout = pl->layout;
    if (s.layout != nullptr) {
        std::uint32_t streams = 0;
        for (const VertexElement& el : *s.layout)
            if (el.slot < kStreams) streams |= 1u << el.slot;
        for (std::uint32_t m = streams; m != 0; m &= m - 1) {
            const unsigned i = static_cast<unsigned>(__builtin_ctz(m));
            auto it = s.vb[i] ? d.buffers.find(s.vb[i]) : d.buffers.end();
            VkBuffer buf = it != d.buffers.end() ? it->second.arena->buf : d.zero_buffer->buf;
            VkDeviceSize off = it != d.buffers.end() ? it->second.off + s.voff[i] : 0;
            if (b.vb[i] == buf && b.vb_off[i] == off) continue;
            vkCmdBindVertexBuffers(c, i, 1, &buf, &off);
            b.vb[i] = buf;
            b.vb_off[i] = off;
        }
    }
    return true;
}

// ---- the window's swapchain ----

bool make_show_pipelines() {
    Device& d = *g_dev;
    if (g_swap.layout == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        vkCreateDescriptorSetLayout(d.dev, &li, nullptr, &g_swap.set_layout);
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &g_swap.set_layout;
        vkCreatePipelineLayout(d.dev, &pli, nullptr, &g_swap.layout);
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(d.dev, &si, nullptr, &g_swap.sampler);
    }
    if (g_swap.show != VK_NULL_HANDLE) return true;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kShowVert;
    mi.pCode = kShowVert;
    vkCreateShaderModule(d.dev, &mi, nullptr, &vs);
    mi.codeSize = sizeof kShowFrag;
    mi.pCode = kShowFrag;
    vkCreateShaderModule(d.dev, &mi, nullptr, &fs);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState a{};
    a.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &a;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &g_swap.format;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = g_swap.layout;
    bool ok = vkCreateGraphicsPipelines(d.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g_swap.show) == VK_SUCCESS;
    // The overlay: the same, blended (premultiplied alpha), alpha kept from the texture.
    a.blendEnable = VK_TRUE;
    a.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    a.colorBlendOp = VK_BLEND_OP_ADD;
    a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    a.alphaBlendOp = VK_BLEND_OP_ADD;
    ok = ok && vkCreateGraphicsPipelines(d.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g_swap.overlay) == VK_SUCCESS;
    if (!ok) problem("show pipeline failed");
    return ok;
}

void destroy_swapchain() {
    Device& d = *g_dev;
    vkDeviceWaitIdle(d.dev);
    for (VkImageView v : g_swap.views) vkDestroyImageView(d.dev, v, nullptr);
    for (VkSemaphore s : g_swap.done) vkDestroySemaphore(d.dev, s, nullptr);
    g_swap.views.clear();
    g_swap.done.clear();
    g_swap.images.clear();
    if (g_swap.swapchain) vkDestroySwapchainKHR(d.dev, g_swap.swapchain, nullptr);
    g_swap.swapchain = VK_NULL_HANDLE;
}

bool make_swapchain() {
    Device& d = *g_dev;
    if (g_swap.surface == VK_NULL_HANDLE) return false;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_swap.window, &pw, &ph);
    if (pw <= 0 || ph <= 0) return false;
    if (g_swap.swapchain != VK_NULL_HANDLE && !g_swap.stale && g_swap.extent.width == static_cast<std::uint32_t>(pw) &&
        g_swap.extent.height == static_cast<std::uint32_t>(ph))
        return true;
    VkSurfaceCapabilitiesKHR caps;
    if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.phys, g_swap.surface, &caps), "surface capabilities")) return false;
    VkSwapchainKHR old = g_swap.swapchain;
    if (old != VK_NULL_HANDLE) {
        // Wait for every frame using the old images.
        while (!d.in_flight.empty()) complete(d.frames[d.in_flight.front()], true);
        vkDeviceWaitIdle(d.dev);
        for (VkImageView v : g_swap.views) vkDestroyImageView(d.dev, v, nullptr);
        for (VkSemaphore s : g_swap.done) vkDestroySemaphore(d.dev, s, nullptr);
        g_swap.views.clear();
        g_swap.done.clear();
    }
    if (g_swap.format == VK_FORMAT_UNDEFINED) {
        std::uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(d.phys, g_swap.surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(d.phys, g_swap.surface, &n, fmts.data());
        g_swap.format = n ? fmts[0].format : VK_FORMAT_B8G8R8A8_UNORM;
        // The game's frame is already sRGB-encoded: a UNORM swapchain passes it through.
        for (const auto& f : fmts)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                g_swap.format = f.format;
                break;
            }
    }
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (const char* v = std::getenv("DCUSA_VSYNC"); v != nullptr && std::strcmp(v, "0") == 0) {
        std::uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(d.phys, g_swap.surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(d.phys, g_swap.surface, &n, modes.data());
        for (VkPresentModeKHR m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR || (m == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR)) mode = m;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) extent = {static_cast<std::uint32_t>(pw), static_cast<std::uint32_t>(ph)};
    if (extent.width == 0 || extent.height == 0) return false;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_swap.surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount != 0) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = g_swap.format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                           : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    if (!check(vkCreateSwapchainKHR(d.dev, &ci, nullptr, &g_swap.swapchain), "vkCreateSwapchainKHR")) {
        g_swap.swapchain = VK_NULL_HANDLE;
        return false;
    }
    if (old != VK_NULL_HANDLE) vkDestroySwapchainKHR(d.dev, old, nullptr);
    g_swap.extent = extent;
    g_swap.stale = false;
    std::uint32_t n = 0;
    vkGetSwapchainImagesKHR(d.dev, g_swap.swapchain, &n, nullptr);
    g_swap.images.resize(n);
    vkGetSwapchainImagesKHR(d.dev, g_swap.swapchain, &n, g_swap.images.data());
    for (VkImage img : g_swap.images) {
        g_swap.views.push_back(make_view(img, VK_IMAGE_VIEW_TYPE_2D, g_swap.format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, false));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vkCreateSemaphore(d.dev, &si, nullptr, &s);
        g_swap.done.push_back(s);
    }
    std::fprintf(stderr, "vulkan: swapchain %ux%u, %u images, %s\n", extent.width, extent.height, n,
                 mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : "no vsync");
    return make_show_pipelines();
}

void image_barrier(VkCommandBuffer c, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src,
                   VkPipelineStageFlags dst, VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = src_access;
    ib.dstAccessMask = dst_access;
    ib.oldLayout = from;
    ib.newLayout = to;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = image;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(c, src, dst, 0, 0, nullptr, 0, nullptr, 1, &ib);
}

// ---- device creation ----

bool load_library() {
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
        if (SDL_Vulkan_GetVkGetInstanceProcAddr() == nullptr) SDL_Vulkan_LoadLibrary(nullptr);
        vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (vkGetInstanceProcAddr != nullptr) return true;
    }
#if defined(__APPLE__)
    const char* names[] = {"libvulkan.1.dylib", "/opt/homebrew/lib/libvulkan.1.dylib", "libMoltenVK.dylib"};
#else
    const char* names[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    for (const char* n : names) {
        if (void* lib = dlopen(n, RTLD_NOW | RTLD_LOCAL)) {
            vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
            if (vkGetInstanceProcAddr != nullptr) return true;
        }
    }
    return false;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

bool create_device() {
    if (!load_library()) {
        std::fprintf(stderr, "vulkan: no Vulkan library\n");
        return false;
    }
#define LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    DCUSA_VK_GLOBAL(LOAD_GLOBAL)
#undef LOAD_GLOBAL
    std::uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
    std::vector<const char*> exts;
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
        Uint32 n = 0;
        if (const char* const* list = SDL_Vulkan_GetInstanceExtensions(&n))
            for (Uint32 i = 0; i < n; ++i) exts.push_back(list[i]);
    }
    std::uint32_t n_ext = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n_ext, nullptr);
    std::vector<VkExtensionProperties> inst_exts(n_ext);
    vkEnumerateInstanceExtensionProperties(nullptr, &n_ext, inst_exts.data());
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (has_extension(inst_exts, "VK_KHR_portability_enumeration")) {
        exts.push_back("VK_KHR_portability_enumeration");
        ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    if (has_extension(inst_exts, "VK_KHR_get_physical_device_properties2")) exts.push_back("VK_KHR_get_physical_device_properties2");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Daytona Championship USA";
    app.pEngineName = "dcusa";
    app.apiVersion = std::min<std::uint32_t>(version, VK_API_VERSION_1_3);
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    std::vector<const char*> layers;
    if (std::getenv("DCUSA_VK_VALIDATION") != nullptr) layers.push_back("VK_LAYER_KHRONOS_validation");
    ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    Device& d = *g_dev;
    if (!check(vkCreateInstance(&ici, nullptr, &d.instance), "vkCreateInstance")) return false;
#define LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(d.instance, #name));
    DCUSA_VK_INSTANCE(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    std::uint32_t n_phys = 0;
    vkEnumeratePhysicalDevices(d.instance, &n_phys, nullptr);
    std::vector<VkPhysicalDevice> phys(n_phys);
    vkEnumeratePhysicalDevices(d.instance, &n_phys, phys.data());
    int best = -1;
    for (std::uint32_t i = 0; i < n_phys; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(phys[i], &p);
        const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (best < 0 || score > best) {
            best = score;
            d.phys = phys[i];
        }
    }
    if (d.phys == VK_NULL_HANDLE) {
        std::fprintf(stderr, "vulkan: no device\n");
        return false;
    }
    vkGetPhysicalDeviceProperties(d.phys, &d.props);
    vkGetPhysicalDeviceMemoryProperties(d.phys, &d.mem_props);
    const std::uint32_t api = std::min(d.props.apiVersion, app.apiVersion);
    std::uint32_t n_fam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &n_fam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n_fam);
    vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &n_fam, fams.data());
    for (std::uint32_t i = 0; i < n_fam; ++i)
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            d.family = i;
            break;
        }
    d.timestamps = fams[d.family].timestampValidBits != 0 && d.props.limits.timestampPeriod > 0;
    d.timestamp_ns = d.props.limits.timestampPeriod;

    std::uint32_t n_dext = 0;
    vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &n_dext, nullptr);
    std::vector<VkExtensionProperties> dexts(n_dext);
    vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &n_dext, dexts.data());
    std::vector<const char*> want;
    if (has_extension(dexts, "VK_KHR_swapchain")) want.push_back("VK_KHR_swapchain");
    if (has_extension(dexts, "VK_KHR_portability_subset")) want.push_back("VK_KHR_portability_subset");
    const bool core13 = api >= VK_API_VERSION_1_3;
    if (!core13) {
        for (const char* e : {"VK_KHR_dynamic_rendering", "VK_EXT_extended_dynamic_state", "VK_KHR_depth_stencil_resolve",
                              "VK_KHR_create_renderpass2", "VK_KHR_maintenance2", "VK_KHR_multiview"}) {
            if (has_extension(dexts, e)) want.push_back(e);
            else if (std::strcmp(e, "VK_KHR_dynamic_rendering") == 0 || std::strcmp(e, "VK_EXT_extended_dynamic_state") == 0) {
                std::fprintf(stderr, "vulkan: %s is missing\n", e);
                return false;
            }
        }
    }
    // Every feature the device has, except robust buffer access (a cost the
    // game does not need): what the translated shaders use is among them.
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeatures fdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (core13) {
        f2.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
    } else {
        f2.pNext = &fdr;
        fdr.pNext = &feds;
        if (api >= VK_API_VERSION_1_2) {
            feds.pNext = &f11;
            f11.pNext = &f12;
        }
    }
    vkGetPhysicalDeviceFeatures2(d.phys, &f2);
    f2.features.robustBufferAccess = VK_FALSE;
    if (core13) {
        f13.robustImageAccess = VK_FALSE;
        if (!f13.dynamicRendering) {
            std::fprintf(stderr, "vulkan: no dynamic rendering\n");
            return false;
        }
    }
    f12.bufferDeviceAddressCaptureReplay = VK_FALSE;
    d.anisotropy = f2.features.samplerAnisotropy;
    d.dual_source = f2.features.dualSrcBlend;
    d.mirror_once = api >= VK_API_VERSION_1_2 ? f12.samplerMirrorClampToEdge : VK_FALSE;
    {
        VkPhysicalDeviceDepthStencilResolveProperties dsr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &dsr;
        if (vkGetPhysicalDeviceProperties2) vkGetPhysicalDeviceProperties2(d.phys, &p2);
        d.depth_resolve = dsr.supportedDepthResolveModes;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = d.family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(want.size());
    dci.ppEnabledExtensionNames = want.data();
    if (!check(vkCreateDevice(d.phys, &dci, nullptr, &d.dev), "vkCreateDevice")) return false;
#define LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(d.dev, #name));
    DCUSA_VK_DEVICE(LOAD_DEVICE)
#undef LOAD_DEVICE
#define LOAD_ALIASED(name, alias)                                                       \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(d.dev, #name));            \
    if (name == nullptr) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(d.dev, alias));
    DCUSA_VK_DEVICE_ALIASED(LOAD_ALIASED)
#undef LOAD_ALIASED
    if (vkCmdBeginRendering == nullptr || vkCmdSetCullMode == nullptr) {
        std::fprintf(stderr, "vulkan: dynamic rendering / extended dynamic state entry points missing\n");
        return false;
    }
    vkGetDeviceQueue(d.dev, d.family, 0, &d.queue);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = d.family;
    vkCreateCommandPool(d.dev, &pi, nullptr, &d.pool);
    for (Frame& f : d.frames) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = d.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(d.dev, &ai, &f.cmd);
        vkAllocateCommandBuffers(d.dev, &ai, &f.pre);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(d.dev, &fi, nullptr, &f.fence);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(d.dev, &si, nullptr, &f.acquired);
        if (d.timestamps) {
            VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpi.queryCount = 2;
            vkCreateQueryPool(d.dev, &qpi, nullptr, &f.queries);
        }
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    vkCreateDescriptorSetLayout(d.dev, &li, nullptr, &d.empty_layout);
    const std::uint32_t major = VK_API_VERSION_MAJOR(d.props.apiVersion), minor = VK_API_VERSION_MINOR(d.props.apiVersion);
    std::fprintf(stderr, "vulkan: %s (Vulkan %u.%u), timestamps %s, dual-source blend %s\n", d.props.deviceName, major, minor,
                 d.timestamps ? "yes" : "no", d.dual_source ? "yes" : "no");
    return true;
}

}  // namespace

// ---- gpu:: ----

bool enabled() {
    if (g_checked) return g_dev != nullptr;
    g_checked = true;
    const char* on = std::getenv("DCUSA_METAL");
    const char* dir = std::getenv("DCUSA_SHADERS");
    if (on == nullptr || std::strcmp(on, "0") == 0 || dir == nullptr) return false;
    g_dev = new Device;
    g_dev->shader_dir = dir;
    if (!create_device()) {
        std::fprintf(stderr, "vulkan: no device; the game runs without graphics\n");
        delete g_dev;
        g_dev = nullptr;
        return false;
    }
    return true;
}

namespace direct {

// The window's surface, made on the caller's thread (the main thread: on a
// Mac the Metal view behind it must be made there).
void prepare_layer(void* window) {
    if (!enabled() || window == nullptr || g_swap.surface != VK_NULL_HANDLE) return;
    g_swap.window = static_cast<SDL_Window*>(window);
    if (!SDL_Vulkan_CreateSurface(g_swap.window, g_dev->instance, nullptr, &g_swap.surface)) {
        std::fprintf(stderr, "vulkan: no surface for the window: %s\n", SDL_GetError());
        g_swap.surface = VK_NULL_HANDLE;
        return;
    }
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_dev->phys, g_dev->family, g_swap.surface, &ok);
    if (!ok) std::fprintf(stderr, "vulkan: the graphics queue cannot present to the window\n");
}

void set_layer(void* window) {
    if (!enabled()) return;
    if (window == nullptr) {
        if (g_swap.swapchain != VK_NULL_HANDLE) destroy_swapchain();
        return;
    }
    prepare_layer(window);
}

void create_texture(std::uint32_t key, const TextureDesc& desc) {
    if (!enabled()) return;
    Device& d = *g_dev;
    if (auto it = d.textures.find(key); it != d.textures.end()) drop_texture(it->second);
    Texture t;
    t.desc = desc;
    t.depth = (desc.bind & 0x40u) != 0;
    if (t.depth) {
        t.format = depth_format(desc.format);
        t.stencil = t.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        t.info = {t.format, 4};
    } else {
        t.info = texture_format(desc.format);
        t.format = t.info.format;
    }
    t.cube = is_cube(desc) && !t.depth;
    if (t.format == VK_FORMAT_UNDEFINED || desc.width == 0 || desc.height == 0) {
        problem("texture format " + std::to_string(desc.format) + " not mapped");
        d.textures[key] = t;
        return;
    }
    // Levels as metal.mm: render targets and generated chains keep theirs; a
    // texture the game fills itself keeps every level it declares.
    const bool chain = desc.mips != 1 && ((desc.misc & 1u) != 0 || (desc.bind & 0x20u) != 0);
    std::uint32_t full = 1;
    for (std::uint32_t w = std::max(desc.width, desc.height); w > 1; w >>= 1) ++full;
    std::uint32_t levels = 1;
    if (chain) levels = desc.mips > 1 ? std::min(full, desc.mips) : full;
    if ((t.cube || !chain) && desc.mips != 1 && (desc.bind & 0x60u) == 0) levels = desc.mips == 0 ? full : std::min(full, desc.mips);
    t.levels = levels;
    t.layers = t.cube ? desc.array : 1;
    t.image = make_image(t, 1, &t.mem);
    if (t.image == VK_NULL_HANDLE) {
        d.textures[key] = t;
        return;
    }
    const bool target = t.depth || (desc.bind & 0x20u) != 0;
    init_image(t, t.image, 1, target);
    if (desc.samples > 1 && (desc.bind & 0x60u) != 0 && !t.cube &&
        (d.props.limits.framebufferColorSampleCounts & desc.samples) != 0) {
        t.samples = desc.samples;
        t.ms = make_image(t, desc.samples, &t.ms_mem);
        if (t.ms != VK_NULL_HANDLE) {
            init_image(t, t.ms, desc.samples, true);
            t.ms_attach = make_view(t.ms, VK_IMAGE_VIEW_TYPE_2D, t.format, aspect(t, false), 0, 1, 0, 1, false);
            if (!t.depth) t.ms_sampled = make_view(t.ms, VK_IMAGE_VIEW_TYPE_2D, t.format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, false);
        } else {
            t.samples = 1;
        }
    }
    t.valid = true;
    ++d.stats.textures;
    d.textures[key] = std::move(t);
}

namespace {
// A copy of guest texels into one subresource region, recorded in order.
void copy_in(Texture& t, std::uint32_t layer, std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t w,
             std::uint32_t h, const std::uint8_t* bytes, std::size_t size, std::uint32_t row_pitch) {
    Device& d = *g_dev;
    const std::uint32_t rows = t.info.block ? (h + 3) / 4 : h;
    if (row_pitch == 0 || static_cast<std::size_t>(row_pitch) * rows > size || t.info.bytes == 0) return;
    BufferRef staging = staging_alloc(size);
    if (staging.arena == nullptr) return;
    std::memcpy(staging.contents(), bytes, size);
    end_pass();
    barrier();
    VkBufferImageCopy r{};
    r.bufferOffset = staging.off;
    r.bufferRowLength = t.info.block ? row_pitch / t.info.bytes * 4 : row_pitch / t.info.bytes;
    r.bufferImageHeight = 0;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
    r.imageOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0};
    r.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd(), staging.arena->buf, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    d.need_barrier = true;
    ++d.stats.uploads;
}
}  // namespace

void upload_texture_sub(std::uint32_t key, std::uint32_t slice, std::uint32_t mip, const std::uint8_t* bytes, std::size_t size,
                        std::uint32_t row_pitch) {
    if (!enabled()) return;
    Texture* t = texture(key);
    if (t == nullptr || t->depth || bytes == nullptr || mip >= t->levels || slice >= t->layers) return;
    const std::uint32_t w = std::max(1u, t->desc.width >> mip), h = std::max(1u, t->desc.height >> mip);
    copy_in(*t, slice, mip, 0, 0, w, h, bytes, size, row_pitch);
}

void upload_texture(std::uint32_t key, const std::uint8_t* bytes, std::size_t size, std::uint32_t row_pitch) {
    upload_texture_sub(key, 0, 0, bytes, size, row_pitch);
}

void upload_texture_region(std::uint32_t key, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h,
                           const std::uint8_t* bytes, std::size_t size, std::uint32_t row_pitch) {
    if (!enabled()) return;
    Texture* t = texture(key);
    if (t == nullptr || t->depth || bytes == nullptr || w == 0 || h == 0) return;
    if (x + w > t->desc.width || y + h > t->desc.height) return;
    copy_in(*t, 0, 0, x, y, w, h, bytes, size, row_pitch);
}

void generate_mips(std::uint32_t key) {
    if (!enabled()) return;
    Device& d = *g_dev;
    Texture* t = texture(key);
    if (t == nullptr || t->levels < 2 || t->info.block || t->depth) return;
    end_pass();
    for (std::uint32_t level = 1; level < t->levels; ++level) {
        d.need_barrier = true;
        barrier();
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, t->layers};
        b.srcOffsets[1] = {static_cast<std::int32_t>(std::max(1u, t->desc.width >> (level - 1))),
                           static_cast<std::int32_t>(std::max(1u, t->desc.height >> (level - 1))), 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, t->layers};
        b.dstOffsets[1] = {static_cast<std::int32_t>(std::max(1u, t->desc.width >> level)),
                           static_cast<std::int32_t>(std::max(1u, t->desc.height >> level)), 1};
        vkCmdBlitImage(cmd(), t->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
    }
    d.need_barrier = true;
}

void upload_buffer(std::uint32_t key, const std::uint8_t* bytes, std::size_t size) {
    if (!enabled() || bytes == nullptr || size == 0) return;
    Device& d = *g_dev;
    const bool first = d.buffers.find(key) == d.buffers.end();
    BufferRef r;
    if (size > kArenaSize / 8) {
        Arena* a = new_arena(size, true);
        if (a == nullptr) return;
        r = {a, 0, size};
    } else if (first) {
        r = static_alloc(size);
    } else {
        r = arena_alloc(size);
    }
    std::memcpy(r.contents(), bytes, size);
    set_buffer(key, r);
    ++d.stats.uploads;
}

void create_sampler(std::uint32_t key, const SamplerDesc& desc) {
    if (!enabled()) return;
    Device& d = *g_dev;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const std::uint32_t f = desc.filter & 0x7Fu;
    si.mipmapMode = (f & 0x1u) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.magFilter = (f & 0x4u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = (f & 0x10u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    if ((f & 0x40u) != 0 && d.anisotropy) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min<float>(d.props.limits.maxSamplerAnisotropy,
                                           static_cast<float>(std::max<std::uint32_t>(1, std::min<std::uint32_t>(16, desc.max_aniso))));
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }
    si.addressModeU = address(desc.address[0], d.mirror_once);
    si.addressModeV = address(desc.address[1], d.mirror_once);
    si.addressModeW = address(desc.address[2], d.mirror_once);
    si.mipLodBias = desc.mip_bias;
    si.minLod = std::max(0.0f, desc.min_lod);
    si.maxLod = desc.max_lod > 1000.0f ? VK_LOD_CLAMP_NONE : desc.max_lod;
    if ((desc.filter & 0x80u) != 0) {
        si.compareEnable = VK_TRUE;
        si.compareOp = compare(desc.compare);
    }
    const bool white = desc.border[0] > 0.5f && desc.border[3] > 0.5f;
    si.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                           : desc.border[3] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    if (!check(vkCreateSampler(d.dev, &si, nullptr, &s), "vkCreateSampler")) return;
    if (auto it = d.samplers.find(key); it != d.samplers.end()) d.garbage.samplers.push_back(it->second);
    d.samplers[key] = s;
}

void destroy(std::uint32_t key) {
    if (!enabled()) return;
    Device& d = *g_dev;
    if (auto it = d.textures.find(key); it != d.textures.end()) {
        drop_texture(it->second);
        d.textures.erase(it);
    }
    set_buffer(key, {});
    if (auto it = d.samplers.find(key); it != d.samplers.end()) {
        d.garbage.samplers.push_back(it->second);
        d.samplers.erase(it);
    }
}

void clear_target(std::uint32_t key, std::uint32_t mip, std::uint32_t slice, const float rgba[4]) {
    if (!enabled()) return;
    Device& d = *g_dev;
    Texture* t = texture(key);
    if (g_trace) std::fprintf(stderr, "clear %06x %g %g %g %g\n", key, rgba[0], rgba[1], rgba[2], rgba[3]);
    if (t == nullptr || t->depth) return;
    end_pass();
    barrier();
    VkClearColorValue v{};
    std::memcpy(v.float32, rgba, sizeof v.float32);
    const std::uint32_t level = std::min(mip, t->levels - 1);
    const std::uint32_t layer = t->cube ? std::min(slice, t->layers - 1) : 0;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, level, 1, layer, 1};
    vkCmdClearColorImage(cmd(), t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    if (t->ms != VK_NULL_HANDLE && level == 0) {
        const VkImageSubresourceRange r0{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd(), t->ms, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r0);
    }
    d.need_barrier = true;
    ++d.stats.clears;
}

void clear_depth(std::uint32_t key, float depth, std::uint8_t stencil, std::uint32_t flags) {
    if (!enabled()) return;
    Device& d = *g_dev;
    Texture* t = texture(key);
    if (t == nullptr || !t->depth) return;
    VkImageAspectFlags asp = 0;
    if (flags & 1u) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((flags & 2u) && t->stencil) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (asp == 0) return;
    end_pass();
    barrier();
    const VkClearDepthStencilValue v{depth, stencil};
    const VkImageSubresourceRange range{asp, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd(), t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    if (t->ms != VK_NULL_HANDLE) vkCmdClearDepthStencilImage(cmd(), t->ms, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    d.need_barrier = true;
    ++d.stats.clears;
}

void draw_indexed(const DrawState& s, std::uint32_t count, std::uint32_t start, std::int32_t base) {
    if (!enabled() || count == 0) return;
    Device& d = *g_dev;
    auto ib = d.buffers.find(s.ib);
    if (ib == d.buffers.end()) {
        trace_draw(s, "indexed", count, "no index buffer");
        ++d.stats.skipped;
        return;
    }
    const bool wide = s.ib_format == 42;
    const VkDeviceSize size = wide ? 4 : 2;
    if (s.ib_off + (static_cast<VkDeviceSize>(start) + count) * size > ib->second.len) {
        ++d.stats.skipped;
        problem("index range outside the index buffer");
        return;
    }
    if (!prepare(s)) {
        trace_draw(s, "indexed", count, "skipped");
        ++d.stats.skipped;
        return;
    }
    VkCommandBuffer c = cmd();
    Device::Bound& b = d.bound;
    const VkDeviceSize off = ib->second.off + s.ib_off;
    const VkIndexType type = wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (b.ib != ib->second.arena->buf || b.ib_off != off || b.ib_type != type) {
        vkCmdBindIndexBuffer(c, ib->second.arena->buf, off, type);
        b.ib = ib->second.arena->buf;
        b.ib_off = off;
        b.ib_type = type;
    }
    vkCmdDrawIndexed(c, count, 1, start, base, 0);
    trace_draw(s, "indexed", count, "drawn");
    ++d.stats.draws;
}

void draw(const DrawState& s, std::uint32_t count, std::uint32_t start) {
    if (!enabled() || count == 0) return;
    if (!prepare(s)) {
        ++g_dev->stats.skipped;
        return;
    }
    vkCmdDraw(cmd(), count, 1, start, 0);
    trace_draw(s, "plain", count, "drawn");
    ++g_dev->stats.draws;
}

void antialias(std::uint32_t) {}  // FXAA: Metal only

void show(std::uint32_t key) {
    if (!enabled() || g_swap.surface == VK_NULL_HANDLE) return;
    Device& d = *g_dev;
    Texture* t = texture(key);
    if (t == nullptr || t->depth || !make_swapchain()) return;
    Frame& f = frame();
    if (f.present_image >= 0) return;  // one picture a frame
    std::uint32_t index = 0;
    const auto a0 = std::chrono::steady_clock::now();
    const VkResult r = vkAcquireNextImageKHR(d.dev, g_swap.swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
    g_rt_drawable_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a0).count();
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_swap.stale = true;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        check(r, "vkAcquireNextImageKHR");
        return;
    }
    if (r == VK_SUBOPTIMAL_KHR) g_swap.stale = true;
    end_pass();
    barrier();
    VkCommandBuffer c = cmd();
    image_barrier(c, g_swap.images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = g_swap.views[index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{0, 0, 0, 1}};
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, g_swap.extent};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &ca;
    vkCmdBeginRendering(c, &info);
    const double pw = g_swap.extent.width, ph = g_swap.extent.height;
    const double fw = t->desc.width, fh = t->desc.height;
    const double sc = std::min(pw / fw, ph / fh);
    const double w = fw * sc, h = fh * sc;
    const VkRect2D scissor{{0, 0}, g_swap.extent};
    vkCmdSetScissor(c, 0, 1, &scissor);
    auto draw_quad = [&](VkPipeline p, VkImageView view, double x, double y, double qw, double qh) {
        const VkViewport v{static_cast<float>(x), static_cast<float>(y), static_cast<float>(qw), static_cast<float>(qh), 0, 1};
        vkCmdSetViewport(c, 0, 1, &v);
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        VkDescriptorSet set = allocate_set(g_swap.set_layout);
        VkDescriptorImageInfo ii{g_swap.sampler, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = set;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &ii;
        vkUpdateDescriptorSets(d.dev, 1, &wr, 0, nullptr);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_swap.layout, 0, 1, &set, 0, nullptr);
        vkCmdDraw(c, 3, 1, 0, 0);
    };
    draw_quad(g_swap.show, sampled_view(*t, 4, 0, 1), (pw - w) / 2, (ph - h) / 2, w, h);
    if (const std::uint32_t ok = g_overlay_key.load(std::memory_order_relaxed); ok != 0) {
        if (Texture* o = texture(ok); o != nullptr && o->desc.height != 0) {
            const double oh = h * 0.045, ow = oh * o->desc.width / o->desc.height;
            const double ox = (pw - w) / 2 + h * 0.015, oy = (ph + h) / 2 - oh - h * 0.015;
            draw_quad(g_swap.overlay, sampled_view(*o, 4, 0, 1), ox, oy, ow, oh);
        }
    }
    vkCmdEndRendering(c);
    image_barrier(c, g_swap.images[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
    d.bound = Device::Bound{};
    f.present_image = static_cast<int>(index);
}

void submit() {
    if (!enabled()) return;
    commit(false);
}

bool read_rgba8(std::uint32_t key, std::vector<std::uint8_t>* out, std::uint32_t* width, std::uint32_t* height) {
    if (!enabled()) return false;
    Device& d = *g_dev;
    Texture* t = texture(key);
    if (t == nullptr || (t->format != VK_FORMAT_R8G8B8A8_UNORM && t->format != VK_FORMAT_B8G8R8A8_UNORM &&
                         t->format != VK_FORMAT_R8G8B8A8_SRGB && t->format != VK_FORMAT_B8G8R8A8_SRGB))
        return false;
    const std::uint32_t w = t->desc.width, h = t->desc.height;
    Arena* buf = new_arena(static_cast<VkDeviceSize>(w) * h * 4, true);
    if (buf == nullptr) return false;
    end_pass();
    barrier();
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd(), t->image, VK_IMAGE_LAYOUT_GENERAL, buf->buf, 1, &r);
    d.need_barrier = true;
    commit(true);
    out->assign(buf->ptr, buf->ptr + static_cast<std::size_t>(w) * h * 4);
    free_arena(buf);
    if (t->format == VK_FORMAT_B8G8R8A8_UNORM || t->format == VK_FORMAT_B8G8R8A8_SRGB)
        for (std::size_t i = 0; i + 3 < out->size(); i += 4) std::swap((*out)[i], (*out)[i + 2]);
    *width = w;
    *height = h;
    return true;
}

void scan_targets() {
    if (!enabled()) return;
    std::fprintf(stderr, "vulkan: DCUSA_METAL_SCAN is not available on Vulkan\n");
}

void set_trace(bool on) { g_trace = on; }

const Stats& stats() {
    static Stats none;
    if (g_dev == nullptr) return none;
    g_dev->stats.gpu_seconds = static_cast<double>(g_gpu_us.load()) / 1e6;
    g_dev->stats.gpu_buffers = g_gpu_buffers.load();
    return g_dev->stats;
}

}  // namespace direct

std::uint64_t gpu_busy_us() { return g_gpu_us.load(std::memory_order_relaxed); }

bool frame_gpu_times(std::uint64_t frame, std::uint64_t* start_us, std::uint64_t* end_us) {
    const FrameTimes& f = g_frame_times[frame % kFrameTimes];
    if (f.frame.load(std::memory_order_acquire) != frame) return false;
    *start_us = f.start_us.load(std::memory_order_relaxed);
    *end_us = f.end_us.load(std::memory_order_relaxed);
    return true;
}

std::string memory_report() {
    if (g_dev == nullptr) return {};
    Device& d = *g_dev;
    double arenas = 0;
    for (Arena* a : d.all_arenas) arenas += static_cast<double>(a->size) / 1048576.0;
    char line[256];
    std::snprintf(line, sizeof line, "textures %zu, buffers %zu, arenas %zu (%.0f MB), pipelines %zu", d.textures.size(),
                  d.buffers.size(), d.all_arenas.size(), arenas, d.pipelines.size());
    return line;
}

void rt_waits(double* gpu_ms, double* drawable_ms) {
    *gpu_ms = g_rt_gpu_wait_ms;
    *drawable_ms = g_rt_drawable_ms;
}

void set_overlay(std::uint32_t texture) { g_overlay_key.store(texture, std::memory_order_relaxed); }

}  // namespace dcusa::gpu

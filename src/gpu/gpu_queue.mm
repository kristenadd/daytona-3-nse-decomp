// gpu:: (metal.hpp) on a render thread.
//
// Encoding a draw for Metal (pipeline and state lookups, binding, the driver's
// own work) was about a fifth of the guest's main thread. The guest's calls are
// recorded here instead, with copies of everything they point at, and the
// render thread replays them through gpu::direct in the same order. A frame's
// calls go over at submit(), so the render thread encodes frame N while the
// guest runs frame N + 1; submit() waits until frame N - 1 is encoded, so the
// guest is never more than one frame ahead of the encoder.
//
// Calls that return something (read_rgba8, scan_targets, stats) hand over what
// is recorded, wait until the render thread is idle, and then run directly.
// DCUSA_GPU_THREAD=0 runs every call directly on the caller's thread.
#include "gpu/metal.hpp"
#include "gpu/metal_direct.hpp"

#import <Foundation/Foundation.h>

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace dcusa::gpu {
namespace {

enum class Op : std::uint8_t {
    CreateTexture, UploadTexture, GenerateMips, UploadBuffer, CreateSampler, Destroy, ClearTarget, ClearDepth,
    DrawIndexed, Draw, Submit, SetLayer, Show, SetTrace,
};

struct Cmd {
    Op op;
    std::uint32_t id = 0, a = 0, b = 0;
    std::int32_t base = 0;
    std::size_t index = 0, size = 0;  // into the batch's bytes, states, textures or samplers
    float f[4] = {};
    void* ptr = nullptr;
};

struct Batch {
    std::vector<Cmd> cmds;
    std::vector<std::uint8_t> bytes;
    std::vector<DrawState> states;  // kept between frames so the copies reuse their storage
    std::size_t used_states = 0;
    std::vector<TextureDesc> textures;
    std::vector<SamplerDesc> samplers;

    void clear() {
        cmds.clear();
        bytes.clear();
        used_states = 0;
        textures.clear();
        samplers.clear();
    }
    std::size_t keep_bytes(const std::uint8_t* p, std::size_t n) {
        const std::size_t at = bytes.size();
        bytes.insert(bytes.end(), p, p + n);
        return at;
    }
    std::size_t keep_state(const DrawState& s) {
        if (used_states < states.size()) states[used_states] = s;
        else states.push_back(s);
        return used_states++;
    }
};

void replay(const Batch& b) {
    @autoreleasepool {
        for (const Cmd& c : b.cmds) {
            switch (c.op) {
            case Op::CreateTexture: direct::create_texture(c.id, b.textures[c.index]); break;
            case Op::UploadTexture: direct::upload_texture(c.id, b.bytes.data() + c.index, c.size, c.a); break;
            case Op::GenerateMips: direct::generate_mips(c.id); break;
            case Op::UploadBuffer: direct::upload_buffer(c.id, b.bytes.data() + c.index, c.size); break;
            case Op::CreateSampler: direct::create_sampler(c.id, b.samplers[c.index]); break;
            case Op::Destroy: direct::destroy(c.id); break;
            case Op::ClearTarget: direct::clear_target(c.id, c.a, c.f); break;
            case Op::ClearDepth: direct::clear_depth(c.id, c.f[0], static_cast<std::uint8_t>(c.b), c.a); break;
            case Op::DrawIndexed: direct::draw_indexed(b.states[c.index], c.a, c.b, c.base); break;
            case Op::Draw: direct::draw(b.states[c.index], c.a, c.b); break;
            case Op::Submit: direct::submit(); break;
            case Op::SetLayer: direct::set_layer(c.ptr); break;
            case Op::Show: direct::show(c.id); break;
            case Op::SetTrace: direct::set_trace(c.a != 0); break;
            }
        }
    }
}

struct Queue {
    std::mutex mu;
    std::condition_variable cv;
    Batch batches[2];
    Batch* filling = &batches[0];  // the guest's
    Batch* pending = nullptr;      // handed to the render thread, until it has replayed it
    bool started = false;
};

Queue& Q() {
    static Queue* q = new Queue;  // never destroyed: the render thread outlives main
    return *q;
}

bool threaded() {
    static const bool on = [] {
        const char* v = std::getenv("DCUSA_GPU_THREAD");
        return enabled() && !(v != nullptr && std::strcmp(v, "0") == 0);
    }();
    return on;
}

void render_thread() {
    Queue& q = Q();
    for (;;) {
        Batch* b;
        {
            std::unique_lock<std::mutex> lk(q.mu);
            q.cv.wait(lk, [&] { return q.pending != nullptr; });
            b = q.pending;
        }
        replay(*b);
        b->clear();
        {
            std::lock_guard<std::mutex> lk(q.mu);
            q.pending = nullptr;
        }
        q.cv.notify_all();
    }
}

// Waits for the render thread to finish the previous batch, then gives it this one.
void hand_over() {
    Queue& q = Q();
    std::unique_lock<std::mutex> lk(q.mu);
    if (!q.started) {
        q.started = true;
        std::thread(render_thread).detach();
    }
    q.cv.wait(lk, [&] { return q.pending == nullptr; });
    if (q.filling->cmds.empty()) return;
    q.pending = q.filling;
    q.filling = q.filling == &q.batches[0] ? &q.batches[1] : &q.batches[0];
    lk.unlock();
    q.cv.notify_all();
}

// Everything recorded has been replayed and the render thread is idle.
void drain() {
    if (!threaded()) return;
    hand_over();
    Queue& q = Q();
    std::unique_lock<std::mutex> lk(q.mu);
    q.cv.wait(lk, [&] { return q.pending == nullptr; });
}

Cmd& record(Op op) {
    Batch& b = *Q().filling;
    b.cmds.push_back(Cmd{op});
    return b.cmds.back();
}

}  // namespace

void create_texture(std::uint32_t id, const TextureDesc& desc) {
    if (!threaded()) return direct::create_texture(id, desc);
    Batch& b = *Q().filling;
    Cmd& c = record(Op::CreateTexture);
    c.id = id;
    c.index = b.textures.size();
    b.textures.push_back(desc);
}

void upload_texture(std::uint32_t id, const std::uint8_t* bytes, std::size_t size, std::uint32_t row_pitch) {
    if (!threaded()) return direct::upload_texture(id, bytes, size, row_pitch);
    if (bytes == nullptr || size == 0) return;
    const std::size_t at = Q().filling->keep_bytes(bytes, size);
    Cmd& c = record(Op::UploadTexture);
    c.id = id;
    c.index = at;
    c.size = size;
    c.a = row_pitch;
}

void generate_mips(std::uint32_t id) {
    if (!threaded()) return direct::generate_mips(id);
    record(Op::GenerateMips).id = id;
}

void upload_buffer(std::uint32_t id, const std::uint8_t* bytes, std::size_t size) {
    if (!threaded()) return direct::upload_buffer(id, bytes, size);
    if (bytes == nullptr || size == 0) return;
    const std::size_t at = Q().filling->keep_bytes(bytes, size);
    Cmd& c = record(Op::UploadBuffer);
    c.id = id;
    c.index = at;
    c.size = size;
}

void create_sampler(std::uint32_t id, const SamplerDesc& desc) {
    if (!threaded()) return direct::create_sampler(id, desc);
    Batch& b = *Q().filling;
    Cmd& c = record(Op::CreateSampler);
    c.id = id;
    c.index = b.samplers.size();
    b.samplers.push_back(desc);
}

void destroy(std::uint32_t id) {
    if (!threaded()) return direct::destroy(id);
    record(Op::Destroy).id = id;
}

void clear_target(std::uint32_t texture, std::uint32_t mip, const float rgba[4]) {
    if (!threaded()) return direct::clear_target(texture, mip, rgba);
    Cmd& c = record(Op::ClearTarget);
    c.id = texture;
    c.a = mip;
    std::memcpy(c.f, rgba, sizeof c.f);
}

void clear_depth(std::uint32_t texture, float depth, std::uint8_t stencil, std::uint32_t flags) {
    if (!threaded()) return direct::clear_depth(texture, depth, stencil, flags);
    Cmd& c = record(Op::ClearDepth);
    c.id = texture;
    c.f[0] = depth;
    c.b = stencil;
    c.a = flags;
}

void draw_indexed(const DrawState& s, std::uint32_t count, std::uint32_t start, std::int32_t base) {
    if (!threaded()) return direct::draw_indexed(s, count, start, base);
    const std::size_t at = Q().filling->keep_state(s);
    Cmd& c = record(Op::DrawIndexed);
    c.index = at;
    c.a = count;
    c.b = start;
    c.base = base;
}

void draw(const DrawState& s, std::uint32_t count, std::uint32_t start) {
    if (!threaded()) return direct::draw(s, count, start);
    const std::size_t at = Q().filling->keep_state(s);
    Cmd& c = record(Op::Draw);
    c.index = at;
    c.a = count;
    c.b = start;
}

void submit() {
    if (!threaded()) return direct::submit();
    record(Op::Submit);
    hand_over();
}

void set_layer(void* ca_metal_layer) {
    if (!threaded()) return direct::set_layer(ca_metal_layer);
    record(Op::SetLayer).ptr = ca_metal_layer;
}

void show(std::uint32_t texture) {
    if (!threaded()) return direct::show(texture);
    record(Op::Show).id = texture;
}

void set_trace(bool on) {
    if (!threaded()) return direct::set_trace(on);
    record(Op::SetTrace).a = on ? 1u : 0u;
}

bool read_rgba8(std::uint32_t texture, std::vector<std::uint8_t>* out, std::uint32_t* width, std::uint32_t* height) {
    drain();
    return direct::read_rgba8(texture, out, width, height);
}

void scan_targets() {
    drain();
    direct::scan_targets();
}

const Stats& stats() {
    drain();
    return direct::stats();
}

}  // namespace dcusa::gpu

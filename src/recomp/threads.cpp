#include "recomp/threads.hpp"

#include <pthread.h>
#include <TargetConditionals.h>
#include <sys/mman.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "recomp/dispatch.hpp"
#include "recomp/rt_lock.hpp"
#include "recomp/guest.hpp"
#include "recomp/guest_memory.hpp"
#include "recomp/x86ops.hpp"
#include "runtime/fail_closed.hpp"
#include "runtime/hitch.hpp"

namespace dcusa::recomp {

// ---- the runtime lock (rt_lock.hpp) ----
namespace {
std::mutex g_rt;
thread_local int t_rt_depth = 0;
}  // namespace

bool rt_parallel() {
    static const bool on = [] {
        const char* v = std::getenv("DCUSA_PARALLEL_THREADS");
        return v != nullptr && *v != 0;
    }();
    return on;
}

void rt_lock() {
    if (!rt_parallel()) return;
    if (t_rt_depth++ == 0) g_rt.lock();
}

void rt_unlock() {
    if (!rt_parallel()) return;
    if (--t_rt_depth == 0) g_rt.unlock();
}

int rt_release_all() {
    if (!rt_parallel()) return 0;
    const int depth = t_rt_depth;
    if (depth > 0) {
        t_rt_depth = 0;
        g_rt.unlock();
    }
    return depth;
}

void rt_reacquire(int depth) {
    if (depth <= 0) return;
    g_rt.lock();
    t_rt_depth = depth;
}

namespace {

using Clock = std::chrono::steady_clock;

// The slot of a parallel ("free") guest thread on this host thread, -1 on the
// others (they are S().current while they run).
thread_local int t_free_slot = -1;

// Preemption: a host timer raises g_preempt every kQuantum while a second
// guest thread exists; translated loops yield when they see it. Without it, a
// loop that waits for another thread without a blocking call (the boot's
// resource walk under f_004FDF60) runs forever and the other threads starve.
constexpr auto kQuantum = std::chrono::milliseconds(2);
std::atomic<unsigned> g_live_threads{1};

void start_preempt_timer() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            // A low-QoS timer has its sleeps coalesced (iOS): preemption would come late.
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            for (;;) {
                std::this_thread::sleep_for(kQuantum);
                if (g_live_threads.load(std::memory_order_relaxed) > 1)
                    g_preempt.store(true, std::memory_order_relaxed);
            }
        }).detach();
    });
}


// Slot 0 is the thread that runs the entry point. Its TEB and stack are the
// loader's. Slots 1-15 have their TEBs in the pages after it (below the PEB at
// 0x20000) and 1 MB stacks below the main stack, above the VirtualAlloc region.
// Slots 16-31 have TEBs from 0x30000 and stacks above the second heap.
constexpr unsigned kSlots = 32;
constexpr std::uint32_t kTebBase = 0x10000u;
constexpr std::uint32_t kTebHigh = 0x30000u;
constexpr std::uint32_t kTebStride = 0x1000u;
constexpr std::uint32_t kStackTop = 0x2FF00000u;
constexpr std::uint32_t kStackHigh = 0x3C000000u;
constexpr std::uint32_t kStackBytes = 0x100000u;

std::uint32_t teb_of(unsigned slot) {
    return slot < 16 ? kTebBase + kTebStride * slot : kTebHigh + kTebStride * (slot - 16);
}

std::uint32_t stack_top_of(unsigned slot) {
    return slot < 16 ? kStackTop - kStackBytes * (slot - 1) : kStackHigh + kStackBytes * (slot - 15);
}
constexpr std::uint32_t kPeb = 0x20000u;
constexpr std::uint32_t kStaticTls = 0x21000u;
// Translated code recurses deeply and keeps guest registers in host locals.
constexpr std::size_t kHostStack = 64u << 20;
// Sleep is shortened: the run is headless and nothing it waits for is real time.
constexpr std::uint32_t kSleepCap = 15;

enum class Kind { Free, Thread, Event, Semaphore, Mutex };

struct Object {
    Kind kind = Kind::Free;
    // Event
    bool manual = false;
    bool signaled = false;
    // Semaphore
    std::uint32_t count = 0;
    std::uint32_t maximum = 0;
    // Mutex: owning thread id and recursion count
    std::uint32_t owner = 0;
    std::uint32_t recursion = 0;
    // Thread
    int slot = -1;
    bool finished = false;
    std::uint32_t exit_code = kStillActive;
};

struct Slot {
    bool used = false;
    // Runs in parallel with the others (DCUSA_PARALLEL_THREADS): never takes a
    // turn, waits by polling, enters the runtime under the runtime lock.
    bool free = false;
    bool started = false;
    bool terminate = false;
    bool waiting_forever = false;
    // Parked: blocked in a wait that cannot have ended (see pick_after).
    bool parked = false;
    std::uint64_t parked_version = 0;
    Clock::time_point wake_at;
    std::uint32_t suspend = 0;
    std::uint32_t id = 0;
    std::uint32_t handle = 0;
    std::uint32_t start = 0;
    std::uint32_t param = 0;
    std::uint32_t terminate_code = 0;
    Cpu* cpu = nullptr;
    std::unique_ptr<Cpu> own;
};

struct Timer {
    std::uint32_t id = 0;
    std::uint32_t event = 0;
    std::uint32_t period = 0;
    bool periodic = false;
    Clock::time_point due;
};

struct Sched {
    std::mutex mu;
    // One per slot: a thread waits on its own until `current` names it, and a
    // handover wakes only that one. A single shared one woke every parked host
    // thread (a dozen) at each switch, to look and sleep again: on an iPhone
    // three of them took a core-fifth each, and every switch paid the herd.
    std::condition_variable wake[kSlots];
    Slot slots[kSlots];
    std::vector<Object> objects;
    std::vector<Timer> timers;
    unsigned current = 0;
    unsigned idle = 0;
    std::uint32_t next_id = 2;
    std::uint32_t next_timer = 1;
    std::exception_ptr failure;
    bool ready = false;
};

// Bumped by everything that can end a wait: an event set, a semaphore or mutex
// released, a critical section left, a timer fired, a thread finished, resumed
// or terminated, a handle closed. A parked thread is passed over until it
// changes or the thread's timeout comes.
std::uint64_t g_version = 0;
// Even then a parked thread gets a turn this often, so a release that does not
// bump g_version delays the waiter instead of deadlocking it.
constexpr auto kParkedTurn = std::chrono::milliseconds(20);

// Never destroyed: host threads parked on the condition variable outlive main.
Sched& S() {
    static Sched* s = new Sched;
    return *s;
}

bool stats() { return std::getenv("DCUSA_STATS") != nullptr; }

Object* object(std::uint32_t handle) {
    Sched& s = S();
    if (handle < kSyncHandleBase) return nullptr;
    const std::uint32_t i = handle - kSyncHandleBase;
    if (i >= s.objects.size() || s.objects[i].kind == Kind::Free) return nullptr;
    return &s.objects[i];
}

std::uint32_t new_object(const Object& o) {
    Sched& s = S();
    for (std::size_t i = 0; i < s.objects.size(); ++i) {
        if (s.objects[i].kind == Kind::Free) {
            s.objects[i] = o;
            return kSyncHandleBase + static_cast<std::uint32_t>(i);
        }
    }
    s.objects.push_back(o);
    return kSyncHandleBase + static_cast<std::uint32_t>(s.objects.size() - 1);
}

void ensure(Cpu& c) {
    if (!S().ready) sched_reset(c);
}

// The calling guest thread's slot.
unsigned me_slot() { return t_free_slot >= 0 ? static_cast<unsigned>(t_free_slot) : S().current; }

std::uint32_t self_id() { return S().slots[me_slot()].id; }

bool runnable(unsigned i) {
    const Slot& t = S().slots[i];
    return t.used && t.suspend == 0 && !t.free;
}

unsigned live_count() {
    unsigned n = 0;
    for (unsigned i = 0; i < kSlots; ++i)
        if (runnable(i)) ++n;
    return n;
}

void fire_timers() {
    Sched& s = S();
    if (s.timers.empty()) return;
    const auto now = Clock::now();
    for (std::size_t i = 0; i < s.timers.size();) {
        Timer& t = s.timers[i];
        if (t.due > now) {
            ++i;
            continue;
        }
        if (Object* e = object(t.event); e && e->kind == Kind::Event) {
            e->signaled = true;
            ++g_version;
        }
        if (!t.periodic) {
            s.timers.erase(s.timers.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        t.due += std::chrono::milliseconds(t.period == 0 ? 1 : t.period);
        if (t.due < now) t.due = now + std::chrono::milliseconds(t.period == 0 ? 1 : t.period);
        ++i;
    }
}

// Runs on the thread that just got the CPU back.
void resumed(unsigned me) {
    Sched& s = S();
    if (s.failure) {
        if (me == 0) {
            std::exception_ptr f = s.failure;
            s.failure = nullptr;
            std::rethrow_exception(f);
        }
        // Hand the CPU to the main thread, which reports the failure, and park.
        std::unique_lock<std::mutex> lk(s.mu);
        s.current = 0;
        s.wake[0].notify_one();
        s.wake[me].wait(lk, [] { return false; });
    }
    Slot& t = s.slots[me];
    if (t.terminate && me != 0) throw GuestThreadExit{t.terminate_code};
}

void switch_to(unsigned me, unsigned next) {
    Sched& s = S();
    if (next != me) {
        RtRelease released;  // the next thread may enter the runtime
        std::unique_lock<std::mutex> lk(s.mu);
        s.current = next;
        s.wake[next].notify_one();
        s.wake[me].wait(lk, [&] { return s.current == me; });
    }
    resumed(me);
}

// The next thread to run after `me`. Threads parked in a wait nothing has
// touched are passed over: preemption at loop back-edges (every 2 ms) used to
// wake each of the game's dozen sleeping threads in turn, about a tenth of the
// main thread in a race.
unsigned pick_after(unsigned me) {
    const auto now = Clock::now();
    for (unsigned k = 1; k <= kSlots; ++k) {
        const unsigned i = (me + k) % kSlots;
        if (!runnable(i)) continue;
        const Slot& t = S().slots[i];
        if (i != me && t.parked && t.parked_version == g_version && now < t.wake_at) continue;
        return i;
    }
    if (runnable(me)) return me;
    for (unsigned k = 1; k <= kSlots; ++k) {  // `me` cannot go on: any runnable thread
        const unsigned i = (me + k) % kSlots;
        if (runnable(i)) return i;
    }
    return me;
}

std::string describe_threads() {
    Sched& s = S();
    std::string out;
    for (unsigned i = 0; i < kSlots; ++i) {
        const Slot& t = s.slots[i];
        if (!t.used) continue;
        char buf[96];
        std::snprintf(buf, sizeof buf, "%sthread %u start %08X eip? esp %08X%s", out.empty() ? "" : ", ", t.id, t.start,
                      t.cpu ? t.cpu->r[Cpu::ESP] : 0u, t.suspend ? " suspended" : "");
        out += buf;
    }
    return out;
}

// Polls `ready` until it holds, the timeout passes, or every thread is stuck.
template <class Ready> bool block(Cpu& c, Ready ready, std::uint32_t ms, bool counts_as_wait) {
    ensure(c);
    Sched& s = S();
    if (t_free_slot >= 0) {
        // A parallel thread polls, without the runtime lock while it sleeps.
        const auto until = Clock::now() + std::chrono::milliseconds(ms == kInfinite ? 0 : ms);
        for (;;) {
            fire_timers();
            if (ready()) return true;
            if (ms == 0 || (ms != kInfinite && Clock::now() >= until)) return false;
            RtRelease released;
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
    }
    const unsigned me = s.current;
    hitch::Timer timer(hitch::Wait, me == 0);  // the main thread's waits stall its frame
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms == kInfinite ? 0 : ms);
    for (;;) {
        fire_timers();
        if (ready()) {
            s.idle = 0;
            s.slots[me].waiting_forever = false;
            return true;
        }
        if (ms == 0) return false;
        if (ms != kInfinite && Clock::now() >= deadline) {
            s.slots[me].waiting_forever = false;
            return false;
        }
        s.slots[me].waiting_forever = counts_as_wait && ms == kInfinite;
        if (++s.idle > live_count()) {
            // Every runnable thread had a turn and none got further. Real time
            // has to pass for a timeout or a timer; with neither, nothing can.
            bool stuck = s.timers.empty();
            for (unsigned i = 0; i < kSlots && stuck; ++i)
                if (runnable(i) && !s.slots[i].waiting_forever) stuck = false;
            if (stuck) {
                std::string what = "every guest thread waits forever: " + describe_threads();
                fail_closed(Unported::Function, s.slots[me].start, what.c_str());
            }
            {
                RtRelease released;
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            s.idle = 0;
        }
        Slot& self = s.slots[me];
        self.parked = true;
        self.parked_version = g_version;
        const auto turn = Clock::now() + kParkedTurn;
        self.wake_at = ms != kInfinite && deadline < turn ? deadline : turn;
        switch_to(me, pick_after(me));
        self.parked = false;
    }
}

bool object_ready(const Object& o, std::uint32_t me) {
    switch (o.kind) {
    case Kind::Thread: return o.finished;
    case Kind::Event: return o.signaled;
    case Kind::Semaphore: return o.count > 0;
    case Kind::Mutex: return o.owner == 0 || o.owner == me;
    default: return false;
    }
}

void consume(Object& o, std::uint32_t me) {
    switch (o.kind) {
    case Kind::Event:
        if (!o.manual) o.signaled = false;
        break;
    case Kind::Semaphore: --o.count; break;
    case Kind::Mutex:
        o.owner = me;
        ++o.recursion;
        break;
    default: break;
    }
}

bool commit(std::uint8_t* M, std::uint32_t address, std::uint32_t size) {
    const std::uint64_t page = GuestMemory::PageSize;
    const std::uint64_t lo = address & ~(page - 1);
    const std::uint64_t hi = (std::uint64_t(address) + size + page - 1) & ~(page - 1);
    return mprotect(M + lo, static_cast<std::size_t>(hi - lo), PROT_READ | PROT_WRITE) == 0;
}

void finish(unsigned slot, std::uint32_t code) {
    Sched& s = S();
    Slot& t = s.slots[slot];
    if (Object* o = object(t.handle)) {
        o->finished = true;
        o->exit_code = code;
        o->slot = -1;
    }
    // Mutexes the thread still owns are abandoned; the next waiter takes them.
    for (Object& o : s.objects)
        if (o.kind == Kind::Mutex && o.owner == t.id) {
            o.owner = 0;
            o.recursion = 0;
        }
    ++g_version;
    if (stats()) std::fprintf(stderr, "thread %u (%08X) exit %u\n", t.id, t.start, code);
    t.used = false;
    t.started = false;
    t.cpu = nullptr;
    g_live_threads.fetch_sub(1, std::memory_order_relaxed);
}

// A parallel thread: runs as soon as it is resumed, and gives no turn back.
void* free_main(unsigned me) {
    Sched& s = S();
    t_free_slot = static_cast<int>(me);
    // Off the performance cores on an iPhone (two of them; the main guest thread
    // needs one): the sound server keeps up on an efficiency core.
    if (TARGET_OS_IPHONE) pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    for (;;) {
        {
            RtGuard locked;
            if (s.slots[me].suspend == 0) break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
    std::uint32_t code = 0;
    try {
        Slot& t = s.slots[me];
        t.started = true;
        GuestFn fn = find_entry(t.start);
        if (!fn) fail_closed(Unported::Function, t.start, "no translation for the thread procedure");
        Cpu& c = *t.cpu;
        fn(c);
        code = c.r[Cpu::EAX];
    } catch (const GuestThreadExit& e) {
        code = e.code;
    } catch (...) {
        RtGuard locked;
        if (!s.failure) s.failure = std::current_exception();
        code = 0xC0000005u;
    }
    RtGuard locked;
    finish(me, code);  // counts the thread out of g_live_threads
    return nullptr;
}

void* host_main(void* p) {
    const unsigned me = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(p));
    Sched& s = S();
    if (s.slots[me].free) return free_main(me);
    {
        std::unique_lock<std::mutex> lk(s.mu);
        s.wake[me].wait(lk, [&] { return s.current == me; });
    }
    std::uint32_t code = 0;
    try {
        resumed(me);
        Slot& t = s.slots[me];
        t.started = true;
        GuestFn fn = find_entry(t.start);
        if (!fn) fail_closed(Unported::Function, t.start, "no translation for the thread procedure");
        Cpu& c = *t.cpu;
        fn(c);
        code = c.r[Cpu::EAX];
    } catch (const GuestThreadExit& e) {
        code = e.code;
    } catch (...) {
        if (!s.failure) s.failure = std::current_exception();
        code = 0xC0000005u;
    }
    {
        RtGuard locked;
        finish(me, code);
    }
    std::unique_lock<std::mutex> lk(s.mu);
    s.current = s.failure ? 0u : pick_after(me);
    if (!runnable(s.current)) s.current = 0;
    s.wake[s.current].notify_one();
    return nullptr;
}

}  // namespace

void sched_reset(Cpu& main) {
    Sched& s = S();
    for (Slot& t : s.slots) {
        t.used = false;
        t.cpu = nullptr;
    }
    s.objects.clear();
    s.timers.clear();
    s.current = 0;
    s.idle = 0;
    s.next_id = 2;
    s.failure = nullptr;
    g_live_threads.store(1, std::memory_order_relaxed);
    Slot& t = s.slots[0];
    t.used = true;
    t.started = true;
    t.suspend = 0;
    t.terminate = false;
    t.cpu = &main;
    t.id = main.mem ? ld32(main.mem, main.fs_base + 0x24) : 1u;
    if (t.id == 0) t.id = 1;
    t.start = 0;
    Object o;
    o.kind = Kind::Thread;
    o.slot = 0;
    t.handle = new_object(o);
    s.ready = true;
}

std::uint32_t sched_create_thread(Cpu& c, std::uint32_t start, std::uint32_t param, bool suspended, std::uint32_t* id) {
    ensure(c);
    Sched& s = S();
    unsigned slot = 0;
    for (unsigned i = 1; i < kSlots; ++i) {
        if (!s.slots[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == 0) return 0;
    std::uint8_t* M = c.mem;
    const std::uint32_t teb = teb_of(slot);
    const std::uint32_t top = stack_top_of(slot);
    if (!commit(M, teb, kTebStride) || !commit(M, top - kStackBytes, kStackBytes)) return 0;
    std::memset(M + teb, 0, kTebStride);
    Slot& t = s.slots[slot];
    t = Slot{};
    t.used = true;
    t.suspend = suspended ? 1u : 0u;
    t.id = s.next_id++;
    t.start = start;
    t.param = param;
    t.own = std::make_unique<Cpu>();
    t.cpu = t.own.get();
    st32(M, teb + 0x00, 0xFFFFFFFFu);
    st32(M, teb + 0x04, top);
    st32(M, teb + 0x08, top - kStackBytes);
    st32(M, teb + 0x18, teb);
    st32(M, teb + 0x20, 1);
    st32(M, teb + 0x24, t.id);
    st32(M, teb + 0x2C, kStaticTls);
    st32(M, teb + 0x30, kPeb);
    // DCUSA_PARALLEL_THREADS=<hex start>[,...]: these run in parallel. The
    // CRT's thread entry (0xC215BB) takes a thread block whose +0x54 holds the
    // procedure (the CRI sound server is 0x59E660).
    if (rt_parallel()) {
        const std::uint32_t real = param != 0 && param + 0x58u > param ? ld32(M, param + 0x54u) : 0u;
        const char* list = std::getenv("DCUSA_PARALLEL_THREADS");
        for (const char* q = list; q != nullptr && *q;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(q, &end, 16);
            if (end == q) break;
            if (v != 0 && (v == start || v == real)) t.free = true;
            q = *end ? end + 1 : end;
        }
        if (stats()) std::fprintf(stderr, "thread start %08X procedure %08X%s\n", start, real, t.free ? " (parallel)" : "");
    }
    Cpu& n = *t.cpu;
    n.mem = M;
    n.fs_base = teb;
    std::uint32_t esp = top - 16;
    esp -= 4;
    st32(M, esp, param);
    esp -= 4;
    st32(M, esp, 0);  // the procedure returns to the host, not to a guest address
    n.r[Cpu::ESP] = esp;
    Object o;
    o.kind = Kind::Thread;
    o.slot = static_cast<int>(slot);
    t.handle = new_object(o);
    if (id) *id = t.id;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // Guest threads take turns with the main one: all of them are the game's
    // frame (on iOS, default QoS may mean an efficiency core).
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t host;
    const int rc = pthread_create(&host, &attr, host_main, reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot)));
    pthread_attr_destroy(&attr);
    if (rc != 0) fail_closed(Unported::Function, start, "host thread for a guest thread could not start");
    g_live_threads.fetch_add(1, std::memory_order_relaxed);
    start_preempt_timer();
    if (stats()) std::fprintf(stderr, "thread %u start %08X param %08X%s\n", t.id, start, param, suspended ? " suspended" : "");
    return t.handle;
}

std::uint32_t sched_resume(std::uint32_t handle) {
    Object* o = object(handle);
    if (!o || o->kind != Kind::Thread) return kWaitFailed;
    if (o->slot < 0) return 0;
    Slot& t = S().slots[o->slot];
    const std::uint32_t previous = t.suspend;
    if (t.suspend > 0) --t.suspend;
    ++g_version;
    return previous;
}

std::uint32_t sched_suspend(Cpu& c, std::uint32_t handle) {
    ensure(c);
    Object* o = object(handle);
    if (!o || o->kind != Kind::Thread) return kWaitFailed;
    if (o->slot < 0) return 0;
    Slot& t = S().slots[o->slot];
    const std::uint32_t previous = t.suspend++;
    const unsigned me = me_slot();
    if (static_cast<unsigned>(o->slot) == me) {
        if (me == 0) fail_closed(Unported::Function, handle, "the main thread suspends itself");
        // Not runnable now: the CPU comes back only after a ResumeThread.
        S().idle = 0;
        switch_to(me, pick_after(me));
    }
    return previous;
}

bool sched_terminate(Cpu& c, std::uint32_t handle, std::uint32_t code) {
    ensure(c);
    Object* o = object(handle);
    if (!o || o->kind != Kind::Thread) return false;
    if (o->slot < 0) return true;
    const unsigned slot = static_cast<unsigned>(o->slot);
    if (slot == 0) fail_closed(Unported::Function, handle, "TerminateThread of the main thread");
    if (slot == me_slot()) throw GuestThreadExit{code};
    Slot& t = S().slots[slot];
    t.terminate = true;
    t.terminate_code = code;
    t.suspend = 0;
    t.parked = false;
    ++g_version;
    return true;
}

bool sched_exit_code(std::uint32_t handle, std::uint32_t* code) {
    Object* o = object(handle);
    if (!o || o->kind != Kind::Thread) return false;
    *code = o->finished ? o->exit_code : kStillActive;
    return true;
}

std::uint32_t sched_current_handle(Cpu& c) {
    ensure(c);
    return S().slots[me_slot()].handle;
}

std::uint32_t sched_thread_count() {
    std::uint32_t n = 0;
    for (const Slot& t : S().slots)
        if (t.used) ++n;
    return n;
}

bool sched_on_free_thread() { return t_free_slot >= 0; }

void sched_yield(Cpu& c) {
    ensure(c);
    Sched& s = S();
    if (t_free_slot >= 0) {  // a parallel thread has no turn to give
        RtRelease released;
        std::this_thread::yield();
        return;
    }
    fire_timers();
    s.idle = 0;
    const unsigned me = s.current;
    s.slots[me].waiting_forever = false;
    switch_to(me, pick_after(me));
}

void sched_frame_yield(Cpu& c) {
    ensure(c);
    Sched& s = S();
    static const int slice_ms = [] {
        const char* env = std::getenv("DCUSA_SLICE");
        return env ? std::max(0, std::atoi(env)) : 0;
    }();
    const auto deadline = Clock::now() + std::chrono::milliseconds(slice_ms);
    const unsigned me = s.current;
    s.slots[me].waiting_forever = false;
    do {
        fire_timers();
        s.idle = 0;
        const unsigned next = pick_after(me);
        if (next == me) return;
        switch_to(me, next);
        // Every other thread polled a wait and got nowhere: the frame goes on.
        if (live_count() <= 1 || s.idle + 1 >= live_count()) break;
    } while (Clock::now() < deadline);
}

void sched_sleep(Cpu& c, std::uint32_t ms) {
    if (ms == 0) {
        sched_yield(c);
        return;
    }
    if (ms > kSleepCap) ms = kSleepCap;
    block(c, [] { return false; }, ms, false);
}

std::uint32_t sched_wait(Cpu& c, const std::uint32_t* handles, unsigned count, bool all, std::uint32_t ms) {
    ensure(c);
    if (count == 0 || count > 64) return kWaitFailed;
    for (unsigned i = 0; i < count; ++i) {
        if (!object(handles[i])) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "wait on handle %08X, which is not a kernel object", handles[i]);
            fail_closed(Unported::Function, handles[i], buf);
        }
    }
    const std::uint32_t me = self_id();
    std::uint32_t result = kWaitTimeout;
    const bool done = block(
        c,
        [&] {
            if (all) {
                for (unsigned i = 0; i < count; ++i)
                    if (!object_ready(*object(handles[i]), me)) return false;
                for (unsigned i = 0; i < count; ++i) consume(*object(handles[i]), me);
                result = 0;
                return true;
            }
            for (unsigned i = 0; i < count; ++i) {
                Object& o = *object(handles[i]);
                if (object_ready(o, me)) {
                    consume(o, me);
                    result = i;
                    return true;
                }
            }
            return false;
        },
        ms, true);
    return done ? result : kWaitTimeout;
}

void sched_block_until(Cpu& c, bool (*ready)(Cpu&, std::uint32_t), std::uint32_t arg) {
    block(c, [&] { return ready(c, arg); }, kInfinite, true);
}

std::uint32_t sched_create_event(bool manual_reset, bool initial) {
    Object o;
    o.kind = Kind::Event;
    o.manual = manual_reset;
    o.signaled = initial;
    return new_object(o);
}

std::uint32_t sched_create_semaphore(std::uint32_t initial, std::uint32_t maximum) {
    Object o;
    o.kind = Kind::Semaphore;
    o.count = initial;
    o.maximum = maximum;
    return new_object(o);
}

std::uint32_t sched_create_mutex(Cpu& c, bool owned) {
    ensure(c);
    Object o;
    o.kind = Kind::Mutex;
    if (owned) {
        o.owner = self_id();
        o.recursion = 1;
    }
    return new_object(o);
}

bool sched_set_event(std::uint32_t handle) {
    Object* o = object(handle);
    if (!o || o->kind != Kind::Event) return false;
    o->signaled = true;
    ++g_version;
    return true;
}

bool sched_reset_event(std::uint32_t handle) {
    Object* o = object(handle);
    if (!o || o->kind != Kind::Event) return false;
    o->signaled = false;
    return true;
}

bool sched_release_semaphore(std::uint32_t handle, std::uint32_t count, std::uint32_t* previous) {
    Object* o = object(handle);
    if (!o || o->kind != Kind::Semaphore) return false;
    if (previous) *previous = o->count;
    if (std::uint64_t(o->count) + count > o->maximum) return false;
    o->count += count;
    ++g_version;
    return true;
}

bool sched_release_mutex(Cpu& c, std::uint32_t handle) {
    ensure(c);
    Object* o = object(handle);
    if (!o || o->kind != Kind::Mutex || o->owner != self_id()) return false;
    if (--o->recursion == 0) o->owner = 0;
    ++g_version;
    return true;
}

bool sched_is_object(std::uint32_t handle) { return object(handle) != nullptr; }

void sched_wake_waiters() { ++g_version; }

bool sched_close(std::uint32_t handle) {
    Object* o = object(handle);
    if (!o) return false;
    // A running thread keeps its object; closing the handle does not stop it.
    if (o->kind == Kind::Thread && !o->finished) return true;
    *o = Object{};
    ++g_version;
    return true;
}

std::uint32_t sched_timer_event(std::uint32_t delay, std::uint32_t event, bool periodic, bool pulse) {
    Sched& s = S();
    const Object* e = object(event);
    if (!e || e->kind != Kind::Event || (pulse && e->manual)) return 0;
    Timer t;
    t.id = s.next_timer++;
    t.event = event;
    t.period = delay;
    t.periodic = periodic;
    t.due = Clock::now() + std::chrono::milliseconds(delay);
    s.timers.push_back(t);
    return t.id;
}

bool sched_kill_timer(std::uint32_t id) {
    Sched& s = S();
    for (std::size_t i = 0; i < s.timers.size(); ++i) {
        if (s.timers[i].id == id) {
            s.timers.erase(s.timers.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

}  // namespace dcusa::recomp

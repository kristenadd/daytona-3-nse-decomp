// Guest threads take turns: a new thread runs when its creator blocks, waits
// see events, semaphores, timers and thread exits, and a wait nothing can end
// fails closed instead of hanging.
#include <cstdint>
#include <string>

#include "check.hpp"
#include "recomp/dispatch.hpp"
#include "recomp/guest_memory.hpp"
#include "recomp/threads.hpp"
#include "recomp/x86ops.hpp"
#include "runtime/fail_closed.hpp"

using namespace dcusa::recomp;

namespace {

constexpr std::uint32_t Teb = 0x10000;
constexpr std::uint32_t Counter = 0x200000;
constexpr std::uint32_t MainStack = 0x210000;

std::uint32_t go = 0;
std::uint32_t done = 0;
std::uint32_t sem = 0;
std::uint32_t order = 0;

std::uint32_t param(const Cpu& c) { return ld32(c.mem, c.r[Cpu::ESP] + 4); }

// Waits for `go`, counts, sets `done`, returns 5.
void counter_thread(Cpu& c) {
    const std::uint32_t at = param(c);
    CHECK(sched_wait(c, &go, 1, false, kInfinite) == 0);
    st32(c.mem, at, ld32(c.mem, at) + 1);
    CHECK(c.fs_base != Teb);
    CHECK(ld32(c.mem, c.fs_base + 0x18) == c.fs_base);
    sched_set_event(done);
    c.r[Cpu::EAX] = 5;
}

void releasing_thread(Cpu&) {
    order = order * 10 + 2;
    std::uint32_t previous = 99;
    CHECK(sched_release_semaphore(sem, 2, &previous));
    CHECK(previous == 0);
    throw GuestThreadExit{7};
}

void sleeping_thread(Cpu& c) {
    sched_sleep(c, 1);
    order = order * 10 + 3;
}

}  // namespace

int main() {
    GuestMemory mem;
    std::string error;
    REQUIRE(mem.reserve(&error));
    REQUIRE(mem.commit(Teb, 0x1000, &error));
    REQUIRE(mem.commit(Counter, 0x20000, &error));
    std::uint8_t* m = mem.base();
    st32(m, Teb + 0x18, Teb);
    st32(m, Teb + 0x24, 1);

    static const GuestEntry entries[] = {
        {0x1000, counter_thread},
        {0x2000, releasing_thread},
        {0x3000, sleeping_thread},
    };
    dispatch_install(entries, {});

    Cpu c;
    c.mem = m;
    c.fs_base = Teb;
    c.r[Cpu::ESP] = MainStack;
    sched_reset(c);

    // Events and a thread handle.
    go = sched_create_event(false, false);
    done = sched_create_event(true, false);
    std::uint32_t id = 0;
    const std::uint32_t t = sched_create_thread(c, 0x1000, Counter, false, &id);
    REQUIRE(t != 0);
    CHECK(id == 2);
    CHECK(ld32(m, Counter) == 0);  // not run until this thread blocks
    std::uint32_t code = 0;
    CHECK(sched_exit_code(t, &code) && code == kStillActive);
    CHECK(sched_wait(c, &done, 1, false, 0) == kWaitTimeout);
    CHECK(sched_set_event(go));
    CHECK(sched_wait(c, &done, 1, false, kInfinite) == 0);
    CHECK(ld32(m, Counter) == 1);
    CHECK(sched_wait(c, &t, 1, false, 1000) == 0);
    CHECK(sched_exit_code(t, &code) && code == 5);
    // Manual reset: still set. Auto reset: consumed by the thread's wait.
    CHECK(sched_wait(c, &done, 1, false, 0) == 0);
    CHECK(sched_wait(c, &go, 1, false, 0) == kWaitTimeout);
    CHECK(sched_close(t));

    // A semaphore released by a thread that exits through ExitThread, and a
    // second thread that sleeps: both run in creation order.
    sem = sched_create_semaphore(0, 2);
    const std::uint32_t a = sched_create_thread(c, 0x2000, 0, false, nullptr);
    const std::uint32_t b = sched_create_thread(c, 0x3000, 0, true, nullptr);
    CHECK(sched_resume(b) == 1);
    order = 1;
    const std::uint32_t both[] = {a, b};
    CHECK(sched_wait(c, both, 2, true, 1000) == 0);
    CHECK(order == 123);
    CHECK(sched_exit_code(a, &code) && code == 7);
    CHECK(sched_wait(c, &sem, 1, false, 0) == 0);
    CHECK(sched_wait(c, &sem, 1, false, 0) == 0);
    CHECK(sched_wait(c, &sem, 1, false, 0) == kWaitTimeout);
    std::uint32_t previous = 0;
    CHECK(!sched_release_semaphore(sem, 3, &previous));

    // Timeouts and a periodic timer event.
    const std::uint32_t tick = sched_create_event(false, false);
    CHECK(sched_wait(c, &tick, 1, false, 5) == kWaitTimeout);
    const std::uint32_t timer = sched_timer_event(2, tick, true, true);
    CHECK(sched_wait(c, &tick, 1, false, 1000) == 0);
    CHECK(sched_wait(c, &tick, 1, false, 1000) == 0);
    CHECK(sched_kill_timer(timer));

    // A mutex is recursive for its owner.
    const std::uint32_t mutex = sched_create_mutex(c, true);
    CHECK(sched_wait(c, &mutex, 1, false, 0) == 0);
    CHECK(sched_release_mutex(c, mutex));
    CHECK(sched_release_mutex(c, mutex));
    CHECK(!sched_release_mutex(c, mutex));

    // Nothing can set this event: fail closed rather than hang.
    const std::uint32_t never = sched_create_event(true, false);
    bool stopped = false;
    try {
        sched_wait(c, &never, 1, false, kInfinite);
    } catch (const dcusa::FailClosed& e) {
        stopped = e.report().what.find("waits forever") != std::string::npos;
    }
    CHECK(stopped);

    // A handle that is not a kernel object.
    stopped = false;
    const std::uint32_t bogus = 0x1234;
    try {
        sched_wait(c, &bogus, 1, false, 0);
    } catch (const dcusa::FailClosed&) {
        stopped = true;
    }
    CHECK(stopped);
    return check_result();
}

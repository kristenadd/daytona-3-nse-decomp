#pragma once
// Guest threads and the kernel objects they wait on.
//
// Every guest thread runs on its own host thread, but only one of them runs at
// a time. A thread gives up the CPU only where Windows code can block: Sleep,
// SwitchToThread, a wait, a contended critical section, and the frame loop's
// message poll. Hand-offs go through one mutex, so guest memory is always seen
// in the order the guest wrote it and no x86 / ARM ordering difference can show.
//
// Handles for threads, events, semaphores and mutexes share one table starting
// at kSyncHandleBase. Waiting on any other handle fails closed.
#include <chrono>
#include <cstdint>

#include "recomp/cpu.hpp"

namespace dcusa::recomp {

constexpr std::uint32_t kSyncHandleBase = 0x50000u;
constexpr std::uint32_t kWaitTimeout = 0x102u;
constexpr std::uint32_t kWaitFailed = 0xFFFFFFFFu;
constexpr std::uint32_t kInfinite = 0xFFFFFFFFu;
constexpr std::uint32_t kStillActive = 259u;

// Thrown by ExitThread and by a terminated thread when it next runs. Caught where
// the guest thread started; never crosses into the main thread.
struct GuestThreadExit {
    std::uint32_t code;
};

// Process start: the thread that runs the entry point. `c` stays valid for the
// whole run. Called again by a test, it resets every object.
void sched_reset(Cpu& main);

// A new guest thread whose procedure is `start` with `param` on its stack. The
// thread does not run until the caller blocks or yields. Returns the handle, or
// 0 when no slot or stack is free. `id` receives the thread id.
std::uint32_t sched_create_thread(Cpu& c, std::uint32_t start, std::uint32_t param, bool suspended, std::uint32_t* id);
// ResumeThread / SuspendThread. The previous suspend count, or kWaitFailed.
std::uint32_t sched_resume(std::uint32_t handle);
std::uint32_t sched_suspend(Cpu& c, std::uint32_t handle);
bool sched_terminate(Cpu& c, std::uint32_t handle, std::uint32_t code);
// False when `handle` is not a thread. `code` is kStillActive while it runs.
bool sched_exit_code(std::uint32_t handle, std::uint32_t* code);
// The handle of the calling thread (DuplicateHandle of the pseudo-handle).
std::uint32_t sched_current_handle(Cpu& c);
std::uint32_t sched_thread_count();

// Gives every other runnable thread one turn, then returns.
void sched_yield(Cpu& c);
// Whether the calling host thread runs a parallel guest thread (DCUSA_PARALLEL_THREADS).
bool sched_on_free_thread();
// The frame loop's poll: the other threads run until each is blocked, or for
// at most DCUSA_SLICE ms (default 0: one turn each). On Windows they run on
// other cores; a slice was measured slower here, since the boot drains its
// resource queue on the main thread once per frame.
void sched_frame_yield(Cpu& c);
// Gives the other threads turns until each is blocked or the deadline passes.
void sched_run_others_until(Cpu& c, std::chrono::steady_clock::time_point deadline);
void sched_sleep(Cpu& c, std::uint32_t ms);
// WAIT_OBJECT_0 + index, kWaitTimeout, or kWaitFailed (with the reason in
// *error when it is not null).
std::uint32_t sched_wait(Cpu& c, const std::uint32_t* handles, unsigned count, bool all, std::uint32_t ms);
// Blocks until `ready(arg)` is true. Used for critical sections.
void sched_block_until(Cpu& c, bool (*ready)(Cpu&, std::uint32_t), std::uint32_t arg);

std::uint32_t sched_create_event(bool manual_reset, bool initial);
std::uint32_t sched_create_semaphore(std::uint32_t initial, std::uint32_t maximum);
std::uint32_t sched_create_mutex(Cpu& c, bool owned);
bool sched_set_event(std::uint32_t handle);
bool sched_reset_event(std::uint32_t handle);
// The previous count in *previous. False when the count would pass the maximum.
bool sched_release_semaphore(std::uint32_t handle, std::uint32_t count, std::uint32_t* previous);
bool sched_release_mutex(Cpu& c, std::uint32_t handle);
bool sched_is_object(std::uint32_t handle);
// Something a sched_block_until condition reads changed (a critical section was
// left): threads parked on it get a turn again.
void sched_wake_waiters();
bool sched_close(std::uint32_t handle);

// timeSetEvent with an event callback: `event` is set every `delay` ms (once
// when not periodic), checked whenever a thread yields or waits. A pulse is
// only accepted on an auto-reset event, where it is a set that the next waiter
// consumes. 0 when the handle is not such an event.
std::uint32_t sched_timer_event(std::uint32_t delay, std::uint32_t event, bool periodic, bool pulse);
bool sched_kill_timer(std::uint32_t id);

}

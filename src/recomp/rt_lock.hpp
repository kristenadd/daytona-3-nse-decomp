#pragma once
// The runtime lock, for guest threads that run in parallel (DCUSA_PARALLEL_THREADS).
//
// Guest threads normally take turns on one host core (threads.cpp), so the
// runtime (imports, the scheduler, D3D) is only ever entered by one of them.
// A parallel ("free") thread runs its translated code at the same time as the
// others; whenever any thread enters the runtime it takes this lock, and every
// wait (a switch of turns, a sleep, a poll) gives it up while it waits.
//
// Without DCUSA_PARALLEL_THREADS every call is a no-op.
namespace dcusa::recomp {

// Whether parallel threads are on (decided once, from the environment).
bool rt_parallel();

// Recursive per host thread: an import handler that calls back into guest code
// (call_guest) and so into another import takes it again.
void rt_lock();
void rt_unlock();

// Gives the lock up completely while a thread waits, and takes it back after.
int rt_release_all();
void rt_reacquire(int depth);

class RtGuard {
public:
    RtGuard() { rt_lock(); }
    ~RtGuard() { rt_unlock(); }
    RtGuard(const RtGuard&) = delete;
    RtGuard& operator=(const RtGuard&) = delete;
};

// Released for the scope (a wait), taken back at its end.
class RtRelease {
public:
    RtRelease() : depth_(rt_release_all()) {}
    ~RtRelease() { rt_reacquire(depth_); }
    RtRelease(const RtRelease&) = delete;
    RtRelease& operator=(const RtRelease&) = delete;

private:
    int depth_;
};

}  // namespace dcusa::recomp

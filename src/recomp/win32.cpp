#include "runtime/platform_compat.hpp"
#include "recomp/win32.hpp"
#include "recomp/game_addresses.hpp"

#include "recomp/avi_video.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <thread>

#include "recomp/host_files.hpp"
#include "recomp/shell.hpp"
#include "recomp/threads.hpp"
#include "runtime/host_platform.hpp"

#include "recomp/d3d11.hpp"
#include "recomp/guest.hpp"
#include "recomp/guest_memory.hpp"
#include "recomp/x86ops.hpp"
#include "runtime/fail_closed.hpp"

namespace dcusa::recomp {

void note_io(std::uint32_t slot, std::uint32_t io, std::uint32_t queued, std::uint32_t flag) {
    if (std::getenv("DCUSA_STATS") == nullptr) return;
    static int seen;
    if (seen++ >= 3) return;
    std::fprintf(stderr, "io slot=%08x io=%08x queued=%u flag=%u\n", slot, io, queued, flag);
}

struct GuestMsg {
    std::uint32_t hwnd = 0;
    std::uint32_t message = 0;
    std::uint32_t wparam = 0;
    std::uint32_t lparam = 0;
};

PadSnapshot pad_snapshot;
std::deque<GuestMsg> messages;

namespace {

std::uint32_t arg(const Cpu& c, unsigned i) { return ld32(c.mem, c.r[Cpu::ESP] + 4u + 4u * i); }

// `arg_bytes` is what the callee pops: 4 per dword argument.
void ret_stdcall(Cpu& c, std::uint32_t eax, unsigned arg_bytes) {
    c.r[Cpu::EAX] = eax;
    c.r[Cpu::ESP] += 4u + arg_bytes;
}

void set_error(Cpu& c, std::uint32_t err);
std::string guest_ascii(const std::uint8_t* mem, std::uint32_t va, bool wide);

// EncodePointer(NULL) must not be NULL: the CRT stores it as the empty sentinel.
constexpr std::uint32_t kPointerCookie = 0x5A17C0DEu;
constexpr std::uint32_t kLastError = 0x34u;
constexpr std::uint32_t kTlsSlots = 0xE10u;
constexpr std::uint32_t kTlsCount = 64u;
constexpr std::uint32_t kHeapZero = 0x8u;
constexpr std::uint32_t kHeapNext = 0u;
constexpr std::uint32_t kHeapTls = 4u;
// The tail of the heap holds the command line and the environment block, so
// allocations stop short of it.
constexpr std::uint32_t kHeapLimit = kProcessHeap + kHeapBytes - 512;
// VirtualAlloc's anonymous region. Above the process heap, below the stack.
constexpr std::uint32_t kVirtBase = kProcessHeap + kHeapBytes;
constexpr std::uint32_t kVirtLimit = 0x2F000000u;
constexpr std::uint32_t kAllocGranularity = 0x10000u;
std::uint32_t virt_bump = kVirtBase;
// Past the low region, the rest of a large-address-aware process's space: above
// the guest thread stacks at 0x3C000000, below the COM and import thunks.
constexpr std::uint32_t kVirtHigh = 0x40000000u;
// NRT's own heap grows by VirtualAlloc to nearly 1 GB once the attract scenes
// load (its statistics at 0x154EF44 / 0x154EF4C); with less, every allocation
// after that fails and a worker retries forever (resource_creator.cpp). A
// large-address-aware process on 64-bit Windows has almost 4 GB to give it.
constexpr std::uint32_t kVirtHighLimit = 0xB0000000u;  // the heap's third region follows
std::uint32_t virt_high = kVirtHigh;

bool commit_guest(std::uint8_t* mem, std::uint32_t address, std::uint32_t size) {
    const std::uint64_t page = GuestMemory::PageSize;
    const std::uint64_t lo = address & ~(page - 1);
    const std::uint64_t hi = (std::uint64_t(address) + size + page - 1) & ~(page - 1);
    if (hi < lo || hi > (1ull << 32)) return false;
    return mprotect(mem + lo, static_cast<std::size_t>(hi - lo), PROT_READ | PROT_WRITE) == 0;
}

// Freed guest memory goes back to the host: the whole host pages inside
// [address, address + size) stop counting toward the process's footprint (what
// iOS kills an app over) until reuse_pages. Their contents are undefined then.
void release_pages(std::uint8_t* mem, std::uint32_t address, std::uint32_t size, bool committed = false) {
    const std::uint64_t page = static_cast<std::uint64_t>(getpagesize());
    const std::uint64_t lo = (address + page - 1) & ~(page - 1);
    const std::uint64_t hi = (std::uint64_t(address) + size) & ~(page - 1);
    if (hi <= lo) return;
    // DCUSA_RELEASE_PAGES=0 keeps them; =zero clears them at once, as the
    // system does when it takes reusable pages under memory pressure (a test
    // for guest code that reads memory after freeing it). Only heap blocks are
    // cleared: a VirtualFree range may hold pages that were never committed.
    static const int mode = [] {
        const char* e = std::getenv("DCUSA_RELEASE_PAGES");
        if (e == nullptr) return 1;
        if (std::strcmp(e, "zero") == 0) return 2;
        return std::atoi(e) != 0 ? 1 : 0;
    }();
    if (mode == 0) return;
    if (mode == 2 && committed) std::memset(mem + lo, 0, static_cast<std::size_t>(hi - lo));
    madvise(mem + lo, static_cast<std::size_t>(hi - lo), MADV_FREE_REUSABLE);
}

void reuse_pages(std::uint8_t* mem, std::uint32_t address, std::uint32_t size) {
    const std::uint64_t page = static_cast<std::uint64_t>(getpagesize());
    const std::uint64_t lo = address & ~(page - 1);
    const std::uint64_t hi = (std::uint64_t(address) + size + page - 1) & ~(page - 1);
    if (hi > lo) madvise(mem + lo, static_cast<std::size_t>(hi - lo), MADV_FREE_REUSE);
}

// Heap blocks this large give their pages back when freed.
constexpr std::uint32_t kHeapReleaseBytes = 256u << 10;

constexpr std::uint32_t kCommandLine = kHeapLimit;
constexpr std::uint32_t kCommandLineW = kHeapLimit + 128;
constexpr std::uint32_t kEnvironment = kHeapLimit + 256;
constexpr char kExeName[] = "Daytona.exe";

// STARTUPINFOW. The CRT's caller tests dwFlags bit 0 (STARTF_USESHOWWINDOW) and,
// when it is clear, uses SW_SHOWDEFAULT. A zeroed structure with cb set is that case.
constexpr std::uint32_t kStartupInfoBytes = 0x44;

bool ascii_eq_i(char a, char b) {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    return lower(a) == lower(b);
}

bool wide_eq_i(const std::uint8_t* M, std::uint32_t va, const char* ascii) {
    for (unsigned n = 0; n < 260; ++n) {
        std::uint16_t w = ld16(M, va);
        char want = ascii[n];
        if (w == 0 || want == 0) return w == 0 && want == 0;
        if (w > 127 || !ascii_eq_i(char(w), want)) return false;
        va += 2;
    }
    return false;
}

std::uint32_t heap2_bump = kHeap2 + 16;

// Past both fixed heaps, blocks come from a large region in the upper half of
// the address space (a large-address-aware process has it), committed as the
// bump pointer reaches it. Course loads ask for single blocks over 50 MB.
constexpr std::uint32_t kHeap3 = 0xB0000000u;
constexpr std::uint32_t kHeap3Limit = 0xE0000000u;
std::uint32_t heap3_bump = kHeap3 + 16;

bool heap_owns(std::uint32_t p) {
    return (p >= kProcessHeap + 16 && p < kHeapLimit) || (p >= kHeap2 + 16 && p < kHeap2 + kHeap2Bytes) ||
           (p >= kHeap3 + 16 && p < kHeap3Limit);
}

// A block: its capacity at p - 8, the size asked for at p - 4 (HeapSize), the
// bytes at p, 16-byte aligned.
std::uint32_t bump_alloc(std::uint8_t* M, std::uint32_t& bump, std::uint32_t limit, std::uint32_t capacity) {
    const std::uint32_t user = (bump + 8u + 15u) & ~15u;
    const std::uint32_t end = user + capacity;
    if (end < user || end > limit) return 0;
    st32(M, user - 8, capacity);
    bump = (end + 15u) & ~15u;
    return user;
}

// Size classes: 16-byte steps up to 256 bytes, then four per power of two
// (at most 25% over). Freed blocks wait on the free stack of their class and
// are reused only for that class, so a small request never takes (and wastes)
// a large freed block and no allocation walks a list. The old single first-fit
// list grew to thousands of blocks over a race; the load after name entry then
// spent minutes in it (a white, then black, screen).
constexpr unsigned kHeapClasses = 16 + 24 * 4;
std::vector<std::uint32_t> heap_bins[kHeapClasses];

unsigned heap_class(std::uint32_t n, std::uint32_t* capacity) {
    if (n <= 256) {
        *capacity = (n + 15u) & ~15u;
        return *capacity / 16u - 1u;
    }
    const unsigned e = 31u - static_cast<unsigned>(__builtin_clz(n - 1u));  // 2^e < n <= 2^(e+1)
    const std::uint32_t step = 1u << (e - 2u);
    const std::uint64_t cap = (static_cast<std::uint64_t>(n) + step - 1u) & ~static_cast<std::uint64_t>(step - 1u);
    *capacity = cap > 0xFFFFFFF0ull ? 0xFFFFFFF0u : static_cast<std::uint32_t>(cap);
    return 16u + (e - 8u) * 4u + static_cast<unsigned>(cap / step - 5u);
}

std::uint32_t heap_alloc(std::uint8_t* M, std::uint32_t size, bool zero) {
    if (size == 0) size = 1;
    if (ld32(M, kProcessHeap + kHeapNext) == 0)  // a fresh guest memory: nothing freed belongs to it
        for (auto& bin : heap_bins) bin.clear();
    std::uint32_t capacity = 0;
    const unsigned cls = heap_class(size, &capacity);
    std::uint32_t user = 0;
    if (cls < kHeapClasses && !heap_bins[cls].empty()) {
        user = heap_bins[cls].back();
        heap_bins[cls].pop_back();
        if (capacity >= kHeapReleaseBytes) reuse_pages(M, user, capacity);
    } else {
        std::uint32_t bump = ld32(M, kProcessHeap + kHeapNext);
        if (bump < kProcessHeap + 16) bump = kProcessHeap + 16;
        if ((user = bump_alloc(M, bump, kHeapLimit, capacity)) != 0) {
            st32(M, kProcessHeap + kHeapNext, bump);
        } else if ((user = bump_alloc(M, heap2_bump, kHeap2 + kHeap2Bytes, capacity)) == 0) {
            const std::uint32_t before = heap3_bump;
            if (capacity > kHeap3Limit - kHeap3 ||
                !commit_guest(M, before, std::min<std::uint32_t>(capacity + 64u, kHeap3Limit - before)))
                return 0;
            if ((user = bump_alloc(M, heap3_bump, kHeap3Limit, capacity)) == 0) return 0;
        }
    }
    st32(M, user - 4, size);
    if (zero) std::memset(M + user, 0, size);
    return user;
}

void heap_free(std::uint8_t* M, std::uint32_t p) {
    std::uint32_t capacity = 0;
    const unsigned cls = heap_class(ld32(M, p - 8), &capacity);
    if (cls < kHeapClasses && capacity == ld32(M, p - 8)) {
        heap_bins[cls].push_back(p);
        if (capacity >= kHeapReleaseBytes) release_pages(M, p, capacity, true);
    }
}

bool heap_committed(std::uint32_t a) {
    return (a >= kProcessHeap && a + 4 <= kHeapLimit) || (a >= kHeap2 && a + 4 <= kHeap2 + kHeap2Bytes) ||
           (a >= kHeap3 && a + 4 <= heap3_bump) || (a >= kVirtBase && a + 4 <= virt_bump) ||
           (a >= kVirtHigh && a + 4 <= virt_high);
}

void GetStartupInfoW(Cpu& c) {
    const std::uint32_t info = arg(c, 0);
    std::memset(c.mem + info, 0, kStartupInfoBytes);
    st32(c.mem, info, kStartupInfoBytes);
    ret_stdcall(c, 0, 4);
}

void GetProcessHeap(Cpu& c) { ret_stdcall(c, kProcessHeap, 0); }

void HeapAlloc(Cpu& c) {
    const std::uint32_t heap = arg(c, 0);
    const std::uint32_t flags = arg(c, 1);
    const std::uint32_t size = arg(c, 2);
    std::uint32_t p = 0;
    if (heap == kProcessHeap) p = heap_alloc(c.mem, size, (flags & kHeapZero) != 0);
    if (p == 0 && std::getenv("DCUSA_STATS") != nullptr) {
        static int misses = 0;
        if (misses < 4) {
            ++misses;
            const std::uint32_t bump = ld32(c.mem, kProcessHeap + kHeapNext);
            std::fprintf(stderr, "heap miss size %u bump %u handle %08x\n", size,
                         bump > kProcessHeap ? bump - kProcessHeap : 0, heap);
        }
    }
    ret_stdcall(c, p, 12);
}

// Flags 0 grows by allocating a new block and copying. HEAP_REALLOC_IN_PLACE_ONLY
// (0x10) keeps the same pointer only when the new size already fits.
void HeapReAlloc(Cpu& c) {
    const std::uint32_t heap = arg(c, 0);
    const std::uint32_t flags = arg(c, 1);
    const std::uint32_t p = arg(c, 2);
    const std::uint32_t size = arg(c, 3);
    std::uint32_t out = 0;
    if (heap == kProcessHeap && heap_owns(p)) {
        const std::uint32_t old = ld32(c.mem, p - 4);
        if (size != 0 && size <= ld32(c.mem, p - 8)) {  // fits the block: in place
            if ((flags & kHeapZero) != 0 && size > old) std::memset(c.mem + p + old, 0, size - old);
            st32(c.mem, p - 4, size);
            out = p;
        } else if ((flags & 0x10u) == 0) {
            const std::uint32_t n = heap_alloc(c.mem, size, (flags & kHeapZero) != 0);
            if (n != 0) {
                const std::uint32_t copy = old < size ? old : size;
                std::memcpy(c.mem + n, c.mem + p, copy);
                heap_free(c.mem, p);
                out = n;
            }
        }
    }
    ret_stdcall(c, out, 16);
}

// A null address bumps through the arena at 64 KB granularity, which is
// Windows' allocation granularity. A requested address is committed in place.
// Regions VirtualAlloc handed out (address -> size), and released ones ready
// for reuse. The boot allocates and releases 53-64 MB streaming buffers over and
// over; without reuse the address space ran out after a minute.
std::map<std::uint32_t, std::uint32_t> virt_regions;
std::map<std::uint32_t, std::uint32_t> virt_free;

std::uint32_t virt_take_free(std::uint32_t need) {
    for (auto it = virt_free.begin(); it != virt_free.end(); ++it) {
        if (it->second < need) continue;
        const std::uint32_t at = it->first;
        const std::uint32_t left = it->second - need;
        virt_free.erase(it);
        if (left != 0) virt_free.emplace(at + need, left);
        return at;
    }
    return 0;
}

void virt_release(std::uint32_t at, std::uint32_t size) {
    auto it = virt_free.emplace(at, size).first;
    if (auto next = std::next(it); next != virt_free.end() && it->first + it->second == next->first) {
        it->second += next->second;
        virt_free.erase(next);
    }
    if (it != virt_free.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            virt_free.erase(it);
        }
    }
}

void VirtualAlloc(Cpu& c) {
    std::uint32_t address = arg(c, 0);
    const std::uint32_t size = arg(c, 1);
    std::uint32_t out = 0;
    bool reused = false;
    if (size != 0 && address == 0) {
        const std::uint32_t need = (size + kAllocGranularity - 1u) & ~(kAllocGranularity - 1u);
        if (need != 0 && (address = virt_take_free(need)) != 0) {
            reused = true;
            reuse_pages(c.mem, address, need);
        } else if (need != 0 && virt_bump <= kVirtLimit && need <= kVirtLimit - virt_bump) {
            address = virt_bump;
            virt_bump += need;
        } else if (need != 0 && virt_high <= kVirtHighLimit && need <= kVirtHighLimit - virt_high) {
            address = virt_high;
            virt_high += need;
        }
        if (address != 0) virt_regions[address] = need;
    }
    if (size != 0 && address != 0 && commit_guest(c.mem, address, size)) out = address;
    // Fresh pages are zero; reused ones are cleared, as Windows commits them.
    if (out != 0 && reused) std::memset(c.mem + out, 0, size);
    if (out == 0 && size != 0 && std::getenv("DCUSA_STATS") != nullptr) {
        std::fprintf(stderr, "virt miss size %u address %08x bump %08x high %08x\n", size, arg(c, 0), virt_bump, virt_high);
    }
    ret_stdcall(c, out, 16);
}

// Windows 7, matching the PEB version. The caller at 0x7798DA requires a major
// version above 5. 0x94 is OSVERSIONINFOA and 0x9C is OSVERSIONINFOEXA.
void write_version(std::uint8_t* mem, std::uint32_t info, bool wide) {
    st32(mem, info + 4, 6);
    st32(mem, info + 8, 1);
    st32(mem, info + 12, 7601);
    st32(mem, info + 16, 2);
    const char sp[] = "Service Pack 1";
    if (!wide) {
        std::memcpy(mem + info + 20, sp, sizeof(sp));
    } else {
        for (unsigned i = 0; i < sizeof(sp); ++i) st16(mem, info + 20 + i * 2, static_cast<std::uint8_t>(sp[i]));
    }
}

void GetVersionExA(Cpu& c) {
    const std::uint32_t info = arg(c, 0);
    const std::uint32_t size = ld32(c.mem, info);
    std::uint32_t ok = 0;
    if (size == 0x94 || size == 0x9C) {
        write_version(c.mem, info, false);
        if (size == 0x9C) {
            st16(c.mem, info + 148, 1);
            c.mem[info + 154] = 1;
        }
        ok = 1;
    }
    ret_stdcall(c, ok, 4);
}

void GetVersionExW(Cpu& c) {
    const std::uint32_t info = arg(c, 0);
    const std::uint32_t size = ld32(c.mem, info);
    std::uint32_t ok = 0;
    if (size == 0x114 || size == 0x11C) {
        write_version(c.mem, info, true);
        if (size == 0x11C) {
            st16(c.mem, info + 276, 1);
            c.mem[info + 282] = 1;
        }
        ok = 1;
    }
    ret_stdcall(c, ok, 4);
}

// VirtualFree(address, size, type). MEM_RELEASE (0x8000) gives a whole region
// back for reuse; MEM_DECOMMIT (0x4000) leaves it committed and in use.
void VirtualFree(Cpu& c) {
    const std::uint32_t address = arg(c, 0);
    const std::uint32_t type = arg(c, 2);
    if ((type & 0x8000u) != 0) {
        const auto it = virt_regions.find(address);
        if (it != virt_regions.end()) {
            release_pages(c.mem, it->first, it->second);
            virt_release(it->first, it->second);
            virt_regions.erase(it);
        }
    }
    ret_stdcall(c, address == 0 ? 0u : 1u, 12);
}

void HeapSize(Cpu& c) {
    const std::uint32_t heap = arg(c, 0);
    const std::uint32_t p = arg(c, 2);
    std::uint32_t size = 0xFFFFFFFFu;
    if (heap == kProcessHeap && heap_owns(p)) size = ld32(c.mem, p - 4);
    ret_stdcall(c, size, 12);
}

void HeapFree(Cpu& c) {
    const std::uint32_t heap = arg(c, 0);
    const std::uint32_t p = arg(c, 2);
    std::uint32_t ok = 0;
    if (heap == kProcessHeap && (p == 0 || heap_owns(p))) {
        if (p != 0) heap_free(c.mem, p);
        ok = 1;
    }
    ret_stdcall(c, ok, 12);
}

void EncodePointer(Cpu& c) { ret_stdcall(c, arg(c, 0) ^ kPointerCookie, 4); }

void DecodePointer(Cpu& c) { ret_stdcall(c, arg(c, 0) ^ kPointerCookie, 4); }

void GetModuleHandleW(Cpu& c) {
    const std::uint32_t name = arg(c, 0);
    std::uint32_t handle = 0;
    if (name == 0)
        handle = ld32(c.mem, 0x20008);  // PEB.ImageBaseAddress
    else if (wide_eq_i(c.mem, name, "kernel32.dll"))
        handle = 1;
    ret_stdcall(c, handle, 4);
}

// Unknown procedures return 0. The CRT then uses the import it already has,
// which is how it runs without FlsAlloc.
void GetProcAddress(Cpu& c) { ret_stdcall(c, 0, 8); }

// LockCount -1 is unlocked. OwningThread (+12) is the owner's thread id and
// RecursionCount (+8) its depth. Another thread blocks until the owner leaves.
void init_critical_section(std::uint8_t* M, std::uint32_t cs, std::uint32_t spin) {
    std::memset(M + cs, 0, 24);
    st32(M, cs + 4, 0xFFFFFFFFu);
    st32(M, cs + 20, spin);
}

void InitializeCriticalSectionAndSpinCount(Cpu& c) {
    init_critical_section(c.mem, arg(c, 0), arg(c, 1));
    ret_stdcall(c, 1, 8);
}

void InitializeCriticalSection(Cpu& c) {
    init_critical_section(c.mem, arg(c, 0), 0);
    ret_stdcall(c, 0, 4);
}

bool critical_section_free(Cpu& c, std::uint32_t cs) {
    const std::uint32_t owner = ld32(c.mem, cs + 12);
    return owner == 0 || owner == ld32(c.mem, c.fs_base + 0x24);
}

void take_critical_section(Cpu& c, std::uint32_t cs) {
    const std::uint32_t self = ld32(c.mem, c.fs_base + 0x24);
    if (self != 0 && ld32(c.mem, cs + 12) == self) {
        st32(c.mem, cs + 8, ld32(c.mem, cs + 8) + 1);
    } else {
        st32(c.mem, cs + 4, 0);
        st32(c.mem, cs + 8, 1);
        st32(c.mem, cs + 12, self);
    }
}

void EnterCriticalSection(Cpu& c) {
    const std::uint32_t cs = arg(c, 0);
    if (!critical_section_free(c, cs)) sched_block_until(c, critical_section_free, cs);
    take_critical_section(c, cs);
    ret_stdcall(c, 0, 4);
}

void LeaveCriticalSection(Cpu& c) {
    const std::uint32_t cs = arg(c, 0);
    const std::uint32_t recursion = ld32(c.mem, cs + 8);
    if (recursion > 1) {
        st32(c.mem, cs + 8, recursion - 1);
    } else {
        st32(c.mem, cs + 8, 0);
        st32(c.mem, cs + 12, 0);
        st32(c.mem, cs + 4, 0xFFFFFFFFu);
        sched_wake_waiters();
    }
    ret_stdcall(c, 0, 4);
}

void TryEnterCriticalSection(Cpu& c) {
    const std::uint32_t cs = arg(c, 0);
    const bool free = critical_section_free(c, cs);
    if (free) take_critical_section(c, cs);
    ret_stdcall(c, free ? 1u : 0u, 4);
}

void DeleteCriticalSection(Cpu& c) {
    std::memset(c.mem + arg(c, 0), 0, 24);
    sched_wake_waiters();
    ret_stdcall(c, 0, 4);
}

void TlsAlloc(Cpu& c) {
    std::uint32_t index = ld32(c.mem, kProcessHeap + kHeapTls);
    std::uint32_t result = 0xFFFFFFFFu;
    if (index < kTlsCount) {
        st32(c.mem, c.fs_base + kTlsSlots + index * 4, 0);
        st32(c.mem, kProcessHeap + kHeapTls, index + 1);
        result = index;
    }
    st32(c.mem, c.fs_base + kLastError, 0);
    ret_stdcall(c, result, 0);
}

void TlsGetValue(Cpu& c) {
    const std::uint32_t index = arg(c, 0);
    std::uint32_t value = 0;
    if (index < kTlsCount) {
        value = ld32(c.mem, c.fs_base + kTlsSlots + index * 4);
        st32(c.mem, c.fs_base + kLastError, 0);
    }
    ret_stdcall(c, value, 4);
}

void TlsSetValue(Cpu& c) {
    const std::uint32_t index = arg(c, 0);
    const std::uint32_t value = arg(c, 1);
    std::uint32_t ok = 0;
    if (index < kTlsCount) {
        st32(c.mem, c.fs_base + kTlsSlots + index * 4, value);
        st32(c.mem, c.fs_base + kLastError, 0);
        ok = 1;
    }
    ret_stdcall(c, ok, 8);
}

void InterlockedIncrement(Cpu& c) {
    const std::uint32_t p = arg(c, 0);
    std::uint32_t old = atomic_rmw<std::uint32_t>(c.mem, p, [](std::uint32_t v) { return v + 1; });
    ret_stdcall(c, old + 1, 4);
}

void InterlockedDecrement(Cpu& c) {
    const std::uint32_t p = arg(c, 0);
    std::uint32_t old = atomic_rmw<std::uint32_t>(c.mem, p, [](std::uint32_t v) { return v - 1; });
    ret_stdcall(c, old - 1, 4);
}

void InterlockedExchange(Cpu& c) {
    const std::uint32_t p = arg(c, 0);
    const std::uint32_t value = arg(c, 1);
    const std::uint32_t old = atomic_rmw<std::uint32_t>(c.mem, p, [value](std::uint32_t) { return value; });
    ret_stdcall(c, old, 8);
}

// US English. The CRT asks for these when its locale block has no code page yet.
constexpr std::uint32_t kAcp = 1252;
constexpr std::uint32_t kOemCp = 437;

void GetACP(Cpu& c) { ret_stdcall(c, kAcp, 0); }

// Matches guest_cpuid: SSE2 is present, SSE4.2 and AVX are not, and fast-fail
// (feature 23) is absent so the CRT's int 0x29 paths stay skipped.
std::uint32_t unhandled_filter = 0;

void SetUnhandledExceptionFilter(Cpu& c) {
    const std::uint32_t next = arg(c, 0);
    const std::uint32_t prev = unhandled_filter;
    unhandled_filter = next;
    ret_stdcall(c, prev, 4);
}

void IsProcessorFeaturePresent(Cpu& c) {
    switch (arg(c, 0)) {
    case 2:   // PF_COMPARE_EXCHANGE_DOUBLE
    case 3:   // PF_MMX_INSTRUCTIONS_AVAILABLE
    case 6:   // PF_XMMI_INSTRUCTIONS_AVAILABLE
    case 8:   // PF_RDTSC_INSTRUCTION_AVAILABLE
    case 10:  // PF_XMMI64_INSTRUCTIONS_AVAILABLE
        ret_stdcall(c, 1, 4);
        return;
    default:
        ret_stdcall(c, 0, 4);
    }
}

// 100 ns ticks since 1601-01-01. Callers convert this to a Unix time by adding
// a fixed bias, so a zero value would come out centuries off.
// DCUSA_FIXED_CLOCK=1: the wall clock (date and time of day) stands still at
// 2020-01-01 12:00:00 UTC, so whatever the game seeds from it repeats from run
// to run (attract mode as a test). The steady clocks pacing uses stay real.
bool fixed_clock() {
    static const bool on = [] {
        const char* v = std::getenv("DCUSA_FIXED_CLOCK");
        return v != nullptr && std::atoi(v) != 0;
    }();
    return on;
}
constexpr std::time_t kFixedClock = 1577880000;  // 2020-01-01 12:00:00 UTC

void GetSystemTimeAsFileTime(Cpu& c) {
    const std::uint32_t out = arg(c, 0);
    using namespace std::chrono;
    const auto since_unix = fixed_clock() ? system_clock::from_time_t(kFixedClock).time_since_epoch() : system_clock::now().time_since_epoch();
    const auto ticks = duration_cast<duration<std::uint64_t, std::ratio<1, 10000000>>>(since_unix).count();
    constexpr std::uint64_t kUnixBias = 11644473600ull * 10000000ull;
    const auto file_time = ticks + kUnixBias;
    st32(c.mem, out, static_cast<std::uint32_t>(file_time));
    st32(c.mem, out + 4, static_cast<std::uint32_t>(file_time >> 32));
    ret_stdcall(c, 0, 4);
}

void GetOEMCP(Cpu& c) { ret_stdcall(c, kOemCp, 0); }

void IsValidCodePage(Cpu& c) {
    const std::uint32_t cp = arg(c, 0);
    ret_stdcall(c, cp == kAcp || cp == kOemCp ? 1u : 0u, 4);
}

// CPINFO: MaxCharSize, DefaultChar[2], LeadByte[12]. Both pages are single-byte
// with no lead bytes, which is the path the CRT's table builder expects.
void GetCPInfo(Cpu& c) {
    const std::uint32_t cp = arg(c, 0);
    const std::uint32_t info = arg(c, 1);
    std::uint32_t ok = 0;
    if (cp == kAcp || cp == kOemCp) {
        std::memset(c.mem + info, 0, 18);
        st32(c.mem, info, 1);
        c.mem[info + 4] = static_cast<std::uint8_t>('?');
        ok = 1;
    }
    ret_stdcall(c, ok, 8);
}

// Windows-1252 differs from Latin-1 in 0x80..0x9F. The four undefined bytes
// stay as themselves so the conversion still succeeds.
constexpr std::uint16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

std::uint16_t decode_byte(std::uint32_t code_page, std::uint8_t b) {
    if (code_page == kAcp && b >= 0x80 && b <= 0x9F) return kCp1252High[b - 0x80];
    return b;
}

void MultiByteToWideChar(Cpu& c) {
    const std::uint32_t code_page = arg(c, 0);
    const std::uint32_t src = arg(c, 2);
    std::int32_t count = static_cast<std::int32_t>(arg(c, 3));
    const std::uint32_t dest = arg(c, 4);
    const std::int32_t dest_cap = static_cast<std::int32_t>(arg(c, 5));
    if (count < 0) {
        count = 0;
        for (std::uint32_t i = 0; i < (1u << 20); ++i) {
            ++count;
            if (c.mem[src + i] == 0) break;
        }
    }
    const auto need = static_cast<std::uint32_t>(count);
    if (dest == 0 || dest_cap == 0) {
        ret_stdcall(c, need, 24);
        return;
    }
    if (dest_cap < count) {
        ret_stdcall(c, 0, 24);
        return;
    }
    for (std::uint32_t i = 0; i < need; ++i)
        st16(c.mem, dest + i * 2, decode_byte(code_page, c.mem[src + i]));
    ret_stdcall(c, need, 24);
}

// CT_CTYPE1 bits. ASCII matches Windows; Latin-1 letters are classified, and
// anything else is left undefined.
std::uint16_t ctype1(std::uint16_t w) {
    constexpr std::uint16_t Upper = 0x0001, Lower = 0x0002, Digit = 0x0004, Space = 0x0008;
    constexpr std::uint16_t Punct = 0x0010, Cntrl = 0x0020, Blank = 0x0040, Xdigit = 0x0080;
    constexpr std::uint16_t Alpha = 0x0100, Defined = 0x0200;
    if (w == '\t' || w == '\n' || w == '\v' || w == '\f' || w == '\r') return Space | Cntrl | Blank | Defined;
    if (w < 0x20 || w == 0x7F) return Cntrl | Defined;
    if (w == ' ') return Space | Blank | Defined;
    if (w >= '0' && w <= '9') return Digit | Xdigit | Defined;
    if (w >= 'A' && w <= 'F') return Upper | Alpha | Xdigit | Defined;
    if (w >= 'G' && w <= 'Z') return Upper | Alpha | Defined;
    if (w >= 'a' && w <= 'f') return Lower | Alpha | Xdigit | Defined;
    if (w >= 'g' && w <= 'z') return Lower | Alpha | Defined;
    if (w <= 0x7E) return Punct | Defined;
    if ((w >= 0xC0 && w <= 0xD6) || (w >= 0xD8 && w <= 0xDE)) return Upper | Alpha | Defined;
    if (w == 0xDF || (w >= 0xE0 && w <= 0xF6) || (w >= 0xF8 && w <= 0xFE)) return Lower | Alpha | Defined;
    if (w == 0xA0) return Space | Blank | Defined;
    return 0;
}

std::uint16_t fold_case(std::uint16_t w, bool upper) {
    if (upper) {
        if (w >= 'a' && w <= 'z') return static_cast<std::uint16_t>(w - 0x20);
        if (w >= 0xE0 && w <= 0xFE && w != 0xF7) return static_cast<std::uint16_t>(w - 0x20);
        if (w == 0x0161 || w == 0x0153 || w == 0x017E) return static_cast<std::uint16_t>(w - 1);
        if (w == 0x00FF) return 0x0178;
        return w;
    }
    if (w >= 'A' && w <= 'Z') return static_cast<std::uint16_t>(w + 0x20);
    if (w >= 0xC0 && w <= 0xDE && w != 0xD7) return static_cast<std::uint16_t>(w + 0x20);
    if (w == 0x0160 || w == 0x0152 || w == 0x017D) return static_cast<std::uint16_t>(w + 1);
    if (w == 0x0178) return 0x00FF;
    return w;
}

// LCMAP_LOWERCASE is 0x100, LCMAP_UPPERCASE is 0x200. The CRT builds its case
// tables with those two. Other maps return 0.
void LCMapStringW(Cpu& c) {
    const std::uint32_t flags = arg(c, 1);
    const std::uint32_t src = arg(c, 2);
    std::int32_t count = static_cast<std::int32_t>(arg(c, 3));
    const std::uint32_t dest = arg(c, 4);
    const std::int32_t dest_cap = static_cast<std::int32_t>(arg(c, 5));
    const bool upper = (flags & 0x200u) != 0;
    const bool lower = (flags & 0x100u) != 0;
    if (upper == lower) {
        ret_stdcall(c, 0, 24);
        return;
    }
    if (count < 0) {
        count = 0;
        for (std::uint32_t i = 0; i < (1u << 20); ++i) {
            ++count;
            if (ld16(c.mem, src + i * 2) == 0) break;
        }
    }
    if (dest == 0 || dest_cap == 0) {
        ret_stdcall(c, static_cast<std::uint32_t>(count), 24);
        return;
    }
    if (count < 0 || dest_cap < count) {
        ret_stdcall(c, 0, 24);
        return;
    }
    for (std::int32_t i = 0; i < count; ++i) {
        const auto at = static_cast<std::uint32_t>(i) * 2;
        st16(c.mem, dest + at, fold_case(ld16(c.mem, src + at), upper));
    }
    ret_stdcall(c, static_cast<std::uint32_t>(count), 24);
}

void GetStringTypeW(Cpu& c) {
    const std::uint32_t kind = arg(c, 0);
    const std::uint32_t src = arg(c, 1);
    const std::int32_t count = static_cast<std::int32_t>(arg(c, 2));
    const std::uint32_t out = arg(c, 3);
    std::uint32_t ok = 0;
    if (kind == 1 && count > 0) {
        for (std::int32_t i = 0; i < count; ++i) st16(c.mem, out + static_cast<std::uint32_t>(i) * 2, ctype1(ld16(c.mem, src + static_cast<std::uint32_t>(i) * 2)));
        ok = 1;
    }
    ret_stdcall(c, ok, 16);
}

// The pseudohandle. Callers pass it to GetThreadTimes and do not close it.
void GetCurrentThread(Cpu& c) { ret_stdcall(c, 0xFFFFFFFEu, 0); }

// The CRT xors the two creation-time dwords into an encoded pointer and ignores
// the other three FILETIMEs. A zero creation time is a valid success.
// x86 SLIST_HEADER is 8 bytes: next pointer, then depth and sequence.
void InitializeSListHead(Cpu& c) {
    const std::uint32_t head = arg(c, 0);
    std::memset(c.mem + head, 0, 8);
    ret_stdcall(c, 0, 4);
}

// 100 ns ticks. QueryPerformanceFrequency reports the matching 10,000,000.
constexpr std::uint64_t kPerfFrequency = 10000000ull;

void QueryPerformanceCounter(Cpu& c) {
    const std::uint32_t out = arg(c, 0);
    using namespace std::chrono;
    auto ticks = duration_cast<duration<std::uint64_t, std::ratio<1, 10000000>>>(steady_clock::now().time_since_epoch()).count();
    if (ticks == 0) ticks = 1;
    st32(c.mem, out, static_cast<std::uint32_t>(ticks));
    st32(c.mem, out + 4, static_cast<std::uint32_t>(ticks >> 32));
    ret_stdcall(c, 1, 4);
}

void QueryPerformanceFrequency(Cpu& c) {
    const std::uint32_t out = arg(c, 0);
    st32(c.mem, out, static_cast<std::uint32_t>(kPerfFrequency));
    st32(c.mem, out + 4, static_cast<std::uint32_t>(kPerfFrequency >> 32));
    ret_stdcall(c, 1, 4);
}

// No config file is present. Windows copies the default and returns its length
// without the terminator. The startup readers parse that default.
// CreateEvent(attributes, manual reset, initially set, name). Names are not
// shared: every call is a new event. 0xC2D061 throws when the handle is null.
void CreateEventA(Cpu& c) { ret_stdcall(c, sched_create_event(arg(c, 1) != 0, arg(c, 2) != 0), 16); }

void CreateEventW(Cpu& c) { CreateEventA(c); }

void SetEvent(Cpu& c) {
    const bool ok = sched_set_event(arg(c, 0));
    set_error(c, ok ? 0u : 6u);
    ret_stdcall(c, ok ? 1u : 0u, 4);
}

void ResetEvent(Cpu& c) {
    const bool ok = sched_reset_event(arg(c, 0));
    set_error(c, ok ? 0u : 6u);
    ret_stdcall(c, ok ? 1u : 0u, 4);
}

// CreateSemaphore(attributes, initial, maximum, name); the Ex form adds flags
// and an access mask.
void CreateSemaphoreA(Cpu& c) { ret_stdcall(c, sched_create_semaphore(arg(c, 1), arg(c, 2)), 16); }

void CreateSemaphoreW(Cpu& c) { CreateSemaphoreA(c); }

void CreateSemaphoreExW(Cpu& c) { ret_stdcall(c, sched_create_semaphore(arg(c, 1), arg(c, 2)), 24); }

void ReleaseSemaphore(Cpu& c) {
    std::uint32_t previous = 0;
    const bool ok = sched_release_semaphore(arg(c, 0), arg(c, 1), &previous);
    if (arg(c, 2) != 0) st32(c.mem, arg(c, 2), previous);
    set_error(c, ok ? 0u : 298u);  // ERROR_TOO_MANY_POSTS
    ret_stdcall(c, ok ? 1u : 0u, 12);
}

void CreateMutexA(Cpu& c) { ret_stdcall(c, sched_create_mutex(c, arg(c, 1) != 0), 12); }

void ReleaseMutex(Cpu& c) {
    const bool ok = sched_release_mutex(c, arg(c, 0));
    set_error(c, ok ? 0u : 288u);  // ERROR_NOT_OWNER
    ret_stdcall(c, ok ? 1u : 0u, 4);
}

// 0x48CDCB asks Credit/Freeplay and passes 0. A missing file or key returns that
// default. A mounted install is read, and never written here.
void GetPrivateProfileIntA(Cpu& c) {
    const std::uint32_t fallback = arg(c, 2);
    std::uint32_t value = fallback;
    const bool log = std::getenv("DCUSA_STATS") != nullptr;
    std::string section, key, file;
    if (log && arg(c, 0) && arg(c, 1)) {
        section = guest_ascii(c.mem, arg(c, 0), false);
        key = guest_ascii(c.mem, arg(c, 1), false);
        if (arg(c, 3)) file = guest_ascii(c.mem, arg(c, 3), false);
    }
    if (guest_files_ready() && arg(c, 0) != 0 && arg(c, 1) != 0 && arg(c, 3) != 0) {
        const auto got = guest_ini_get(guest_ascii(c.mem, arg(c, 3), false), guest_ascii(c.mem, arg(c, 0), false),
                                        guest_ascii(c.mem, arg(c, 1), false));
        if (got) {
            const char* s = got->c_str();
            while (*s == ' ' || *s == '\t') ++s;
            int sign = 1;
            if (*s == '-') {
                sign = -1;
                ++s;
            }
            int base = 10;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                base = 16;
                s += 2;
            }
            std::uint32_t n = 0;
            bool digit = false;
            for (; *s; ++s) {
                int d = -1;
                if (*s >= '0' && *s <= '9') d = *s - '0';
                else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
                else break;
                digit = true;
                n = n * static_cast<std::uint32_t>(base) + static_cast<std::uint32_t>(d);
            }
            if (digit) value = static_cast<std::uint32_t>(sign < 0 ? -static_cast<std::int32_t>(n) : static_cast<std::int32_t>(n));
        }
    }
    if (log) {
        static int shown = 0;
        if (shown < 16) {
            ++shown;
            std::fprintf(stderr, "ini [%s] %s=%u file %s\n", section.c_str(), key.c_str(), value, file.c_str());
        }
    }
    ret_stdcall(c, value, 16);
}

void copy_profile_default(Cpu& c, std::uint32_t fallback, std::uint32_t out, std::uint32_t cap) {
    std::uint32_t n = 0;
    if (fallback != 0) {
        while (n + 1 < cap && n < 65536u && c.mem[fallback + n] != 0) {
            c.mem[out + n] = c.mem[fallback + n];
            ++n;
        }
    }
    c.mem[out + n] = 0;
    ret_stdcall(c, n, 24);
}

void GetPrivateProfileStringA(Cpu& c) {
    const std::uint32_t fallback = arg(c, 2);
    const std::uint32_t out = arg(c, 3);
    const std::uint32_t cap = arg(c, 4);
    if (cap == 0) {
        ret_stdcall(c, 0, 24);
        return;
    }
    if (guest_files_ready() && arg(c, 0) != 0 && arg(c, 1) != 0 && arg(c, 5) != 0) {
        const auto got = guest_ini_get(guest_ascii(c.mem, arg(c, 5), false), guest_ascii(c.mem, arg(c, 0), false),
                                        guest_ascii(c.mem, arg(c, 1), false));
        if (got) {
            std::uint32_t n = 0;
            while (n + 1 < cap && n < got->size()) {
                c.mem[out + n] = static_cast<std::uint8_t>((*got)[n]);
                ++n;
            }
            c.mem[out + n] = 0;
            ret_stdcall(c, n, 24);
            return;
        }
    }
    copy_profile_default(c, fallback, out, cap);
}

void GetThreadTimes(Cpu& c) {
    const std::uint32_t thread = arg(c, 0);
    if (thread != 0xFFFFFFFEu) {
        ret_stdcall(c, 0, 20);
        return;
    }
    auto write_zero = [&](std::uint32_t p) {
        if (p != 0) {
            st32(c.mem, p, 0);
            st32(c.mem, p + 4, 0);
        }
    };
    write_zero(arg(c, 1));
    write_zero(arg(c, 2));
    write_zero(arg(c, 3));
    write_zero(arg(c, 4));
    ret_stdcall(c, 1, 20);
}

void GetCurrentThreadId(Cpu& c) { ret_stdcall(c, ld32(c.mem, c.fs_base + 0x24), 0); }

void GetLastError(Cpu& c) { ret_stdcall(c, ld32(c.mem, c.fs_base + kLastError), 0); }

void SetLastError(Cpu& c) {
    st32(c.mem, c.fs_base + kLastError, arg(c, 0));
    ret_stdcall(c, 0, 4);
}

// No console is attached. The CRT marks file descriptors 0, 1 and 2 invalid and
// continues, which is what a headless run wants.
void GetStdHandle(Cpu& c) { ret_stdcall(c, 0, 4); }

void GetCommandLineA(Cpu& c) {
    std::memcpy(c.mem + kCommandLine, kExeName, sizeof(kExeName));
    ret_stdcall(c, kCommandLine, 0);
}

void GetCommandLineW(Cpu& c) {
    for (unsigned i = 0; i < sizeof(kExeName); ++i) st16(c.mem, kCommandLineW + i * 2, static_cast<std::uint8_t>(kExeName[i]));
    ret_stdcall(c, kCommandLineW, 0);
}

// S_OK. The startup call is CoInitializeEx(NULL, COINIT_MULTITHREADED) and it
// does not test the HRESULT. CoUninitialize is the matching shutdown.
void CoInitialize(Cpu& c) { ret_stdcall(c, 0, 4); }

void CoInitializeEx(Cpu& c) { ret_stdcall(c, 0, 8); }

void CoUninitialize(Cpu& c) { ret_stdcall(c, 0, 0); }

// REGDB_E_CLASSNOTREG. 0x85B250 drops the pointer when the HRESULT is negative.
// Nothing is registered: WIC and the camera stay closed.
void CoCreateInstance(Cpu& c) {
    const std::uint32_t out = arg(c, 4);
    if (out != 0) st32(c.mem, out, 0);
    ret_stdcall(c, 0x80040154u, 20);
}

// TIMERR_NOERROR is 0. The startup call asks for a 1 ms period and keeps going
// either way. timeGetTime is milliseconds.
void timeBeginPeriod(Cpu& c) { ret_stdcall(c, 0, 4); }

void timeEndPeriod(Cpu& c) { ret_stdcall(c, 0, 4); }

void timeGetTime(Cpu& c) {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    ret_stdcall(c, static_cast<std::uint32_t>(ms), 0);
}

void timeKillEvent(Cpu& c) { ret_stdcall(c, sched_kill_timer(arg(c, 0)) ? 0u : 97u, 4); }

// timeSetEvent(delay, resolution, callback, user, flags). 0x59DD26 passes 0x21:
// TIME_PERIODIC plus TIME_CALLBACK_EVENT_PULSE, so the callback is an
// auto-reset event pulsed on the tick. A function callback would need a timer
// thread and fails closed. 0 is failure.
void timeSetEvent(Cpu& c) {
    const std::uint32_t delay = arg(c, 0);
    const std::uint32_t callback = arg(c, 2);
    const std::uint32_t flags = arg(c, 4);
    if ((flags & 0x30u) == 0) fail_closed(Unported::Import, callback, "timeSetEvent with a function callback");
    const std::uint32_t id = sched_timer_event(delay, callback, (flags & 1u) != 0, (flags & 0x20u) != 0);
    if (id == 0) fail_closed(Unported::Import, callback, "timeSetEvent on a handle that is not a matching event");
    ret_stdcall(c, id, 20);
}

std::uint32_t wchar_len(const std::uint8_t* mem, std::uint32_t p) {
    std::uint32_t n = 0;
    while (n < 0x8000u && ld16(mem, p + n * 2) != 0) ++n;
    return n;
}

void set_error(Cpu& c, std::uint32_t err) { st32(c.mem, c.fs_base + kLastError, err); }

std::string guest_ascii(const std::uint8_t* mem, std::uint32_t va, bool wide) {
    std::string s;
    if (va == 0) return s;
    for (std::uint32_t i = 0; i < 1024; ++i) {
        std::uint32_t ch = wide ? ld16(mem, va + i * 2) : ld8(mem, va + i);
        if (ch == 0) break;
        s.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return s;
}

void write_guest(std::uint8_t* mem, std::uint32_t va, std::uint32_t cap, const std::string& text, bool wide) {
    if (va == 0 || cap == 0) return;
    const std::uint32_t n = static_cast<std::uint32_t>(text.size()) < cap ? static_cast<std::uint32_t>(text.size()) : cap - 1;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (wide) st16(mem, va + i * 2, static_cast<std::uint8_t>(text[i]));
        else mem[va + i] = static_cast<std::uint8_t>(text[i]);
    }
    if (wide) st16(mem, va + n * 2, 0);
    else mem[va + n] = 0;
}

void write_find_w(std::uint8_t* mem, std::uint32_t va, const GuestDirEntry& e) {
    if (va == 0) return;
    std::memset(mem + va, 0, 592);
    st32(mem, va, e.attributes);
    st32(mem, va + 28, static_cast<std::uint32_t>(e.size >> 32));
    st32(mem, va + 32, static_cast<std::uint32_t>(e.size));
    write_guest(mem, va + 44, 260, e.name, true);
}

void write_find_a(std::uint8_t* mem, std::uint32_t va, const GuestDirEntry& e) {
    if (va == 0) return;
    std::memset(mem + va, 0, 320);
    st32(mem, va, e.attributes);
    st32(mem, va + 28, static_cast<std::uint32_t>(e.size >> 32));
    st32(mem, va + 32, static_cast<std::uint32_t>(e.size));
    write_guest(mem, va + 44, 260, e.name, false);
}

void write_attr(std::uint8_t* mem, std::uint32_t va, const GuestFileInfo& info) {
    if (va == 0) return;
    std::memset(mem + va, 0, 36);
    st32(mem, va, info.attributes);
    st32(mem, va + 28, static_cast<std::uint32_t>(info.size >> 32));
    st32(mem, va + 32, static_cast<std::uint32_t>(info.size));
}

// Strips the last component. A name with no slash becomes empty, which is the
// base case the directory walk treats as an existing root.
void PathRemoveFileSpecW(Cpu& c) {
    const std::uint32_t path = arg(c, 0);
    const std::uint32_t n = wchar_len(c.mem, path);
    int sep = -1;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint16_t w = ld16(c.mem, path + i * 2);
        if (w == '\\' || w == '/') sep = static_cast<int>(i);
    }
    std::uint32_t changed = 0;
    if (n != 0 && sep < 0) {
        st16(c.mem, path, 0);
        changed = 1;
    } else if (sep == 0) {
        st16(c.mem, path + 2, 0);
        changed = 1;
    } else if (sep > 0) {
        const auto at = static_cast<std::uint32_t>(sep);
        const bool drive = at == 2 && ld16(c.mem, path + 2) == ':';
        st16(c.mem, path + (drive ? at + 1 : at) * 2, 0);
        changed = 1;
    }
    ret_stdcall(c, changed, 4);
}

// An empty string and a drive root are directories, so the parent walk in
// 0x6C0210 stops. Any other path is a directory only when the install has it.
// Wine's rule: strip file specs until the path is a root. "D:\\a\\b" -> "D:\\",
// "\\a" -> "\\", "\\\\server\\share\\a" -> "\\\\server\\share". A relative path has no
// root: it ends up empty and the result is FALSE.
void PathStripToRootW(Cpu& c) {
    const std::uint32_t path = arg(c, 0);
    const std::uint32_t n = wchar_len(c.mem, path);
    auto at = [&](std::uint32_t i) { return i < n ? ld16(c.mem, path + i * 2) : std::uint16_t(0); };
    auto sep = [&](std::uint32_t i) { return at(i) == '\\' || at(i) == '/'; };
    std::uint32_t keep = 0;
    if (n >= 2 && at(1) == ':') {
        keep = sep(2) ? 3 : 2;
    } else if (sep(0) && sep(1)) {
        std::uint32_t i = 2;
        for (int parts = 0; i < n; ++i) {
            if (sep(i) && ++parts == 2) break;
        }
        keep = i;
    } else if (sep(0)) {
        keep = 1;
    }
    st16(c.mem, path + keep * 2, 0);
    ret_stdcall(c, keep != 0 ? 1u : 0u, 4);
}

// GetDiskFreeSpaceW(root, sectors per cluster, bytes per sector, free clusters,
// total clusters). The container is on the host disk; a fixed 64 GB free of
// 128 GB keeps the answer reproducible.
void GetDiskFreeSpaceW(Cpu& c) {
    const std::uint32_t outs[4] = {arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)};
    const std::uint32_t values[4] = {8u, 512u, 0x01000000u, 0x02000000u};
    for (int i = 0; i < 4; ++i)
        if (outs[i] != 0) st32(c.mem, outs[i], values[i]);
    set_error(c, 0);
    ret_stdcall(c, 1, 20);
}

void PathIsDirectoryW(Cpu& c) {
    const std::uint32_t path = arg(c, 0);
    const std::uint32_t n = wchar_len(c.mem, path);
    std::uint32_t yes = 0;
    if (n == 0) yes = 1;
    else if (n == 1 && (ld16(c.mem, path) == '\\' || ld16(c.mem, path) == '/')) yes = 1;
    else if (n >= 2 && n <= 3 && ld16(c.mem, path + 2) == ':') yes = 1;
    else if (guest_files_ready()) {
        GuestFileInfo info;
        if (guest_stat(guest_ascii(c.mem, path, true), &info) == 0 && info.is_dir) yes = 1;
    }
    ret_stdcall(c, yes, 4);
}

// Joins a relative piece onto a path, inserting one backslash. A result that
// would pass MAX_PATH is left unchanged and returns 0; the caller checks for 1.
void PathAppendW(Cpu& c) {
    const std::uint32_t path = arg(c, 0);
    const std::uint32_t more = arg(c, 1);
    const std::uint32_t n = wchar_len(c.mem, path);
    const std::uint32_t m = wchar_len(c.mem, more);
    const std::uint16_t head = m == 0 ? 0 : ld16(c.mem, more);
    const bool absolute = head == '\\' || head == '/' || (m >= 2 && ld16(c.mem, more + 2) == ':');
    auto put = [&](std::uint32_t src, std::uint32_t count) {
        if (count >= 260) return false;
        for (std::uint32_t i = 0; i < count; ++i) st16(c.mem, path + i * 2, ld16(c.mem, src + i * 2));
        st16(c.mem, path + count * 2, 0);
        return true;
    };
    std::uint32_t ok = 0;
    if (absolute) {
        if (put(more, m)) ok = 1;
    } else {
        const bool slash = n > 0 && ld16(c.mem, path + (n - 1) * 2) != '\\' && ld16(c.mem, path + (n - 1) * 2) != '/';
        const std::uint32_t total = n + m + (slash ? 1u : 0u);
        if (total < 260) {
            std::uint32_t at = n;
            if (slash) st16(c.mem, path + at++ * 2, '\\');
            for (std::uint32_t i = 0; i < m; ++i) st16(c.mem, path + (at + i) * 2, ld16(c.mem, more + i * 2));
            st16(c.mem, path + (at + m) * 2, 0);
            ok = 1;
        }
    }
    ret_stdcall(c, ok, 8);
}

// Cleans '.' and '..' without touching the disk. 0x6C0F3A treats zero as failure.
void PathCanonicalizeW(Cpu& c) {
    const std::uint32_t dst = arg(c, 0);
    const std::uint32_t src = arg(c, 1);
    std::uint16_t in[520];
    std::uint32_t n = 0;
    while (n < 519) {
        const std::uint16_t w = ld16(c.mem, src + n * 2);
        in[n++] = w;
        if (w == 0) break;
    }
    if (in[n - 1] != 0) {
        ret_stdcall(c, 0, 8);
        return;
    }
    --n;
    std::uint16_t out[260];
    std::uint32_t at = 0;
    std::uint32_t i = 0;
    std::uint32_t root = 0;
    if (n >= 2 && in[1] == ':') {
        out[at++] = in[0];
        out[at++] = ':';
        i = 2;
    }
    if (i < n && (in[i] == '\\' || in[i] == '/')) {
        out[at++] = '\\';
        ++i;
    }
    root = at;
    std::uint32_t mark[64];
    int depth = 0;
    bool ok = true;
    while (i <= n && ok) {
        std::uint32_t b = i;
        while (i < n && in[i] != '\\' && in[i] != '/') ++i;
        const std::uint32_t len = i - b;
        const bool dot = len == 1 && in[b] == '.';
        const bool dotdot = len == 2 && in[b] == '.' && in[b + 1] == '.';
        if (len != 0 && !dot) {
            if (dotdot) {
                if (depth > 0) at = mark[--depth];
            } else if (at + (at > root ? 1u : 0u) + len >= 260) {
                ok = false;
            } else {
                if (at > root) out[at++] = '\\';
                if (depth < 64) mark[depth++] = at;
                for (std::uint32_t k = 0; k < len; ++k) out[at++] = in[b + k];
            }
        }
        if (i < n) ++i;
        else break;
    }
    if (!ok) {
        ret_stdcall(c, 0, 8);
        return;
    }
    if (at == 0) out[at++] = '.';
    out[at] = 0;
    for (std::uint32_t k = 0; k <= at; ++k) st16(c.mem, dst + k * 2, out[k]);
    ret_stdcall(c, 1, 8);
}

void PathFileExistsW(Cpu& c) {
    std::uint32_t yes = 0;
    if (guest_files_ready() && arg(c, 0) != 0) {
        GuestFileInfo info;
        if (guest_stat(guest_ascii(c.mem, arg(c, 0), true), &info) == 0 && info.exists) yes = 1;
    }
    ret_stdcall(c, yes, 4);
}

void PathFileExistsA(Cpu& c) {
    std::uint32_t yes = 0;
    if (guest_files_ready() && arg(c, 0) != 0) {
        GuestFileInfo info;
        if (guest_stat(guest_ascii(c.mem, arg(c, 0), false), &info) == 0 && info.exists) yes = 1;
    }
    ret_stdcall(c, yes, 4);
}

// The caller compares the result with 1. With no install mounted, any non-null
// path is reported as created and the string is not read.
void CreateDirectoryW(Cpu& c) {
    const std::uint32_t path = arg(c, 0);
    if (!guest_files_ready()) {
        ret_stdcall(c, path == 0 ? 0u : 1u, 8);
        return;
    }
    const std::uint32_t err = path == 0 ? 3u : guest_mkdir(guest_ascii(c.mem, path, true));
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 8);
}

// The display counter. There is no cursor to draw; the count is what callers read.
std::int32_t cursor_count = 0;

// Ordinal 115. The caller at 0x529E02 requests 2.2 and treats any non-zero as failure.
// The 0x194-byte frame holds a WSADATA (398 bytes) plus the cookie, so only the
// version words and a short description are written.
void WSAStartup(Cpu& c) {
    const std::uint32_t data = arg(c, 1);
    if (data != 0) {
        st16(c.mem, data, 0x0202);
        st16(c.mem, data + 2, 0x0202);
        const char desc[] = "WinSock 2.0";
        for (std::uint32_t i = 0; i < sizeof(desc); ++i) st8(c.mem, data + 4 + i, static_cast<std::uint8_t>(desc[i]));
        st16(c.mem, data + 390, 0);
        st16(c.mem, data + 392, 0);
        st32(c.mem, data + 394, 0);
    }
    ret_stdcall(c, 0, 8);
}

// CreateProcessA. After boot, 0x5283E0 starts c:\\sega\\shell\\InpWrapper.exe, the
// parallel-port helper for the cabinet's force-feedback motor, and skips it when
// the call fails. No process is ever started here: FALSE, ERROR_FILE_NOT_FOUND.
void CreateProcessA(Cpu& c) {
    if (std::getenv("DCUSA_STATS") != nullptr)
        std::fprintf(stderr, "CreateProcessA %s (not started)\n",
                     arg(c, 0) ? guest_ascii(c.mem, arg(c, 0), false).c_str() : "(null)");
    set_error(c, 2);
    ret_stdcall(c, 0, 40);
}

// closesocket (#3). The socket was never a host socket.
void ws_closesocket(Cpu& c) { ret_stdcall(c, 0, 4); }

// WSACleanup. No host socket was ever opened, so there is nothing to release.
void WSACleanup(Cpu& c) { ret_stdcall(c, 0, 0); }

// Ordinal 23. The loop at 0x529E23 stores the handle and does not test it here.
// INVALID_SOCKET is -1, so the fake handle stays in 1..0xFFFFFFFE.
std::uint32_t next_socket = 1;

// Ordinal 11. "127.0.0.1" at 0x54058A is stored and used as a network-order address.
std::uint32_t swap16(std::uint32_t v) { return ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu); }

std::uint32_t swap32(std::uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
}

// Ordinals 9, 15, and 8. 0x529EA6 swaps port 0x6978 into a sockaddr_in.
// Ordinals 2 and 13. 0x529EDC binds 192.168.1.x:27000 and treats non-zero as failure.
// No host socket is opened; accept is still unanswered until a caller reaches it.
// Ordinal 4. 0x52A608 retries while the result is non-zero, so 0 skips that loop
// (the shell link on loopback). No host connection is opened.
// Ordinal 10. 0x52BDFD passes FIONBIO and a zero, and ignores the result.
// Ordinal 21. 0x52A3A1 sets SO_RCVTIMEO to 30000 ms and returns this result to its caller.
// Ordinal 19. Callers treat -1 as failure and compare a full send against the length
// (0x52AD67). Nothing is transmitted.
// The cabinet check at 0x577622 accepts 127.0.0.1. The translated call passes the
// address word itself, because the inet_ntoa in front of it was not emitted.
// A real string pointer still parses. This word is the one that word produces.
std::uint32_t wsa_error = 0;
std::uint32_t shell_socket = 0;
// Packets the shell has for the game, oldest first (src/recomp/shell.hpp).
std::deque<std::vector<std::uint8_t>> shell_inbox;
bool shell_greeted = false;

std::vector<std::uint8_t> dyt_packet(std::uint16_t id, const std::uint8_t* payload, std::uint16_t len) {
    std::vector<std::uint8_t> p = {'D', 'Y', 'T', static_cast<std::uint8_t>(id >> 8), static_cast<std::uint8_t>(id),
                                   static_cast<std::uint8_t>(len >> 8), static_cast<std::uint8_t>(len), 0, 0, 0, 0};
    p.insert(p.end(), payload, payload + len);
    return p;
}

constexpr std::uint32_t kHostent = 0x36110000u;

bool commit_hostent(std::uint8_t* mem) {
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mem) & ~std::uintptr_t{0xFFF};
    const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(mem + kHostent);
    const std::uintptr_t lo = addr & ~std::uintptr_t{0xFFF};
    const std::uintptr_t hi = (addr + 0x200u + 0xFFFu) & ~std::uintptr_t{0xFFF};
    if (lo < base) return false;
    return mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE) == 0;
}

void write_hostent(std::uint8_t* mem) {
    const char* name = "localhost";
    for (std::uint32_t i = 0; name[i] != 0; ++i) st8(mem, kHostent + 0x100 + i, static_cast<std::uint8_t>(name[i]));
    st8(mem, kHostent + 0x100 + 9, 0);
    const char* ip = "127.0.0.1";
    for (std::uint32_t i = 0; ip[i] != 0; ++i) st8(mem, kHostent + 0x140 + i, static_cast<std::uint8_t>(ip[i]));
    st8(mem, kHostent + 0x140 + 9, 0);
    st32(mem, kHostent + 0, kHostent + 0x100);
    st32(mem, kHostent + 4, 0);
    st16(mem, kHostent + 8, 2);
    st16(mem, kHostent + 10, 4);
    st32(mem, kHostent + 12, kHostent + 0x20);
    st32(mem, kHostent + 0x20, kHostent + 0x30);
    st32(mem, kHostent + 0x24, 0);
    st8(mem, kHostent + 0x30, 0x7F);
    st8(mem, kHostent + 0x31, 0);
    st8(mem, kHostent + 0x32, 0);
    st8(mem, kHostent + 0x33, 1);
}

// Ordinal 57. 0x5775F8 and 0x5641A8 both continue only when this is zero.
// The name is localhost so the following lookup is the loopback address.
void ws_gethostname(Cpu& c) {
    const std::uint32_t name = arg(c, 0);
    const std::uint32_t cap = arg(c, 1);
    const char text[] = "localhost";
    if (name != 0 && cap > sizeof(text)) {
        for (std::uint32_t i = 0; i < sizeof(text); ++i) st8(c.mem, name + i, static_cast<std::uint8_t>(text[i]));
    }
    ret_stdcall(c, name != 0 && cap > sizeof(text) ? 0u : 0xFFFFFFFFu, 8);
}

// Ordinal 52. Both callers read a hostent and take the first address.
void ws_gethostbyname(Cpu& c) {
    if (!commit_hostent(c.mem)) {
        ret_stdcall(c, 0, 4);
        return;
    }
    write_hostent(c.mem);
    ret_stdcall(c, kHostent, 4);
}

// Ordinal 12. The argument is the address word. 0x5641CA turns it back into text.
void ws_inet_ntoa(Cpu& c) {
    const std::uint32_t addr = arg(c, 0);
    if (!commit_hostent(c.mem)) {
        ret_stdcall(c, 0, 4);
        return;
    }
    const unsigned a = addr & 0xFFu;
    const unsigned b = (addr >> 8) & 0xFFu;
    const unsigned d = (addr >> 16) & 0xFFu;
    const unsigned e = (addr >> 24) & 0xFFu;
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", a, b, d, e);
    for (std::uint32_t i = 0; i < 16; ++i) st8(c.mem, kHostent + 0x140 + i, static_cast<std::uint8_t>(text[i]));
    ret_stdcall(c, kHostent + 0x140, 4);
}

// Ordinal 111. 0x584C6C treats 0x2733 as "try again".
void ws_last_error(Cpu& c) { ret_stdcall(c, wsa_error, 0); }

// Ordinal 151. 0x584BA8 asks whether the shell socket is still in the set.
void ws_fd_isset(Cpu& c) {
    const std::uint32_t sock = arg(c, 0);
    const std::uint32_t set = arg(c, 1);
    std::uint32_t found = 0;
    if (set != 0) {
        const std::uint32_t count = ld32(c.mem, set);
        const std::uint32_t n = count < 64u ? count : 64u;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (ld32(c.mem, set + 4 + i * 4) == sock) found = 1;
        }
    }
    ret_stdcall(c, found, 8);
}

void ws_send(Cpu& c) {
    const std::uint32_t buf = arg(c, 1);
    const std::uint32_t n = arg(c, 2);
    // DYT packets: 'D' 'Y' 'T', id (2 bytes, big end first), payload length
    // (2), four zero bytes, payload. The first is the cabinet saying it is up;
    // the shell answers once with id 0x2C, the message that sets the ready byte
    // at +0x38. A 0x2A carries a security challenge, answered with 0x2E.
    if (buf != 0 && n >= 11 && c.mem[buf] == 'D' && c.mem[buf + 1] == 'Y' && c.mem[buf + 2] == 'T') {
        shell_socket = arg(c, 0);
        const std::uint16_t id = static_cast<std::uint16_t>(c.mem[buf + 3] << 8 | c.mem[buf + 4]);
        const std::uint16_t len = static_cast<std::uint16_t>(c.mem[buf + 5] << 8 | c.mem[buf + 6]);
        if (!shell_greeted) {
            shell_greeted = true;
            shell_inbox.push_back(dyt_packet(0x2C, nullptr, 0));
        }
        if (id == 0x2A && len == 4 && n >= 15) {
            const std::array<std::uint8_t, 4> challenge = {c.mem[buf + 11], c.mem[buf + 12], c.mem[buf + 13], c.mem[buf + 14]};
            const std::array<std::uint8_t, 4> answer = shell_answer(challenge);
            shell_inbox.push_back(dyt_packet(0x2E, answer.data(), 4));
        }
    }
    static int shown = 0;
    if (std::getenv("DCUSA_STATS") != nullptr && shown < 8 && buf != 0 && n != 0) {
        ++shown;
        const std::uint32_t take = n < 16u ? n : 16u;
        std::fprintf(stderr, "send %u", n);
        for (std::uint32_t i = 0; i < take; ++i) std::fprintf(stderr, " %02x", c.mem[buf + i]);
        std::fprintf(stderr, "\n");
    }
    ret_stdcall(c, n, 16);
}

// Ordinal 18, select. 0x66BB0D, 0x57DE98, 0x57DF9C and 0x584B8D treat a negative
// result as a socket error. Zero is a timeout: those callers skip the receive.
bool shell_fd(const std::uint8_t* mem, std::uint32_t set) {
    if (set == 0 || shell_socket == 0) return false;
    const std::uint32_t count = ld32(mem, set);
    const std::uint32_t n = count < 64u ? count : 64u;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (ld32(mem, set + 4 + i * 4) == shell_socket) return true;
    }
    return false;
}

// Ordinal 18. Zero is a timeout. The shell poll at 0x584B85 is the one socket
// that has a reply waiting; every other set stays quiet.
void ws_select(Cpu& c) {
    const std::uint32_t ready = !shell_inbox.empty() && shell_fd(c.mem, arg(c, 1)) ? 1u : 0u;
    ret_stdcall(c, ready, 20);
}

// Ordinal 16. The poll peeks 11 bytes twice and then reads them. A short read
// is "try again" so some other socket does not consume the shell packet.
void ws_recv(Cpu& c) {
    const std::uint32_t sock = arg(c, 0);
    const std::uint32_t buf = arg(c, 1);
    const std::uint32_t len = arg(c, 2);
    const std::uint32_t flags = arg(c, 3);
    if (shell_inbox.empty() || sock != shell_socket || buf == 0 || len == 0) {
        wsa_error = 0x2733;  // WSAEWOULDBLOCK
        ret_stdcall(c, 0xFFFFFFFFu, 16);
        return;
    }
    // A stream: MSG_PEEK (2) copies without consuming; a read takes what fits.
    std::vector<std::uint8_t>& front = shell_inbox.front();
    const std::uint32_t n = len < front.size() ? len : static_cast<std::uint32_t>(front.size());
    for (std::uint32_t i = 0; i < n; ++i) st8(c.mem, buf + i, front[i]);
    if ((flags & 2u) == 0) {
        front.erase(front.begin(), front.begin() + n);
        if (front.empty()) shell_inbox.pop_front();
    }
    ret_stdcall(c, n, 16);
}

void ws_setsockopt(Cpu& c) { ret_stdcall(c, 0, 20); }

void ws_ioctlsocket(Cpu& c) { ret_stdcall(c, 0, 12); }

// connect(s, sockaddr_in*, len). Loopback (the shell link, "127.0.0.1")
// connects; any other address is refused, as on a cabinet with no others on
// its network. The 2017 build's cabinet discovery (ping.cpp, master thread
// 0x576AD0) connects to 192.168.1.2 .. .8 and retries while that fails; had
// it connected, it waited for a reply that never came, flagged a network
// error and restarted discovery every frame. 2019 skips discovery
// ([Network] Enabled=0).
void ws_connect(Cpu& c) {
    const std::uint32_t addr = arg(c, 1);
    const bool loopback = addr != 0 && c.mem[addr + 4] == 127;
    if (!loopback) {
        wsa_error = 10061;  // WSAECONNREFUSED
        if (std::getenv("DCUSA_STATS") != nullptr) {
            static int shown = 0;
            if (shown++ < 8 && addr != 0)
                std::fprintf(stderr, "connect refused %u.%u.%u.%u:%u\n", c.mem[addr + 4], c.mem[addr + 5],
                             c.mem[addr + 6], c.mem[addr + 7], c.mem[addr + 2] << 8 | c.mem[addr + 3]);
        }
        ret_stdcall(c, 0xFFFFFFFFu, 12);
        return;
    }
    ret_stdcall(c, 0, 12);
}

void ws_bind(Cpu& c) { ret_stdcall(c, 0, 12); }

void ws_listen(Cpu& c) { ret_stdcall(c, 0, 8); }

// Ordinal 1, accept. Nothing ever connects. The 2017 build's NRT network
// thread (nrt_network_new_socket.cpp, 0x852A10) polls its listener: on an
// error it reports no connection, sleeps and asks again (0x84C9F2). 2019
// does not reach it.
void ws_accept(Cpu& c) {
    wsa_error = 0x2733;  // WSAEWOULDBLOCK
    ret_stdcall(c, 0xFFFFFFFFu, 12);
}

void ws_htons(Cpu& c) { ret_stdcall(c, swap16(arg(c, 0)), 4); }

void ws_ntohs(Cpu& c) { ret_stdcall(c, swap16(arg(c, 0)), 4); }

void ws_htonl(Cpu& c) { ret_stdcall(c, swap32(arg(c, 0)), 4); }

void ws_inet_addr(Cpu& c) {
    const std::uint32_t text = arg(c, 0);
    std::uint32_t parts[4] = {};
    int count = 0;
    std::uint32_t cur = 0;
    bool digit = false;
    bool ok = false;
    for (std::uint32_t i = 0; i < 64; ++i) {
        const std::uint8_t b = ld8(c.mem, text + i);
        if (b >= '0' && b <= '9') {
            digit = true;
            cur = cur * 10u + static_cast<std::uint32_t>(b - '0');
            if (cur > 255) break;
        } else if (b == '.') {
            if (!digit || count >= 3) break;
            parts[count++] = cur;
            cur = 0;
            digit = false;
        } else if (b == 0) {
            if (digit && count == 3) {
                parts[3] = cur;
                ok = true;
            }
            break;
        } else {
            break;
        }
    }
    std::uint32_t addr = 0xFFFFFFFFu;
    if (ok) addr = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    else if (text == 0x0100007Fu) addr = text;
    ret_stdcall(c, addr, 4);
}

void ws_socket(Cpu& c) {
    const std::uint32_t handle = next_socket++;
    if (next_socket == 0 || next_socket == 0xFFFFFFFFu) next_socket = 1;
    ret_stdcall(c, handle, 12);
}

void ShowCursor(Cpu& c) {
    cursor_count += arg(c, 0) ? 1 : -1;
    ret_stdcall(c, static_cast<std::uint32_t>(cursor_count), 4);
}

// 0x422167. The presenter has no caption, so the client rectangle is already the
// window size. The caller subtracts left from right and does not test the BOOL.
void AdjustWindowRect(Cpu& c) { ret_stdcall(c, 1, 12); }

void LoadIconW(Cpu& c) { ret_stdcall(c, 0x1001, 8); }

// No resource image. 0x42220F falls back to LoadIconW when this is zero.
void LoadImageW(Cpu& c) { ret_stdcall(c, 0, 24); }

void LoadCursorW(Cpu& c) { ret_stdcall(c, 0x1002, 8); }

// 0x42225B asks for BLACK_BRUSH (4) and stores it as the class background.
void GetStockObject(Cpu& c) { ret_stdcall(c, arg(c, 0) == 0 ? 1u : arg(c, 0), 4); }

// 0x774987 stores the font and selects it into a memory DC. No host font is built.
void CreateFontW(Cpu& c) { ret_stdcall(c, 0xF001, 56); }

void CreateCompatibleDC(Cpu& c) { ret_stdcall(c, 0xF002, 4); }

void SelectObject(Cpu& c) { ret_stdcall(c, 1, 8); }

// 0x774DD0 tears a font object down: DeleteDC of its memory DC, then
// DeleteObject of its HFONT. Neither holds a host resource.
void DeleteDC(Cpu& c) { ret_stdcall(c, arg(c, 0) != 0 ? 1u : 0u, 4); }

void DeleteObject(Cpu& c) { ret_stdcall(c, arg(c, 0) != 0 ? 1u : 0u, 4); }

// TEXTMETRICW at the caller's esi+8. A zero height would divide later, so the
// face is a 16-pixel ANSI font. No host font is measured.
void GetTextMetricsW(Cpu& c) {
    const std::uint32_t tm = arg(c, 1);
    if (tm != 0) {
        std::memset(c.mem + tm, 0, 60);
        st32(c.mem, tm + 0, 16);
        st32(c.mem, tm + 4, 12);
        st32(c.mem, tm + 8, 4);
        st32(c.mem, tm + 20, 8);
        st32(c.mem, tm + 24, 16);
        st32(c.mem, tm + 28, 400);
        st16(c.mem, tm + 44, 32);
        st16(c.mem, tm + 46, 126);
        st16(c.mem, tm + 48, 32);
        st16(c.mem, tm + 50, 32);
    }
    ret_stdcall(c, 1, 8);
}

void RegisterClassExW(Cpu& c) { ret_stdcall(c, 0xC001, 4); }

// 0x59E3F2 treats a zero atom as failure and leaves the window uncreated.
void RegisterClassA(Cpu& c) { ret_stdcall(c, 0xC001, 4); }

// 0x4222A6 stores this as the window. No host window is created.
void CreateWindowExW(Cpu& c) { ret_stdcall(c, 0x2001, 48); }

// 0x59E423 treats a null handle as failure. No host window is created.
void CreateWindowExA(Cpu& c) { ret_stdcall(c, 0x2001, 48); }

// 0x59E2FD tears down the helper window 0x59E423 made, once its last user is
// gone, then unregisters its class. No host window exists, so both succeed.
void DestroyWindow(Cpu& c) { ret_stdcall(c, arg(c, 0) != 0 ? 1u : 0u, 4); }

void UnregisterClassA(Cpu& c) { ret_stdcall(c, 1, 8); }

// 0x40E0A4 passes SW_HIDE or SW_SHOW and ignores the previous state.
void ShowWindow(Cpu& c) { ret_stdcall(c, 0, 8); }

// No host window. Zero means it was already enabled, which is what the caller stores.
void EnableWindow(Cpu& c) { ret_stdcall(c, 0, 8); }

// 0x6B9E09. The only window is the one CreateWindowExW returned.
void GetForegroundWindow(Cpu& c) { ret_stdcall(c, 0x2001, 0); }

// The process-id pointer at 0x6B9E07 is null. The thread id matches GetCurrentThreadId.
void GetWindowThreadProcessId(Cpu& c) {
    const std::uint32_t pid = arg(c, 1);
    if (pid != 0) st32(c.mem, pid, 1);
    ret_stdcall(c, 1, 8);
}

void AttachThreadInput(Cpu& c) { ret_stdcall(c, 1, 12); }

void SetForegroundWindow(Cpu& c) { ret_stdcall(c, 1, 4); }

bool nrt_class(const std::string& text) { return text.find("NRT") != std::string::npos; }

void FindWindowA(Cpu& c) {
    const bool ours = nrt_class(guest_ascii(c.mem, arg(c, 0), false)) || nrt_class(guest_ascii(c.mem, arg(c, 1), false));
    ret_stdcall(c, ours ? 0x2001u : 0u, 8);
}

void FindWindowW(Cpu& c) {
    const bool ours = nrt_class(guest_ascii(c.mem, arg(c, 0), true)) || nrt_class(guest_ascii(c.mem, arg(c, 1), true));
    ret_stdcall(c, ours ? 0x2001u : 0u, 8);
}

// The one window's long values, by index: GWL_WNDPROC (-4), GWL_STYLE (-16),
// GWL_EXSTYLE (-20), GWL_USERDATA (-21), GWL_ID (-12). 0x6A00E7 installs
// 0x6A0740 as GWL_WNDPROC and stores the previous value; the stub window starts
// with none. Style is WS_POPUP | WS_VISIBLE, a borderless full-screen window.
std::uint32_t window_long(std::int32_t index, std::uint32_t* set) {
    static std::uint32_t wndproc = 0, style = 0x90000000u, exstyle = 0, userdata = 0, id = 0;
    std::uint32_t* slot = nullptr;
    switch (index) {
    case -4: slot = &wndproc; break;
    case -16: slot = &style; break;
    case -20: slot = &exstyle; break;
    case -21: slot = &userdata; break;
    case -12: slot = &id; break;
    default: return 0xFFFFFFFFu;
    }
    const std::uint32_t old = *slot;
    if (set) *slot = *set;
    return old;
}

void SetWindowLongW(Cpu& c) {
    std::uint32_t value = arg(c, 2);
    const std::uint32_t old = window_long(static_cast<std::int32_t>(arg(c, 1)), &value);
    if (old == 0xFFFFFFFFu) fail_closed(Unported::Import, arg(c, 1), "SetWindowLongW of an unknown index");
    ret_stdcall(c, old, 12);
}

void GetWindowLongW(Cpu& c) {
    const std::uint32_t value = window_long(static_cast<std::int32_t>(arg(c, 1)), nullptr);
    if (value == 0xFFFFFFFFu) fail_closed(Unported::Import, arg(c, 1), "GetWindowLongW of an unknown index");
    ret_stdcall(c, value, 8);
}

// 0x6A0D95. The window has no frame and its origin is the screen origin, so the point
// is already in client coordinates. The caller reads the words and ignores the BOOL.
void ScreenToClient(Cpu& c) { ret_stdcall(c, 1, 8); }

// 0x422779 reads the point and does not test the BOOL. The cursor stays at the origin.
void GetCursorPos(Cpu& c) {
    const std::uint32_t pt = arg(c, 0);
    if (pt != 0) {
        st32(c.mem, pt, 0);
        st32(c.mem, pt + 4, 0);
    }
    ret_stdcall(c, 1, 4);
}

// 0x6A027A probes users 0..3. A non-zero result is "not connected" and the init
// still returns success. The cabinet's controls are the SEGA IO board, which the
// game reads through DirectInput (d3d11.cpp), so no XInput pad is ever reported:
// a pad here as well would reach the game twice.
void XInputGetState(Cpu& c) { ret_stdcall(c, 1167, 8); }

void write_msg(std::uint8_t* mem, std::uint32_t dst, const GuestMsg& msg) {
    st32(mem, dst, msg.hwnd);
    st32(mem, dst + 4, msg.message);
    st32(mem, dst + 8, msg.wparam);
    st32(mem, dst + 12, msg.lparam);
    st32(mem, dst + 16, 0);
    st32(mem, dst + 20, 0);
    st32(mem, dst + 24, 0);
}

bool take_message(std::uint8_t* mem, std::uint32_t dst, std::uint32_t hwnd, std::uint32_t lo, std::uint32_t hi, bool remove) {
    for (auto it = messages.begin(); it != messages.end(); ++it) {
        if (it->message != 0x12) {
            if (hwnd != 0 && it->hwnd != hwnd) continue;
            if (lo != 0 && it->message < lo) continue;
            if (hi != 0 && it->message > hi) continue;
        }
        if (dst != 0) write_msg(mem, dst, *it);
        if (remove) messages.erase(it);
        return true;
    }
    return false;
}

// 0x41ECF6 peeks with the filter open and PM_REMOVE. Zero leaves the frame in the
// loop. 0x41ED00 compares the message with 0x12, which is WM_QUIT.
void PeekMessageCommon(Cpu& c) {
    // The frame loop's poll is where the other guest threads get their time.
    sched_frame_yield(c);
    const bool got = take_message(c.mem, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4) != 0);
    ret_stdcall(c, got ? 1u : 0u, 20);
}

void PeekMessageW(Cpu& c) { PeekMessageCommon(c); }

void PeekMessageA(Cpu& c) { PeekMessageCommon(c); }

void PostMessageW(Cpu& c) {
    if (std::getenv("DCUSA_STATS") != nullptr) {
        static int shown = 0;
        if (shown++ < 20)
            std::fprintf(stderr, "PostMessage(%08x, %04x, %08x, %08x) from %08X\n", arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3),
                         ld32(c.mem, c.r[Cpu::ESP]));
    }
    messages.push_back(GuestMsg{arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)});
    ret_stdcall(c, 1, 16);
}

void PostQuitMessage(Cpu& c) {
    if (std::getenv("DCUSA_STATS") != nullptr)
        std::fprintf(stderr, "PostQuitMessage(%u) from %08X thread %u\n", arg(c, 0), ld32(c.mem, c.r[Cpu::ESP]),
                     ld32(c.mem, c.fs_base + 0x24));
    messages.push_back(GuestMsg{0, 0x12, arg(c, 0), 0});
    ret_stdcall(c, 0, 4);
}

void SendMessageW(Cpu& c) { PostMessageW(c); }

void SendMessageA(Cpu& c) { PostMessageW(c); }

void DispatchMessageW(Cpu& c) { ret_stdcall(c, arg(c, 0) == 0 ? 0u : ld32(c.mem, arg(c, 0) + 8), 4); }

void TranslateMessage(Cpu& c) { ret_stdcall(c, 0, 4); }

void GetClientRect(Cpu& c) {
    const std::uint32_t rect = arg(c, 1);
    if (rect != 0) {
        st32(c.mem, rect, 0);
        st32(c.mem, rect + 4, 0);
        std::uint32_t w = 0, h = 0;
        display_size(&w, &h);
        st32(c.mem, rect + 8, w);
        st32(c.mem, rect + 12, h);
    }
    ret_stdcall(c, 1, 8);
}

// PIX markers. 0x6B1BA0 and 0x6B1BC0 forward a color and a name, then ret 8,
// and neither reads EAX. EndEvent is a tail jump from 0x6B4AF0 and 0x6B4B00;
// those callers push nothing and do not adjust ESP afterwards.
void D3DPERF_BeginEvent(Cpu& c) { ret_stdcall(c, 0, 8); }
void D3DPERF_EndEvent(Cpu& c) { ret_stdcall(c, 0, 0); }

// 0x77AFF9 raises 0x406D1388, the Visual C++ thread-name exception. The function's
// own __except ignores it. Any other code is a real fault and stays fail-closed.
void RaiseException(Cpu& c) {
    const std::uint32_t code = arg(c, 0);
    if (code == 0x406D1388u) {
        ret_stdcall(c, 0, 16);
        return;
    }
    const std::uint32_t frame = c.r[Cpu::EBP];
    char buf[96];
    std::snprintf(buf, sizeof buf, "RaiseException %08X thrower %08X object %08X", code,
                  ld32(c.mem, frame + 4), ld32(c.mem, frame + 8));
    fail_closed(Unported::Import, code, buf);
}


// 0xC55FF9 treats zero as failure and throws. Priority is not applied.
void SetThreadPriority(Cpu& c) { ret_stdcall(c, 1, 8); }

// THREAD_PRIORITY_NORMAL. The sound server stores this and does not test it.
void GetThreadPriority(Cpu& c) { ret_stdcall(c, 0, 4); }

// 0xC375B1 treats -1 as failure.
void ResumeThread(Cpu& c) { ret_stdcall(c, sched_resume(arg(c, 0)), 4); }

void SuspendThread(Cpu& c) { ret_stdcall(c, sched_suspend(c, arg(c, 0)), 4); }

void SwitchToThread(Cpu& c) {
    sched_yield(c);
    ret_stdcall(c, 1, 0);
}

[[noreturn]] void ExitThread(Cpu& c) { throw GuestThreadExit{arg(c, 0)}; }

// DuplicateHandle(source process, source, target process, *target, access,
// inherit, options). The thread pseudo-handle becomes the thread's real handle;
// a kernel object keeps its handle, since closing a running thread's handle
// does not end it and objects are not reference counted.
void DuplicateHandle(Cpu& c) {
    const std::uint32_t source = arg(c, 1);
    const std::uint32_t target = arg(c, 3);
    std::uint32_t out = 0;
    if (source == 0xFFFFFFFEu) out = sched_current_handle(c);
    else if (sched_is_object(source)) out = source;
    else fail_closed(Unported::Import, source, "DuplicateHandle of a handle that is not a kernel object");
    if (target != 0) st32(c.mem, target, out);
    set_error(c, 0);
    ret_stdcall(c, 1, 28);
}

// Code page 0 means the ANSI page, which is 1252. That page has no lead bytes.
// 0x779EC8 turns the result into a boolean.
void IsDBCSLeadByteEx(Cpu& c) { ret_stdcall(c, 0, 8); }

// The three callers read dwNumberOfProcessors (offset 20). One processor matches
// the single guest thread.
void GetSystemInfo(Cpu& c) {
    const std::uint32_t info = arg(c, 0);
    if (info != 0) {
        st16(c.mem, info, 0);
        st16(c.mem, info + 2, 0);
        st32(c.mem, info + 4, 4096);
        st32(c.mem, info + 8, 0x10000);
        st32(c.mem, info + 12, 0x7FFEFFFF);
        st32(c.mem, info + 16, 1);
        st32(c.mem, info + 20, 1);
        st32(c.mem, info + 24, 586);
        st32(c.mem, info + 28, 0x10000);
        st16(c.mem, info + 32, 6);
        st16(c.mem, info + 34, 0x1A05);
    }
    ret_stdcall(c, 0, 4);
}

// CreateThread(attributes, stack size, start, parameter, flags, *id). The
// thread runs when this one next blocks or yields. CREATE_SUSPENDED is 4.
void CreateThread(Cpu& c) {
    const std::uint32_t start = arg(c, 2);
    const std::uint32_t param = arg(c, 3);
    const std::uint32_t flags = arg(c, 4);
    const std::uint32_t tid = arg(c, 5);
    std::uint32_t id = 0;
    const std::uint32_t handle = sched_create_thread(c, start, param, (flags & 4u) != 0, &id);
    if (handle == 0) fail_closed(Unported::Import, start, "CreateThread: every guest thread slot is in use");
    if (tid != 0) st32(c.mem, tid, id);
    ret_stdcall(c, handle, 24);
}

// The full path, as Windows gives it: callers strip the file name and append
// "\\nrt\\..." or "\\..\\GameData", so a bare name would make those drive-root paths.
// GetModuleFileName truncates to the buffer and returns the buffer size then.
std::uint32_t module_file_name(Cpu& c, bool wide) {
    const std::uint32_t module = arg(c, 0);
    const std::uint32_t buffer = arg(c, 1);
    const std::uint32_t cap = arg(c, 2);
    const std::uint32_t image = ld32(c.mem, 0x20008);
    if (module != 0 && module != image) {
        set_error(c, 126);  // ERROR_MOD_NOT_FOUND
        return 0;
    }
    const std::string path = guest_module_path(kExeName);
    if (cap == 0) {
        set_error(c, 122);
        return 0;
    }
    const std::uint32_t n = static_cast<std::uint32_t>(path.size()) < cap ? static_cast<std::uint32_t>(path.size()) : cap - 1;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (wide) st16(c.mem, buffer + i * 2, static_cast<std::uint8_t>(path[i]));
        else c.mem[buffer + i] = static_cast<std::uint8_t>(path[i]);
    }
    if (wide) st16(c.mem, buffer + n * 2, 0);
    else c.mem[buffer + n] = 0;
    if (n < path.size()) {
        set_error(c, 122);  // ERROR_INSUFFICIENT_BUFFER
        return cap;
    }
    set_error(c, 0);
    return n;
}

void GetModuleFileNameW(Cpu& c) { ret_stdcall(c, module_file_name(c, true), 12); }

// GetModuleHandleExW(flags, name or address, *module). The CRT's thread exit
// asks with GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS (4) for its own code. The
// executable is the only module: an address inside it, or a null name, gives
// the image base; anything else is ERROR_MOD_NOT_FOUND.
void GetModuleHandleExW(Cpu& c) {
    const std::uint32_t flags = arg(c, 0);
    const std::uint32_t name = arg(c, 1);
    const std::uint32_t out = arg(c, 2);
    const std::uint32_t image = 0x400000u;
    const std::uint32_t image_end = image + ld32(c.mem, image + ld32(c.mem, image + 0x3C) + 0x50);
    const bool found = (flags & 4u) != 0 ? (name >= image && name < image_end) : name == 0;
    if (out != 0) st32(c.mem, out, found ? image : 0u);
    set_error(c, found ? 0u : 126u);
    ret_stdcall(c, found ? 1u : 0u, 12);
}

// NULL is the executable. 0xC2B9D0 compares the handle with the image base.
void GetModuleHandleA(Cpu& c) {
    const std::uint32_t name = arg(c, 0);
    ret_stdcall(c, name == 0 ? 0x400000u : 0u, 4);
}

void GetModuleFileNameA(Cpu& c) { ret_stdcall(c, module_file_name(c, false), 12); }

// No DLL is loaded at run time. A name with no file behind it (in the game's
// folder, or the path given) fails as on Windows: NULL and ERROR_MOD_NOT_FOUND.
// OpenCV asks for opencv_ffmpeg2410.dll, which the install does not ship, after
// the first attract loop and goes on without it. A DLL that does exist fails
// closed: its code is not translated.
// Video for Windows, as OpenCV 2.4's file capture (CvCaptureAVI_VFW) uses it
// for the two AVIs in rom\movie: the name-entry letter strip after a race and
// the attract camera stand-in. Files with Microsoft Video 1 16-bit video are
// decoded natively (avi_video.cpp) and handed out as 24-bit bottom-up DIBs, as
// AVIStreamGetFrame does. One object per open file; the PAVIFILE, PAVISTREAM and
// PGETFRAME handed to the guest are its handle + 0, + 4 and + 8. The writing
// and compression entry points stay fail-closed.
struct AviOpen {
    std::uint32_t file = 0;    // guest_create_file handle
    AviVideo video;
    std::uint32_t frame = 0;   // guest buffer: BITMAPINFOHEADER and the bits
};
std::unordered_map<std::uint32_t, std::unique_ptr<AviOpen>> g_avis;
std::uint32_t g_next_avi = 0x7A000000u;
constexpr std::uint32_t kAviErrFileOpen = 0x80044067u, kAviErrNoData = 0x80044075u;

AviOpen* avi_of(std::uint32_t handle, std::uint32_t tag) {
    if ((handle & 0xFu) != tag) return nullptr;
    auto it = g_avis.find(handle - tag);
    return it == g_avis.end() ? nullptr : it->second.get();
}

void AVIFileInit(Cpu& c) { ret_stdcall(c, 0, 0); }

void AVIFileOpenA(Cpu& c) {
    const std::uint32_t out = arg(c, 0);
    if (out != 0) st32(c.mem, out, 0);
    const std::string name = arg(c, 1) ? guest_ascii(c.mem, arg(c, 1), false) : std::string();
    auto avi = std::make_unique<AviOpen>();
    std::uint32_t err = 0;
    std::uint64_t size = 0;
    bool ok = !name.empty() && guest_create_file(name, kGenericRead, 3u /* OPEN_EXISTING */, 0, &avi->file, &err) == 0 &&
              guest_file_size(avi->file, &size) == 0;
    if (ok) {
        const std::uint32_t fh = avi->file;
        ok = avi->video.open(
            [fh](std::uint64_t offset, std::uint32_t n, std::uint8_t* dst) {
                std::int64_t now = 0;
                std::uint32_t got = 0;
                return guest_seek_file(fh, static_cast<std::int64_t>(offset), 0, &now) == 0 &&
                       guest_read_file(fh, dst, n, &got) == 0 && got == n;
            },
            size);
    }
    if (std::getenv("DCUSA_STATS") != nullptr)
        std::fprintf(stderr, "AVIFileOpenA %s: %s %ux%u %u frames\n", name.c_str(), ok ? "opened" : "not opened",
                     avi->video.width(), avi->video.height(), avi->video.frames());
    if (!ok) {
        if (avi->file != 0) guest_close_file(avi->file);
        ret_stdcall(c, kAviErrFileOpen, 16);
        return;
    }
    const std::uint32_t handle = g_next_avi;
    g_next_avi += 0x10u;
    g_avis[handle] = std::move(avi);
    if (out != 0) st32(c.mem, out, handle);
    ret_stdcall(c, 0, 16);
}

// (pfile, ppavi, fccType, lParam): the one video stream.
void AVIFileGetStream(Cpu& c) {
    AviOpen* a = avi_of(arg(c, 0), 0);
    const std::uint32_t out = arg(c, 1), type = arg(c, 2);
    const bool video = type == 0 || type == 0x73646976u;  // 'vids'
    if (out != 0) st32(c.mem, out, a != nullptr && video && arg(c, 3) == 0 ? arg(c, 0) + 4u : 0u);
    ret_stdcall(c, a != nullptr && video && arg(c, 3) == 0 ? 0u : kAviErrNoData, 16);
}

// AVISTREAMINFOA, 140 bytes.
void AVIStreamInfoA(Cpu& c) {
    AviOpen* a = avi_of(arg(c, 0), 4);
    const std::uint32_t si = arg(c, 1), size = arg(c, 2);
    if (a == nullptr || si == 0 || size < 140) {
        ret_stdcall(c, kAviErrNoData, 12);
        return;
    }
    std::memset(c.mem + si, 0, size < 140 ? size : 140);
    st32(c.mem, si + 0, 0x73646976u);  // 'vids'
    st32(c.mem, si + 4, a->video.handler());
    st32(c.mem, si + 20, a->video.scale());
    st32(c.mem, si + 24, a->video.rate());
    st32(c.mem, si + 32, a->video.frames());
    st32(c.mem, si + 40, a->video.row_bytes() * a->video.height());
    st32(c.mem, si + 44, 0xFFFFFFFFu);
    st32(c.mem, si + 60, a->video.width());   // rcFrame.right
    st32(c.mem, si + 64, a->video.height());  // rcFrame.bottom
    ret_stdcall(c, 0, 12);
}

// (pavi, lpbiWanted): 24-bit frames (lpbiWanted NULL or a 24-bit header).
void AVIStreamGetFrameOpen(Cpu& c) {
    AviOpen* a = avi_of(arg(c, 0), 4);
    const std::uint32_t want = arg(c, 1);
    const bool rgb24 = want == 0 || (ld16(c.mem, want + 14) == 24 && ld32(c.mem, want + 16) == 0);
    if (a == nullptr || !rgb24) {
        ret_stdcall(c, 0, 8);
        return;
    }
    if (a->frame == 0) a->frame = heap_alloc(c.mem, 40u + a->video.row_bytes() * a->video.height(), true);
    ret_stdcall(c, a->frame != 0 ? arg(c, 0) + 4u : 0u, 8);
}

// (pg, lPos): the frame as a packed DIB, or NULL past the end.
void AVIStreamGetFrame(Cpu& c) {
    AviOpen* a = avi_of(arg(c, 0), 8);
    const std::uint32_t pos = arg(c, 1);
    if (a == nullptr || a->frame == 0 || pos >= a->video.frames()) {
        ret_stdcall(c, 0, 8);
        return;
    }
    const std::vector<std::uint8_t>& bits = a->video.frame_bgr24(pos);
    if (bits.empty()) {
        ret_stdcall(c, 0, 8);
        return;
    }
    const std::uint32_t h = a->frame;
    st32(c.mem, h + 0, 40);
    st32(c.mem, h + 4, a->video.width());
    st32(c.mem, h + 8, a->video.height());  // positive: bottom-up
    st16(c.mem, h + 12, 1);
    st16(c.mem, h + 14, 24);
    st32(c.mem, h + 16, 0);  // BI_RGB
    st32(c.mem, h + 20, static_cast<std::uint32_t>(bits.size()));
    std::memset(c.mem + h + 24, 0, 16);
    std::memcpy(c.mem + h + 40, bits.data(), bits.size());
    ret_stdcall(c, h, 8);
}

void AVIStreamGetFrameClose(Cpu& c) { ret_stdcall(c, avi_of(arg(c, 0), 8) != nullptr ? 0u : kAviErrNoData, 4); }

void AVIStreamRelease(Cpu& c) { ret_stdcall(c, 0, 4); }

void AVIFileRelease(Cpu& c) {
    auto it = g_avis.find(arg(c, 0));
    if (it != g_avis.end()) {
        if (it->second->frame != 0) heap_free(c.mem, it->second->frame);
        guest_close_file(it->second->file);
        g_avis.erase(it);
    }
    ret_stdcall(c, 0, 4);
}

void LoadLibraryA(Cpu& c) {
    const std::string name = arg(c, 0) ? guest_ascii(c.mem, arg(c, 0), false) : std::string();
    std::string path = name;
    if (name.find_first_of("\\/:") == std::string::npos) {
        const std::string module = guest_module_path(kExeName);
        const std::size_t slash = module.find_last_of('\\');
        path = slash == std::string::npos ? name : module.substr(0, slash + 1) + name;
    }
    GuestFileInfo info;
    if (!name.empty() && guest_stat(path, &info) == 0 && info.exists)
        fail_closed(Unported::Import, 0, "LoadLibraryA of a DLL that exists: " + name);
    set_error(c, 126u);
    ret_stdcall(c, 0, 4);
}

// An empty environment: one UTF-16 null. The CRT's converter asks for the size
// of that single character and then copies it.
void GetEnvironmentStringsW(Cpu& c) {
    st32(c.mem, kEnvironment, 0);
    ret_stdcall(c, kEnvironment, 0);
}

void FreeEnvironmentStringsW(Cpu& c) { ret_stdcall(c, 1, 4); }

void WideCharToMultiByte(Cpu& c) {
    const std::uint32_t src = arg(c, 2);
    std::int32_t count = static_cast<std::int32_t>(arg(c, 3));
    const std::uint32_t dest = arg(c, 4);
    const std::uint32_t dest_cap = arg(c, 5);
    if (count < 0) {
        count = 0;
        for (std::uint32_t i = 0; i < (1u << 20); ++i) {
            ++count;
            if (ld16(c.mem, src + i * 2) == 0) break;
        }
    }
    const auto need = static_cast<std::uint32_t>(count);
    if (dest == 0 || dest_cap == 0) {
        ret_stdcall(c, need, 32);
        return;
    }
    if (dest_cap < need) {
        ret_stdcall(c, 0, 32);
        return;
    }
    for (std::uint32_t i = 0; i < need; ++i) {
        std::uint16_t w = ld16(c.mem, src + i * 2);
        c.mem[dest + i] = w < 256 ? static_cast<std::uint8_t>(w) : static_cast<std::uint8_t>('?');
    }
    ret_stdcall(c, need, 32);
}

void GetCurrentDirectoryW(Cpu& c) {
    const std::uint32_t cap = arg(c, 0);
    const std::uint32_t buf = arg(c, 1);
    const std::string path = guest_cwd();
    if (buf == 0 || cap <= path.size()) {
        ret_stdcall(c, static_cast<std::uint32_t>(path.size() + 1), 8);
        return;
    }
    write_guest(c.mem, buf, cap, path, true);
    ret_stdcall(c, static_cast<std::uint32_t>(path.size()), 8);
}

void SetCurrentDirectoryW(Cpu& c) {
    const std::uint32_t path = arg(c, 0);
    const std::uint32_t err = path == 0 ? 3u : guest_set_cwd(guest_ascii(c.mem, path, true));
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 4);
}

void GetFullPathNameW(Cpu& c) {
    const std::uint32_t name = arg(c, 0);
    const std::uint32_t cap = arg(c, 1);
    const std::uint32_t buf = arg(c, 2);
    const std::uint32_t part = arg(c, 3);
    const std::string full = name == 0 ? std::string() : guest_full_path(guest_ascii(c.mem, name, true));
    if (full.empty()) {
        set_error(c, 3);
        ret_stdcall(c, 0, 16);
        return;
    }
    if (buf == 0 || cap <= full.size()) {
        ret_stdcall(c, static_cast<std::uint32_t>(full.size() + 1), 16);
        return;
    }
    write_guest(c.mem, buf, cap, full, true);
    if (part != 0) {
        const auto slash = full.find_last_of('\\');
        const std::uint32_t at = slash == std::string::npos ? 0 : static_cast<std::uint32_t>(slash + 1);
        st32(c.mem, part, buf + at * 2);
    }
    ret_stdcall(c, static_cast<std::uint32_t>(full.size()), 16);
}

void GetFullPathNameA(Cpu& c) {
    const std::uint32_t name = arg(c, 0);
    const std::uint32_t cap = arg(c, 1);
    const std::uint32_t buf = arg(c, 2);
    const std::uint32_t part = arg(c, 3);
    const std::string full = name == 0 ? std::string() : guest_full_path(guest_ascii(c.mem, name, false));
    if (full.empty()) {
        set_error(c, 3);
        ret_stdcall(c, 0, 16);
        return;
    }
    if (buf == 0 || cap <= full.size()) {
        ret_stdcall(c, static_cast<std::uint32_t>(full.size() + 1), 16);
        return;
    }
    write_guest(c.mem, buf, cap, full, false);
    if (part != 0) {
        const auto slash = full.find_last_of('\\');
        const std::uint32_t at = slash == std::string::npos ? 0 : static_cast<std::uint32_t>(slash + 1);
        st32(c.mem, part, buf + at);
    }
    ret_stdcall(c, static_cast<std::uint32_t>(full.size()), 16);
}

void CreateDirectoryA(Cpu& c) {
    const std::uint32_t path = arg(c, 0);
    if (!guest_files_ready()) {
        ret_stdcall(c, path == 0 ? 0u : 1u, 8);
        return;
    }
    const std::uint32_t err = path == 0 ? 3u : guest_mkdir(guest_ascii(c.mem, path, false));
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 8);
}

void CreateFileCommon(Cpu& c, bool wide) {
    const std::uint32_t name = arg(c, 0);
    std::uint32_t handle = 0xFFFFFFFFu;
    std::uint32_t err = 2;
    if (name != 0) {
        const std::string path = guest_ascii(c.mem, name, wide);
        guest_create_file(path, arg(c, 1), arg(c, 4), arg(c, 5), &handle, &err);
        if (std::getenv("DCUSA_IO") != nullptr) {
            std::fprintf(stderr, "open %s handle %x err %u thread %u\n", path.c_str(), handle, err,
                         ld32(c.mem, c.fs_base + 0x24));
            static int stacks = 0;
            if (std::getenv("DCUSA_IO_STACK") != nullptr && path.find(std::getenv("DCUSA_IO_STACK")) != std::string::npos &&
                stacks++ < 2) {
                // The EBP frame chain: [ebp] is the caller's ebp, [ebp+4] its return address.
                std::fprintf(stderr, "  frames: %08X", ld32(c.mem, c.r[Cpu::ESP]));
                const std::uint32_t top = ld32(c.mem, c.fs_base + 4), bottom = ld32(c.mem, c.fs_base + 8);
                std::uint32_t bp = c.r[Cpu::EBP];
                for (int n = 0; n < 16 && bp >= bottom && bp + 8 <= top; ++n) {
                    std::fprintf(stderr, " %08X", ld32(c.mem, bp + 4));
                    const std::uint32_t next = ld32(c.mem, bp);
                    if (next <= bp) break;
                    bp = next;
                }
                std::fprintf(stderr, "\n");
            }
        } else if (std::getenv("DCUSA_STATS") != nullptr) {
            static int opened = 0;
            if (opened < 30 || err != 0) {
                if (opened < 40) {
                    ++opened;
                    std::fprintf(stderr, "file %s err %u\n", path.c_str(), err);
                }
            }
        }
    }
    set_error(c, err);
    ret_stdcall(c, handle, 28);
}

void CreateFileW(Cpu& c) { CreateFileCommon(c, true); }

void CreateFileA(Cpu& c) { CreateFileCommon(c, false); }

// An OVERLAPPED read or write (argument 5) completes before the call returns:
// it uses the structure's offset, fills Internal / InternalHigh, and sets the
// event, so a later wait on that event or GetOverlappedResult sees it done.
std::uint32_t overlapped_seek(Cpu& c, std::uint32_t handle, std::uint32_t ov) {
    if (ov == 0) return 0;
    const std::int64_t offset = static_cast<std::int64_t>(ld32(c.mem, ov + 8)) |
                                static_cast<std::int64_t>(ld32(c.mem, ov + 12)) << 32;
    std::int64_t now = 0;
    return guest_seek_file(handle, offset, 0, &now);
}

void overlapped_done(Cpu& c, std::uint32_t ov, std::uint32_t err, std::uint32_t bytes) {
    if (ov == 0) return;
    st32(c.mem, ov, err == 0 ? 0u : 0xC0000001u);
    st32(c.mem, ov + 4, bytes);
    if (const std::uint32_t event = ld32(c.mem, ov + 16); event != 0) sched_set_event(event);
}

void ReadFile(Cpu& c) {
    const std::uint32_t handle = arg(c, 0);
    const std::uint32_t buf = arg(c, 1);
    const std::uint32_t n = arg(c, 2);
    const std::uint32_t got_at = arg(c, 3);
    const std::uint32_t ov = arg(c, 4);
    std::uint32_t got = 0;
    std::uint32_t err = overlapped_seek(c, handle, ov);
    if (err == 0) err = guest_read_file(handle, c.mem + buf, n, &got);
    if (std::getenv("DCUSA_IO") != nullptr)
        std::fprintf(stderr, "read h %x buf %08x n %u got %u ov %08x err %u thread %u\n", handle, buf, n, got, ov, err,
                     ld32(c.mem, c.fs_base + 0x24));
    if (got_at != 0) st32(c.mem, got_at, got);
    overlapped_done(c, ov, err, got);
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 20);
}

void WriteFile(Cpu& c) {
    const std::uint32_t handle = arg(c, 0);
    const std::uint32_t buf = arg(c, 1);
    const std::uint32_t n = arg(c, 2);
    const std::uint32_t put_at = arg(c, 3);
    const std::uint32_t ov = arg(c, 4);
    std::uint32_t put = 0;
    std::uint32_t err = overlapped_seek(c, handle, ov);
    if (err == 0) err = guest_write_file(handle, c.mem + buf, n, &put);
    if (std::getenv("DCUSA_IO") != nullptr) {
        static int shown = 0;
        if (shown++ < 40) {
            std::string text(reinterpret_cast<const char*>(c.mem + buf), std::min<std::uint32_t>(n, 160u));
            for (char& ch : text) if (ch == '\n' || ch == '\r') ch = '|';
            std::fprintf(stderr, "write h %x n %u put %u err %u: %s\n", handle, n, put, err, text.c_str());
        }
    }
    if (put_at != 0) st32(c.mem, put_at, put);
    overlapped_done(c, ov, err, put);
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 20);
}

void SetFilePointer(Cpu& c) {
    const std::uint32_t high_at = arg(c, 2);
    std::int64_t distance = static_cast<std::int32_t>(arg(c, 1));
    if (high_at != 0) {
        const std::int32_t high = static_cast<std::int32_t>(ld32(c.mem, high_at));
        distance = (static_cast<std::int64_t>(high) << 32) | arg(c, 1);
    }
    std::int64_t now = 0;
    const std::uint32_t err = guest_seek_file(arg(c, 0), distance, arg(c, 3), &now);
    set_error(c, err);
    if (err == 0 && high_at != 0) st32(c.mem, high_at, static_cast<std::uint32_t>(now >> 32));
    ret_stdcall(c, err == 0 ? static_cast<std::uint32_t>(now) : 0xFFFFFFFFu, 16);
}

void SetFilePointerEx(Cpu& c) {
    const std::int64_t distance = (static_cast<std::int64_t>(static_cast<std::int32_t>(arg(c, 2))) << 32) | arg(c, 1);
    const std::uint32_t out = arg(c, 3);
    std::int64_t now = 0;
    const std::uint32_t err = guest_seek_file(arg(c, 0), distance, arg(c, 4), &now);
    set_error(c, err);
    if (err == 0 && out != 0) {
        st32(c.mem, out, static_cast<std::uint32_t>(now));
        st32(c.mem, out + 4, static_cast<std::uint32_t>(now >> 32));
    }
    ret_stdcall(c, err == 0 ? 1u : 0u, 20);
}

void GetFileSize(Cpu& c) {
    const std::uint32_t high_at = arg(c, 1);
    std::uint64_t size = 0;
    const std::uint32_t err = guest_file_size(arg(c, 0), &size);
    set_error(c, err);
    if (err == 0 && high_at != 0) st32(c.mem, high_at, static_cast<std::uint32_t>(size >> 32));
    ret_stdcall(c, err == 0 ? static_cast<std::uint32_t>(size) : 0xFFFFFFFFu, 8);
}

void GetFileSizeEx(Cpu& c) {
    const std::uint32_t out = arg(c, 1);
    std::uint64_t size = 0;
    const std::uint32_t err = guest_file_size(arg(c, 0), &size);
    set_error(c, err);
    if (err == 0 && out != 0) {
        st32(c.mem, out, static_cast<std::uint32_t>(size));
        st32(c.mem, out + 4, static_cast<std::uint32_t>(size >> 32));
    }
    ret_stdcall(c, err == 0 ? 1u : 0u, 8);
}

// A fixed FILETIME is enough for the open path. It only checks that the call
// succeeded and then converts the stamp into a SYSTEMTIME.
void write_stamp(std::uint8_t* mem, std::uint32_t at) {
    if (at == 0) return;
    st32(mem, at, 0);
    st32(mem, at + 4, 0x01D4B6C8u);
}

void GetFileTime(Cpu& c) {
    if (!guest_is_file(arg(c, 0))) {
        set_error(c, 6);
        ret_stdcall(c, 0, 16);
        return;
    }
    write_stamp(c.mem, arg(c, 1));
    write_stamp(c.mem, arg(c, 2));
    write_stamp(c.mem, arg(c, 3));
    set_error(c, 0);
    ret_stdcall(c, 1, 16);
}

void FileTimeToLocalFileTime(Cpu& c) {
    const std::uint32_t src = arg(c, 0);
    const std::uint32_t dst = arg(c, 1);
    if (src == 0 || dst == 0) {
        ret_stdcall(c, 0, 8);
        return;
    }
    st32(c.mem, dst, ld32(c.mem, src));
    st32(c.mem, dst + 4, ld32(c.mem, src + 4));
    ret_stdcall(c, 1, 8);
}

void FileTimeToSystemTime(Cpu& c) {
    const std::uint32_t dst = arg(c, 1);
    if (arg(c, 0) == 0 || dst == 0) {
        ret_stdcall(c, 0, 8);
        return;
    }
    st16(c.mem, dst, 2019);
    st16(c.mem, dst + 2, 1);
    st16(c.mem, dst + 4, 2);
    st16(c.mem, dst + 6, 1);
    st16(c.mem, dst + 8, 0);
    st16(c.mem, dst + 10, 0);
    st16(c.mem, dst + 12, 0);
    st16(c.mem, dst + 14, 0);
    ret_stdcall(c, 1, 8);
}

// SYSTEMTIME (year, month, weekday, day, hour, minute, second, ms; the weekday
// is ignored) to FILETIME, 100 ns since 1601. The 2017 build calls it at
// 0x529DA5 on GetLocalTime's result; 2019 never does. FALSE for a field out of
// range, as Windows (ERROR_INVALID_PARAMETER).
void SystemTimeToFileTime(Cpu& c) {
    const std::uint32_t src = arg(c, 0);
    const std::uint32_t dst = arg(c, 1);
    if (src == 0 || dst == 0) {
        set_error(c, 87);
        ret_stdcall(c, 0, 8);
        return;
    }
    const unsigned year = ld16(c.mem, src), month = ld16(c.mem, src + 2), day = ld16(c.mem, src + 6);
    const unsigned hour = ld16(c.mem, src + 8), minute = ld16(c.mem, src + 10), second = ld16(c.mem, src + 12);
    const unsigned ms = ld16(c.mem, src + 14);
    static constexpr unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (year < 1601 || year > 30827 || month < 1 || month > 12 || day < 1 ||
        day > kDays[month - 1] + (month == 2 && leap ? 1u : 0u) || hour > 23 || minute > 59 || second > 59 || ms > 999) {
        set_error(c, 87);
        ret_stdcall(c, 0, 8);
        return;
    }
    // Days since 1601-01-01 (proleptic Gregorian), by the civil-from-days inverse.
    const int y = static_cast<int>(year) - (month <= 2 ? 1 : 0);
    const int era = y / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days_1970 = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
    const std::int64_t days = days_1970 + 134774;  // 1601-01-01 to 1970-01-01
    const std::uint64_t ft = static_cast<std::uint64_t>(((days * 24 + hour) * 60 + minute) * 60 + second) * 10000000ull +
                             ms * 10000ull;
    st32(c.mem, dst, static_cast<std::uint32_t>(ft));
    st32(c.mem, dst + 4, static_cast<std::uint32_t>(ft >> 32));
    set_error(c, 0);
    ret_stdcall(c, 1, 8);
}

void CloseHandle(Cpu& c) {
    const std::uint32_t handle = arg(c, 0);
    std::uint32_t err = 0;
    std::uint32_t ok = 1;
    if (handle == 0) {
        err = 6;
        ok = 0;
    } else if (guest_is_file(handle)) {
        err = guest_close_file(handle);
        ok = err == 0 ? 1u : 0u;
    } else if (sched_is_object(handle)) {
        sched_close(handle);
    }
    set_error(c, err);
    ret_stdcall(c, ok, 4);
}

// The file loader asks this before choosing the ANSI path. The default is ANSI.
void AreFileApisANSI(Cpu& c) { ret_stdcall(c, 1, 0); }

void OutputDebugStringA(Cpu& c) {
    if (std::getenv("DCUSA_STATS") != nullptr && arg(c, 0) != 0) {
        static int shown = 0;
        static const int cap = std::getenv("DCUSA_DEBUG_OUTPUT") ? 1 << 30 : 8;
        if (shown < cap) {
            ++shown;
            std::fprintf(stderr, "dbg %s\n", guest_ascii(c.mem, arg(c, 0), false).c_str());
        }
    }
    ret_stdcall(c, 0, 4);
}

void GetFileAttributesA(Cpu& c) {
    GuestFileInfo info;
    const std::uint32_t err = arg(c, 0) == 0 ? 3u : guest_stat(guest_ascii(c.mem, arg(c, 0), false), &info);
    set_error(c, info.exists ? 0u : (err == 0 ? 2u : err));
    ret_stdcall(c, info.exists ? info.attributes : 0xFFFFFFFFu, 4);
}

void GetFileAttributesExW(Cpu& c) {
    GuestFileInfo info;
    const std::uint32_t err = arg(c, 0) == 0 ? 3u : guest_stat(guest_ascii(c.mem, arg(c, 0), true), &info);
    if (err == 0 && info.exists) write_attr(c.mem, arg(c, 2), info);
    set_error(c, info.exists ? 0u : (err == 0 ? 2u : err));
    ret_stdcall(c, err == 0 && info.exists ? 1u : 0u, 12);
}

void GetFileType(Cpu& c) {
    std::uint32_t attr = 0;
    const std::uint32_t err = guest_file_attributes(arg(c, 0), &attr);
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 4);
}

void FindFirstCommon(Cpu& c, bool wide, unsigned data_arg, unsigned arg_bytes) {
    const std::uint32_t name = arg(c, 0);
    const std::uint32_t data = arg(c, data_arg);
    GuestDirEntry first;
    std::uint32_t handle = 0xFFFFFFFFu;
    const std::uint32_t err = name == 0 ? 2u : guest_find_first(guest_ascii(c.mem, name, wide), &first, &handle);
    if (err == 0) {
        if (wide) write_find_w(c.mem, data, first);
        else write_find_a(c.mem, data, first);
    }
    set_error(c, err);
    ret_stdcall(c, err == 0 ? handle : 0xFFFFFFFFu, arg_bytes);
}

void FindFirstFileW(Cpu& c) { FindFirstCommon(c, true, 1, 8); }

void FindFirstFileA(Cpu& c) { FindFirstCommon(c, false, 1, 8); }

void FindFirstFileExW(Cpu& c) { FindFirstCommon(c, true, 2, 24); }

void FindFirstFileExA(Cpu& c) { FindFirstCommon(c, false, 2, 24); }

void FindNextCommon(Cpu& c, bool wide) {
    GuestDirEntry next;
    const std::uint32_t err = guest_find_next(arg(c, 0), &next);
    if (err == 0) {
        if (wide) write_find_w(c.mem, arg(c, 1), next);
        else write_find_a(c.mem, arg(c, 1), next);
    }
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 8);
}

void FindNextFileW(Cpu& c) { FindNextCommon(c, true); }

void FindNextFileA(Cpu& c) { FindNextCommon(c, false); }

void FindClose(Cpu& c) {
    const std::uint32_t err = guest_find_close(arg(c, 0));
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 4);
}

void DeleteFileW(Cpu& c) {
    const std::uint32_t err = arg(c, 0) == 0 ? 3u : guest_delete_file(guest_ascii(c.mem, arg(c, 0), true));
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 4);
}

void DeleteFileA(Cpu& c) {
    const std::uint32_t err = arg(c, 0) == 0 ? 3u : guest_delete_file(guest_ascii(c.mem, arg(c, 0), false));
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 4);
}

void SetEndOfFile(Cpu& c) {
    const std::uint32_t err = guest_truncate_file(arg(c, 0));
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 4);
}

void FlushFileBuffers(Cpu& c) {
    const std::uint32_t err = guest_flush_file(arg(c, 0));
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 4);
}

void WritePrivateProfileStringA(Cpu& c) {
    const bool has = arg(c, 0) != 0 && arg(c, 3) != 0;
    std::optional<std::string> value;
    if (arg(c, 2) != 0) value = guest_ascii(c.mem, arg(c, 2), false);
    const std::uint32_t err =
        has ? guest_ini_set(guest_ascii(c.mem, arg(c, 3), false), guest_ascii(c.mem, arg(c, 0), false),
                            arg(c, 1) == 0 ? std::string() : guest_ascii(c.mem, arg(c, 1), false), value)
            : 87u;
    set_error(c, err);
    ret_stdcall(c, err == 0 ? 1u : 0u, 16);
}

// 0x4881C4 treats 0 as finished and 0x102 as a timeout that then calls
// TerminateThread. 0x4FC81D continues only when the wait returns 0.
void WaitForSingleObject(Cpu& c) {
    const std::uint32_t handle = arg(c, 0);
    const std::uint32_t ms = arg(c, 1);
    ret_stdcall(c, sched_wait(c, &handle, 1, false, ms), 8);
}

// (count, handles, wait all, milliseconds, alertable). No APC is ever queued,
// so alertable waits never return WAIT_IO_COMPLETION.
void WaitForMultipleObjectsEx(Cpu& c) {
    const std::uint32_t count = arg(c, 0);
    const std::uint32_t list = arg(c, 1);
    std::uint32_t handles[64] = {};
    if (count > 64) fail_closed(Unported::Import, count, "WaitForMultipleObjectsEx of more than 64 handles");
    for (std::uint32_t i = 0; i < count; ++i) handles[i] = ld32(c.mem, list + 4 * i);
    ret_stdcall(c, sched_wait(c, handles, count, arg(c, 2) != 0, arg(c, 3)), 20);
}

// 0x4FC830 treats STILL_ACTIVE (259) as failure.
void GetExitCodeThread(Cpu& c) {
    const std::uint32_t out = arg(c, 1);
    std::uint32_t code = 0;
    const bool ok = sched_exit_code(arg(c, 0), &code);
    if (ok && out != 0) st32(c.mem, out, code);
    set_error(c, ok ? 0u : 6u);
    ret_stdcall(c, ok ? 1u : 0u, 8);
}

void TerminateThread(Cpu& c) {
    const std::uint32_t handle = arg(c, 0);
    const std::uint32_t code = arg(c, 1);
    ret_stdcall(c, sched_terminate(c, handle, code) ? 1u : 0u, 8);
}

void Sleep(Cpu& c) {
    const std::uint32_t ms = arg(c, 0);
    ret_stdcall(c, 0, 4);
    // After the return is set up: a terminated thread unwinds from here.
    sched_sleep(c, ms);
}

// 0x4DB20C fills a MEMORYSTATUS (32 bytes) only to log it when a game starts.
// The figures are those of a 4 GB machine running a large-address-aware
// 32-bit process; nothing reads them back.
void GlobalMemoryStatus(Cpu& c) {
    const std::uint32_t out = arg(c, 0);
    if (out != 0) {
        st32(c.mem, out + 0, 32);            // dwLength
        st32(c.mem, out + 4, 25);            // dwMemoryLoad, percent
        st32(c.mem, out + 8, 0xFFFFFFFFu);   // dwTotalPhys (capped, as Windows does)
        st32(c.mem, out + 12, 0xC0000000u);  // dwAvailPhys
        st32(c.mem, out + 16, 0xFFFFFFFFu);  // dwTotalPageFile
        st32(c.mem, out + 20, 0xC0000000u);  // dwAvailPageFile
        st32(c.mem, out + 24, 0xFFFE0000u);  // dwTotalVirtual
        st32(c.mem, out + 28, 0x80000000u);  // dwAvailVirtual
    }
    ret_stdcall(c, 0, 4);
}

void GetTickCount(Cpu& c) {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    ret_stdcall(c, static_cast<std::uint32_t>(ms), 0);
}

void write_system_time(Cpu& c, bool local) {
    const std::uint32_t out = arg(c, 0);
    if (out != 0) {
        const std::time_t now = fixed_clock() ? kFixedClock : std::time(nullptr);
        std::tm tm {};
        if (local) localtime_r(&now, &tm);
        else gmtime_r(&now, &tm);
        st16(c.mem, out, static_cast<std::uint16_t>(tm.tm_year + 1900));
        st16(c.mem, out + 2, static_cast<std::uint16_t>(tm.tm_mon + 1));
        st16(c.mem, out + 4, static_cast<std::uint16_t>(tm.tm_wday));
        st16(c.mem, out + 6, static_cast<std::uint16_t>(tm.tm_mday));
        st16(c.mem, out + 8, static_cast<std::uint16_t>(tm.tm_hour));
        st16(c.mem, out + 10, static_cast<std::uint16_t>(tm.tm_min));
        st16(c.mem, out + 12, static_cast<std::uint16_t>(tm.tm_sec));
        st16(c.mem, out + 14, 0);
    }
    ret_stdcall(c, 0, 4);
}

void GetLocalTime(Cpu& c) { write_system_time(c, true); }

void GetSystemTime(Cpu& c) { write_system_time(c, false); }

struct Handler {
    const char* name;
    GuestFn fn;
};

constexpr Handler kHandlers[] = {
    {"KERNEL32.dll!GetStartupInfoW", GetStartupInfoW},
    {"KERNEL32.dll!GetProcessHeap", GetProcessHeap},
    {"KERNEL32.dll!HeapAlloc", HeapAlloc},
    {"KERNEL32.dll!HeapFree", HeapFree},
    {"KERNEL32.dll!HeapSize", HeapSize},
    {"KERNEL32.dll!HeapReAlloc", HeapReAlloc},
    {"KERNEL32.dll!VirtualAlloc", VirtualAlloc},
    {"KERNEL32.dll!VirtualFree", VirtualFree},
    {"KERNEL32.dll!GetVersionExA", GetVersionExA},
    {"KERNEL32.dll!GetVersionExW", GetVersionExW},
    {"KERNEL32.dll!EncodePointer", EncodePointer},
    {"KERNEL32.dll!DecodePointer", DecodePointer},
    {"KERNEL32.dll!GetModuleHandleW", GetModuleHandleW},
    {"KERNEL32.dll!GetProcAddress", GetProcAddress},
    {"KERNEL32.dll!InitializeCriticalSectionAndSpinCount", InitializeCriticalSectionAndSpinCount},
    {"KERNEL32.dll!InitializeCriticalSection", InitializeCriticalSection},
    {"KERNEL32.dll!EnterCriticalSection", EnterCriticalSection},
    {"KERNEL32.dll!LeaveCriticalSection", LeaveCriticalSection},
    {"KERNEL32.dll!TryEnterCriticalSection", TryEnterCriticalSection},
    {"KERNEL32.dll!DeleteCriticalSection", DeleteCriticalSection},
    {"KERNEL32.dll!TlsAlloc", TlsAlloc},
    {"KERNEL32.dll!TlsGetValue", TlsGetValue},
    {"KERNEL32.dll!TlsSetValue", TlsSetValue},
    {"KERNEL32.dll!InterlockedIncrement", InterlockedIncrement},
    {"KERNEL32.dll!InterlockedDecrement", InterlockedDecrement},
    {"KERNEL32.dll!InterlockedExchange", InterlockedExchange},
    {"KERNEL32.dll!GetACP", GetACP},
    {"KERNEL32.dll!IsProcessorFeaturePresent", IsProcessorFeaturePresent},
    {"KERNEL32.dll!SetUnhandledExceptionFilter", SetUnhandledExceptionFilter},
    {"KERNEL32.dll!GetSystemTimeAsFileTime", GetSystemTimeAsFileTime},
    {"KERNEL32.dll!GetOEMCP", GetOEMCP},
    {"KERNEL32.dll!IsValidCodePage", IsValidCodePage},
    {"KERNEL32.dll!GetCPInfo", GetCPInfo},
    {"KERNEL32.dll!MultiByteToWideChar", MultiByteToWideChar},
    {"KERNEL32.dll!GetStringTypeW", GetStringTypeW},
    {"KERNEL32.dll!LCMapStringW", LCMapStringW},
    {"KERNEL32.dll!GetCurrentThread", GetCurrentThread},
    {"KERNEL32.dll!GetThreadTimes", GetThreadTimes},
    {"KERNEL32.dll!QueryPerformanceCounter", QueryPerformanceCounter},
    {"KERNEL32.dll!QueryPerformanceFrequency", QueryPerformanceFrequency},
    {"KERNEL32.dll!GetPrivateProfileStringA", GetPrivateProfileStringA},
    {"KERNEL32.dll!GetPrivateProfileIntA", GetPrivateProfileIntA},
    {"KERNEL32.dll!CreateEventA", CreateEventA},
    {"KERNEL32.dll!CreateEventW", CreateEventW},
    {"KERNEL32.dll!SetEvent", SetEvent},
    {"KERNEL32.dll!ResetEvent", ResetEvent},
    {"KERNEL32.dll!InitializeSListHead", InitializeSListHead},
    {"KERNEL32.dll!GetCurrentThreadId", GetCurrentThreadId},
    {"KERNEL32.dll!GetLastError", GetLastError},
    {"KERNEL32.dll!SetLastError", SetLastError},
    {"KERNEL32.dll!GetStdHandle", GetStdHandle},
    {"KERNEL32.dll!GetCommandLineA", GetCommandLineA},
    {"KERNEL32.dll!GetCommandLineW", GetCommandLineW},
    {"ole32.dll!CoInitialize", CoInitialize},
    {"ole32.dll!CoInitializeEx", CoInitializeEx},
    {"ole32.dll!CoUninitialize", CoUninitialize},
    {"ole32.dll!CoCreateInstance", CoCreateInstance},
    {"WINMM.dll!timeBeginPeriod", timeBeginPeriod},
    {"WINMM.dll!timeEndPeriod", timeEndPeriod},
    {"WINMM.dll!timeGetTime", timeGetTime},
    {"WINMM.dll!timeKillEvent", timeKillEvent},
    {"WINMM.dll!timeSetEvent", timeSetEvent},
    {"SHLWAPI.dll!PathRemoveFileSpecW", PathRemoveFileSpecW},
    {"SHLWAPI.dll!PathAppendW", PathAppendW},
    {"SHLWAPI.dll!PathCanonicalizeW", PathCanonicalizeW},
    {"SHLWAPI.dll!PathIsDirectoryW", PathIsDirectoryW},
    {"SHLWAPI.dll!PathFileExistsW", PathFileExistsW},
    {"SHLWAPI.dll!PathFileExistsA", PathFileExistsA},
    {"KERNEL32.dll!CreateDirectoryW", CreateDirectoryW},
    {"USER32.dll!ShowCursor", ShowCursor},
    {"USER32.dll!AdjustWindowRect", AdjustWindowRect},
    {"USER32.dll!LoadIconW", LoadIconW},
    {"USER32.dll!LoadImageW", LoadImageW},
    {"USER32.dll!LoadCursorW", LoadCursorW},
    {"GDI32.dll!DeleteDC", DeleteDC},
    {"GDI32.dll!DeleteObject", DeleteObject},
    {"GDI32.dll!GetStockObject", GetStockObject},
    {"GDI32.dll!CreateFontW", CreateFontW},
    {"GDI32.dll!CreateCompatibleDC", CreateCompatibleDC},
    {"GDI32.dll!SelectObject", SelectObject},
    {"GDI32.dll!GetTextMetricsW", GetTextMetricsW},
    {"USER32.dll!RegisterClassExW", RegisterClassExW},
    {"USER32.dll!RegisterClassA", RegisterClassA},
    {"USER32.dll!CreateWindowExW", CreateWindowExW},
    {"USER32.dll!CreateWindowExA", CreateWindowExA},
    {"USER32.dll!ShowWindow", ShowWindow},
    {"USER32.dll!EnableWindow", EnableWindow},
    {"USER32.dll!GetForegroundWindow", GetForegroundWindow},
    {"USER32.dll!GetWindowThreadProcessId", GetWindowThreadProcessId},
    {"USER32.dll!AttachThreadInput", AttachThreadInput},
    {"USER32.dll!SetForegroundWindow", SetForegroundWindow},
    {"USER32.dll!FindWindowA", FindWindowA},
    {"USER32.dll!FindWindowW", FindWindowW},
    {"DINPUT8.dll!DirectInput8Create", DirectInput8Create},
    {"DSOUND.dll!#11", DirectSoundCreate8},
    {"USER32.dll!SetWindowLongW", SetWindowLongW},
    {"USER32.dll!GetCursorPos", GetCursorPos},
    {"USER32.dll!ScreenToClient", ScreenToClient},
    {"XINPUT9_1_0.dll!XInputGetState", XInputGetState},
    {"USER32.dll!PeekMessageW", PeekMessageW},
    {"USER32.dll!PeekMessageA", PeekMessageA},
    {"USER32.dll!PostMessageW", PostMessageW},
    {"USER32.dll!PostQuitMessage", PostQuitMessage},
    {"USER32.dll!GetWindowLongW", GetWindowLongW},
    {"USER32.dll!DestroyWindow", DestroyWindow},
    {"USER32.dll!UnregisterClassA", UnregisterClassA},
    {"USER32.dll!SendMessageW", SendMessageW},
    {"USER32.dll!SendMessageA", SendMessageA},
    {"USER32.dll!DispatchMessageW", DispatchMessageW},
    {"USER32.dll!TranslateMessage", TranslateMessage},
    {"USER32.dll!GetClientRect", GetClientRect},
    {"KERNEL32.dll!RaiseException", RaiseException},
    {"d3d11.dll!D3D11CreateDevice", D3D11CreateDevice},
    {"d3d9.dll!D3DPERF_BeginEvent", D3DPERF_BeginEvent},
    {"d3d9.dll!D3DPERF_EndEvent", D3DPERF_EndEvent},
    {"D3DCOMPILER_46.dll!D3DCompile", D3DCompile},
    {"D3DCOMPILER_46.dll!D3DReflect", D3DReflect},
    {"KERNEL32.dll!CreateThread", CreateThread},
    {"KERNEL32.dll!SetThreadPriority", SetThreadPriority},
    {"KERNEL32.dll!GetThreadPriority", GetThreadPriority},
    {"KERNEL32.dll!ResumeThread", ResumeThread},
    {"KERNEL32.dll!SuspendThread", SuspendThread},
    {"SHLWAPI.dll!PathStripToRootW", PathStripToRootW},
    {"KERNEL32.dll!GetDiskFreeSpaceW", GetDiskFreeSpaceW},
    {"KERNEL32.dll!SwitchToThread", SwitchToThread},
    {"KERNEL32.dll!ExitThread", ExitThread},
    {"KERNEL32.dll!DuplicateHandle", DuplicateHandle},
    {"KERNEL32.dll!CreateSemaphoreA", CreateSemaphoreA},
    {"KERNEL32.dll!CreateSemaphoreW", CreateSemaphoreW},
    {"KERNEL32.dll!CreateSemaphoreExW", CreateSemaphoreExW},
    {"KERNEL32.dll!ReleaseSemaphore", ReleaseSemaphore},
    {"KERNEL32.dll!CreateMutexA", CreateMutexA},
    {"KERNEL32.dll!ReleaseMutex", ReleaseMutex},
    {"KERNEL32.dll!IsDBCSLeadByteEx", IsDBCSLeadByteEx},
    {"KERNEL32.dll!GetSystemInfo", GetSystemInfo},
    {"WS2_32.dll!#115", WSAStartup},
    {"WS2_32.dll!#116", WSACleanup},
    {"WS2_32.dll!#3", ws_closesocket},
    {"KERNEL32.dll!CreateProcessA", CreateProcessA},
    {"WS2_32.dll!#23", ws_socket},
    {"WS2_32.dll!#11", ws_inet_addr},
    {"WS2_32.dll!#9", ws_htons},
    {"WS2_32.dll!#2", ws_bind},
    {"WS2_32.dll!#4", ws_connect},
    {"WS2_32.dll!#10", ws_ioctlsocket},
    {"WS2_32.dll!#21", ws_setsockopt},
    {"WS2_32.dll!#19", ws_send},
    {"WS2_32.dll!#18", ws_select},
    {"WS2_32.dll!#57", ws_gethostname},
    {"WS2_32.dll!#52", ws_gethostbyname},
    {"WS2_32.dll!#12", ws_inet_ntoa},
    {"WS2_32.dll!#16", ws_recv},
    {"WS2_32.dll!#111", ws_last_error},
    {"WS2_32.dll!#151", ws_fd_isset},
    {"WS2_32.dll!#13", ws_listen},
    {"WS2_32.dll!#1", ws_accept},
    {"WS2_32.dll!#15", ws_ntohs},
    {"WS2_32.dll!#8", ws_htonl},
    {"KERNEL32.dll!GetModuleHandleA", GetModuleHandleA},
    {"KERNEL32.dll!LoadLibraryA", LoadLibraryA},
    {"AVIFIL32.dll!AVIFileInit", AVIFileInit},
    {"AVIFIL32.dll!AVIFileOpenA", AVIFileOpenA},
    {"AVIFIL32.dll!AVIFileGetStream", AVIFileGetStream},
    {"AVIFIL32.dll!AVIStreamInfoA", AVIStreamInfoA},
    {"AVIFIL32.dll!AVIStreamGetFrameOpen", AVIStreamGetFrameOpen},
    {"AVIFIL32.dll!AVIStreamGetFrame", AVIStreamGetFrame},
    {"AVIFIL32.dll!AVIStreamGetFrameClose", AVIStreamGetFrameClose},
    {"AVIFIL32.dll!AVIStreamRelease", AVIStreamRelease},
    {"AVIFIL32.dll!AVIFileRelease", AVIFileRelease},
    {"KERNEL32.dll!GetModuleHandleExW", GetModuleHandleExW},
    {"KERNEL32.dll!GetModuleFileNameA", GetModuleFileNameA},
    {"KERNEL32.dll!GetModuleFileNameW", GetModuleFileNameW},
    {"KERNEL32.dll!GetCurrentDirectoryW", GetCurrentDirectoryW},
    {"KERNEL32.dll!SetCurrentDirectoryW", SetCurrentDirectoryW},
    {"KERNEL32.dll!GetFullPathNameA", GetFullPathNameA},
    {"KERNEL32.dll!GetFullPathNameW", GetFullPathNameW},
    {"KERNEL32.dll!CreateDirectoryA", CreateDirectoryA},
    {"KERNEL32.dll!CreateFileA", CreateFileA},
    {"KERNEL32.dll!CreateFileW", CreateFileW},
    {"KERNEL32.dll!ReadFile", ReadFile},
    {"KERNEL32.dll!WriteFile", WriteFile},
    {"KERNEL32.dll!SetFilePointer", SetFilePointer},
    {"KERNEL32.dll!SetFilePointerEx", SetFilePointerEx},
    {"KERNEL32.dll!GetFileSize", GetFileSize},
    {"KERNEL32.dll!GetFileSizeEx", GetFileSizeEx},
    {"KERNEL32.dll!GetFileTime", GetFileTime},
    {"KERNEL32.dll!FileTimeToLocalFileTime", FileTimeToLocalFileTime},
    {"KERNEL32.dll!FileTimeToSystemTime", FileTimeToSystemTime},
    {"KERNEL32.dll!SystemTimeToFileTime", SystemTimeToFileTime},
    {"KERNEL32.dll!CloseHandle", CloseHandle},
    {"KERNEL32.dll!AreFileApisANSI", AreFileApisANSI},
    {"KERNEL32.dll!OutputDebugStringA", OutputDebugStringA},
    {"KERNEL32.dll!GetFileAttributesA", GetFileAttributesA},
    {"KERNEL32.dll!GetFileAttributesExW", GetFileAttributesExW},
    {"KERNEL32.dll!GetFileType", GetFileType},
    {"KERNEL32.dll!FindFirstFileA", FindFirstFileA},
    {"KERNEL32.dll!FindFirstFileW", FindFirstFileW},
    {"KERNEL32.dll!FindFirstFileExA", FindFirstFileExA},
    {"KERNEL32.dll!FindFirstFileExW", FindFirstFileExW},
    {"KERNEL32.dll!FindNextFileA", FindNextFileA},
    {"KERNEL32.dll!FindNextFileW", FindNextFileW},
    {"KERNEL32.dll!FindClose", FindClose},
    {"KERNEL32.dll!DeleteFileA", DeleteFileA},
    {"KERNEL32.dll!DeleteFileW", DeleteFileW},
    {"KERNEL32.dll!SetEndOfFile", SetEndOfFile},
    {"KERNEL32.dll!FlushFileBuffers", FlushFileBuffers},
    {"KERNEL32.dll!WritePrivateProfileStringA", WritePrivateProfileStringA},
    {"KERNEL32.dll!WaitForSingleObject", WaitForSingleObject},
    {"KERNEL32.dll!WaitForMultipleObjectsEx", WaitForMultipleObjectsEx},
    {"KERNEL32.dll!GetExitCodeThread", GetExitCodeThread},
    {"KERNEL32.dll!TerminateThread", TerminateThread},
    {"KERNEL32.dll!Sleep", Sleep},
    {"KERNEL32.dll!GetTickCount", GetTickCount},
    {"KERNEL32.dll!GlobalMemoryStatus", GlobalMemoryStatus},
    {"KERNEL32.dll!GetLocalTime", GetLocalTime},
    {"KERNEL32.dll!GetSystemTime", GetSystemTime},
    {"KERNEL32.dll!GetEnvironmentStringsW", GetEnvironmentStringsW},
    {"KERNEL32.dll!FreeEnvironmentStringsW", FreeEnvironmentStringsW},
    {"KERNEL32.dll!WideCharToMultiByte", WideCharToMultiByte},
};

}  // namespace

void set_pad_snapshot(const PadSnapshot& pad) { pad_snapshot = pad; }

namespace {
PadSource* pad_source = nullptr;
}

void set_pad_source(PadSource* source) { pad_source = source; }

void sample_pad() {
    if (pad_source != nullptr) pad_snapshot = pad_source->sample();
}

// SHELL_MSG_SETCREDIT (0x2D) carries the credit count itself. The game spends
// credits on its own, so a coin is the count it holds now (2019 0x15278F8 + 7, 0xFF
// before the first message) plus one.
void shell_insert_coin(const std::uint8_t* mem) {
    if (shell_socket == 0) return;  // the link is not up yet
    const std::uint8_t now = mem[game_addresses().shell_credits + 7u];
    const std::uint8_t credits = static_cast<std::uint8_t>(now == 0xFF ? 1 : std::min(now + 1, 99));
    shell_inbox.push_back(dyt_packet(0x2D, &credits, 1));
}

const dcusa::PadSnapshot& current_pad() { return pad_snapshot; }

void post_guest_message(std::uint32_t hwnd, std::uint32_t message, std::uint32_t wparam, std::uint32_t lparam) {
    messages.push_back(GuestMsg{hwnd, message, wparam, lparam});
}

bool guest_heap_committed(std::uint32_t address) { return heap_committed(address); }

GuestFn import_handler(std::string_view name) {
    for (const Handler& h : kHandlers)
        if (name == h.name) return h.fn;
    return nullptr;
}

}

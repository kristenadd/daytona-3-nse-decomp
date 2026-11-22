// Stdcall import handlers: the callee pops its arguments, and an unknown name
// stays fail-closed.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>

#include "recomp/host_files.hpp"

#include "check.hpp"
#include "recomp/d3d11.hpp"
#include "recomp/native.hpp"
#include "recomp/guest_memory.hpp"
#include "recomp/threads.hpp"
#include "recomp/win32.hpp"
#include "recomp/x86ops.hpp"

using namespace dcusa::recomp;

namespace {

constexpr std::uint32_t Stack = 0x210000;
constexpr std::uint32_t Buf = 0x200000;

Cpu make_cpu(GuestMemory& mem) {
    Cpu c;
    c.mem = mem.base();
    c.fs_base = 0x10000u;
    c.r[Cpu::ESI] = 0x11111111u;
    c.r[Cpu::EDI] = 0x22222222u;
    c.r[Cpu::EBX] = 0x33333333u;
    c.r[Cpu::EBP] = 0x44444444u;
    c.df = 1;
    return c;
}

void push_args(Cpu& c, std::initializer_list<std::uint32_t> args) {
    st32(c.mem, Stack, 0);
    unsigned i = 0;
    for (std::uint32_t a : args) st32(c.mem, Stack + 4u + 4u * i++, a);
    c.r[Cpu::ESP] = Stack;
}

void call(Cpu& c, const char* name, std::initializer_list<std::uint32_t> args) {
    GuestFn fn = import_handler(name);
    REQUIRE(fn != nullptr);
    push_args(c, args);
    fn(c);
}

void expect_saved(const Cpu& c) {
    CHECK(c.r[Cpu::ESI] == 0x11111111u);
    CHECK(c.r[Cpu::EDI] == 0x22222222u);
    CHECK(c.r[Cpu::EBX] == 0x33333333u);
    CHECK(c.r[Cpu::EBP] == 0x44444444u);
    CHECK(c.df == 1);
}

}  // namespace

int main() {
    CHECK(import_handler("KERNEL32.dll!DoesNotExist") == nullptr);
    GuestFn startup = import_handler("KERNEL32.dll!GetStartupInfoW");
    REQUIRE(startup != nullptr);

    GuestMemory mem;
    std::string error;
    REQUIRE(mem.reserve(&error));
    REQUIRE(mem.commit(Buf, 0x1000, &error));
    REQUIRE(mem.commit(Stack, 0x1000, &error));
    REQUIRE(mem.commit(kProcessHeap, kHeapBytes, &error));
    REQUIRE(mem.commit(0x10000, 0x1000, &error));
    REQUIRE(mem.commit(0x20000, 0x1000, &error));
    std::uint8_t* m = mem.base();
    std::memset(m + Buf, 0xFF, 0x50);
    st32(m, 0x10000 + 0x24, 1);  // the main thread's id

    Cpu c = make_cpu(mem);
    push_args(c, {Buf});
    startup(c);
    expect_saved(c);
    CHECK(c.r[Cpu::ESP] == Stack + 8);
    CHECK(c.r[Cpu::EAX] == 0);
    std::uint8_t expect[0x44] = {};
    expect[0] = 0x44;  // cb. dwFlags at +0x2C stays 0, so STARTF_USESHOWWINDOW is clear.
    CHECK(std::memcmp(m + Buf, expect, 0x44) == 0);
    CHECK(m[Buf + 0x44] == 0xFF);

    st32(m, 0x10024, 7);
    st32(m, 0x20008, 0x400000);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetProcessHeap", {});
    expect_saved(c);
    CHECK(c.r[Cpu::ESP] == Stack + 4);
    CHECK(c.r[Cpu::EAX] == kProcessHeap);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!HeapAlloc", {kProcessHeap, 0x8u, 4u});
    expect_saved(c);
    CHECK(c.r[Cpu::ESP] == Stack + 16);
    std::uint32_t block = c.r[Cpu::EAX];
    CHECK(block != 0 && (block & 15) == 0);
    CHECK(ld32(m, block) == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!HeapSize", {kProcessHeap, 0, block});
    CHECK(c.r[Cpu::EAX] == 4);
    st32(m, block, 0xAABBCCDD);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!HeapReAlloc", {kProcessHeap, 0, block, 8u});
    CHECK(c.r[Cpu::EAX] != 0);
    CHECK(c.r[Cpu::ESP] == Stack + 20);
    std::uint32_t grown = c.r[Cpu::EAX];
    CHECK(ld32(m, grown) == 0xAABBCCDD);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!HeapReAlloc", {kProcessHeap, 0x10u, grown, 64u});
    CHECK(c.r[Cpu::EAX] == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!HeapAlloc", {kProcessHeap, 0x8u, 4u});
    CHECK(c.r[Cpu::EAX] == block);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!EncodePointer", {0});
    CHECK(c.r[Cpu::EAX] != 0);
    std::uint32_t encoded_null = c.r[Cpu::EAX];
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!DecodePointer", {encoded_null});
    CHECK(c.r[Cpu::EAX] == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!EncodePointer", {0x1234u});
    std::uint32_t encoded = c.r[Cpu::EAX];
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!DecodePointer", {encoded});
    CHECK(c.r[Cpu::EAX] == 0x1234u);

    unsigned i = 0;
    for (char ch : {'k', 'e', 'r', 'n', 'e', 'l', '3', '2', '.', 'd', 'l', 'l'}) {
        st16(m, Buf + 2 * i++, std::uint16_t(ch));
    }
    st16(m, Buf + 2 * i, 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetModuleHandleW", {Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetModuleHandleW", {0});
    CHECK(c.r[Cpu::EAX] == 0x400000u);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetProcAddress", {1u, Buf});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 12);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!TlsAlloc", {});
    CHECK(c.r[Cpu::EAX] == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!TlsSetValue", {0u, 0xABCDu});
    CHECK(c.r[Cpu::EAX] == 1);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!TlsGetValue", {0u});
    CHECK(c.r[Cpu::EAX] == 0xABCDu);
    CHECK(ld32(m, 0x10000 + 0x34) == 0);

    st32(m, Buf, 41);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!InterlockedIncrement", {Buf});
    CHECK(c.r[Cpu::EAX] == 42);
    CHECK(ld32(m, Buf) == 42);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetCurrentThread", {});
    CHECK(c.r[Cpu::EAX] == 0xFFFFFFFEu);
    CHECK(c.r[Cpu::ESP] == Stack + 4);

    std::memset(m + Buf, 0xFF, 32);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetThreadTimes", {0xFFFFFFFEu, Buf, Buf + 8, Buf + 16, Buf + 24});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(c.r[Cpu::ESP] == Stack + 24);
    CHECK(ld32(m, Buf) == 0 && ld32(m, Buf + 4) == 0);
    CHECK(ld32(m, Buf + 8) == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetThreadTimes", {0u, Buf, Buf + 8, Buf + 16, Buf + 24});
    CHECK(c.r[Cpu::EAX] == 0);

    std::memset(m + Buf, 0, 8);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!QueryPerformanceCounter", {Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(c.r[Cpu::ESP] == Stack + 8);
    CHECK((ld32(m, Buf) | ld32(m, Buf + 4)) != 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!QueryPerformanceFrequency", {Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf) == 10000000u);
    CHECK(ld32(m, Buf + 4) == 0);

    st32(m, Buf, 0x94);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetVersionExA", {Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf + 4) == 6);
    CHECK(ld32(m, Buf + 8) == 1);
    st32(m, Buf, 4);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetVersionExA", {Buf});
    CHECK(c.r[Cpu::EAX] == 0);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!VirtualAlloc", {0u, 0x1Cu, 0x101000u, 4u});
    CHECK(c.r[Cpu::EAX] == kProcessHeap + kHeapBytes);
    CHECK(c.r[Cpu::ESP] == Stack + 20);
    st32(m, c.r[Cpu::EAX], 0x11);
    CHECK(ld32(m, kProcessHeap + kHeapBytes) == 0x11);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!VirtualFree", {kProcessHeap + kHeapBytes, 0u, 0x8000u});
    CHECK(c.r[Cpu::EAX] == 1);

    const char spec[] = "a\\b";
    for (unsigned n = 0; n < sizeof(spec); ++n) st16(m, Buf + n * 2, static_cast<std::uint16_t>(spec[n]));
    c = make_cpu(mem);
    call(c, "SHLWAPI.dll!PathRemoveFileSpecW", {Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld16(m, Buf) == 'a');
    CHECK(ld16(m, Buf + 2) == 0);
    c = make_cpu(mem);
    call(c, "SHLWAPI.dll!PathIsDirectoryW", {Buf});
    CHECK(c.r[Cpu::EAX] == 0);
    st16(m, Buf, 0);
    c = make_cpu(mem);
    call(c, "SHLWAPI.dll!PathIsDirectoryW", {Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    st16(m, Buf, 'a');
    st16(m, Buf + 2, 0);
    st16(m, Buf + 4, 'b');
    st16(m, Buf + 6, 0);
    c = make_cpu(mem);
    call(c, "SHLWAPI.dll!PathAppendW", {Buf, Buf + 4});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld16(m, Buf + 2) == '\\');
    CHECK(ld16(m, Buf + 4) == 'b');

    const char loopback[] = "127.0.0.1";
    for (unsigned n = 0; n < sizeof(loopback); ++n) st8(m, Buf + n, static_cast<std::uint8_t>(loopback[n]));
    c = make_cpu(mem);
    call(c, "WS2_32.dll!#57", {Buf, 255u});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(m[Buf] == 'l');
    for (unsigned n = 0; n < sizeof(loopback); ++n) st8(m, Buf + n, static_cast<std::uint8_t>(loopback[n]));

    c = make_cpu(mem);
    call(c, "WS2_32.dll!#19", {1u, Buf, 4u, 0u});
    CHECK(c.r[Cpu::EAX] == 4);

    c = make_cpu(mem);
    call(c, "WS2_32.dll!#21", {1u, 0xFFFFu, 0x1006u, Buf, 4u});
    CHECK(c.r[Cpu::EAX] == 0);

    c = make_cpu(mem);
    call(c, "WS2_32.dll!#10", {1u, 0x8004667Eu, Buf});
    CHECK(c.r[Cpu::EAX] == 0);

    c = make_cpu(mem);
    call(c, "WS2_32.dll!#4", {1u, Buf, 16u});
    CHECK(c.r[Cpu::EAX] == 0);

    c = make_cpu(mem);
    call(c, "WS2_32.dll!#2", {1u, Buf, 16u});
    CHECK(c.r[Cpu::EAX] == 0);
    c = make_cpu(mem);
    call(c, "WS2_32.dll!#13", {1u, 5u});
    CHECK(c.r[Cpu::EAX] == 0);

    c = make_cpu(mem);
    call(c, "WS2_32.dll!#9", {0x6978u});
    CHECK(c.r[Cpu::EAX] == 0x7869u);
    c = make_cpu(mem);
    call(c, "WS2_32.dll!#8", {0x0100007Fu});
    CHECK(c.r[Cpu::EAX] == 0x7F000001u);

    c = make_cpu(mem);
    call(c, "WS2_32.dll!#11", {Buf});
    CHECK(c.r[Cpu::EAX] == 0x0100007Fu);

    c = make_cpu(mem);
    call(c, "WS2_32.dll!#23", {2u, 1u, 6u});
    CHECK(c.r[Cpu::EAX] == 1);

    c = make_cpu(mem);
    call(c, "WS2_32.dll!#115", {0x202u, Buf});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld16(m, Buf) == 0x0202);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetPrivateProfileIntA", {0u, 0u, 7u, 0u});
    CHECK(c.r[Cpu::EAX] == 7);

    st32(m, Buf, 0);
    st32(m, Buf + 4, 0);
    st32(m, Buf + 8, 640);
    st32(m, Buf + 12, 480);
    c = make_cpu(mem);
    call(c, "USER32.dll!AdjustWindowRect", {Buf, 0x00CF0000u, 0u});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf + 8) == 640);
    const char messy[] = "a\\..\\b";
    for (unsigned k = 0; k < sizeof(messy); ++k) st16(m, Buf + k * 2, static_cast<std::uint16_t>(messy[k]));
    c = make_cpu(mem);
    call(c, "SHLWAPI.dll!PathCanonicalizeW", {Buf + 64, Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld16(m, Buf + 64) == 'b');
    CHECK(ld16(m, Buf + 66) == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetSystemInfo", {Buf});
    CHECK(ld32(m, Buf + 20) == 1);
    CHECK(ld32(m, Buf + 4) == 4096);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!IsDBCSLeadByteEx", {0u, 0x81u});
    CHECK(c.r[Cpu::EAX] == 0);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!SetThreadPriority", {0x3001u, 2u});
    CHECK(c.r[Cpu::EAX] == 1);

    c = make_cpu(mem);
    // Suspended: the procedure is not in this test's dispatch table.
    call(c, "KERNEL32.dll!CreateThread", {0u, 0u, 0x591C30u, 0x1000u, 4u, Buf});
    const std::uint32_t thread = c.r[Cpu::EAX];
    CHECK(thread >= kSyncHandleBase);
    CHECK(ld32(m, Buf) == 2);

    c = make_cpu(mem);
    call(c, "USER32.dll!ShowWindow", {0x2001u, 5u});
    CHECK(c.r[Cpu::EAX] == 0);

    c = make_cpu(mem);
    call(c, "USER32.dll!CreateWindowExW", {0u, 0u, 0u, 0u, 0u, 0u, 640u, 480u, 0u, 0u, 0u, 0u});
    CHECK(c.r[Cpu::EAX] == 0x2001u);

    c = make_cpu(mem);
    call(c, "USER32.dll!ShowCursor", {1u});
    CHECK(c.r[Cpu::EAX] == 1);
    c = make_cpu(mem);
    call(c, "USER32.dll!ShowCursor", {0u});
    CHECK(c.r[Cpu::EAX] == 0);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!CreateDirectoryW", {Buf + 4, 0u});
    CHECK(c.r[Cpu::EAX] == 1);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetModuleFileNameW", {0u, Buf, 0x104u});
    CHECK(c.r[Cpu::EAX] == 11);
    CHECK(ld16(m, Buf) == 'D');
    CHECK(ld16(m, Buf + 22) == 0);

    c = make_cpu(mem);
    call(c, "WINMM.dll!timeBeginPeriod", {1u});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 8);
    c = make_cpu(mem);
    call(c, "WINMM.dll!timeGetTime", {});
    CHECK(c.r[Cpu::ESP] == Stack + 4);

    c = make_cpu(mem);
    call(c, "ole32.dll!CoInitializeEx", {0u, 0u});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 12);
    c = make_cpu(mem);
    call(c, "ole32.dll!CoUninitialize", {});
    CHECK(c.r[Cpu::ESP] == Stack + 4);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetCommandLineW", {});
    CHECK(c.r[Cpu::EAX] == kProcessHeap + kHeapBytes - 512 + 128);
    CHECK(ld16(m, c.r[Cpu::EAX]) == 'D');
    CHECK(ld16(m, c.r[Cpu::EAX] + 22) == 0);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!CreateEventA", {0u, 0u, 0u, 0u});
    const std::uint32_t event_a = c.r[Cpu::EAX];
    CHECK(event_a >= kSyncHandleBase);
    CHECK(c.r[Cpu::ESP] == Stack + 20);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!CreateEventA", {0u, 1u, 1u, 0u});
    const std::uint32_t event_set = c.r[Cpu::EAX];
    CHECK(event_set != event_a);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!CreateEventW", {0u, 0u, 0u, 0u});
    CHECK(c.r[Cpu::EAX] != event_set && c.r[Cpu::EAX] != event_a);

    std::memcpy(m + Buf, "1.0f", 5);
    std::memset(m + Buf + 16, 0xFF, 8);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetPrivateProfileStringA", {0u, 0u, Buf, Buf + 16, 10u, 0u});
    CHECK(c.r[Cpu::EAX] == 4);
    CHECK(c.r[Cpu::ESP] == Stack + 28);
    CHECK(std::memcmp(m + Buf + 16, "1.0f", 5) == 0);

    std::memset(m + Buf, 0xFF, 9);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!InitializeSListHead", {Buf});
    CHECK(c.r[Cpu::ESP] == Stack + 8);
    CHECK(ld32(m, Buf) == 0 && ld32(m, Buf + 4) == 0);
    CHECK(m[Buf + 8] == 0xFF);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetCurrentThreadId", {});
    CHECK(c.r[Cpu::EAX] == 7);
    CHECK(c.r[Cpu::ESP] == Stack + 4);

    std::memset(m + Buf, 0xFF, 24);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!InitializeCriticalSectionAndSpinCount", {Buf, 0xFA0u});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf + 4) == 0xFFFFFFFFu);
    CHECK(ld32(m, Buf + 20) == 0xFA0u);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!EnterCriticalSection", {Buf});
    CHECK(ld32(m, Buf + 12) == 7);
    CHECK(ld32(m, Buf + 8) == 1);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!EnterCriticalSection", {Buf});
    CHECK(ld32(m, Buf + 8) == 2);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!LeaveCriticalSection", {Buf});
    CHECK(ld32(m, Buf + 8) == 1);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!LeaveCriticalSection", {Buf});
    CHECK(ld32(m, Buf + 12) == 0);
    CHECK(ld32(m, Buf + 4) == 0xFFFFFFFFu);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetStdHandle", {0xFFFFFFF6u});
    expect_saved(c);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 8);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetCommandLineA", {});
    CHECK(c.r[Cpu::ESP] == Stack + 4);
    CHECK(std::strcmp(reinterpret_cast<char*>(m + c.r[Cpu::EAX]), "Daytona.exe") == 0);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetModuleFileNameA", {0u, Buf, 0x104u});
    CHECK(c.r[Cpu::EAX] == 11);
    CHECK(std::strcmp(reinterpret_cast<char*>(m + Buf), "Daytona.exe") == 0);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetEnvironmentStringsW", {});
    std::uint32_t env = c.r[Cpu::EAX];
    CHECK(ld16(m, env) == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!WideCharToMultiByte", {0u, 0u, env, 1u, 0u, 0u, 0u, 0u});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(c.r[Cpu::ESP] == Stack + 36);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!WideCharToMultiByte", {0u, 0u, env, 1u, Buf, 4u, 0u, 0u});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(m[Buf] == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!FreeEnvironmentStringsW", {env});
    CHECK(c.r[Cpu::EAX] == 1);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!SetLastError", {0x20u});
    CHECK(ld32(m, 0x10000 + 0x34) == 0x20);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetLastError", {});
    CHECK(c.r[Cpu::EAX] == 0x20);
    CHECK(c.r[Cpu::ESP] == Stack + 4);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetSystemTimeAsFileTime", {Buf});
    CHECK(ld32(m, Buf + 4) > 0x01C00000u);
    CHECK(c.r[Cpu::ESP] == Stack + 8);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!SetUnhandledExceptionFilter", {0xC3F542u});
    CHECK(c.r[Cpu::EAX] == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!SetUnhandledExceptionFilter", {0u});
    CHECK(c.r[Cpu::EAX] == 0xC3F542u);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!IsProcessorFeaturePresent", {10u});
    CHECK(c.r[Cpu::EAX] == 1);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!IsProcessorFeaturePresent", {23u});
    CHECK(c.r[Cpu::EAX] == 0);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetACP", {});
    CHECK(c.r[Cpu::EAX] == 1252);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetOEMCP", {});
    CHECK(c.r[Cpu::EAX] == 437);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!IsValidCodePage", {1252u});
    CHECK(c.r[Cpu::EAX] == 1);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!IsValidCodePage", {99999u});
    CHECK(c.r[Cpu::EAX] == 0);

    std::memset(m + Buf, 0xFF, 18);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetCPInfo", {1252u, Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf) == 1);
    CHECK(m[Buf + 4] == '?');
    CHECK(m[Buf + 6] == 0);

    m[Buf] = 0x80;
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!MultiByteToWideChar", {1252u, 0u, Buf, 1u, Buf + 16, 1u});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld16(m, Buf + 16) == 0x20AC);

    st16(m, Buf, 'A');
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetStringTypeW", {1u, Buf, 1u, Buf + 16});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld16(m, Buf + 16) == 0x0381);

    st16(m, Buf, 'b');
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!LCMapStringW", {0u, 0x200u, Buf, 1u, Buf + 16, 1u});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld16(m, Buf + 16) == 'B');
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!LCMapStringW", {0u, 0x300u, Buf, 1u, Buf + 16, 1u});
    CHECK(c.r[Cpu::EAX] == 0);

    c = make_cpu(mem);
    call(c, "USER32.dll!GetForegroundWindow", {});
    CHECK(c.r[Cpu::EAX] == 0x2001u);
    c = make_cpu(mem);
    call(c, "USER32.dll!SetForegroundWindow", {0x2001u});
    CHECK(c.r[Cpu::EAX] == 1);
    c = make_cpu(mem);
    call(c, "USER32.dll!SetWindowLongW", {0x2001u, 0xFFFFFFFCu, 0x6A0740u});
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Buf, 9);
    st32(m, Buf + 4, 9);
    c = make_cpu(mem);
    call(c, "USER32.dll!GetCursorPos", {Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf) == 0);
    CHECK(ld32(m, Buf + 4) == 0);
    st32(m, Buf, 3);
    st32(m, Buf + 4, 4);
    c = make_cpu(mem);
    call(c, "USER32.dll!ScreenToClient", {0x2001u, Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf) == 3);
    CHECK(ld32(m, Buf + 4) == 4);
    c = make_cpu(mem);
    call(c, "XINPUT9_1_0.dll!XInputGetState", {0u, Buf});
    CHECK(c.r[Cpu::EAX] == 1167);
    c = make_cpu(mem);
    call(c, "USER32.dll!PeekMessageW", {Buf, 0x2001u, 0u, 0u, 1u});
    CHECK(c.r[Cpu::EAX] == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!RaiseException", {0x406D1388u, 0u, 4u, Buf});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 20);

    c = make_cpu(mem);
    call(c, "d3d11.dll!D3D11CreateDevice", {0u, 1u, 0u, 0u, 0u, 7u, 7u, Buf, Buf + 4, Buf + 8});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 4) == 0xB000u);
    const std::uint32_t device = ld32(m, Buf);
    CHECK(device != 0);
    const std::uint32_t slot = ld32(m, ld32(m, device) + 0x84);
    st32(m, Buf + 64, 1);
    st32(m, Buf + 68, 1);
    st32(m, Stack, 0);
    st32(m, Stack + 4, device);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, Buf + 64);
    st32(m, Stack + 16, 8);
    c.r[Cpu::ESP] = Stack;
    call_com(c, slot);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 64) == 0);

    st32(m, Buf + 64, 4);
    st32(m, Buf + 68, 0);
    st32(m, Buf + 72, 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, device);
    st32(m, Stack + 8, Buf + 64);
    st32(m, Stack + 12, Buf + 72);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, device) + 0x60));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 72) != 0);

    c = make_cpu(mem);
    call(c, "D3DCOMPILER_46.dll!D3DCompile", {0u, 4u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, Buf + 96, 0u});
    CHECK(c.r[Cpu::EAX] == 0);
    const std::uint32_t blob = ld32(m, Buf + 96);
    CHECK(blob != 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, blob);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, blob) + 0x10));
    CHECK(c.r[Cpu::EAX] == 16);
    st32(m, Stack, 0);
    st32(m, Stack + 4, blob);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, blob) + 0x0C));
    CHECK(ld32(m, c.r[Cpu::EAX]) == 0x43425844u);

    st32(m, Buf + 112, 0x8D536CA1u);
    st16(m, Buf + 116, 0x0CCA);
    st16(m, Buf + 118, 0x4956);
    const std::uint8_t rest[] = {0xA8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84};
    for (std::uint32_t b = 0; b < 8; ++b) m[Buf + 120 + b] = rest[b];
    c = make_cpu(mem);
    call(c, "D3DCOMPILER_46.dll!D3DReflect", {blob + 32, 16u, Buf + 112, Buf + 128});
    CHECK(c.r[Cpu::EAX] == 0);
    const std::uint32_t refl = ld32(m, Buf + 128);
    CHECK(refl != 0);
    st32(m, Buf + 144, 1);
    st32(m, Stack, 0);
    st32(m, Stack + 4, refl);
    st32(m, Stack + 8, Buf + 144);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, refl) + 0x0C));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 144 + 0x10) == 0);

    st32(m, Buf + 160, 64);
    st32(m, Buf + 164, 0);
    st32(m, Buf + 168, 1);
    st32(m, Buf + 176, 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, device);
    st32(m, Stack + 8, Buf + 160);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, Buf + 176);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, device) + 0x0C));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 176) != 0);

    st32(m, Buf + 192, 4);
    st32(m, Buf + 196, 4);
    st32(m, Buf + 208, 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, device);
    st32(m, Stack + 8, Buf + 192);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, Buf + 208);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, device) + 0x14));
    const std::uint32_t tex = ld32(m, Buf + 208);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(tex != 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, tex);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 4);
    st32(m, Stack + 16, Buf);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, tex) + 0x14));
    CHECK(c.r[Cpu::EAX] == 0);

    const std::uint32_t ctx = ld32(m, Buf + 8);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0x84));
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 1);
    st32(m, Stack + 12, Buf + 256);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0xB0));
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 1);
    st32(m, Stack + 12, Buf + 256);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0xB4));
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, Buf + 256);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0xC8));
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0x6C));
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0x70));
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0x1B8));
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Buf + 80, 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, device);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, Buf + 64);
    st32(m, Stack + 16, Buf + 80);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, device) + 0x28));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 80) != 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0xD8));
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 3);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0x20));
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 1);
    st32(m, Stack + 16, 0);
    st32(m, Stack + 20, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0xD4));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 24);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 1);
    st32(m, Stack + 16, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0x64));
    CHECK(c.r[Cpu::EAX] == 0);
    st32(m, Buf + 96, 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, 4);
    st32(m, Stack + 20, 0);
    st32(m, Stack + 24, Buf + 96);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0x38));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 96) == 0x31000000u);
    CHECK(c.r[Cpu::ESP] == Stack + 28);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0x3C));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 16);
    st32(m, Buf + 100, 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, device);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 3);
    st32(m, Stack + 16, 0);
    st32(m, Stack + 20, 16);
    st32(m, Stack + 24, Buf + 100);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, device) + 0x2C));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 100) != 0);
    CHECK(c.r[Cpu::ESP] == Stack + 28);
    st32(m, Buf + 104, 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, device);
    st32(m, Stack + 8, Buf + 64);
    st32(m, Stack + 12, Buf + 104);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, device) + 0x58));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 104) != 0);
    st32(m, Buf + 108, 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, device);
    st32(m, Stack + 8, Buf + 64);
    st32(m, Stack + 12, Buf + 108);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, device) + 0x54));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 108) != 0);
    st32(m, Buf + 112, 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, device);
    st32(m, Stack + 8, Buf + 64);
    st32(m, Stack + 12, Buf + 112);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, device) + 0x50));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 112) != 0);

    auto ctx_slot = [&](std::uint32_t slot, unsigned args, std::uint32_t expect_esp) {
        st32(m, Stack, 0);
        st32(m, Stack + 4, ctx);
        for (unsigned i = 0; i < args; ++i) st32(m, Stack + 8 + 4u * i, 0);
        c.r[Cpu::ESP] = Stack;
        call_com(c, ld32(m, ld32(m, ctx) + slot));
        CHECK(c.r[Cpu::EAX] == 0);
        CHECK(c.r[Cpu::ESP] == expect_esp);
    };
    ctx_slot(0x48, 5, Stack + 28);
    ctx_slot(0x4C, 3, Stack + 20);
    ctx_slot(0x24, 3, Stack + 20);
    ctx_slot(0x30, 3, Stack + 20);
    ctx_slot(0x44, 1, Stack + 12);
    ctx_slot(0x60, 1, Stack + 12);
    ctx_slot(0xAC, 1, Stack + 12);
    ctx_slot(0x90, 2, Stack + 16);
    st32(m, Buf + 128, 0xFFFFFFFFu);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ctx);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, Buf + 128);
    st32(m, Stack + 16, 4);
    st32(m, Stack + 20, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, ld32(m, ld32(m, ctx) + 0x74));
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 128) == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 24);
    st32(m, Buf + 132, 1);
    st32(m, Buf + 136, 1);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 1);
    st32(m, Stack + 8, Buf + 132);
    st32(m, Stack + 12, Buf + 136);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (6u << 12) + 0x2C);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 132) == 0);
    CHECK(ld32(m, Buf + 136) == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 16);
    st32(m, Buf + 140, 0);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 1);
    st32(m, Stack + 8, Buf + 140);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (6u << 12) + 0x3C);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 140) != 0);
    CHECK(c.r[Cpu::ESP] == Stack + 12);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 1);
    st32(m, Stack + 8, Buf + 160);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (6u << 12) + 0x30);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 168) == 60);
    CHECK(ld32(m, Buf + 172) == 1);
    CHECK(c.r[Cpu::ESP] == Stack + 12);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 1);
    st32(m, Stack + 8, 1);
    st32(m, Stack + 12, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (6u << 12) + 0x20);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 16);

    m[Buf] = 3;
    m[Buf + 1] = 'e';
    m[Buf + 2] = 'x';
    m[Buf + 3] = 'p';
    m[Buf + 4] = 0;
    c = make_cpu(mem);
    c.r[Cpu::EDX] = Buf;
    c.fpu.push(0);
    native_crt_unary(c);
    CHECK(c.fpu.at(0) == 1.0);

    c = make_cpu(mem);
    call(c, "WS2_32.dll!#18", {0u, Buf, 0u, 0u, 0u});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 24);

    c = make_cpu(mem);
    call(c, "d3d9.dll!D3DPERF_BeginEvent", {0xFFFFFFFFu, Buf});
    expect_saved(c);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 12);
    call(c, "d3d9.dll!D3DPERF_EndEvent", {});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 4);

    st32(m, Buf, 42);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!InterlockedDecrement", {Buf});
    CHECK(c.r[Cpu::EAX] == 41);
    CHECK(ld32(m, Buf) == 41);

    c = make_cpu(mem);
    call(c, "KERNEL32.dll!WaitForSingleObject", {event_set, 0x14u});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 12);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!WaitForSingleObject", {event_a, 0u});
    CHECK(c.r[Cpu::EAX] == 0x102u);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!SetEvent", {event_a});
    CHECK(c.r[Cpu::EAX] == 1);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!WaitForSingleObject", {event_a, 0u});
    CHECK(c.r[Cpu::EAX] == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!WaitForSingleObject", {event_a, 0u});
    CHECK(c.r[Cpu::EAX] == 0x102u);  // auto reset
    st32(m, Buf, 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetExitCodeThread", {thread, Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf) == 259u);  // suspended, still active

    namespace fs = std::filesystem;
    const fs::path root = fs::current_path() / "file-test-root";
    const fs::path over = fs::current_path() / "file-test-write";
    fs::remove_all(root);
    fs::remove_all(over);
    fs::create_directories(root / "Daytona" / "rom");
    fs::create_directories(root / "GameData");
    {
        std::ofstream note(root / "Daytona" / "rom" / "note.txt", std::ios::binary);
        note << "hello";
        std::ofstream ini(root / "GameData" / "config.ini", std::ios::binary);
        ini << "[Credit]\nFreeplay=1\n";
    }
    set_guest_files(root, "Daytona", over);

    auto wide = [&](std::uint32_t at, const char* text) {
        std::uint32_t i = 0;
        for (; text[i] != 0; ++i) st16(m, at + i * 2, static_cast<std::uint8_t>(text[i]));
        st16(m, at + i * 2, 0);
    };
    wide(Buf, "rom\\note.txt");
    c = make_cpu(mem);
    call(c, "SHLWAPI.dll!PathFileExistsW", {Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    wide(Buf, "rom");
    c = make_cpu(mem);
    call(c, "SHLWAPI.dll!PathIsDirectoryW", {Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    wide(Buf, "D:\\GameData\\config.ini");
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetFileAttributesExW", {Buf, 0u, Buf + 0x200u});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf + 0x200 + 32) == 20);

    wide(Buf, "rom\\note.txt");
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!CreateFileW", {Buf, 0x80000000u, 1u, 0u, 3u, 0x80u, 0u});
    const std::uint32_t file = c.r[Cpu::EAX];
    CHECK(file != 0xFFFFFFFFu);
    CHECK(c.r[Cpu::ESP] == Stack + 32);
    st32(m, Buf + 0x180, 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!ReadFile", {file, Buf + 0x100u, 8u, Buf + 0x180u, 0u});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf + 0x180) == 5);
    CHECK(std::memcmp(m + Buf + 0x100, "hello", 5) == 0);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetFileSize", {file, 0u});
    CHECK(c.r[Cpu::EAX] == 5);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!CloseHandle", {file});
    CHECK(c.r[Cpu::EAX] == 1);

    std::memcpy(m + Buf, "Credit", 7);
    std::memcpy(m + Buf + 16, "Freeplay", 9);
    std::memcpy(m + Buf + 32, "..\\GameData\\config.ini", 23);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!GetPrivateProfileIntA", {Buf, Buf + 16, 0u, Buf + 32});
    CHECK(c.r[Cpu::EAX] == 1);

    wide(Buf, "rom\\*");
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!FindFirstFileW", {Buf, Buf + 0x200u});
    const std::uint32_t found = c.r[Cpu::EAX];
    CHECK(found != 0xFFFFFFFFu);
    bool saw_note = false;
    auto name_is = [&](const char* want) {
        for (std::uint32_t i = 0; want[i] != 0; ++i)
            if (ld16(m, Buf + 0x200 + 44 + i * 2) != static_cast<std::uint8_t>(want[i])) return false;
        return ld16(m, Buf + 0x200 + 44 + static_cast<std::uint32_t>(std::strlen(want)) * 2) == 0;
    };
    for (;;) {
        if (name_is("note.txt")) saw_note = true;
        c = make_cpu(mem);
        call(c, "KERNEL32.dll!FindNextFileW", {found, Buf + 0x200u});
        if (c.r[Cpu::EAX] == 0) break;
    }
    CHECK(saw_note);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!FindClose", {found});
    CHECK(c.r[Cpu::EAX] == 1);

    wide(Buf, "GameData\\out.txt");
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!SetCurrentDirectoryW", {Buf});
    CHECK(c.r[Cpu::EAX] == 0);
    wide(Buf, "D:\\GameData");
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!SetCurrentDirectoryW", {Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    std::memcpy(m + Buf, "out.txt", 8);
    std::memcpy(m + Buf + 16, "abc", 3);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!CreateFileA", {Buf, 0x40000000u, 0u, 0u, 2u, 0x80u, 0u});
    const std::uint32_t out = c.r[Cpu::EAX];
    CHECK(out != 0xFFFFFFFFu);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!WriteFile", {out, Buf + 16, 3u, Buf + 0x180u, 0u});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf + 0x180) == 3);
    c = make_cpu(mem);
    call(c, "KERNEL32.dll!CloseHandle", {out});
    CHECK(fs::file_size(over / "GameData" / "out.txt") == 3);
    CHECK(fs::is_regular_file(root / "Daytona" / "rom" / "note.txt"));

    c = make_cpu(mem);
    call(c, "USER32.dll!PeekMessageW", {Buf, 0u, 0u, 0u, 1u});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 24);
    c = make_cpu(mem);
    call(c, "USER32.dll!PostQuitMessage", {0u});
    c = make_cpu(mem);
    call(c, "USER32.dll!PeekMessageW", {Buf, 0x2001u, 0u, 0u, 1u});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf + 4) == 0x12);
    c = make_cpu(mem);
    call(c, "USER32.dll!GetClientRect", {0x2001u, Buf});
    CHECK(c.r[Cpu::EAX] == 1);
    CHECK(ld32(m, Buf + 8) == 1920);
    CHECK(ld32(m, Buf + 12) == 1080);

    dcusa::PadSnapshot pad;
    pad.connected = true;
    pad.steering = 1.0f;
    pad.accelerator = 1.0f;
    pad.buttons = dcusa::PadSnapshot::Start;
    set_pad_snapshot(pad);
    c = make_cpu(mem);
    call(c, "XINPUT9_1_0.dll!XInputGetState", {0u, Buf});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld16(m, Buf + 4) == 0x0010);
    CHECK(m[Buf + 7] == 255);
    CHECK(static_cast<std::int16_t>(ld16(m, Buf + 8)) == 32767);
    pad = {};
    set_pad_snapshot(pad);

    std::memset(m + Buf, 0, 64);
    st32(m, Buf, 4);
    std::memcpy(m + Buf + 48, "abcd", 4);
    st32(m, Buf + 32, Buf + 48);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, Buf);
    st32(m, Stack + 12, Buf + 32);
    st32(m, Stack + 16, Buf + 40);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + 0x0C);
    CHECK(c.r[Cpu::EAX] == 0);
    const std::uint32_t buffer = ld32(m, Buf + 40);
    CHECK(buffer != 0);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, buffer);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, 2);
    st32(m, Stack + 20, 0);
    st32(m, Stack + 24, Buf + 64);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0x38);
    CHECK(c.r[Cpu::EAX] == 0);
    const std::uint32_t mapped = ld32(m, Buf + 64);
    CHECK(std::memcmp(m + mapped, "abcd", 4) == 0);
    m[mapped] = 'z';
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, buffer);
    st32(m, Stack + 12, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0x3C);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, buffer);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, 1);
    st32(m, Stack + 20, 0);
    st32(m, Stack + 24, Buf + 64);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0x38);
    CHECK(m[ld32(m, Buf + 64)] == 'z');

    std::memset(m + Buf, 0, 0x400);
    st32(m, Buf, 4);
    st32(m, Buf + 4, 4);
    st32(m, Buf + 8, 1);
    st32(m, Buf + 12, 1);
    st32(m, Buf + 16, 28);
    st32(m, Buf + 20, 1);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, Buf);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, Buf + 64);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + 0x14);
    const std::uint32_t color_tex = ld32(m, Buf + 64);
    CHECK(color_tex != 0);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, color_tex);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, Buf + 68);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + 0x24);
    const std::uint32_t color_rtv = ld32(m, Buf + 68);
    CHECK(color_rtv != 0);
    float red[4] = {1, 0, 0, 1};
    std::memcpy(m + Buf + 80, red, sizeof red);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, color_rtv);
    st32(m, Stack + 12, Buf + 80);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0xC8);
    st32(m, Buf + 72, color_rtv);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, 1);
    st32(m, Stack + 12, Buf + 72);
    st32(m, Stack + 16, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0x84);
    std::memcpy(m + Buf + 96, "POSITION", 9);
    st32(m, Buf + 112, Buf + 96);
    st32(m, Buf + 120, 16);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, Buf + 112);
    st32(m, Stack + 12, 1);
    st32(m, Stack + 16, 0);
    st32(m, Stack + 20, 0);
    st32(m, Stack + 24, Buf + 256);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + 0x2C);
    const std::uint32_t tri_layout = ld32(m, Buf + 256);
    CHECK(tri_layout != 0);
    float tri[6] = {0, 0, 4, 0, 0, 4};
    std::memcpy(m + Buf + 160, tri, sizeof tri);
    st32(m, Buf + 192, Buf + 160);
    st32(m, Buf + 208, 24);
    st32(m, Buf + 216, 1);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, Buf + 208);
    st32(m, Stack + 12, Buf + 192);
    st32(m, Stack + 16, Buf + 260);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + 0x0C);
    const std::uint32_t tri_vb = ld32(m, Buf + 260);
    CHECK(tri_vb != 0);
    st16(m, Buf + 280, 0);
    st16(m, Buf + 282, 1);
    st16(m, Buf + 284, 2);
    st32(m, Buf + 288, Buf + 280);
    st32(m, Buf + 304, 6);
    st32(m, Buf + 312, 2);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, Buf + 304);
    st32(m, Stack + 12, Buf + 288);
    st32(m, Stack + 16, Buf + 336);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + 0x0C);
    const std::uint32_t tri_ib = ld32(m, Buf + 336);
    CHECK(tri_ib != 0);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, tri_layout);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0x44);
    st32(m, Buf + 340, tri_vb);
    st32(m, Buf + 344, 8);
    st32(m, Buf + 348, 0);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 1);
    st32(m, Stack + 16, Buf + 340);
    st32(m, Stack + 20, Buf + 344);
    st32(m, Stack + 24, Buf + 348);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0x48);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, tri_ib);
    st32(m, Stack + 12, 57);
    st32(m, Stack + 16, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0x4C);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, 4);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0x60);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0xB4);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, 3);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (1u << 12) + 0x30);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (6u << 12) + 0x20);
    std::uint32_t fw = 0, fh = 0;
    const std::uint8_t* frame = nullptr;
    CHECK(presented_frame(&fw, &fh, &frame));
    CHECK(fw == 4 && fh == 4);
    CHECK(frame[0] == 255 && frame[1] == 255 && frame[2] == 255);
    CHECK(frame[(3 * 4 + 3) * 4] == 255 && frame[(3 * 4 + 3) * 4 + 1] == 0);

    std::memcpy(m + Buf, "NRT Application", 16);
    c = make_cpu(mem);
    call(c, "USER32.dll!FindWindowA", {Buf, 0u});
    CHECK(c.r[Cpu::EAX] == 0x2001u);
    std::memcpy(m + Buf, "Other", 6);
    c = make_cpu(mem);
    call(c, "USER32.dll!FindWindowA", {Buf, 0u});
    CHECK(c.r[Cpu::EAX] == 0);

    pad.connected = true;
    pad.steering = 1.0f;
    pad.buttons = dcusa::PadSnapshot::Start;
    set_pad_snapshot(pad);
    c = make_cpu(mem);
    call(c, "DINPUT8.dll!DirectInput8Create", {0u, 0x800u, Buf, Buf + 32, 0u});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 32) != 0);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, Buf + 48);
    st32(m, Stack + 12, Buf + 36);
    st32(m, Stack + 16, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (21u << 12) + 0x0C);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 36) != 0);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, 80);
    st32(m, Stack + 12, Buf + 64);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (22u << 12) + 0x24);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 64) == 65534u);
    CHECK(m[Buf + 64 + 48] == 0x80);
    pad = {};
    set_pad_snapshot(pad);

    st32(m, Buf + 16, 36);
    st32(m, Buf + 24, 256);
    c = make_cpu(mem);
    call(c, "DSOUND.dll!#11", {0u, Buf + 8, 0u});
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(ld32(m, Buf + 8) != 0);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, 0);
    st32(m, Stack + 8, Buf + 16);
    st32(m, Stack + 12, Buf + 12);
    st32(m, Stack + 16, 0);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (23u << 12) + 0x0C);
    CHECK(ld32(m, Buf + 12) != 0);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, ld32(m, Buf + 12));
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 256);
    st32(m, Stack + 16, Buf + 80);
    st32(m, Stack + 20, Buf + 84);
    st32(m, Stack + 24, Buf + 88);
    st32(m, Stack + 28, Buf + 92);
    st32(m, Stack + 32, 0);
    c.r[Cpu::ESP] = Stack;
    // IDirectSoundBuffer::Lock is slot 0x2C (dsound.h order), 8 dwords popped.
    call_com(c, kComCall + (24u << 12) + 0x2C);
    CHECK(c.r[Cpu::EAX] == 0);
    CHECK(c.r[Cpu::ESP] == Stack + 36);
    CHECK(ld32(m, Buf + 80) == 0x36000000u);
    CHECK(ld32(m, Buf + 84) == 256u);
    // Play looping (+0x30), then GetStatus (+0x24): DSBSTATUS_PLAYING | LOOPING.
    const std::uint32_t sound_buffer = ld32(m, Buf + 12);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, sound_buffer);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, 0);
    st32(m, Stack + 16, 1);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (24u << 12) + 0x30);
    CHECK(c.r[Cpu::ESP] == Stack + 20);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, sound_buffer);
    st32(m, Stack + 8, Buf + 96);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (24u << 12) + 0x24);
    CHECK(ld32(m, Buf + 96) == 5u);

    const char shader[] = "Texture2D tex0;\nSamplerState samp0;\n";
    std::memcpy(m + Buf, shader, sizeof shader);
    c = make_cpu(mem);
    call(c, "D3DCOMPILER_46.dll!D3DCompile",
         {Buf, static_cast<std::uint32_t>(sizeof shader - 1), 0u, 0u, 0u, 0u, 0u, 0u, 0u, Buf + 64, 0u});
    CHECK(c.r[Cpu::EAX] == 0);
    const std::uint32_t shader_blob = ld32(m, Buf + 64);
    st32(m, Buf + 80, 0x8D536CA1u);
    st16(m, Buf + 84, 0x0CCA);
    st16(m, Buf + 86, 0x4956);
    const std::uint8_t iid_rest[] = {0xA8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84};
    for (std::uint32_t b = 0; b < 8; ++b) m[Buf + 88 + b] = iid_rest[b];
    c = make_cpu(mem);
    call(c, "D3DCOMPILER_46.dll!D3DReflect", {shader_blob + 32, 16u, Buf + 80, Buf + 68});
    const std::uint32_t shader_refl = ld32(m, Buf + 68);
    CHECK(shader_refl != 0);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, shader_refl);
    st32(m, Stack + 8, Buf + 128);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (14u << 12) + 0x0C);
    CHECK(ld32(m, Buf + 128 + 16) == 2);
    c = make_cpu(mem);
    st32(m, Stack, 0);
    st32(m, Stack + 4, shader_refl);
    st32(m, Stack + 8, 0);
    st32(m, Stack + 12, Buf + 160);
    c.r[Cpu::ESP] = Stack;
    call_com(c, kComCall + (14u << 12) + 0x18);
    CHECK(ld32(m, Buf + 164) == 2);
    CHECK(std::memcmp(m + ld32(m, Buf + 160), "tex0", 4) == 0);

    // With DCUSA_SHADERS, a compile request is captured under its key, and a
    // translated module's .refl decides the slots D3DReflect reports.
    {
        namespace fs = std::filesystem;
        const fs::path shaders = fs::current_path() / "shader-test";
        fs::remove_all(shaders);
        setenv("DCUSA_SHADERS", shaders.c_str(), 1);
        auto compile = [&]() {
            c = make_cpu(mem);
            call(c, "D3DCOMPILER_46.dll!D3DCompile",
                 {Buf, static_cast<std::uint32_t>(sizeof shader - 1), 0u, 0u, 0u, 0u, 0u, 0u, 0u, Buf + 64, 0u});
            c = make_cpu(mem);
            call(c, "D3DCOMPILER_46.dll!D3DReflect", {ld32(m, Buf + 64) + 32, 16u, Buf + 80, Buf + 68});
            return ld32(m, Buf + 68);
        };
        std::memcpy(m + Buf, shader, sizeof shader);
        compile();
        std::string key;
        for (const auto& e : fs::directory_iterator(shaders / "src"))
            if (e.path().extension() == ".json") key = e.path().stem().string();
        CHECK(key.size() == 16);
        CHECK(fs::exists(shaders / "src" / (key + ".hlsl")));
        fs::create_directories(shaders / "msl");
        std::ofstream(shaders / "msl" / (key + ".refl")) << "stage frag\ntexture 3 4 tex0\nsampler 1 0 samp0\n"
                                                             "cbuffer 0 16 $Globals\nmember 0 4 float 0 - x\n"
                                                             "member 4 12 vec3 0 - dir\n";
        std::memcpy(m + Buf, shader, sizeof shader);
        const std::uint32_t prepared_refl = compile();
        c = make_cpu(mem);
        st32(m, Stack, 0);
        st32(m, Stack + 4, prepared_refl);
        st32(m, Stack + 8, Buf + 128);
        c.r[Cpu::ESP] = Stack;
        call_com(c, kComCall + (14u << 12) + 0x0C);
        CHECK(ld32(m, Buf + 128 + 12) == 1);  // ConstantBuffers
        CHECK(ld32(m, Buf + 128 + 16) == 3);  // BoundResources: texture, sampler, cbuffer
        c = make_cpu(mem);
        st32(m, Stack, 0);
        st32(m, Stack + 4, prepared_refl);
        st32(m, Stack + 8, 0);
        st32(m, Stack + 12, Buf + 160);
        c.r[Cpu::ESP] = Stack;
        call_com(c, kComCall + (14u << 12) + 0x18);
        CHECK(ld32(m, Buf + 164) == 2);  // texture
        CHECK(ld32(m, Buf + 168) == 3);  // slot from the .refl, not declaration order
        // A COM method with `this` and up to two dword arguments; eax back.
        auto com = [&](std::uint32_t self, std::uint32_t method, std::uint32_t a1, std::uint32_t a2) {
            c = make_cpu(mem);
            st32(m, Stack, 0);
            st32(m, Stack + 4, self);
            st32(m, Stack + 8, a1);
            st32(m, Stack + 12, a2);
            c.r[Cpu::ESP] = Stack;
            call_com(c, method);
            return c.r[Cpu::EAX];
        };
        const std::uint32_t reflector = kComCall + (14u << 12), cb_vt = kComCall + (25u << 12),
                            var_vt = kComCall + (26u << 12), type_vt = kComCall + (27u << 12);
        const std::uint32_t cb_obj = com(prepared_refl, reflector + 0x10, 0, 0);
        CHECK(com(cb_obj, cb_vt + 0x00, Buf + 200, 0) == 0);
        CHECK(std::strcmp(reinterpret_cast<const char*>(m + ld32(m, Buf + 200)), "$Globals") == 0);
        CHECK(ld32(m, Buf + 208) == 2);   // Variables
        CHECK(ld32(m, Buf + 212) == 16);  // Size
        CHECK(com(prepared_refl, reflector + 0x10, 0, 0) == cb_obj);  // same object again
        const std::uint32_t dir = com(cb_obj, cb_vt + 0x04, 1, 0);
        CHECK(com(dir, var_vt + 0x00, Buf + 240, 0) == 0);
        CHECK(std::strcmp(reinterpret_cast<const char*>(m + ld32(m, Buf + 240)), "dir") == 0);
        CHECK(ld32(m, Buf + 244) == 4);   // StartOffset
        CHECK(ld32(m, Buf + 248) == 12);  // Size
        const std::uint32_t dir_type = com(dir, var_vt + 0x04, 0, 0);
        CHECK(com(dir_type, type_vt + 0x00, Buf + 280, 0) == 0);
        CHECK(ld32(m, Buf + 280) == 1);   // D3D_SVC_VECTOR
        CHECK(ld32(m, Buf + 284) == 3);   // D3D_SVT_FLOAT
        CHECK(ld32(m, Buf + 292) == 3);   // Columns
        CHECK(std::strcmp(reinterpret_cast<const char*>(m + ld32(m, Buf + 308)), "float3") == 0);
        const std::uint32_t by_name = com(prepared_refl, reflector + 0x28,
                                          ld32(m, Buf + 240), 0);  // GetVariableByName("dir")
        CHECK(by_name == dir);
        const std::uint32_t missing = com(prepared_refl, reflector + 0x10, 5, 0);
        CHECK(com(missing, cb_vt + 0x00, Buf + 200, 0) == 0x80004005u);
        unsetenv("DCUSA_SHADERS");
        fs::remove_all(shaders);
    }

    clear_guest_files();
    return check_result();
}

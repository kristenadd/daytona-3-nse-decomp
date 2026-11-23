# Phase 2 — translator for Daytona: status and remaining work

Phase 2 from [PHASE0.md](PHASE0.md): turn the game's x86-32 code into C++ that
builds for arm64.

**Exit criterion:** all game, NRT and m2e functions translate with zero
unsupported operands (or an explicit native override), and the generated code
compiles in under about 15 minutes incremental.

The NRT engine and the m2e (Model 2 / i960) interpreter are both linked into
`Daytona.exe`. The `nrt/` folder holds shaders and data only, so "NRT and m2e
functions" means code inside the EXE.

The exit criterion is met. `dcrun` on the 2019 image runs from the translated
entry through locale init, the CRT startup imports, Winsock setup, window
creation and the first frame. `PeekMessageW` returns 0, so the frame never
sees `WM_QUIT` and the run stays in that loop. An 8 second timeout with no
`not ported` line is the current stop.

## Where it stands

| | 2019 (New Season) | 2017 |
| --- | --- | --- |
| Functions discovered | 97,543 | 95,591 |
| Reachable from real roots (translation set) | 51,804 | 50,621 |
| Reachable functions translated | 51,804 (100%) | 50,621 (100%) |
| Instructions translated | 2,832,960 | 2,794,031 |
| Instructions that fail closed if reached | 74 | 75 |
| Native overrides (CRT copies and searches) | 6 | 6 |
| Shards (fixed address ranges) | 91 | 89 |
| Clean build of the generated code (8 jobs, -O2) | 3 min 32 s | 3 min 32 s |

Instruction semantics: `test_x86_semantics` runs 881 cases × 8 inputs. Every
case runs as x86_64 under Rosetta 2, and the same snippet, assembled for i386,
goes through the translator. All 7,048 runs match, including locked operations
and fences. Cases longer than a few instructions are also forced through
function splitting.

### Done

- **Discovery** (`tools/x86tr/discover.py`, `cfg.py`).
  - Roots: entry point, RTTI vtables, SafeSEH handlers, C++ EH FuncInfo funclets, code
    pointers in data and immediates, and gap scanning.
  - Jump tables, including the two-level byte-indexed form; noreturn inference; tail calls.
  - Reachability from the entry, vtable, SEH, EH, data and m2e-table roots.
- **Translator** (`translate.py`, `x87.py`, `sse.py`, `semantics.py`).
  - Registers live in locals and are synced around calls and returns. The guest stack is exact:
    calls push the return address and `ret n` pops it.
  - Per-function flag liveness. A `cmp` / `test` with a single reader becomes a direct comparison.
  - Covers: integer, shifts and rotates, multiply and divide (with traps), bit ops, string ops
    (with a `memmove` / `memset` fast path), x87 decoded from the opcode bytes, MMX, SSE, SSE2,
    SSE3.
  - x86 NaN rules: x86's default NaN is negative, and the first NaN operand wins.
  - Indefinite results for float-to-integer conversions.
  - `fs:` addressing through `Cpu::fs_base`; fixed Windows segment selectors for reads.
  - Calls: jump tables become `switch`; indirect and IAT calls go through `call_indirect`;
    direct calls go straight to the target function.
  - Locked read-modify-writes and `xchg` with memory are sequentially consistent atomics;
    `mfence`, `sfence` and `lfence` are thread fences.
  - Unsupported instructions trap per instruction, not per function.
  - Oversized functions are cut into chained parts where no branch crosses the cut.
- **Output:** shards named by a frozen address cut (`tr_<start>.cpp`, 91 in 2019 and 89 in 2017),
  plus `stubs.cpp` (unreachable
  functions fail closed), `entries.cpp` (the sorted entry table) and `coverage.json`.
- **Runtime** (`src/recomp`): `Cpu` / `Fpu` state, `GuestMemory` (a 4 GB reservation), dispatch,
  and `guest.hpp` helpers (lanes, 80-bit load/store, `fxam`, `fprem`, `fscale`, NaN rules,
  `cpuid` reporting SSE2 only, `rdtsc`).
- **Loader and runner.** `GuestProcess` maps the prepared image, fills every IAT slot with
  `ImportBase + 16*k` before read-only sections are protected, seeds `__security_cookie` to
  `0x0C0FFEE1` so `__security_init_cookie` does not call the kernel, and builds a TEB (exception
  list, stack bounds, self, client id, TLS pointer, PEB image base and OS version). `dcrun` calls
  the translated entry and prints the `FailClosed` report. On the 2019 image it gets
  through locale init, the CRT startup imports, Winsock setup, window creation and
  the first frame, and stays in the frame loop.
  `test_loader` covers the mapping, the slot, the cookie and the TEB. `test_imports`
  covers the stdcall handlers.
- **CMake:**
  - `dcusa_recomp` library.
  - `dcusa_generated` and `dcrun`, built from `-DDCUSA_PREPARED_CACHE=` (the build runs
    discovery and translation) or from a hand-translated `DCUSA_GENERATED_DIR`.
  - `test_x86_semantics`, regenerated from the scripts at build time.

### Exit criterion status

| Part | Status |
| --- | --- |
| Reachable game, NRT and m2e code translates | Yes, apart from the gaps listed under item 1 below |
| Zero unsupported operands | The 74 / 75 that still trap are on the reviewed allow-list (item 2). A new trap fails translation |
| Native override path for what isn't translated | **Built** for the six CRT copy and search routines (item 6). The m2e hot loop and any lock-free sites are still open |
| Generated code compiles in under 15 min | Yes. A full 2019 rebuild is 3 min 32 s at 8 jobs. Rewriting one ordinary shard and relinking `dcrun` is 5 s; rewriting one of the 137,452-instruction constructors is 2 min 7 s (item 9) |

## Remaining work

The exit criterion is met, and the checks below are done. Two defects stay
recorded rather than fixed here: the stale-flag tails in item 10, which are not
on the path the run takes, and the 12-bit reciprocal and the 64-bit x87
precision, which wait on a Phase 5 trace. The notes stay in the order they
were written.

### 1. Discovery gaps

- **Epilogs past an SEH thunk stay in the function.** MSVC plants a tiny cleanup (`push imm;
  call; pop; ret`) between a body and its epilog and calls it, so the cleanup became the next
  function start and the walk treated the epilog as a tail to a non-entry. `dcrun` died there
  at `0xC437E1` (`call_indirect` of an address with no function). A jump past only those tiny
  returning thunks is now part of the function. Rediscovering 2019 added 8 reachable functions
  and 2,185 instructions; the trap list is unchanged. A later pass adopts every direct
  call or jump whose target is not yet a function, and splits the body that used to contain
  the label. That is what turned `call 0xC42594` from a fail-closed indirect call into a real
  function. 2019 shared-tail overlap is 2,439 bytes. 2017 keeps the counts in the table until
  its next translation.

- **m2e dispatch tables are roots now.** Both editions have a 1,025-entry table and a 4,097-entry
  table (2019: `.rdata 0x11AAEBC` and `0x11EC90C`; 2017: `0x11994E4` and `0x11DAF34`). All 426
  distinct handlers are reachable. Two of them, `0x005E0060` and `0x005F0060`, had been missed
  because those addresses look like UTF-16 text, which cut the table scan short and made the
  general pointer scan reject them. `m2e_tables` does not apply that filter.
- **`__xc` / `__xi` initializer tables are roots now.** `init_tables` finds the ranges the CRT
  actually walks (`mov esi, start; mov edi, end` beside `call eax`, and `push end; push start;
  call _initterm`). 2019: `0x116C620` (326), `0x116CB3C` (8), `0x116CB60` (6). 2017: `0x115B624`
  (328), `0x115BB48` (8), `0x115BB6C` (6). Every entry is a function start or zero. Two 2017
  initializers (`0xE28700`, 135,014 instructions, and `0xFD1B90`, 82,988) had been missed: a
  misdecoded immediate inside them was accepted first and the real function then failed the
  overlap check. Walking the tables before the immediate scan fixes that. The earlier candidate
  runs at `0x116C068` and `0x116C5DC` are not tables the CRT walks, and their mid-function
  pointers (for example `0x1125142`) stay out. Adding the real tables also dropped 27 (2019) and
  28 (2017) reachable "functions" whose addresses were bytes in the middle of an instruction.
- **CRT copy-routine jump tables are tables now.** `cfg.py` treats `and reg, M` (M one of
  1, 3, 7, 15, 31) as a bound of M + 1 entries, and a bounded table whose only non-code word is
  the final slot keeps the earlier targets. That second case is `cmp ecx, 8; jb`, where the
  compare bound is one high. 2019 `0xC1B790` and `0xC1C2C0` (2017 `0xC15F70` and `0xC16AA0`)
  each picked up an 8-entry table and had their `and edx, 3` tables marked bounded. Jump tables
  went from 668 to 674. Unresolved indirect jumps went from 428 to 424 (2019) and from 414 to
  410 (2017). Reachable instruction counts rose by 36 in each edition; the function counts did
  not change. Eight `jmp [reg*4 + imm]` sites inside those same functions still point into the
  instruction stream (the first table word is not a code address, for example `0xC1B9C9`) and
  stay indirect. Those bodies are the memmove overrides in item 6, so the translator no
  longer emits them.
- **The remaining indirect jumps are not switches.** Of the 424 in 2019, 258 sit in reachable
  functions: 107 `jmp reg`, 48 `jmp [reg + disp]`, 56 `jmp [imm32]` import thunks, 5
  `jmp [reg]`, 16 `jmp [reg+4]`, 10 `jmp [reg+8]`, the eight copy-routine sites above, and
  8 far `jmp ptr [...]` forms (`FF /5`; 10 in the whole image). Import thunks fail closed
  until that import has a handler. The far jumps are left as indirect calls.
- **Unbounded jump tables** (17, was 19). Every target of each one lies inside the same
  function, and the dword after the table is int3 padding or the next function's prologue.
  Lengths are 4 to 35. They do not pull foreign data in as code. They stay unbounded because
  the index compare is more than 12 instructions before the jump, or, for `0xC1BABF` and
  `0xC1C5EF`, because that jump is itself a case of the 8-entry table and the walker enters
  jump-table targets with an empty trace. Those two are the same 4-entry tables as the bounded
  `and edx, 3` jumps.
- **Vtable tails are no longer code.** `rtti_vtables` used to keep reading while the next word
  landed in `.text`, so the bytes after a real vtable became methods: UTF-16 (`0x650064`,
  `0x730072`) and, on the `JpegEncoder` vtable, the marker `0xFFD8FF`. A slot is kept only when
  it is a real method (it follows a `ret` / `jmp` / int3 pad, or it is a prologue or a one-instruction
  getter). A pointer into int3 padding is skipped. That removed 44 reachable functions in 2019 and
  60 in 2017, including the 58,000-instruction decode of the JPEG marker, and it cut the bytes
  claimed by two functions from 2,007 to 980 (2019) and from 222,978 to 892 (2017). The 2017
  undecodable body was one of those functions and is gone.
- **Gap-only functions** (47,707 in 2019, 47,613 in 2017) are stubs. They are believed to be
  dead code (the linker's /OPT:NOREF keeps unreferenced functions), but anything called through
  a pointer that discovery missed will fail closed at run time. Phase 5 traces will list any
  that are hit; the translator can translate them all with `--all` if needed.
- **Overlap is shared tails.** 810 bytes (2019) and 684 bytes (2017) belong to two functions.
  Both copies decode identically, both functions are reachable tail-call targets, and neither
  entry sits inside the other. The long runs are the switch at `0xB47192` (2017 `0xB41A82`)
  merging into the epilogues beside it. A function no longer covers one of its own bytes twice:
  that was 170 bytes in 2019 and 208 in 2017, from data pointers that landed inside an
  instruction (`0x495241` is the immediate of `push 0` at the real entry `0x495230`). A data or
  gap root is now kept only when it follows padding, a `ret`, or the `lea esp, [esp]` hot-patch
  nop, and only when its own instructions do not overlap. That dropped the false roots and the
  traps that only existed inside them (`gs`, `mov` to a segment register, `rdpmc`, and four
  `pmulld` in each edition).

### 2. Trapped instructions (74 in 2019, 75 in 2017)

Every one of these is on the reviewed allow-list in `tools/x86tr/trap_allow.py`. Translation
fails if a trap appears that is not listed, or if a listed address is no longer a trap.
`guest_cpuid` reports SSE2 and leaves leaf 1 ECX at 0, which is what keeps the SSE4 and AVX
rows unreachable.

| Kind | Count (2019) | Why it stays a trap |
| --- | --- | --- |
| SSSE3 / SSE4.1 / SSE4.2: `pmulld`, `pabsd`, `pminsd`, `pmaxsd`, `pcmpistri` | 12 | Behind `cmp [__isa_available], 2`, which falls through to the scalar loop. The CRT initializer (2019 `0xC285FF`, 2017 `0xC22DEF`) is the only writer, and it stores 2 only when ECX bit 20 (SSE4.2) is set. 2017 has one more `pmulld`, 13 in this row. The CRT `palignr`, `pinsrb` and three `pcmpistri` left with the overrides in item 6 |
| `pblendvb` | 2 | Behind a byte copied out of the game's feature table at static init. 2019 copies the AVX byte (`table+0xE`) to `0x1EC5D1D`; 2017 copies the SSE4.2 byte (`table+0xB`) to `0x1EB10D5`. Both stay 0 |
| AVX: `vmulps`, `vaddps`, `vmovups`, `vbroadcastss` (all `ymm`) | 54 | Two helpers (2019 `0xA1D3C0` and `0xA1D590`; 2017 `0xA17CC0` and `0xA17E90`). Each caller tests feature index `0xA` first (`0x8B6FB0` / `0x8B18B0`), the byte at `table+0xE`, set by `0x8B78F0` / `0x8B21F0` only when AVX and OSXSAVE are both set. No `xgetbv`; the two cpuid bits are enough, and both are clear |
| `int 0x29` (`__fastfail`) | 4 | Each site calls `IsProcessorFeaturePresent(23)` and skips the interrupt when it returns 0. A crash path; left as a trap |
| `fnsave` / `frstor` | 2 | The CRT x87 exception helper (2019 `0xC3946D`, 2017 `0xC33C5D`), after a special value fails the status check. Left as a trap |
| Garbage decodes (`in`, `les`, `push es`, `mov` to a segment, `gs`, `rdpmc`) | 0 | Removed with the false vtable slots and the misaligned data roots in item 1 |

### 3. Thread safety of translated code (needed before a second guest thread runs)

- **Locked operations are atomic now.** A `lock` prefix, and `xchg` with a memory operand (locked
  even without the prefix), compile to sequentially consistent `__atomic` operations
  (`atomic_rmw`, `atomic_xchg`, `atomic_cmpxchg` in `guest.hpp`). The flag results are computed
  from the value the atomic exchange observed. An unaligned locked access traps instead of
  becoming a split lock. `mfence` / `sfence` / `lfence` emit thread fences. The 2019 code has 242
  real locked instructions, all of them `xadd`, `cmpxchg`, `or`, `add` or `and`, and the semantics
  tests cover the locked forms. `pause` stays a no-op.
- **Ordinary loads and stores stay relaxed.** x86 is TSO and ARM is weakly ordered, but the
  cross-thread hand-offs in this image do not rely on a plain store becoming visible in program
  order. Audited on the 2019 image:
  - Publication is a `lock cmpxchg`, `lock xadd`, or an `xchg` (already sequentially consistent),
    or a kernel object. The CRT lock cluster at `0xC2ACD0`..`0xC2BAAB` is entirely those
    instructions. `InterlockedIncrement` and `InterlockedDecrement` are sequentially consistent
    handlers. `InterlockedExchange` and the SList trio are still unwritten.
  - The one plain load that waits for another thread is `0xC2B422`: read `*object`, `pause`, then
    `Sleep` (`0x116C2A8`). Its three callers (`0xC2AF24`, `0xC2AF54`, `0xC2B0BE`) claim the word
    with `lock cmpxchg` / `lock xadd`. The matching backoff state machines are `0x91A0B0` and
    `0xC2B245`. `0xC7A4A7` pauses between calls and publishes with `xchg`.
  - A scan for a tight loop that reloads a fixed global found 44 sites. The ones checked are
    counted copies and one-shot tests (`0x5EA360`, `0xB804E0`, `0xB84D30`), not a second thread
    waiting on a flag. There is no separate NRT render queue in the executable; NRT is code in
    this image, and it synchronizes through the same kernel objects and locked operations.
  - Decision: no TSO build mode. Acquire/release on every guest access would tax the ordinary
    loads and stores, and nothing found needs it. When a second guest thread is added, the polled
    load in `0xC2B422` has to be an acquire load so it synchronizes with the sequentially
    consistent store it observes. `pause` stays a no-op; these waits also call `Sleep`.
- Generated code has no globals, so one `Cpu` per guest thread is enough. Guest stacks are
  allocated in guest memory.

### 4. TEB, SEH and the C++ throw policy

- **The main thread's TEB is in place** (`src/recomp/loader.cpp`). ExceptionList starts at
  `0xFFFFFFFF`, StackBase / StackLimit / Self / ClientId / TLS pointer / PEB are set, and
  LastError and the TLS slots are zero. `Cpu::fs_base` points at it. The 2019 startup writes
  `fs:[0]` in `__SEH_prolog4` and reaches a later import, so those fields are being read.
  A second guest thread will need its own TEB and stack; the layout is the same.
- **SEH frames** are ordinary guest memory writes (`push fs:[0]` chains), so they need no
  translator work.
- **Throwing stays abort-on-throw.** `RaiseException` has four call sites. `0x77AFF9` raises
  `0x406D1388`, the thread-name exception, and that is the one the startup run hits; it
  already returns. `0xC1BE34` is `_CxxThrowException` (`0xC1BDE3`), which copies a record
  whose code is `0xE06D7363`. `0xC45B01` is the CRT floating-point raiser, called from three
  sites inside the math helper. `0xC610C2` is the EH runtime rethrowing, not a game throw.
  Of the 1,190 direct calls of `_CxxThrowException`, 606 pass two nulls, which is a rethrow
  and only runs inside a catch. The rest are typed failures: `ios_base::failure` (112),
  OpenEXR `Iex` errors (ArgExc 67, TypeExc 53, InputExc 30, the errno subclasses 77, and a
  few LogicExc, OverflowExc, IoExc and BaseExc), `bad_cast` (40), `invalid_argument` (23),
  and the Concurrency runtime (about 80, in the worker that is not run). `bad_alloc` is
  thrown through `0xC023B9` from 360 sites and from 28 other sites. `length_error` goes
  through `0xC0243C` (265 sites) and `out_of_range` through `0xC0246A` (110 sites).
  OpenCV's reporter at `0x8B7190` throws `cv::Exception` and has 776 callers. JSON throws
  through `0x6477C0` (12 callers).
  The 6,183 FuncInfo records discovery reaches have 642 catch handlers: 621 `catch (...)`,
  9 `Iex::BaseExc` (the handlers are in `0xA8xxxx`–`0xABxxxx` and continue), 4
  `std::exception`, 2 `bad_alloc` (both in the CRT, at `0xC60D57` and `0xC60E4F`), one
  `out_of_range`, and five Concurrency catches. The 8 second run does not throw. These
  sites are a failed allocation, a failed range check, a bad image or stream, an OpenCV
  error, or the thread pool. None of them is on the path the run takes, so the dispatcher
  stays unbuilt. A later run that stops on `RaiseException E06D7363` is a C++ exception
  one of these catches expected.
- **Security cookie:** the loader writes `0x0C0FFEE1` over the default `0xBB40E64E` before
  entry, which is the condition `__security_init_cookie` accepts without calling
  `GetSystemTimeAsFileTime`. The CRT writes the complement itself. A bad cookie still reaches
  `__report_gsfailure`, which fails closed.

### 5. Loader and the linking binary (the handoff to Phase 3 / 4)

Done for the fail-closed handoff. `dcrun [--edition 2017|2019] <cache>` loads the image, installs
the translated entries and runs until `FailClosed`. The 2019 run reaches the frame
loop and stays there, because `PeekMessageW` never delivers `WM_QUIT`.

Handlers live in `src/recomp/win32.cpp`. A null handler still fails closed with the DLL!name.
The slot type is `ImportSlot { iat, name, handler }`.

- `GetStartupInfoW` writes a 68-byte `STARTUPINFOW` with `cb` set and the rest zero, so
  `dwFlags` bit 0 (`STARTF_USESHOWWINDOW`) is clear. The caller at `0xC3F21E` then returns 10
  (`SW_SHOWDEFAULT`).
- `GetProcessHeap` returns `0x24000000`, a 16 MB bump heap with a free list (`HeapAlloc` /
  `HeapFree`). `HEAP_ZERO_MEMORY` zeroes the block. Pointers are 16-byte aligned.
- `EncodePointer` / `DecodePointer` xor with `0x5A17C0DE`, so an encoded null is not null.
- `GetModuleHandleW(L"kernel32.dll")` returns 1. `GetProcAddress` returns 0, and the CRT then
  uses the `TlsAlloc` it already imports instead of `FlsAlloc`.
- `TlsAlloc` / `TlsGetValue` / `TlsSetValue` use the 64 slots at TEB `0xE10`. `TlsGetValue`
  clears the last-error slot.
- `InterlockedIncrement` and `InterlockedDecrement` are sequentially consistent and return the
  new value.
- `GetCurrentThreadId` reads TEB `0x24` (the loader stores 1). `GetLastError` /
  `SetLastError` use TEB `0x34`.
- `InitializeCriticalSectionAndSpinCount` sets `LockCount` to -1. `EnterCriticalSection` and
  `LeaveCriticalSection` track the owner and the recursion count. One guest thread takes the
  lock immediately; a second thread has to wait.
- `GetStdHandle` returns 0. The CRT's ioinit then marks descriptors 0, 1 and 2 invalid and
  goes on, which is the headless case.
- `GetCommandLineA` and `GetModuleFileNameA` both report `Daytona.exe`. The environment block
  is empty. `WideCharToMultiByte` converts that one null character, one byte per code unit.
- `GetACP` returns 1252 and `GetOEMCP` returns 437. `IsValidCodePage` accepts those two.
  `GetCPInfo` reports a single-byte page with no lead bytes. `MultiByteToWideChar` decodes
  1252, `GetStringTypeW` fills `CT_CTYPE1`, and `LCMapStringW` does upper and lower case.
  That is what the code-page table builder at `0xC437E9` asks for.
- `GetSystemTimeAsFileTime` writes the host time as a `FILETIME`. `IsProcessorFeaturePresent`
  returns true for compare-exchange-double, MMX, SSE, SSE2 and `rdtsc`, and false for
  everything else, including fast-fail (feature 23), so `__isa_available` stays below 2.
- `SetUnhandledExceptionFilter` stores the filter and returns the previous one (0 the first
  time). `HeapSize` returns the size recorded at allocation. `GetCurrentThread` returns the
  pseudohandle `0xFFFFFFFE`.
- `GetThreadTimes` accepts the current-thread pseudohandle, writes four zero `FILETIME`s,
  and returns success. The caller xors the creation time into an encoded pointer.
- `InitializeSListHead` zeroes the 8-byte x86 header. `HeapReAlloc` copies into a new block.
  `HEAP_REALLOC_IN_PLACE_ONLY` returns null when the new size does not already fit.
- `QueryPerformanceCounter` writes a 100 ns tick count and
  `QueryPerformanceFrequency` reports 10,000,000. `GetPrivateProfileStringA` copies the
  caller's default, which is the no-config-file result. The first reader asks for
  `Camera/FOVY_RATE` and gets `1.0f`.
- `CreateEventA` returns a distinct non-zero handle. Nothing waits on it yet.
- `CoInitialize`, `CoInitializeEx` and `CoUninitialize` return `S_OK`. The call is
  `CoInitializeEx(NULL, 0)` at `0x41D93D`, and the HRESULT is not tested.
- `GetCommandLineW` is the UTF-16 `Daytona.exe`, stored apart from the ANSI command line the
  CRT already saved. `GetModuleFileNameW` reports the same name.
- `VirtualAlloc` commits a 64 KB-granularity arena at `0x26000000`. A non-zero address is
  committed in place. `VirtualFree` returns success and does not unmap.
- `GetVersionExA` and `GetVersionExW` report Windows 7 (6.1, build 7601). The caller at
  `0x7798DA` requires the major version to be greater than 5.
- `timeBeginPeriod`, `timeEndPeriod` and `timeKillEvent` succeed. `timeGetTime` is a
  millisecond clock. No multimedia timer is armed.
- `PathRemoveFileSpecW` strips the last component, and a name with no slash becomes empty.
  `PathIsDirectoryW` is true only for that empty root and for a drive root, so the walk in
  `0x6C0210` stops. `CreateDirectoryW` reports success and creates nothing. `PathAppendW`
  joins a relative piece and fails a result past `MAX_PATH`. `PathFileExistsA` and
  `PathFileExistsW` return false.
- `ShowCursor` keeps a display count. `GetPrivateProfileIntA` returns the caller's default,
  the same no-file result as the string form. The first reader is `Credit/Freeplay`, default 0.
- Winsock is answered by ordinal. `WSAStartup` (#115) fills a 2.2 `WSADATA` and returns 0.
  `socket` (#23) returns a fake handle. `inet_addr` (#11) parses a dotted quad (`127.0.0.1`
  at `0x54058A`). `htons`, `ntohs` and `htonl` (#9, #15, #8) swap bytes. `bind` (#2) and
  `listen` (#13) succeed without opening a host socket; the listen is `192.168.1.x:27000`.
  `connect` (#4) returns 0, which skips the retry at `0x52A618`. `ioctlsocket` (#10) and
  `setsockopt` (#21, `SO_RCVTIMEO` of 30000 ms) succeed. `send` (#19) reports the full
  length and transmits nothing. `gethostname` (#57) fails, so `0x5641A8` keeps the first
  address it already parsed.
- The window setup at `0x422167` leaves the client rectangle unchanged (`AdjustWindowRect`;
  there is no caption). `LoadIconW` and `LoadCursorW` return handles, `LoadImageW` returns 0
  so the class icon is the fallback, `GetStockObject` returns the stock id, `RegisterClassExW`
  returns an atom, and `CreateWindowExW` returns `0x2001` without creating a host window.
  `ShowWindow` at `0x40E0A4` takes `SW_HIDE` or `SW_SHOW` and the previous state is ignored.

- `CreateThread` at `0x591C63` returns a handle and writes a thread id. The procedure is
  `0x591C30`, whose virtual method `0x56AE20` loops while byte `+0x1C` is set and then sleeps.
  That loop does not return, so it is not run on this CPU. `SetThreadPriority` returns success;
  `0xC55FF9` throws when it does not.
- `IsDBCSLeadByteEx` returns false. Code page 0 is the ANSI page, 1252, which has no lead bytes.
- `PathCanonicalizeW` removes `.` and `..`. `0x6C0F3A` treats zero as failure.
- `GetSystemInfo` reports one processor. The callers at `0x779367`, `0x91A2E0` and `0xBA44DD`
  read `dwNumberOfProcessors`. The page size is 4096 and the allocation granularity is 64 KB.

- `D3D11CreateDevice` (thunk `0x78B1E4`, call `0x6B967F`) returns a device at feature level
  `0xB000`. The object answers the startup calls: `CheckFeatureSupport` for threading (both
  flags false), `CheckMultisampleQualityLevels` (only one sample), `CreateBuffer`,
  `CreateTexture2D`, `CreateShaderResourceView`, `CreateRenderTargetView`, `CreateSamplerState`,
  `CreateQuery` (562 queries at `0x6B28C0`), and `UpdateSubresource` (the bytes are not stored).
  `D3DCompile` (`0x6DB5CF`) returns a blob; `CreateVertexShader`, `CreatePixelShader` and
  `CreateComputeShader` accept that pointer. `D3DReflect` reports no bound resources, which
  `0x6DC860` treats as success. DXGI comes along: one adapter ("Port", 512 MB), one output, a
  1920×1080 mode, a factory, a swap chain, fullscreen, and `GetBuffer` as an `ID3D11Texture2D`.
  `SetPrivateData` accepts the debug name and the caller ignores the result.
  `OMSetRenderTargets`, `RSSetViewports`, `RSSetScissorRects` and `ClearRenderTargetView` accept
  the call and keep no pixels. Query `Begin` and `End` return success. `ClearState` is context
  slot `+0x1B8`, so each COM vtable is 256 slots; a 64-slot table made that load land in the
  next interface. `CreateDepthStencilView`, `GenerateMips`, `PSSetShaderResources`,
  `VSSetShaderResources` and `ClearDepthStencilView` return success and keep no pixels.
  `Map` (`+0x38`) returns a 64 MB scratch at `0x31000000` so the copy at `0x4092A1` has
  somewhere to write; `Unmap` pops 12 bytes because both callers push an extra zero.
  `CreateInputLayout`, `CreateRasterizerState`, `CreateDepthStencilState` and
  `CreateBlendState` hand back objects. An unknown method fails closed with the
  interface and the slot.
  The draw at `0x6CFDB0` then binds state and draws: `VSSetConstantBuffers` (`+0x1C`),
  `PSSetShader` (`+0x24`), `PSSetSamplers` (`+0x28`), `VSSetShader` (`+0x2C`),
  `DrawIndexed` (`+0x30`), `PSSetConstantBuffers` (`+0x40`), `IASetInputLayout`
  (`+0x44`), `IASetVertexBuffers` (`+0x48`, five arguments), `IASetIndexBuffer`
  (`+0x4C`), `IASetPrimitiveTopology` (`+0x60`), `VSSetSamplers` (`+0x68`),
  `OMSetBlendState` (`+0x8C`), `OMSetDepthStencilState` (`+0x90`) and `RSSetState`
  (`+0xAC`). `PSSetShader` pops 16. The caller pushes an extra zero, and the epilog's
  `mov esp, ebp` drops it; popping 20 restored `esi` as 0 and faulted at `0x43BDE7`.
  `GetData` (`+0x74`) returns `S_OK` and zeroes the caller's buffer. `0x6B89F6`
  skips the copy on any other HRESULT.
  `IDXGISwapChain::Present` (`+0x20`, `0x6BBA27` and `0x6BBA31`) returns `S_OK` and
  the callers ignore it. `GetFullscreenState` (`+0x2C`) writes windowed and a null
  target. `GetDesc` (`+0x30`) writes 1920×1080 and a 60/1 refresh; the caller at
  `0x6BB6F4` divides the denominator by the numerator, so the numerator stays
  non-zero. `GetContainingOutput` (`+0x3C`) returns the output singleton. A null
  output is the fallback clock at `0x6BBAE2`. `IDXGIOutput::WaitForVBlank` (`+0x28`)
  returns `S_OK`; the next call overwrites `EAX`.
  `D3DPERF_BeginEvent` and `D3DPERF_EndEvent` return 0. `select` (Winsock ordinal 18)
  returns 0, which `0x66BB0D`, `0x57DE98`, `0x57DF9C` and `0x584B8D` treat as a
  timeout and skip the receive. No host socket is opened.
  The CRT unary dispatcher at `0xC45035` calls `native_crt_unary`. That is a hand
  edit in `tr_00C41530.cpp`; the dispatcher at `0xC450C1` is untouched. A fresh
  translation would put the jump back.
  `GetForegroundWindow` is that device's window (`0x2001`); `SetForegroundWindow` reports
  success. `SetWindowLongW` installs `0x6A0740` and reports no previous procedure.
  `CreateEventW` returns a handle and signals nothing. `XInputGetState` returns
  `ERROR_DEVICE_NOT_CONNECTED` (1167); `0x6A0281` counts that as no pad and the init still
  succeeds. `PeekMessageW` returns zero, so the frame does not translate or dispatch.
  `GetCursorPos` writes (0, 0). `ScreenToClient` leaves the point where it is.
  `RaiseException` of `0x406D1388` is the Visual C++ thread-name exception. `0x77AF40` catches
  it, and the translated function continues at its epilog. Any other exception code stays
  fail-closed. A fresh translation would mark that call noreturn again.

The startup path and the first frame are covered. The run stays in the frame loop.
- The guest `ret` returns to the runner rather than following a sentinel on the guest stack,
  because the translator's `ret` leaves the C++ function. The CRT calls `ExitProcess` instead of
  returning, so the sentinel is unused.

### 6. Native overrides

The registry is `OVERRIDES` in `tools/x86tr/translate.py`, keyed by edition and guest
address. A hit emits `void f_<address>(Cpu& c) { native_xxx(c); }` and counts zero
instructions and zero traps, so the function stays in the entry table and its callers are
unchanged. The bodies live in `src/recomp/native.cpp`. Each is a cdecl callee: arguments are
the dwords at `[esp+4]`, `[esp+8]`, …, the result is `eax`, and `esp` moves past the return
address only. `ebx`, `ebp`, `esi` and `edi` are left alone. `test_native` covers overlap,
a zero length, a missing character, a search for the terminator, and an empty needle.

| | memmove | memmove | memset | strstr | strchr | strrchr |
| --- | --- | --- | --- | --- | --- | --- |
| 2019 | `0xC1B790` | `0xC1C2C0` | `0xC1D7F0` | `0xC1FE60` | `0xC24040` | `0xC28B20` |
| 2017 | `0xC15F70` | `0xC16AA0` | `0xC17FD0` | `0xC1A640` | `0xC1E830` | `0xC23310` |

Both memmove entries are the same routine (the second is the overlapping copy). They and
`strrchr` clear `DF` on the way out, because the CRT sets it for the backward scan and clears
it before `ret`. `memset`, `strchr` and `strstr` leave `DF` as they found it. There is no
separate `strlen`: the SSE4.2 traps were inside `strstr` and `strrchr`. Replacing these six
dropped 22 traps in each edition (`palignr` 18, `pcmpistri` 3, `pinsrb` 1) and about 930
instructions.

Left translated on purpose: `_ftol` (`0xC21230`), the CPU-feature init (`0xC285FF`), the FPU
exception-status converter (`0xC3A376`), the `fnsave` / `frstor` helper (`0xC3946D`, 2017
`0xC33C5D`), and the `ldmxcsr` SEH stub (`0xC45D36`).

The lock-free audit (item 3) did not ask for another override. The m2e hot loop stays a Phase 5
candidate.

**Speed-ups in the emitted code** (frame-rate work, PHASE3 "Frame-rate profile"):

- `__security_check_cookie` (2019 `0xC1BDD4`, 2017 `0xC165B4`, in `COOKIE_CHECKS`) is
  inlined at its 12,425 direct call sites: `if (ecx == [cookie]) esp += 4; else` the real
  call. The common case writes back and reloads no registers. Only sites where the flags are
  dead after the call are inlined, and the translator checks the entry's bytes are
  `cmp ecx, [cookie]`. It had been 7% of the main thread.
- xmm registers live in locals (`xm0_` … `xm7_`) like the GPRs, for the registers a
  function names: loaded after `PROLOGUE`, written back at every `SYNC` and read again at
  every `RELOAD` (`keep_xmm_local`). In `c.xmm` they had to be reloaded after every guest
  store, which goes through the byte pointer `M` and may alias the `Cpu`.
- A `ret` in a function that returns no flags writes back only the registers (`SYNC_R`).
  Measured as neutral; kept because it is the same rule the flag liveness already applies.

Together the first two took about 7% off a race frame.

### 7. Static initializers

The restricted evaluator is dropped. Initializers run translated, through the CRT's own walk.

`dcrun` now stays in the frame loop, so this is from the tables the
CRT actually walks rather than from a run. 2019 has three: `0x116C620` (325 functions),
`0x116CB3C` (7) and `0x116CB60` (5). 2017 has the same shape at `0x115B624` (328 words),
`0x115BB48` and `0x115BB6C`.

The twelve large constructors (2019: four of 137,452 instructions and four of 82,988, plus four
smaller ones over 6,000) call one helper each and no import, so they run as translated code.
Twenty-two initializers in 2019 (23 in 2017) call an import directly or through one callee:
`InterlockedIncrement`, `EncodePointer`, `HeapAlloc`, `InitializeCriticalSection`,
`QueryPerformanceCounter`, `GetPrivateProfileStringA`, `IsProcessorFeaturePresent`, and a few
neighbors. Those fail closed at the import, which is the same policy as every other unported
call. An evaluator would still have to answer those imports, so it would be a second import
layer.

### 8. Calling conventions, float returns

Nothing translator-side is left. Registers, the x87 stack and the guest stack are modelled
exactly, so fastcall / thiscall arguments in ecx / edx and float results in `ST(0)` work as on
x86. Stdcall cleanup for import handlers is `ret_stdcall` in `src/recomp/win32.cpp` (pop the
return address and the arguments, result in `eax`). `ST(0)` and `edx:eax` results are not needed
by a handler yet.

### 9. Build integration and incremental builds

- **Shards are fixed address ranges now.** `SHARD_STARTS` in `translate.py` is a frozen list of
  cuts, shared by both editions. A function is written to `tr_<start>.cpp` for the greatest cut
  at or below its entry, so a function that grows rewrites that file only. The cuts were chosen
  so a shard holds about 25,000 instructions, and each of the eight large constructors is alone
  in its shard (137,452 instructions in 2019, 135,014 in 2017). 2019 emits 91 shards, 2017 emits
  89. Recomputing the cuts from a later instruction count would move the later files; leave the
  list alone.
- **Incremental build, 8 jobs, 2019 `dcrun`.** The full rebuild after the rename was 3 min 32 s.
  A rebuild with nothing changed reports `ninja: no work to do`. Changing one ordinary shard
  (`tr_00495850.cpp`) recompiled that file, the static library and `dcrun` in 5 s. Changing one
  constructor shard (`tr_00D239B0.cpp`) did the same in 2 min 7 s. Both are inside the 15-minute
  criterion. Ninja rebuilt only the shard that changed.
- **Discovery and translation run from the build.** Pass `-DDCUSA_PREPARED_CACHE=` to the
  prepared image. CMake reads the edition out of the cache header and uses
  `build/x86tr/<edition>/`. `cmake/translate.cmake` runs `discover.py` and `translate.py` when
  the cache or a script under `tools/x86tr` is newer than `sources.txt`, and it returns
  immediately when the translation is already current (Ninja invokes a new custom command once
  even then). The first configure, when `sources.txt` is absent, runs them before generating
  the build. `DCUSA_GENERATED_DIR` still points at a folder translated by hand, and leaving
  both empty still builds without game code. The `mac-host` tree is configured with the 2019
  cache.
- Translation fails if a trap is not on the reviewed allow-list, or a listed address is no
  longer trapped (`tools/x86tr/trap_allow.py`, item 2).

### 10. More verification

- **Translator assumptions to audit:**
  - flags are dead across calls and at returns;
  - the stack is balanced at returns (callee-cleaned `ret n` is modelled; `esp` tricks are not
    checked);
  - `call $+5` is the only call to a non-entry.
  Scan the translation set for functions that read a flag before writing it on entry, or right
  after a call, and for hand-written CRT assembly (`_chkstk` / `_alloca_probe`, `_aullshr`,
  `_ftol2`, `memcpy` variants) that might pass flags or play stack tricks.
  Done for the 2019 reachable set (51,796 functions disassembled). There is no `call $+5`.
  Three calls in the gap function `0xBA1AF0` target `0x116A4B0` (twice) and `0x116A4D0`,
  which are real callees in the `PSFD00` section. They are entries now: discovery treats
  every executable section as code (`image.py` `code_sections`), which adds the 21
  functions of `PSFD00`; starting a race reached `0x116A4B0`. Nothing else calls outside
  the entry set. `_chkstk` (`0xC21430`) and `_ftol` (`0xC21230`) use flags only
  inside themselves; `_ftol` aligns `esp` and undoes it with `leave`.
  Flags are copied through `Cpu` on every call (`PROLOGUE` / `SYNC` / `RELOAD`). A compare
  whose only reader the liveness pass can see is a local branch does not update those flag
  locals. A tail that then reads the flag gets the stale value. That happens at `0xB1B87C`,
  `0xB47D10` and `0xC046E6` (`jne` on `ZF`), at `0xC75C76` (`je` on `ZF`), at `0xC41F20`
  (`je` after an `or` that did not write `ZF`), and at `0xC3B058` (`sbb eax, eax` on `CF`,
  the unequal exit of the compare loop at `0xC3B010`). The CRT helper `0xC63085` has the
  same hole: its `cmp` does not write `ZF`, and the callers `0xC3949C`, `0xC396F8`,
  `0xC39848`, `0xC398FD`, `0xC39A38`, `0xC3AC58`, `0xC3AD88` and `0xC44C98` test `ZF`
  after it returns. The other flag reads in the scan are data roots, the native memmove
  bodies, or instructions after a throw. None of the stale tails is on the path that
  reaches the frame loop. Fixing it belongs in the liveness pass; a fresh translation
  would also drop the two hand edits.
  Fixed for returned flags: `find_flag_returning` (translate.py) scans the instructions
  after every direct call, and a callee whose flags are read before anything writes
  them, or a call that leads straight into a function reading flags on entry, keeps
  every flag live at its `ret` (with the pieces it tail-jumps to). The CRT sqrt wrapper
  `0xC398E0` calls `0xC630C8` (`cmp eax, 0x7ff00000; ret`) and then `0xC398FD`, whose
  first `je` reads that compare. With the flags dropped, sqrt returned its argument.
  CRI's ADX coefficients (`0xC00050`, `√2 − cos(…)` and a square root) came out as
  about −2.0 and −2.8, the sound-effect decoder `0xBFF940` overflowed to NaN, and the
  game's 5.1 output was all −32768.
- **More ground-truth cases:**
  - Locked operations under contention wait for a second guest thread. Item 3 decided the
    ordering and did not add that thread, so there is no contention to test.
  - 16-bit addressing does not occur. No reachable instruction carries an address-size
    prefix.
  - `rcl` / `rcr` mask the count with 31 and then reduce it modulo 9, 17 or 33. That is the
    i386 rule. Rosetta, running the same formula's inputs as x86_64 with the carry set,
    matched the result and `CF` for every count below 32, including 9, 17 and 18, on 8-bit,
    16-bit and 32-bit forms, and matched `OF` where the count is 1. Counts of 32 and above
    are outside the i386 mask; Rosetta's 64-bit mask is not the guest's.
  - x87 values live in doubles. The 23 `fld tbyte` and 3 `fstp tbyte` in reachable code are
    in the CRT helper: an indefinite, the two infinities, and full-mantissa constants such
    as the one at `0x13F7DAA`. `ld80` rounds the 64-bit significand to a double, so those
    constants lose the extra 11 bits. None of them has an exponent outside the double range.
- Function-level checks against the original binary need 32-bit x86 execution, which Rosetta
  can't do (no 32-bit processes). They come from Windows traces in Phase 5 (oracle hashes of
  guest RAM per frame).
- Reachable code uses `rsqrtss` 85 times, `rcpps` once and `rcpss` once. They are translated
  as exact reciprocals. Hardware gives about 12 bits. A table that reproduces Intel's
  values waits on the Phase 5 frame oracle.
- Rounding follows the x87 control word's RC field, and the game does change it: 83
  reachable sites `or` the saved word with `0x0C00` and load it back, which selects
  round-toward-zero for the conversion that follows, then restore the old word. MXCSR's
  rounding field is not written. `cvtss2si` and the other MXCSR-rounded conversions
  therefore stay at round-to-nearest, which is the mode the game leaves them in. One site,
  `0x5BE35C`, sets MXCSR flush-to-zero (`or eax, 0x8800`) and the SEH stub at `0xC45D8F`
  clears the denormals-are-zero bit. Neither flag is modelled.
  Precision is the field the translator does not honour. Three CRT helpers (`0xC1FCCB`,
  `0xC28F2A`, `0xC6D4A0`) `or` the control word with `0x0300` before `fldcw`, which selects
  64-bit precision, and `0xC63059` keeps whatever precision the caller passed. The guest FPU
  stays at 53-bit precision, which is Windows' default and is what the rest of the `fldcw`
  sites leave in place.

### 11. 2017 edition

Translated and compiled with the same results. Nothing more is needed for Phase 2. Whether
2017 runs on the 2019 engine or needs its own translated set is decided in Phase 6.

### 12. Guest threads and translator fixes (added while Phase 3 was blocked)

Phase 3 could not load a course because the NRT IO thread, the resource worker and the CRI
workers never ran. Earlier runs worked around that with hand edits inside the generated
files (`build/x86tr/2019/gen`), which a re-translation silently dropped. Those edits are gone;
everything below comes from the translator or from `src/recomp`.

- **Cooperative guest threads** (`src/recomp/threads.cpp`). `CreateThread` starts a real
  guest thread: its own `Cpu`, TEB (slots 1-15 at `0x11000`.., 16-31 at `0x30000`..) and
  1 MB guest stack (`0x2F000000`.. and `0x3C000000`..), on a host thread with a 64 MB stack.
  Only one guest thread runs at a time. The CPU moves only where Windows code can block:
  `Sleep`, `SwitchToThread`, a wait, a contended critical section, `pause`, and the frame
  loop's `PeekMessage`. Hand-offs go through one mutex, so the relaxed-ordering question in
  item 3 does not arise: no two guest threads ever run at the same moment, and the polled
  load at `0xC2B422` needs no acquire.
- **Preemption at loop back-edges.** A host timer raises `g_preempt` every 2 ms while
  a second guest thread exists; the translator emits `GUEST_PREEMPT()` before every
  backward jump, which yields when the flag is up. Guest code already expects to be
  interrupted anywhere; handlers are never interrupted. Without it the boot's
  resource walk starved the loader threads it was waiting for.
- Threads, events (manual / auto reset), semaphores and mutexes share one handle table at
  `0x50000`. `WaitForSingleObject` / `WaitForMultipleObjectsEx` (any / all, timeouts),
  `CreateThread` (suspended too), `ResumeThread` / `SuspendThread` (a thread may suspend
  itself), `TerminateThread`, `ExitThread`, `GetExitCodeThread` (`STILL_ACTIVE` while it
  runs), `DuplicateHandle` of the thread pseudo-handle, and `timeSetEvent` with an event
  callback (the game passes `TIME_PERIODIC | TIME_CALLBACK_EVENT_PULSE`) are real.
  Critical sections block on the owner. A wait on a handle that is not a kernel object, or a
  state where every thread waits forever with no timer armed, fails closed with the thread
  list. A fail-closed stop inside a worker is reported by the main thread.
- **Flags stay live across exits to other functions.** Liveness used to treat a tail jump or
  a fall-through into the next discovered function as "flags dead". Discovery does start
  functions at labels (zlib's `inflate` is 109 pieces), so a `cmp` in one piece lost the
  flags its `jcc` read in the next. Every non-`ret` exit now keeps all flags live; fused
  compares are not used across one.
- **Back-to-back thunks are function starts.** A data or immediate root directly after a
  `jmp` that is the last instruction of a known function is accepted (CRI's `0xBFB722`,
  `0xBFB727`, `0xBCF600` callback thunks, and ~9,800 EH funclets and initializer thunks in
  `0xC00000`-`0xDFFFFF`). 61,775 functions are reachable now.
- **`RaiseException(0x406D1388)` returns.** The walker no longer marks the call after a
  `push 0x406d1388` as noreturn, so `0x77AFF9` continues into the thread procedure.
- **Site overrides.** `SITE_OVERRIDES` in `translate.py` replaces one instruction with native
  code (plus an optional statement after `PROLOGUE`). `0xC45035`, the CRT unary math
  dispatcher's `jmp [ebx]`, calls `native_crt_unary`. A site override must leave every flag
  the next instruction reads set; it is not passed through the fused-compare logic.
- `dcrun` reports a guest access to unmapped memory with the guest address and the host pc
  (symbolize with `atos -o dcrun -l <0x100000000 + slide> <pc>`). An unported import names
  its caller's return address and thread, and with `DCUSA_STATS` the code addresses on the
  guest stack.

## Suggested order

1. Loader, fail-closed import table, TEB and runner (items 4 and 5). Done: `dcrun` on the 2019
   image reaches the frame loop and stays there.
2. Discovery (item 1) is done. m2e tables, `__xc` / `__xi` tables, vtable tails, the CRT jump
   tables, and the overlap check are in. What remains indirect is import thunks and register
   jumps. The eight copy-routine sites whose tables overlap the instruction stream are inside
   the memmove bodies, which are native overrides.
3. The override registry is in (item 6), and the remaining 74 / 75 traps are on the reviewed
   allow-list (item 2).
4. Stable shards, the incremental-build measurement, and the CMake translation command are done
   (item 9). Configure with `-DDCUSA_PREPARED_CACHE=` instead of pointing at a generated folder.
5. The lock-free memory-ordering audit is done (item 3). Plain loads and stores stay relaxed.
   The polled load in `0xC2B422` becomes an acquire load when a second guest thread is added.
6. The static-initializer evaluator is dropped (item 7). The constructors run translated; the
   ones that call an import fail closed there.

7. Locale init, the CRT startup imports, Winsock, window creation, the first D3D11
   device and the first frame are handled (item 5). Guest threads run (item 12). The frame
   loop does not exit, because `PeekMessageW` never returns `WM_QUIT`.
8. The throw-site count is done (item 4). Abort-on-throw stays. The startup run raises only
   the thread-name exception, which already returns.
9. The item 10 scan is done. `rcl` / `rcr` at the 9 / 17 boundaries match Rosetta. The game
   sets x87 rounding to chop and the translator follows it; it also sets 64-bit precision
   in three CRT helpers, which the double-wide FPU does not. `rsqrtss` is used, and its
   12-bit table waits on the Phase 5 oracle. The stale-flag tails stay until the liveness
   pass is changed.

Phase 2 is complete. The next work is [Phase 3](PHASE3.md) and [Phase 4](PHASE4.md),
which can run together.

# Phase 2 — translator for Daytona: status and remaining work

Phase 2 from [PHASE0.md](PHASE0.md): turn the game's x86-32 code into C++ that
builds for arm64.

**Exit criterion:** all game, NRT and m2e functions translate with zero
unsupported operands (or an explicit native override), and the generated code
compiles in under about 15 minutes incremental.

The NRT engine and the m2e (Model 2 / i960) interpreter are both linked into
`Daytona.exe`. The `nrt/` folder holds shaders and data only, so "NRT and m2e
functions" means code inside the EXE.

## Where it stands

| | 2019 (New Season) | 2017 |
| --- | --- | --- |
| Functions discovered | 96,374 | 96,132 |
| Reachable from real roots (translation set) | 51,899 | 51,585 |
| Reachable functions translated | 51,899 (100%) | 51,585 (100%) |
| Instructions translated | 2,866,303 | 2,797,519 |
| Instructions that fail closed if reached | 103 | 107 |
| Clean build of the generated code (8 jobs, -O2) | 3 min 26 s, 66 MB library | 3 min 31 s |

Instruction semantics: `test_x86_semantics` runs 863 cases × 8 inputs. Every
case runs as x86_64 under Rosetta 2, and the same snippet, assembled for i386,
goes through the translator. All 6,904 runs match. Cases longer than a few
instructions are also forced through function splitting.

### Done

- **Discovery** (`tools/x86tr/discover.py`, `cfg.py`).
  - Roots: entry point, RTTI vtables, SafeSEH handlers, C++ EH FuncInfo funclets, code
    pointers in data and immediates, and gap scanning.
  - Jump tables, including the two-level byte-indexed form; noreturn inference; tail calls.
  - Reachability from the entry, vtable, SEH, EH and data roots.
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
  - Unsupported instructions trap per instruction, not per function.
  - Oversized functions are cut into chained parts where no branch crosses the cut.
- **Output:** about 86 shards of roughly 25,000 instructions each, plus `stubs.cpp` (unreachable
  functions fail closed), `entries.cpp` (the sorted entry table) and `coverage.json`.
- **Runtime** (`src/recomp`): `Cpu` / `Fpu` state, `GuestMemory` (a 4 GB reservation), dispatch,
  and `guest.hpp` helpers (lanes, 80-bit load/store, `fxam`, `fprem`, `fscale`, NaN rules,
  `cpuid` reporting SSE2 only, `rdtsc`).
- **CMake:**
  - `dcusa_recomp` library.
  - `dcusa_generated`, built when `DCUSA_GENERATED_DIR` is set.
  - `test_x86_semantics`, regenerated from the scripts at build time.

### Exit criterion status

| Part | Status |
| --- | --- |
| Reachable game, NRT and m2e code translates | Yes, apart from the gaps listed under item 1 below |
| Zero unsupported operands | **No.** 103 / 107 instructions trap if reached (item 2) |
| Native override path for what isn't translated | **Not built** (item 6) |
| Generated code compiles in under 15 min | A clean build takes 3.5 min. Incremental builds are not measured; shard boundaries are not yet stable (item 9) |

## Remaining work

Ordered roughly by how much it blocks Phase 3 / 4 (a linking binary with
fail-closed imports) and Phase 5 (running headless).

### 1. Discovery gaps

- **Two m2e table entries are not translated.** `0x005E0060` is in the 1,025-entry table at
  `.rdata 0x11AAEBC`; `0x005F0060` is index 545 of the 4,097-entry table at `0x11EC90C`.
  Discovery found both only by gap scanning, so they are stubs. Fix: add both tables as explicit
  roots (every non-zero entry). The other 125 + 299 distinct entries are translated.
- **`__xc` / `__xi` initializer tables.** There are candidate tables at `0x116C068` (221 entries)
  and `0x116C5DC` (366 entries), and some entries point mid-function (for example `0x1125142`).
  To do: find the real `__xc_a..__xc_z` / `__xi` bounds from `_initterm` call sites in the CRT
  startup, and add the entries as roots. Then check whether the mid-function entries are real (a
  thunk, or a function entry discovery missed) and fix discovery if so.
- **Unresolved indirect jumps** (429 in 2019, 415 in 2017). These are currently translated as
  tail calls through `call_indirect`, which fails closed if the target is inside the current
  function. To do: classify them (switch forms the walker misses, `jmp [reg]` thunks, CRT
  `longjmp` / EH continuations) and handle switch forms as jump tables.
- **Unbounded jump tables** (19): check each target list against the code; an over-long list
  pulls data in as code.
- **Data mistaken for code.** A few reachable "functions" are text or image data, for example
  `0x00650064` / `0x00730072` (UTF-16) and `0x00FFD8FF` (a JPEG header). They show up as the
  garbage traps (`in`, `les`, `push es`, `mov` to a segment register). Tighten the data-root
  acceptance so they drop out. They are harmless as traps but make the coverage numbers noisy.
- **Gap-only functions** (about 48,000) are stubs. They are believed to be dead code (the
  linker's /OPT:NOREF keeps unreferenced functions), but anything called through a pointer that
  discovery missed will fail closed at run time. Phase 5 traces will list any that are hit; the
  translator can translate them all with `--all` if needed.
- Overlap: 1,996 bytes are claimed by more than one function. Check that these are shared tails
  and not misdecodes.

### 2. Trapped instructions (103 in 2019, 107 in 2017)

| Kind | Count (2019) | Plan |
| --- | --- | --- |
| SSSE3 / SSE4.1 / SSE4.2: `palignr`, `pmulld`, `pabsd`, `pminsd`, `pmaxsd`, `pblendvb`, `pinsrb`, `pcmpistri` | 40 | Check each site is behind a CPU-feature dispatch (the CRT's `__isa_available` or the game's own `cpuid` checks), so the reported SSE2-only `cpuid` keeps them unreachable; record the dispatch site. Otherwise translate them (all are simple lane ops except `pcmpistri`) and add ground-truth cases |
| AVX: `vmulps`, `vaddps`, `vmovups`, `vbroadcastss` | 54 | Same check. They should be gated by `cpuid` AVX / OSXSAVE plus `xgetbv`, and neither is reported |
| `int` (0x29 fast-fail, 0x2C assertion) | 4 | Leave as traps. They are crash paths |
| `fnsave` / `frstor` | 2 | CRT floating-point exception paths. Translate if they are reachable in normal flow, otherwise leave |
| Garbage decodes | 3 | Removed by the discovery fixes in item 1 |

The exit criterion wants zero, or an explicit override. Every remaining trap must therefore end
up as one of: translated, proven unreachable and listed in a reviewed allow-list (the dispatch
site recorded), or replaced by a native override.

### 3. Thread safety of translated code (needed before a second guest thread runs)

- **Locked operations are not atomic.** The `lock` prefix is currently dropped, so `lock xadd`,
  `lock cmpxchg`, `lock inc` / `dec` / `add` / `or` / `and`, `cmpxchg8b`, and `xchg` with a memory
  operand (implicitly locked) are plain read-modify-writes. Emit `__atomic` builtins
  (sequentially consistent) on the host address for these, keeping the flag results.
- **Memory ordering.** x86 is TSO; ARM is weakly ordered. Locked operations become full
  barriers via the atomics above, and `mfence` / `sfence` / `lfence` should emit fences (they
  are currently no-ops). Plain loads and stores stay relaxed. Audit the game's lock-free
  hand-offs (flags polled without locks, NRT render-thread queues) and decide between per-site
  native overrides and a "TSO" build mode (acquire / release on all guest memory accesses) that
  costs speed but is safe.
- Generated code has no globals, so one `Cpu` per guest thread is enough. Guest stacks are
  allocated in guest memory.

### 4. TEB, SEH and the C++ throw policy

- **Per-thread TEB in guest memory:**
  - `NT_TIB`: ExceptionList, StackBase / StackLimit, Self;
  - TLS slots (`TlsGetValue` and friends);
  - LastError;
  - a PEB stub with only the fields the CRT reads.
  `Cpu::fs_base` points at it. The translator already routes `fs:` accesses there.
- **SEH frames** are already ordinary guest memory writes (`push fs:[0]` chains), so they need
  no translator work.
- **Throwing.** `RaiseException` (and `_CxxThrowException`, which calls it) fails closed with a
  report: the "abort on throw" policy. To do: count the call sites of `_CxxThrowException` and
  `RaiseException` reachable in normal flow. If the game throws on a normal path (for example
  `std::bad_alloc` handling or file-not-found probes), implement SEH dispatch: walk ExceptionList,
  call handlers, and unwind via the C++ EH FuncInfo tables discovery already parses.
- **Security cookie:** initialise `__security_cookie` as the CRT does; a failure goes to
  `__report_gsfailure`, which fails closed.

### 5. Loader and the linking binary (the handoff to Phase 3 / 4)

- Map the prepared image into `GuestMemory` at `0x400000`, section by section, with
  protections (read-only for `.rdata` and code pages).
- Fill each IAT slot k with `ImportBase + 16*k`. Generate the import table (DLL!name → handler)
  with every handler failing closed and naming the import.
- **Runner:**
  - create the main `Cpu`, guest stack and TEB;
  - push a return sentinel and call the entry;
  - catch `FailClosed` and print the report.
  This is the "linking binary with fail-closed imports" PHASE0 puts at the end of Phase 2.
- Declare `translated_entries()` in a header and call `dispatch_install` from the runner.

### 6. Native overrides

- A registry of native C++ replacements keyed by guest address, for example
  `DCUSA_NATIVE(0x00C1B790, memcpy_sse)`. The translator reads the list and emits the override
  instead of the translation (or alongside it for verification). Unported pieces keep failing
  closed.
- First users:
  - CRT `memcpy` / `memset` / `strlen`, whose translations contain the SSSE3 / SSE4.2 paths;
  - the m2e hot loop (Phase 5);
  - lock-free sites from item 3, if needed.

### 7. Static initializers

PHASE0 planned a restricted evaluator for static initializers. With the giant initializers now
compiling (the course-table constructors of up to 137,452 instructions, split into parts), the
simpler path is to **run them translated** through the CRT's own `_initterm`. Remaining
decision: keep the evaluator only for initializers that call imports before the import layer
can answer (find them by running `_initterm` under the fail-closed runner), or drop it.

### 8. Calling conventions, float returns

Nothing translator-side is left. Registers, the x87 stack and the guest stack are modelled
exactly, so fastcall / thiscall arguments in ecx / edx and float results in `ST(0)` work as on
x86. Native overrides (item 6) and import handlers (item 5) need a small set of argument and
return helpers (stdcall cleanup, `ST(0)` / `edx:eax` results).

### 9. Build integration and incremental builds

- Stable shards. Shards are currently filled in address order up to about 25,000 instructions,
  so a function that grows or shrinks shifts every later shard boundary. Assign functions to
  shards by fixed address ranges instead, so a translator or discovery change rebuilds only the
  affected shards. Then measure the incremental build against the 15-minute criterion.
- A CMake custom command that runs discovery and translation from the prepared cache (the cache
  path set at configure time), so `DCUSA_GENERATED_DIR` no longer has to be set by hand.
- Translation-time checks: fail the build if `coverage.json` has a trap that is not on the
  reviewed allow-list (item 2).
- The `mac-host` build directory currently has `DCUSA_GENERATED_DIR` set to
  `build/x86tr/2019/gen`; configure with `-DDCUSA_GENERATED_DIR=` to build without game code.

### 10. More verification

- **Translator assumptions to audit:**
  - flags are dead across calls and at returns;
  - the stack is balanced at returns (callee-cleaned `ret n` is modelled; `esp` tricks are not
    checked);
  - `call $+5` is the only call to a non-entry.
  Scan the translation set for functions that read a flag before writing it on entry, or right
  after a call, and for hand-written CRT assembly (`_chkstk` / `_alloca_probe`, `_aullshr`,
  `_ftol2`, `memcpy` variants) that might pass flags or play stack tricks.
- **More ground-truth cases:**
  - locked operations under contention, once item 3 lands;
  - 16-bit addressing forms never occur, so no work there;
  - `rcl` / `rcr` at count multiples of 9 / 17;
  - x87 at the edges of the 80-bit range (results that overflow double but not extended
    precision).
- Function-level checks against the original binary need 32-bit x86 execution, which Rosetta
  can't do (no 32-bit processes). They come from Windows traces in Phase 5 (oracle hashes of
  guest RAM per frame).
- `rcpps` / `rsqrtps` are translated as exact reciprocals, but hardware gives about 12-bit
  approximations. If the oracle shows divergence, use a table that reproduces Intel's values.
- `cvtss2si` / `cvtps2dq` and the other MXCSR-rounded conversions use the host's
  round-to-nearest and ignore MXCSR rounding-mode changes. The same goes for any precision-control
  change made through x87 `fldcw` (only the rounding control is honoured). Check whether the
  game ever changes them.

### 11. 2017 edition

Translated and compiled with the same results. Nothing more is needed for Phase 2. Whether
2017 runs on the 2019 engine or needs its own translated set is decided in Phase 6.

## Suggested order

1. Atomics for locked operations and fences (item 3). They are small, and wrong without them.
2. m2e and `__xc` roots, plus the other discovery fixes (item 1).
3. Loader, fail-closed import table, TEB and runner (items 4 and 5). This gives a linking
   binary that runs until its first import, which unblocks Phases 3 and 4.
4. Native override registry, then resolve each trapped instruction (items 6 and 2).
5. Stable shards and an incremental-build measurement (item 9). This closes the exit criterion.
6. Static-initializer decision once `_initterm` has been run under the runner (item 7).

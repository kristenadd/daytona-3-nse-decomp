# Daytona Championship USA — Phase 0 inventory and go / no-go

Source folder (read-only, nothing copied): `~/Projects/Daytona Championship USA/`

- `Daytona Championship USA (2017-12-21)[Sega PC][TP]/` — called **2017** below
- `Daytona Championship USA New Season Edition (2019)[Sega PC][TP]/Sega/` — called **2019** below

Method: REA (PE / Rich header / imports / load config / entropy / strings), a full linear
`objdump` sweep of both `.text` sections, RTTI and string-reference bucketing, and a byte diff of
the two 2017 EXEs. Numbers marked "≈" are heuristic. Anything not verified is listed under
[Open unknowns](#open-unknowns).

## TL;DR — **GO**

- **Both game EXEs are plain, unpacked, unencrypted 32-bit x86 (PE32) built with MSVC 2012.** They
  are not x64. The code reads without the security hardware. The keychip / dongle check is a
  window-message handshake with a separate `Shell.exe`, so it can be replaced natively.
- So this is the **same machine model as OutRun 2006**: 32-bit guest pointers, a fixed base of
  0x400000, no relocations, and no `.pdata`. **No x86-64 translator is needed.** The existing
  translator needs *extending* (SSE2 double and packed integer, atomics, SEH / C++ exceptions,
  threads, and about 5× OutRun's scale), not replacing.
- **The big surprise: DCUSA runs the original 1994 Daytona USA i960 program inside an embedded
  Sega Model 2 emulator ("m2e").** The ROM images ship in `rom/daytona_rom/`. The EXE carries the
  original routine labels (`main_loop`, `send_polygon`, `GAME_DSP` … 196 labels) and an i960
  interpreter. The NRT HD renderer redraws the Model 2 scene. The emulator is an interpreter, not
  a JIT (no executable memory is ever allocated), so static translation works.
- Graphics are **Direct3D 11**, not D3D9. Shaders ship as **HLSL source** inside the EXE (60
  modules) and are compiled at run time with `D3DCompile`. The pipeline needs a D3D11-shaped
  interface and an HLSL→MSL path; OutRun's D3D9 device doesn't carry over.
- **The 2017 `Daytona.exe` is a TeknoParrot-patched build** (1,050 patched runs; the pristine file
  is `Daytona.exe.original`). The 2019 EXE is unpacked and unpatched. SHA-256 manifests must key
  on the pristine files.
- Recommended target: **the 2019 EXE as the primary engine.** 2017 content runs on the same engine
  through a version-selected content root, with 2017-EXE-specific behaviour only where verified
  (see [Phase 6](#phased-plan)).

---

## 1. Binaries

### Game executables

| | 2017 `Daytona/Daytona.exe` | 2019 `Sega/Daytona/Daytona.exe` |
|---|---|---|
| Size | 18,366,464 | 18,442,240 |
| SHA-256 | `9464f87f…` (patched) / `.original` `f5d6fa89…` | `8ba4048f9a6920e3…5d379e` |
| Format | PE32, i386 (0x14C) | PE32, i386 (0x14C) |
| Link time | 2017-12-21 | 2019-03-06 |
| Linker / compiler | MSVC 11.0 (VS2012; Rich prod 206/207, builds 50727 / 61030) plus some VS2005 static libs (prod 109/110) | same |
| PDB | `E:\Work\SAI\Daytona\trunk\SegaRacingClassic\d1a\d1a_Win32_Release.pdb` | `…\d1a_Win32_Final.pdb` |
| Image | base 0x400000, SizeOfImage 0x1F67000, no `.reloc` | base 0x400000, SizeOfImage 0x1F7C000, no `.reloc` |
| Flags | LARGE_ADDRESS_AWARE, NX, no ASLR, no CFG, no Authenticode | same |
| TLS callbacks | — | none (no TLS directory) |
| Load config | — | SafeSEH table with **8,143 handlers**, `/GS` cookie, no CFG |
| `.pdata` | n/a (PE32 has none) | n/a |
| Instructions (linear sweep) | 4,344,715 | 4,362,147 |
| Direct call targets in `.text` | 20,172 | 20,232 |
| Indirect calls / jumps | 11,589 / 1,514 | 11,660 / — |
| Estimated function count | ≈ 39K (direct-call targets plus ≈ 19K code pointers in data); upper bound ≈ 71K padding-aligned starts | similar |
| Imports | 369 functions from 24 DLLs | same, minus `dbghelp`, plus `EnableWindow` |
| RTTI classes | 1,371 | ≈ same |

**`.text` layout (2017)**, from string-reference bucketing and RTTI:

| Range | Size | Direct call targets | Contents |
|---|---|---|---|
| 0x401000–0x681000 | 2.50 MB | 5,646 | game (`prj`, `soc`), `test_mode`, `comm_lib`, m2e (Model 2 emulator) |
| 0x681000–0x841000 | 1.75 MB | 4,038 | NRT engine (`nrt::`, `brrenderer`, `render_pass`, `a2d`, `spr`) |
| 0x841000–0xA81000 | 2.25 MB | 2,371 | OpenCV 2.4.10 + OpenEXR |
| 0xA81000–0xB41000 | 0.75 MB | 1,631 | libpng 1.5.12, libjpeg, libtiff, zlib 1.2.7, tinyxml2 |
| 0xB41000–0xC01000 | 0.75 MB | 3,867 | CRI middleware (ADX, Sofdec, CSB, CPK) |
| 0xC01000–0xD00000 | 1.0 MB | 2,619 | MSVC CRT, ConcRT / PPL, C++ EH unwind funclets |
| 0xD00000–0x1150000 | **4.31 MB** | 0 | **static initializers**: ≈ 80K calls to the `Vec3` constructor 0x423570 that fill global float tables. This is deterministic data written as code. |

Only about **4.25 MB (≈ 9.7K direct targets, probably 15–25K functions with vtable-only
methods)** is game and engine code that needs real translation. That's roughly **5× OutRun's
4,206 translated functions**.

### Other binaries (none needed by the port)

| File | Arch | Size | What it is |
|---|---|---|---|
| `shell/Shell.exe` (2017: 1,475,072; 2019: 1,487,360) | PE32, MSVC 2008 | | Cabinet platform "HGP15001 Donnington Shell": I/O board, dongle, credits, bookkeeping, force-feedback motor, billboard, d3d9 UI. **Replaced natively.** |
| `shell/InpWrapper.exe` | **PE32+ x64**, MSVC 2008 | 614,400 | Helper for `inpoutx64` (from "HGP13006 Dirt Showdown Shell") |
| `shell/hidapi.dll` | PE32 | 40,448 | USB HID for the I/O board |
| `inpout32.dll` / `inpoutx64.dll` | PE32 / PE32+ | 213,109 / 98,304 | Parallel-port I/O for the force-feedback driver (`ParallelPort=888`) |
| `shell/EloIntf.dll`, `EloProp.dll`, `EloPubIf.dll` | PE32 | 351K / 420K / 81K | Elo touchscreen SDK |
| `shell/d3dx9d.dll`, `d3dx9d_35.dll` | PE32 | 2.3 MB / 3.7 MB | Debug D3DX9 for the shell UI |
| `shell/setup.exe` (2019 only) | PE32 | 3,021,800 | Installer |
| `Daytona/d3dcompiler_46.dll` | PE32 | 3,231,696 | Microsoft HLSL compiler (Win8 SDK). Runtime shader compilation. |
| `Daytona/DaytonaLaunch.exe` (2017 only) | PE32, **VB6** | 49,152 | Third-party loader (TeknoParrot era), not Sega |
| `Sega/Launcher/SystemLauncher.exe` (2019 only) | PE32, MSVC 2008 | 88,576 | Sega launcher that installs / updates and starts the Shell |
| `DK2WIN32.dll` | — | not shipped | Dinkey dongle runtime that `Shell.exe` loads dynamically |

## 2. Packing, encryption, protection

**None that blocks reading the code.**

- File entropy is 6.69 bits/byte. Only ≈ 192 KB of high-entropy blocks, all compressed data
  (images, zlib).
- The sections are normal. `PSFD00` is CRI's marker section, not a packer stub. The imports are
  intact (369 named functions). The full linear sweep decodes, with 214 undecodable spots, which
  are data in `.text`.
- No TLS callbacks, no anti-debug imports beyond the ordinary, no self-modifying code.
  `VirtualAlloc` is only ever called with `PAGE_READWRITE`. The single `VirtualProtect` (CRT) sets
  `PAGE_READONLY` / `PAGE_NOACCESS` guard pages. Nothing becomes executable at run time.
- **The security check is a protocol, not crypto on code.**
  - The game sends `GAME_MSG_SECURITY_CHALLENGE` to the Shell window.
  - The Shell talks to the Dinkey dongle (`DK2WIN32.dll`, "DK2 driver not installed") and the USB
    I/O board unlock (`HIDioThread`), then answers with `SHELL_MSG_SECURITY_AUTHENTICATE`.
  - There are also "P-ras" errors.
  - The port provides a native Shell service that answers the challenge as an authenticated
    cabinet, or bypasses the check at the call site. This is the same approach TeknoParrot takes.
- **The 2017 `Daytona.exe` is patched.**
  - It differs from `Daytona.exe.original` in 1,050 runs (353,914 bytes).
  - The code edits are at file offsets 0x7C487, 0x7C4E8, 0xDCA97 and 0x12898F.
  - Most bytes are code caves written over the embedded NRT web-debug JavaScript in `.data`, from
    0xE3F1D3 onward.
  - The patch is credited to Ducon2016 / jackchen (PPF30, PPF-Studio).
  - **Manifests and translation use the pristine `.original` hashes.** If a player only has the
    patched file, we refuse it: we don't ship the reverse patch.

## 3. APIs, middleware, engine

| Area | What the EXE uses |
|---|---|
| Graphics | **Direct3D 11** (`D3D11CreateDevice`, feature levels up to 11_0). `d3d9` is imported only for `D3DPERF_*` markers. DirectWrite for text. |
| Shaders | **60 HLSL source modules embedded in the EXE** (`#ifdef HLSL`, ≈ 1,300 `float4`, 321 `SV_Position`). Compiled at run time with `D3DCOMPILER_46!D3DCompile`; reflected with `D3DReflect`. Profiles: `vs/ps_4_0`, `vs/ps/gs/cs_5_0` (and 5_1 strings). `GameData/.shaders/shader_cache.bin` caches 434 DXBC blobs (compiler 9.30.9200.20499). The player may not have it. The Model 2 path uses `rom/shader/*.fxo` (DXBC plus DX9 bytecode) and `m2*.glsl` source. |
| Audio | DirectSound 8 (`DSOUND` ordinal 11) under **CRI ADX 10.71c** (ADXT streams) and **CRI CSB** (cue sheets, `SE.csb`). WinMM for timers. |
| Video | **CRI Sofdec** (`mwPly`, SFD = MPEG-1/2 PS) for movies. VFW (`AVIFIL32`, `MSVFW32`) for AVI. `AVICAP32` plus OpenCV for the **cabinet webcam** (face billboard, haarcascade XMLs). |
| Input | DirectInput 8 ("Generic USB Joystick", "Logicool Driving Force RX USB") and XInput 9.1.0. Force feedback is computed in-game by `soc::ForceFeedbackImpl` (`config.ini [Wheel] FORCE_*`) and sent through the Shell's parallel-port motor driver. |
| Network | Winsock 2, IPHLPAPI, NetBIOS (MAC/ID), WinHTTP. `comm_lib` link (nodes / cabinets), `RankComm` master/slave, `ImageComm`, heartbeat, Satellite / TV modes. |
| Middleware | CRI (2010: ADXT, Sofdec, CSB, CPK); OpenCV 2.4.10 + OpenEXR (Iex/Imf/IlmThread); zlib 1.2.7; libpng 1.5.12; libjpeg; libtiff; tinyxml2; MSVC ConcRT / PPL; an embedded NRT web-debug tool (JS/WebGL, unused in release). |
| Engine | **Sega NRT** (`E:\Work\NuSX\Daytona\trunk\nrt\…`, RTTI `nrt::` 286 classes) on the *Sega Racing Classic* framework (`SegaRacingClassic\d1a`), plus **m2e**, an embedded Model 2 emulator. |

**RTTI by namespace:** nrt 286, cv 240, (global) 173, soc 114, Concurrency 99, std 96, Iex 84, dw 73, Imf 51, prj 35,
test_mode 34, brrenderer 16, tinyxml2 15, render_pass 9, a2d 8, spr 8.

### The embedded Model 2 emulator

- The EXE contains an **i960 instruction table** (166 mnemonic entries: `ATADD`, `CMPDECI`, `EMUL`,
  `EXTRACT`, `MODAC`, …) and two dispatch tables:
  - `.data` 0x11994E8: 1,024 entries → handlers 0x5DB060–0x5E1C90.
  - `.data` 0x11DAF38: 4,096 entries → handlers 0x5E8D30–0x5F0AA0.
  - Handler 0x5DB060 decodes guest bus addresses (`and ecx,0xFFEFFFFF; cmp ecx,0x1A14000`).
- It carries a **196-entry label table of the original 1994 program**, including:
  - `start_ip`, `sysinit_l1`, `copro_down`, `geo_func`, `main_loop`, `interrupt_wait`;
  - `send_polygon`, `send_tex_map`, `ADV_INT` / `RANK_DSP` / `SEL_Control` / `GAME_DSP` /
    `GOAL_DSP` / `NAME_DSP`;
  - `camera`, `dsp_player_car`, `dsp_other_car_level1..4`, `car_z_sorting`, `put_needle`, ….
- Also present: `TaskM2E`, `suspend_task_m2e` and `NrtModelDraw(model_id)`.
- The original game program runs from `rom/daytona_rom/*.bin` (code1/2, ep1, data, pol, tex,
  copro) plus `rom/fv_rom/rom_ep2.bin`. NRT draws HD models in place of Model 2 polygons, keyed by
  model id.
- **This shapes the port.** The race logic, physics and AI that players know are i960 code
  interpreted by x86 code. The i960 interpreter is a hot loop: it runs every frame for the whole
  race.

## 4. Arcade services

| Service | Original | Port plan |
|---|---|---|
| I/O board | USB HID board via `Shell.exe` (`hidapi`, `HIDioThread` unlock handshake). The game itself reads controls through DirectInput / XInput. JVS strings exist only as error-table entries. | `PadSnapshot` → a native DInput8/XInput shim (axes: wheel, gas, brake; buttons: view, gear up/down / H-shifter, start, test, service, coin) |
| Security | Shell ↔ Dinkey dongle and I/O-board unlock; the game sends a challenge over window messages; "P-ras" errors | Native Shell service that answers authenticated, or a bypass at the call site. **Fail closed** if any other path is hit. |
| Credits / coins | Shell (`SHELL_MSG_SETCREDIT`, bookkeeping, `CheckCredit`) | Native: free play or a virtual coin button |
| Force feedback | Game computes it (`soc::ForceFeedbackImpl`); Shell drives the motor over the parallel port (`inpout32 Out32/Inp32`) | Map to Core Haptics / GameController rumble. Wheel FFB is out of scope at first. |
| Shell window messages | `GAME_MSG_SECURITY_CHALLENGE`, `SHELL_MSG_SECURITY_AUTHENTICATE`, `SHELL_MSG_SETCREDIT`, `GAME_MSG_GEAR_SHIFT`, high score, party mode, continue | Native message table: each known message is handled, unknown messages abort |
| Network | `comm_lib`, `RankComm`, `ImageComm`, heartbeat, Satellite; `-sm server\|tv\|terminal\|satellite` | **Fail closed**: force standalone (`NETWORK_TYPE=2`). Linked play is a later, optional item. |
| Test menu | `test_mode::` (34 classes), `rom/message/src/test_mode_en.properties`, shell `test*.xml`, BOOKKEEPING, `-t` flag | Reachable via the test button; settings persist to the app container |
| Settings | `GameData/config.ini`, `ShellData/ShellData.ini`, `GameSettings.ini` (2019), `HighScoreData.xml`, `Tournament.ini`, `ipconfig.ini`, `user.properties`, `shell/variables/*`; i960 EEPROM / backup RAM emulation (`_Check_EEPROM`, `EEPROM_to_DRAM`) | Writable copies in the app container, never in the player's game folder |
| Autoplay | 2019 has `AutoPlaySetting.ini`, a built-in autoplay mode | A gift for headless testing (Phase 5) |
| Command line | `-t -m -s -sm … -ss -ve -fs -aa`, resolution flags, `-model2` | Fixed native arguments |
| Camera billboard | AVICAP32 + OpenCV face capture | **Stub, fail closed** (report "no camera"). OpenCV is not translated. |
| Raspberry Pi marquee | `assets/` (h264, jpg, ARM ELF) and Shell TCP | Ignored |

## 5. Data layout

About **5.5 GB for 2017, 5.2 GB for 2019** (≈ 11 GB on disk together). None of it is encrypted.

| Path | Format | Size | Notes |
|---|---|---|---|
| `nrt/dx11/` | **NRFC** containers (magic `NRFC`, version 20150701) | ≈ 3.5–3.75 GB | `car/` 1.1 GB, `course/` 2.1 GB, course `.nds` files 370–400 MB each. Plaintext names, Maya-exporter XML, embedded DDS and zlib streams. |
| loose `*.dds` / `*.tga` | DDS (BCn), TGA | 453 DDS in 2017 / 525 in 2019, plus 4 TGA | |
| `rom/sprite/` | Sega **FARC** (54 `FArc` uncompressed, 10 `FArC` zlib; no `FARC`-encrypted ones) | ≈ 556 MB | Plus `sprdb.bin` and `fontmap.farc` |
| `rom/auth2d/` | AET in FARC plus `aetdb.bin` | | 2D layout and animation |
| `rom/movie/` | 24 Sofdec `.sfd` (MPEG-PS) plus 2 AVI | 481 MB + 173 MB | |
| `rom/sound/` | 58 ADX streams plus `SE.csb` (CRI `@UTF` table) | 122 MB + 15.8 MB | |
| `rom/daytona_rom/`, `rom/fv_rom/` | Raw Model 2 ROM images (i960 code, data, polygons, textures, copro) | | `fixpage.bin` 5.3 MB and the larger `fv_rom` code ROMs: role not confirmed |
| `rom/shader/` | `.fxo` (DXBC + DX9) and `m2*.glsl` | | Model 2 path shaders |
| `rom/message/` | `.properties` text | | Test menu and UI strings |
| `GameData/` | ini, `.shaders/shader_cache.bin` | | Runtime config and shader cache |

**Loading.** Synchronous and streaming file reads through Win32 `CreateFile` / `ReadFile` on the
NRT resource system and CRI's file system. Course `.nds` files are huge but are streamed. Assets
are addressed by path relative to the game folder. Nothing goes through a single archive index, so
a path-mapping file layer is enough.

**2017 vs 2019 content.**
- The Model 2 ROMs are identical.
- 2019 adds 104 files and removes none.
- About 1,491 shared files differ in size (re-exported NRT assets, sprites, sound).
- The two EXEs differ too: same code base, link times 15 months apart, shifted addresses.

## 6. Comparison with OutRun 2006 C2C

### Carries over (with no or small changes)

- The **player-supplies-files model.** `ExeImage`, range SHA-256 checks and a prepared image cache
  (new name and manifest). Two manifests: pristine 2017, pristine 2019.
- **Translator architecture.** Objdump listing → per-instruction C++, register state in `Cpu`, a
  guest memory window, fail-closed reporting of unsupported operands. It translates **98.3% of
  Daytona 2019's instructions by count** today (143 of 408 mnemonics).
- **Native functions by address** overriding translated ones; the fail-closed default for anything
  unported.
- **Platform contract** `PadSnapshot` / `AudioOutput` / `Presenter`; CTest, oracle and autoplay
  harness; the Mac / iOS / visionOS app shells (pattern, not code).
- **32-bit guest at 0x400000 with no relocations:** the same guest window and `host(c,a)` model.

### Needs extending

**Translator: instruction coverage.** In order of occurrence in 2019:
- **SSE2 scalar double:** `movsd` 15K, `mulsd`, `addsd`, `subsd`, `divsd`, `comisd`, `ucomisd`,
  `cvtsd2ss`, `cvtss2sd`, `cvttsd2si`.
- **Packed float / double:** `mulps`, `addps`, `subps`, `minps`, `maxps`, `shufps`, `unpck*`,
  `mulpd`, `addpd`, `sqrtpd`, `rsqrtss`, `andpd`, `xorpd`, `orpd`.
- **Packed conversions:** `cvtps2pd`, `cvtdq2ps`, `cvtdq2pd`, `cvtpd2ps`, `cvtps2dq`.
- **SSE2 / MMX integer:** `movd`, `movdqa`, `movdqu`, `pand`, `pxor`, `por`, `padd*`, `psub*`,
  `pmaddwd`, `pmulhw`, `pack*`, `punpck*`, `pshufd`, `pshufw`, `pextrw`, `pinsrw`, `pavgb`,
  `pmax*`, `pmin*`, shifts, `emms`.
- **Strings:** `stos*`, `movs*`, `scas*`.
- **Atomics:** `lock` prefix, `xadd`, `cmpxchg` (132 sites).
- **Possibly missed by my name match:** `setcc` and a few x87 forms (`fimul`, `faddp`, `fmulp`,
  `fsubp`).
- **Not needed:** `in`, `out`, `arpl`, `bound`, `aaa`, `les`, `insb` appear only where the linear
  sweep decoded data.
- **Mostly outside the code we translate:** the rare SSSE3 / SSE4.2 (`palignr`, `pcmpistri`,
  `pblendvb`, `pabsd`) and 54 AVX instructions, which sit in OpenCV and CRT dispatch paths. Leave
  them unsupported, failing closed.
- **Note:** SSE scalar `ss`/`sd` is 13% of all instructions (`movss` alone ≈ 501K). The NRT engine
  is SSE math, not x87 (x87 is 0.49%), so lane-accurate SSE matters more here than it did in
  OutRun.

**Translator: function discovery.** There is no `.pdata` (32-bit), so, as in OutRun, use the union
of:
- direct call targets;
- vtables (from RTTI: 1,371 classes);
- `__xc_a` initializer tables;
- SafeSEH / `_CxxFrameHandler3` FuncInfo tables (8,143 handlers);
- data code pointers;
- the m2e dispatch tables.

**Translator: scale.**
- About 15–25K functions in the region we translate, against OutRun's 4,206 (≈ 157K lines,
  11.7 MB `bulk_tr.cpp`).
- Expect **≈ 1–1.5M lines of generated C++.** The build needs sharding (hundreds of translation
  units), unity builds per address range, and a compile-time budget (CI cache).

**Translator: exceptions.**
- 24K `fs:` accesses are x86 SEH frames for MSVC C++ exceptions (`_CxxFrameHandler3`).
- Plan: implement `fs:[0]` frame chains in the guest TEB, and run `throw` / unwind natively against
  the FuncInfo tables (no host C++ exceptions crossing translated frames). Most throws are
  error-only, so step one is "throw = fail-closed abort with a report". Full unwinding follows only
  if a real path needs it.

**Translator: threads.**
- `CreateThread` sites (loader, sound, m2e task, CRI) and ConcRT.
- Each guest thread gets its own `Cpu` and stack inside the guest window.
- `lock` ops map to host atomics on guest memory.
- x86 TSO versus ARM weak ordering: emit acquire/release on all guest loads/stores *only* in
  functions flagged as shared (a per-function list), elsewhere plain accesses. Start with a
  single-host-thread scheduler where possible (cooperative, like the original task system) to
  avoid ordering bugs entirely.

**Translator: indirect calls.**
- 11.6K indirect calls (vtables, function pointers) go through an address → host-function table
  generated from the discovery set.
- An unknown target aborts with its address (fail closed).

**Graphics.** OutRun's `PcD3D9Device` doesn't fit, so a new **`PcD3D11Device`** copies the D3D11
API the EXE actually calls. The vtable call sites tell us which methods.

**ExeImage.** The prepared cache also stores the **post-initializer `.data` snapshot** (see
static initializers below).

### New compared with OutRun

1. **i960 / Model 2 layer.**
   - First translate the EXE's own m2e interpreter like any other code: it's correct by
     construction, and the oracle can compare guest RAM.
   - Then replace the hot dispatch loop with a native, address-named implementation (the
     translated interpreter running the i960 interpreter is double interpretation, a risk on
     iPhone).
   - MAME's `i960` and `model2` drivers are a behavioural reference only; no code is taken.
   - ROM-derived code can never be compiled into the app, so this stays an interpreter.
2. **HLSL→MSL.**
   - The shader source is in the player's EXE, so translation happens **at prepare time on the
     device**.
   - Options, in order of preference:
     - (a) a small vendored HLSL→MSL path (DXC → SPIR-V → SPIRV-Cross as a library, prebuilt for
       Apple targets) that runs once and caches `.metallib` source in the prepared image;
     - (b) DXBC → MSL from the compiled blobs (a DXVK/Mesa-style DXBC front end, more work);
     - (c) hand-written MSL equivalents keyed by shader hash, with `#ifdef` variants. The shaders
       are original Sega code, so they can't be committed verbatim; but an MSL rewrite written to
       match behaviour can, the same way the C++ port is.
   - Recommendation: (a) for coverage, with (c) for hot shaders that need tuning.
   - The Model 2 `.fxo`/GLSL path: (b) or (c), small.
3. **Software reference device for D3D11.** A full D3D11 rasterizer (geometry shaders, compute,
   BCn, MRT) is far bigger than OutRun's D3D9 one. Proposal:
   - a *minimal* reference rasterizer covering the subset used by attract / 2D;
   - the **oracle for 3D is captured Windows frames** (RenderDoc / apitrace captures from a
     Windows machine, frame-numbered under autoplay), compared with a perceptual tolerance.
4. **CRI middleware.**
   - Translate it (0.75 MB, 3.9K targets, threads and DirectSound underneath), or replace it
     natively:
     - ADX decoder (documented format, small);
     - CSB / `@UTF` cue player;
     - Sofdec via an MPEG-1/2 decoder (FFmpeg is LGPL; AVFoundation can't decode MPEG-PS).
   - Recommendation: **native replacements at the CRI API boundary** (`ADXT_*`, `mwPly*`,
     `criAtom*`-style entry points identified by address). This removes CRI's threads from the
     translated set.
5. **Static initializers.** Don't translate the 4.3 MB of `Vec3` construction.
   - At prepare time, run each `__xc_a` entry in a **tiny restricted x86 evaluator** that supports
     only the patterns present (push imm / `fld` / `movss` const / `call ctor` / store).
   - Fail closed on anything else.
   - Store the resulting `.data`/`.bss` image in the cache. The prepared-image check covers it.
6. **Shell replacement.** Window-message table, credits, security, settings files, test / service
   buttons, all native.
7. **DInput8 / XInput shim** from `PadSnapshot`, and **force feedback → haptics**.
8. **Two content versions.** Covered in Phase 6.
9. **Webcam / OpenCV and networking:** stubbed and failing closed. OpenCV, OpenEXR, libtiff and
   WinHTTP aren't translated at all.

### Registers, RIP-relative addressing, SSE / AVX, ABI, host pointers

The brief's checklist assumed x64. The evidence says x86-32:

| Topic | Finding | Translator change |
|---|---|---|
| Registers | 8 GPRs (32-bit), 8 XMM, x87, MMX aliasing x87 | Add `mm[8]` aliasing `Fpu` regs (MMX is rare; `emms` 83 sites) |
| RIP-relative | **Not applicable** (32-bit absolute addressing, base fixed at 0x400000, no relocs) | None: absolute addresses go through `host(c,a)` as today |
| SSE / AVX | SSE / SSE2 heavy (scalar 13%, packed ≈ 1.4% incl. integer), SSSE3 / SSE4 rare, AVX 54 | Full SSE2 lane semantics (NaN, denormal and MXCSR rounding as on x86; DAZ/FTZ off). SSSE3+, AVX: fail closed. |
| ABI | **MSVC x86**: `cdecl`, `stdcall`, `thiscall` (ecx = this), `fastcall`; x87 `st0` return for floats; SEH frames on `fs:[0]` | Native overrides declare the calling convention per address (stack cleanup differs). OutRun already handles cdecl / stdcall / thiscall; add the `fastcall` and float-return checks. |
| Host vs guest pointers | Guest pointers are 32-bit values in structures, vtables and the i960 bus map | **Remapped guest memory**: a reserved 4 GB window, `base + addr`. LAA means guest addresses above 2 GB are possible, so reserve the full 4 GB. Host pointers never enter guest memory. Native code keeps guest addresses and converts at use. iOS / visionOS need `com.apple.developer.kernel.extended-virtual-addressing` (and probably `increased-memory-limit`). |
| Memory pressure | Courses are 370–400 MB files streamed into a 32-bit space; SizeOfImage ≈ 33 MB | The guest heap is a native allocator inside the window. Textures and buffers live in *host* GPU memory behind handles, not in the guest window. Watch the iPhone jetsam limits. |

## 7. Risks, recommendation, plan

### Risks (highest first)

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **Scale**: ≈ 5× OutRun's translated code; ≈ 1–1.5M generated lines; compile time and binary size | High | High | Translate only game + NRT + m2e; native CRI; skip OpenCV/EXR/TIFF/initializers; sharded builds; per-shard caching |
| 2 | **D3D11 → Metal and HLSL → MSL** without a D3D11 reference rasterizer | High | High | DXC/SPIRV-Cross at prepare time; Windows frame captures as the 3D oracle; reference device only for 2D |
| 3 | **Threads and memory ordering** (ConcRT, loader, m2e task, CRI) | Medium | High | Cooperative single-host-thread scheduling first; native CRI; atomics only where `lock` appears |
| 4 | **i960 performance on mobile** (double interpretation) | Medium | High | Native interpreter for the hot loop early; profile on an A-series device in Phase 5 |
| 5 | **C++ exceptions across translated frames** | Medium | Medium | Abort-on-throw first; implement FuncInfo unwinding only if a real path throws |
| 6 | **BCn textures on iPhone / visionOS** | Medium | Medium | BC support on iOS is per device (`MTLDevice.supportsBCTextureCompression`: M-series iPads yes, iPhones not guaranteed). Transcode to ASTC at prepare time where it's missing. |
| 7 | **Memory on iOS / visionOS** (4 GB reserve plus big course streams) | Medium | Medium | Extended-VA entitlement; host-side GPU resources; measure in Phase 8 |
| 8 | **2017 vs 2019 EXE differences** | Medium | Medium | One engine (2019) plus a verified per-version delta list; see Phase 6 |
| 9 | **Legal / provenance**: shaders, ROM data, patched EXE | Low | High | Nothing from the game folder enters the repo; shaders and ROM translated on device; pristine-only hashes |
| 10 | **Unknowns** (`fv_rom` role, NRFC internals) | Medium | Low | Resolved during Phase 2–5 with REA/Ghidra |

### Recommendation: **GO**

- Nothing requires the security hardware. The code is plain MSVC x86-32 and the security check is
  a replaceable protocol.
- The machine model matches what the OutRun pipeline already handles. The new work is
  well-bounded: D3D11 / HLSL, the i960 layer, CRI, scale, threads.
- The main cost is volume, not a blocker.

### Phased plan

Sizes are relative to the OutRun port, which took about 6 weeks from start to iOS: S ≈ days,
M ≈ 1–2 weeks, L ≈ 3–5 weeks, XL ≈ 6+ weeks.

| Phase | Work | Exit criterion | Size |
|---|---|---|---|
| **1. Repo skeleton** | CMake / CTest layout following OutRun's pattern (written fresh, not copied). `ExeImage` with **pristine 2017 + 2019** range manifests and cache. Content-root picker. Fail-closed stubs. `PHASE0` unknowns turned into tracked issues. | `prepare` verifies a user's folder and writes the cache; tests green | **S** |
| **2. Translator for Daytona** | Function discovery (calls, RTTI vtables, `__xc` tables, SafeSEH / FuncInfo, data pointers, m2e tables). SSE2 double / packed / integer, MMX, string ops, atomics. `fastcall` and float returns. `fs:` TEB and SEH frames with abort-on-throw. Multi-`Cpu` threads. Indirect-call table. Sharded output. Restricted evaluator for static initializers. Per-instruction unit tests against x86 ground truth (a corpus recorded on Windows or Rosetta). | All game + NRT + m2e functions translate with zero unsupported operands (or an explicit native override); generated code compiles in under ≈ 15 min incremental | **XL** |
| **3. Graphics** | `PcD3D11Device` (the subset the EXE calls). Metal backend (buffers, textures incl. BCn, samplers, blend / depth state, MRT, GS emulation or rewrite where used, CS). HLSL → MSL at prepare time (DXC + SPIRV-Cross) with a hash-keyed MSL override table. Model 2 `.fxo` path. Software reference device for 2D / UI. Windows frame-capture oracle and comparison tool. | Attract-mode frames match Windows captures within tolerance | **XL** |
| **4. Audio, video, input, arcade services (fail closed)** | Native ADX / CSB / Sofdec at the CRI API boundary → `AudioOutput` / texture upload. DInput8 / XInput shim from `PadSnapshot`. Shell service (security answer, credits / free play, messages, settings files, test / service). Networking forced standalone. Camera / OpenCV stub. EEPROM / backup persistence. | Every Win32 import used by translated code is native or fails closed with a report | **L** |
| **5. Headless Mac runner + oracle** | Boot to attract, autoplay a race (2019 `AutoPlaySetting.ini`, or a scripted pad). Oracle: guest-RAM / i960-state hashes per frame against Windows traces; frame captures for graphics. Native i960 hot loop. Performance profile. | Deterministic attract plus a full race, headless, in CTest; oracle diffs clean | **L** |
| **6. Both 2017 and 2019 content** | Pick at start-up. The engine is the 2019 EXE; 2017 runs on it with the 2017 content root. Differences between the 2017 and 2019 EXEs are found with REA `match_native_functions` / `compare_functions` and ported as native overrides where behaviour differs. If the content isn't compatible with the 2019 engine, fall back to a second translated set with address maps from the function matching (+M). | Both versions reach attract and complete a race under the oracle | **M** (to **L** if a second translated set is needed) |
| **7. macOS app** | AppKit / SwiftUI shell, `Presenter` on `CAMetalLayer`, GameController and wheel input, folder picker and prepare UI, settings, test menu access | Playable from the Finder; signed build | **M** |
| **8. iOS / iPadOS** | Touch / tilt / controller input, haptics for force feedback, entitlements (extended VA, increased memory), BCn fallback, storage for the user-supplied folder (Files import), performance tuning | Playable at a stable frame rate on a recent iPad and iPhone | **M–L** |
| **9. visionOS** | Window or immersive presentation, controller input, memory tuning | Playable in a window on Vision Pro | **S–M** |

Critical path: 1 → 2 → (3 ‖ 4) → 5 → 6 → 7 → 8 → 9. Phases 3 and 4 can run in parallel once
Phase 2 produces a linking binary with fail-closed imports.

### Open unknowns

- **`rom/fv_rom/`**: the role of `fixpage.bin` (5.3 MB) and the larger code ROMs, beyond
  `rom_ep2.bin` (possibly the extended / new-content i960 program or fixed-page textures).
- **m2e internals**: confirmed to be an interpreter with no JIT (no executable memory). Not yet
  known: how the 4,096-entry table is indexed (opcode × mode?) and how the bus map routes copro /
  geometry calls to NRT.
- **NRFC internals**: which chunks use zlib versus raw data, and how the streaming is laid out. It
  isn't encrypted.
- **Exact function count.** The heuristic range is 39K–71K across the whole EXE. A Ghidra/REA
  auto-analysis in Phase 2 settles it.
- **How compatible 2017 content is with the 2019 engine.** This decides whether Phase 6 is M or L.
- **Which D3D11 features are used** (geometry and compute shaders, tessellation?). The profiles say
  GS and CS exist; usage is counted in Phase 3.

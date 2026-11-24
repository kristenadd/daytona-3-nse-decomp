# Daytona Championship USA — native port (work in progress)

A port of Sega's *Daytona Championship USA* (2017) and *New Season Edition*
(2019) to macOS on Apple silicon, iOS / iPadOS and visionOS. See
[PHASE0.md](PHASE0.md) for the inventory, the go / no-go and the phased plan.

The repository holds **code only**. Players supply their own game files; the
port reads them, never changes them, and keeps its prepared data in its own
folder.

## Status

Phase 1 (skeleton): the player's installation is found, its executable and
Model 2 ROMs are verified against SHA-256 manifests, and a prepared image is
cached.

Phase 2 (translator): the executable's functions are discovered and translated
to C++ that builds for arm64. Guest threads run cooperatively on host threads,
one at a time.

Phases 3 and 4 (in progress): `dcrun --content <folder>` boots the 2019
executable in a window and plays: attract mode, then a race from Start, drawn
with Metal from the game's HLSL translated to MSL. Boot takes about two minutes.
An Xbox or PlayStation controller drives it (layout in [PHASE4.md](PHASE4.md)).
The attract loop's colours match a Windows capture. There is no sound or movie
playback yet. See [PHASE3.md](PHASE3.md) and [PHASE4.md](PHASE4.md).

## Playing

```bash
build/mac-host/dcrun --content "<folder>" build/mac-host/prepared/DCUSA2019.cache
```

This opens a 1280×720 window (resizable; Ctrl-Cmd-F for full screen, Cmd-Q to
quit). Metal, the shader folder (`build/mac-host/prepared/shaders`) and
translation of new shaders are on by default. Pad controls: Back coin, Start
start, right/left trigger gas/brake, left stick steering, right/left shoulder
shift up/down, Y/X/B/A views 1-4. Menus are confirmed with the gas.
`--headless` (or `DCUSA_WINDOW=0`) runs without a window, for scripted runs.

## iPhone and iPad

The same game code builds as an iOS / iPadOS app (`src/platform/ios`). It is
verified in the Simulator (iPad Pro 13-inch, iOS 27.2): it boots to attract and
draws the 3D demo. A real device is not tried yet.

```bash
cmake --preset ios-sim
cmake --build --preset ios-sim
```

That gives `build/ios-sim/DaytonaUSA.app`, signed ad hoc for the Simulator.
The app reads its files from its Documents folder (reachable from the Files
app, or Finder with the device connected): `DCUSA2019.cache` and the
`shaders` folder (both from the Mac's `build/mac-host/prepared`) and the game
folder. Shaders cannot be translated on the device, so the shaders folder must
already hold every shader the game uses. In the Simulator, put them in place
with `xcrun simctl get_app_container booted local.dcusa.DaytonaUSA data` and
copy into its `Documents`; `cp -cR` clones the 5 GB game folder without using
space. (A symlink does not work: the install scan does not follow links.)

GPUs without BC texture support (the Simulator, older iPhones) get BC1-BC5 and
BC7 decoded on upload (`src/gpu/bc_decode`), matching the Mac GPU's own decode
(`test_bc_decode`). BC6H is not decoded yet. `DCUSA_BC_DECODE=1` forces the
decoder on a Mac.

For a device, still to do: a signing team (`-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM`
or a codesign identity), and the extended-virtual-addressing and
increased-memory-limit entitlements, since the guest address space is 4 GB.

## Building and testing (macOS, Apple silicon)

```bash
cmake --preset mac-host
cmake --build --preset mac-host
ctest --preset mac-host
```

`test_game_files` uses the installation in `../Daytona Championship USA`
(override with `-DDCUSA_GAME_LIBRARY=<folder>`) and skips when it is absent.

## Preparing the game files

```bash
build/mac-host/dcprepare "<folder>" [--edition 2017|2019|all] [--cache-dir <dir>]
```

`<folder>` can be the game folder (the one holding `Daytona.exe`), an
edition's top folder, or a folder holding both editions. The 2017 release
must be the unpatched executable: a TeknoParrot-patched `Daytona.exe` is
refused, and `Daytona.exe.original` beside it is used instead.
Caches default to `~/Library/Application Support/DaytonaUSA/Cache`.

## Translating the game code

Needs Python 3 with Capstone 5 (`pip install --target build/pylib capstone==5.0.3`).
The output is derived from the player's executable, so it stays in the build
tree and is never committed.

```bash
cmake --preset mac-host -DDCUSA_PREPARED_CACHE=$PWD/build/mac-host/prepared/DCUSA2019.cache
cmake --build --preset mac-host --target dcrun
```

The build runs `discover.py` and `translate.py` when the cache or a translator script is newer
than the last translation. Output goes to `build/x86tr/<edition>/` (2017 or 2019, read from the
cache header). The first configure translates immediately when that folder has no `sources.txt`.
`DCUSA_GENERATED_DIR` still selects a folder that was translated by hand.

`discover.py` finds function entries (call graph, RTTI vtables, SafeSEH
handlers, C++ EH funclets, code pointers in data) and marks what is reachable
from real roots. `translate.py` writes one C++ function per reachable guest
function; unreachable ones become stubs that fail closed, as does any
instruction the translator does not handle (`coverage.json` lists them).
`test_x86_semantics` checks the translation of ~860 instruction cases against
the same code run as x86_64 under Rosetta 2.

## Translating the shaders

Needs `brew install glslang spirv-cross`. The game builds its HLSL at run time,
so the requests are captured from a run, then translated:

```bash
DCUSA_SHADERS=$PWD/build/mac-host/prepared/shaders build/mac-host/dcrun --content "<folder>" build/mac-host/prepared/DCUSA2019.cache
python3 tools/shaderprep/translate_shaders.py build/mac-host/prepared/shaders
build/mac-host/mslcheck build/mac-host/prepared/shaders/msl
```

Later runs with the same `DCUSA_SHADERS` use the translations' bindings, and
capture any request they have not seen (a windowed run does this by itself).
Headless, add `DCUSA_METAL=1` to draw with Metal instead of the 2D software
reference; `DCUSA_FRAME=<path>` writes presented frames as PPM. The shader folder holds the player's
shader source and stays out of the repository.

## Layout

| Path | What |
| --- | --- |
| `src/system` | SHA-256, PE32 mapping, editions, EXE / ROM manifests (`*.inc`, generated), prepared image cache, installation discovery |
| `src/runtime` | fail-closed reporting, the platform contract (`host_platform.hpp`) |
| `src/recomp` | guest machine state, the 4 GB guest address space, indirect-call dispatch, helpers the translated code calls, Win32 / D3D11 / DirectSound handlers, cooperative guest threads (`threads.cpp`) |
| `tools/x86tr` | function discovery, the x86 → C++ translator, the Rosetta ground-truth instruction tests |
| `src/gpu` | the offscreen Metal device behind the D3D11 calls; BC texture decoding for GPUs without it |
| `src/platform/ios` | the iOS / iPadOS app: UIKit scene, Metal layer, game thread |
| `tools/shaderprep` | HLSL → SPIR-V → MSL for the captured compile requests, with per-class binding slots and reflection |
| `tools/mslcheck` | compiles prepared MSL with the system Metal device (CTest `test_msl`) |
| `tools/dcprepare` | verifies installations and writes the prepared images |
| `tools/make_manifests.py` | regenerates the manifests from pristine executables (digests only, no game bytes) |
| `tests` | CTest suite |

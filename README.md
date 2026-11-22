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
executable and plays its attract mode, drawn by an offscreen Metal device from
the game's HLSL translated to MSL (`DCUSA_METAL=1 DCUSA_SHADERS=<dir>
DCUSA_TRANSLATE=1`); boot takes about 8,000 frames. Without `DCUSA_METAL` the 2D
software reference draws instead, and the 3D scene stays black. The frames are
not yet compared with Windows captures, and there is no audio or video yet.
See [PHASE3.md](PHASE3.md) and [PHASE4.md](PHASE4.md).

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
capture any request they have not seen. Add `DCUSA_METAL=1` to draw with the
offscreen Metal device instead of the 2D software reference; `DCUSA_FRAME=<path>`
writes presented frames as PPM. The shader folder holds the player's
shader source and stays out of the repository.

## Layout

| Path | What |
| --- | --- |
| `src/system` | SHA-256, PE32 mapping, editions, EXE / ROM manifests (`*.inc`, generated), prepared image cache, installation discovery |
| `src/runtime` | fail-closed reporting, the platform contract (`host_platform.hpp`) |
| `src/recomp` | guest machine state, the 4 GB guest address space, indirect-call dispatch, helpers the translated code calls, Win32 / D3D11 / DirectSound handlers, cooperative guest threads (`threads.cpp`) |
| `tools/x86tr` | function discovery, the x86 → C++ translator, the Rosetta ground-truth instruction tests |
| `src/gpu` | the offscreen Metal device behind the D3D11 calls |
| `tools/shaderprep` | HLSL → SPIR-V → MSL for the captured compile requests, with per-class binding slots and reflection |
| `tools/mslcheck` | compiles prepared MSL with the system Metal device (CTest `test_msl`) |
| `tools/dcprepare` | verifies installations and writes the prepared images |
| `tools/make_manifests.py` | regenerates the manifests from pristine executables (digests only, no game bytes) |
| `tests` | CTest suite |

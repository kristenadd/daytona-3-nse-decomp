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

Phase 2 (translator, in progress): the executable's functions are discovered
and translated to C++ that builds for arm64. No game code runs yet: the
loader, imports and threads come next.

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
export PYTHONPATH=build/pylib
python3 tools/x86tr/discover.py  build/mac-host/prepared/DCUSA2019.cache build/x86tr/2019
python3 tools/x86tr/translate.py build/mac-host/prepared/DCUSA2019.cache build/x86tr/2019/functions.json build/x86tr/2019/gen
cmake --preset mac-host -DDCUSA_GENERATED_DIR=$PWD/build/x86tr/2019/gen
cmake --build --preset mac-host --target dcusa_generated
```

`discover.py` finds function entries (call graph, RTTI vtables, SafeSEH
handlers, C++ EH funclets, code pointers in data) and marks what is reachable
from real roots. `translate.py` writes one C++ function per reachable guest
function; unreachable ones become stubs that fail closed, as does any
instruction the translator does not handle (`coverage.json` lists them).
`test_x86_semantics` checks the translation of ~860 instruction cases against
the same code run as x86_64 under Rosetta 2.

## Layout

| Path | What |
| --- | --- |
| `src/system` | SHA-256, PE32 mapping, editions, EXE / ROM manifests (`*.inc`, generated), prepared image cache, installation discovery |
| `src/runtime` | fail-closed reporting, the platform contract (`host_platform.hpp`) |
| `src/recomp` | guest machine state, the 4 GB guest address space, indirect-call dispatch, helpers the translated code calls |
| `tools/x86tr` | function discovery, the x86 → C++ translator, the Rosetta ground-truth instruction tests |
| `tools/dcprepare` | verifies installations and writes the prepared images |
| `tools/make_manifests.py` | regenerates the manifests from pristine executables (digests only, no game bytes) |
| `tests` | CTest suite |

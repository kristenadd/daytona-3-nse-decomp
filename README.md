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

**Boot screen (optional).** In place of the black "Data Loading" and network
check screens, the boot can show the attract title with a progress bar in its
colours, filled from the game's own loading percentage, then "Checking
network...". The title is rendered from your game files (it is never in
the repository):

```bash
tools/bootscreen/capture_title.py --dcrun build/mac-host/dcrun --cache build/mac-host/prepared/DCUSA2019.cache --content "<folder>" --out build/mac-host/prepared/boot/title.png
DCUSA_BOOT_SCREEN=1 build/mac-host/dcrun --content "<folder>" build/mac-host/prepared/DCUSA2019.cache
```

The capture runs attract headless for a few minutes with the "PRESS START
BUTTON" / "FREE PLAY" image left out (the copyright stays) and keeps the first
settled title frame. `DCUSA_BOOT_SCREEN_IMAGE=<png>` picks another image. On
iOS the build puts the title in the app (`DCUSA_BOOT_TITLE`, default that
path) for the launch screen and the boot screen, which the Settings app
switches off ("Title boot screen").

## iPhone and iPad

The same game code builds as an iOS / iPadOS app (`src/platform/ios`). It is
verified in the Simulator (iPad Pro 13-inch, iOS 27.2): it boots to attract and
draws the 3D demo. On an iPhone 15 Pro Max it boots, plays attract and races
(slowly, so far). An iPad mini 6 (4 GB) still runs out of memory early in boot:
its GPU has no BC textures, and decoded they do not fit (CLAUDE.md, item 8).

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

GPUs without BC texture support (the Simulator, older iPhones) get every BC
format the game uses (BC1-BC7) decoded on upload (`src/gpu/bc_decode`), matching the Mac GPU's own decode
(`test_bc_decode`). `DCUSA_BC_DECODE=1` forces the
decoder on a Mac.

On a device (an Xcode project, signed automatically for your team):

```bash
cmake --preset ios-device -DDCUSA_IOS_TEAM=<team id> -DDCUSA_GENERATED_DIR=$PWD/build/x86tr/2019/gen
xcodebuild -project build/ios-device/DaytonaUSA.xcodeproj -target DaytonaUSA -configuration RelWithDebInfo -allowProvisioningUpdates
xcrun devicectl device install app --device <udid> build/ios-device/RelWithDebInfo-iphoneos/DaytonaUSA.app
```

`-allowProvisioningUpdates` registers the app ID (`DCUSA_IOS_BUNDLE_ID`) with
the two entitlements in `src/platform/ios/DaytonaUSA.entitlements`: extended
virtual addressing (the guest's 4 GB address space) and the increased memory
limit. Xcode must be signed in to an account of that team. Copy the files in
with `xcrun devicectl device copy to --domain-type appDataContainer
--domain-identifier local.dcusa.DaytonaUSA --source <file or folder>
--destination Documents/<name>` (folders copy whole), or the Files app. The
app writes its messages to `Documents/log.txt`; read it back with
`devicectl device copy from ... --source Documents/log.txt`. A memory kill
leaves a `JetsamEvent-*.ips` in `--domain-type systemCrashLogs`. The log also
gets the footprint every 10 s. Variables for a run go through devicectl with
a `DEVICECTL_CHILD_` prefix; relative paths are in Documents, so
`DEVICECTL_CHILD_DCUSA_FRAME=f DEVICECTL_CHILD_DCUSA_EVERY=500` saves frames there
and `DEVICECTL_CHILD_DCUSA_PAD_SCRIPT=race.txt` plays a pad script.
`DCUSA_INI="Section.Key=Value;..."` overrides the game's GameData/config.ini
settings (shadow map sizes, the dynamic resolution range).

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

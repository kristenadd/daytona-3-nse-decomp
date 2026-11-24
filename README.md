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
settled title frame. `DCUSA_BOOT_SCREEN_IMAGE=<png>` picks another image. The
label uses `build/mac-host/prepared/boot/font.ttf` when there is one
(`DCUSA_BOOT_SCREEN_FONT=<ttf>` otherwise; Eurostile matches the game's text,
and it is not in the repository either), else Helvetica Neue Bold. On
iOS the build puts the title in the app (`DCUSA_BOOT_TITLE`, default that
path, with font.ttf beside it) for the launch screen and the boot screen, which the Settings app
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
## Settings

The port has no settings file of its own. The game reads three .ini
files, and the port's own options are environment variables. On iOS the
variables go through devicectl with a `DEVICECTL_CHILD_` prefix (see above).

### GameData/config.ini (the game's settings)

Location: `<game folder>/GameData/config.ini` (the 2019 edition keeps it under
`Sega/`). The game reads it from the game folder. To override keys without
editing the file, use `DCUSA_INI="Section.Key=Value;..."` (section and key in
any case; the last value given wins). On iOS, `Render.DYNAMIC_RESOLUTION_MAX=960`
is built in (`kPhoneIni`, `src/recomp/host_files.cpp`), and `DCUSA_INI`
overrides that too.

| Section | Keys (2019 defaults) |
| --- | --- |
| `Render` | `ENABLE_DYNAMIC_RESOLUTION=1`, `DYNAMIC_RESOLUTION_MAX=1920`, `DYNAMIC_RESOLUTION_MIN=960` (the scene width steps between them, driven by GPU time), `ENABLE_DYNAMIC_MSAA=0`, `ENABLE_DYNAMIC_BAKE_FREQUENCY=1` |
| `ShadowMap` | `{CLASSIC,NEW}_{BIGINNER,ADVANCED,EXPERT}_SHADOW_MAP_SIZE` (1024, 1500 for classic expert), and the same with `_STATIC_` (4096, 6144 for expert) |
| `Reflection` | `ENABLE_SKY_REFLECTION_ONLY_IN_NEW_BEGINNER=0`, `ENABLE_WATER_REFLECTION_IN_NEW_EXPERT=1` |
| `CarTexture` | `ENABLE_CAR_TEXTURE_QUATER_SIZE=1` |
| `Camera` | `FOVY_RATE=0.5` |
| `FullHD` | `ENABLE_FULLHD=1`, `ENABLE_FULL_SCREEN=1` |
| `SpeedScale` | `<course>_<difficulty>_<race length>` speed factors (0.7 to 1.0) |
| `Wheel` | force feedback: `DEAD_ZONE`, `PERIOD1-3`, `FORCE1/2_S/N/W`, `GRASS_*`, `GARIGARI_FORCE_*`, `ANGLE_PARAM`, `VIB_*` |
| `Network` | `FORCE_NET_VALID=0`, `NETWORK_TYPE=0` (0 slave, 1 master, 2 single play) |
| `OpenCV` | camera capture (`ENABLE_CAPTURE`, `CAPTURE_FPS`, `DEFAULT_CROP_X/Y`) |
| `Log`, `CarBox`, `HDR Texture` | debug switches, off |

### ShellData/ShellData.ini (operator settings)

The cabinet's operator menu belongs to `Shell.exe`, which does not run here.
The game reads a copy of the file kept in a `container` folder next to the
cache file: `build/mac-host/prepared/container/ShellData/ShellData.ini` with
the usual cache path, `Documents/container/ShellData/ShellData.ini` on iOS.
The copy is made from the game folder's file the first time you change a
setting. Edit the copy directly, or use:

```bash
build/mac-host/dcrun --shell-setting Credit.Freeplay=0 --content "<folder>" build/mac-host/prepared/DCUSA2019.cache
```

| Section | Keys (defaults) |
| --- | --- |
| `Credit` | `Freeplay=1` (0: coins), `EntryType=0` |
| `Audio` | `MusicVolume=80`, `SFXVolume=100`, `VoiceVolume=90`, `AttractMusic=3` |
| `CalibrationData` | `AccelUp=30`, `AccelDown=200`, `BrakeUp=30`, `BrakeDown=200`, `SteeringLeft=15`, `SteeringRight=247`, `FeedbackStrength=2` |
| `Network` | `Enabled=0`, `CabinetID=1`, `NumCabinets=1` |

### ShellData/GameSettings.ini (game rules: difficulty, race length, selection)

The game itself reads `../ShellData/GameSettings.ini` (2019 edition), one
section `[GameSettings]` whose values are option names. Like ShellData.ini,
the game reads a copy in `container/ShellData/`. Edit it with `--shell-setting`
and section `GameSettings` (quote keys that contain spaces):

```bash
build/mac-host/dcrun --shell-setting "GameSettings.DIFFICULTY=VERY HARD" --shell-setting "GameSettings.GAME LENGTH=LONG" --content "<folder>" build/mac-host/prepared/DCUSA2019.cache
```

The table is the game's own list (`0x118A510` in the 2019 exe). The reader
`0x5189B0` stores each choice's index as a byte at `[0x15C2298]+4+row`, so
`DCUSA_WATCH="[15C2298]+4"` shows DIFFICULTY in the low byte. A key missing
from the file should take the game's default (not checked). The installed file sets 21 of the
24 keys, and some of its values differ from the game's defaults.

| Row | Key | Options (game default first, then the installed file's value) |
| --- | --- | --- |
| 0 | `DIFFICULTY` | VERY EASY, EASY, MEDIUM, HARD, VERY HARD (MEDIUM; installed EASY) |
| 1 | `GAME LENGTH` | VERY SHORT, SHORT, MEDIUM, LONG, VERY LONG, CLASSIC MODE (MEDIUM) |
| 2 | `CAMERA ENABLED` | YES, NO (YES) |
| 3 | `INSTANT REMATCH` | YES, NO (YES) |
| 4 | `START ON COIN ENTRY` | YES, NO (NO) |
| 5 | `MENU TIMINGS` | SHORT, MEDIUM, LONG (MEDIUM) |
| 6 | `CAR SELECT` | CHOOSE, RANDOM, 1-5 (CHOOSE) |
| 7 | `TRACK SELECT` | CHOOSE, RANDOM, EASY ONLY, 37_SPEED, DINOSAUR, SEASIDE, DAYTONA, LAKESIDE, METRO (CHOOSE) |
| 8 | `TRANSMISSION SELECT` | ON, OFF (ON) |
| 9 | `CHAMPIONSHIP MODE` | YES, NO (YES) |
| 10 | `CHAMPIONSHIP CONTINUE` | YES, NO (YES) |
| 11 | `AUTO DRIVE` | YES, NO (YES; installed NO). See below. |
| 12 | `KIDS ONLY MODE` | YES, NO (NO; not in the installed file) |
| 13 | `TOURNAMENT MODE` | ON, OFF, EXCLUSIVE (OFF) |
| 14 | `TOURNAMENT DURATION` | 1-30 days (14) |
| 15 | `TOURNAMENT TYPE` | CHAMPIONSHIP, CUSTOM (CUSTOM) |
| 16 | `CUSTOM TOURNAMENT CAR` | as CAR SELECT (1) |
| 17 | `CUSTOM TOURNAMENT TRACK` | as TRACK SELECT (37_SPEED) |
| 18 | `CUSTOM TOURNAMENT TRANSMISSION` | MANUAL, AUTOMATIC, CHOOSE (CHOOSE) |
| 19 | `TOURNAMENT CONTINUE` | YES, NO (NO) |
| 20 | `TOURNAMENT DATE FORMAT` | MM/DD/YY, YY/MM/DD, DD/MM/YY (MM/DD/YY) |
| 21 | `DRESS CODE` | ON, OFF (OFF) |
| 22 | `DEFAULT CAMERA` | 1-4 (3; not in the installed file) |
| 23 | `LANGUAGE` | ENGLISH, FRENCH, ITALIAN, SPANISH, RUSSIAN, TURKISH, PORTUGUESE (ENGLISH; not in the installed file) |

`AUTO DRIVE` (`0x62E9E0`): with YES, when a 16-bit value of the Model 2
code (`0x501054` in its memory) has been 0 for 192 ticks (about 3.2 s),
the game sends 0xFF to Model 2 input 3 instead, until the value is nonzero
again. Which control that input is has not been checked.

The shell's menu (`shell/GameTest.ini`, `GameAdvancedTest.ini`) also lists
`PARTY MODE`, which the game does not read. The 2017 edition has no
GameSettings.ini; `--shell-setting GameSettings...` then writes a file with
only the keys given, and whether that edition reads one is not checked.

### iOS Settings app

"Title boot screen" (on by default) shows the boot screen (`DCUSA_BOOT_SCREEN`).

### Environment variables: playing

| Variable | Effect |
| --- | --- |
| `DCUSA_WINDOW=0` | No window (the same as `--headless`). |
| `DCUSA_PAD=0` | Ignore game controllers. |
| `DCUSA_PAD_SCRIPT=<file>` | Play a pad script instead of a controller (`src/runtime/pad_script.hpp`; on iOS a file in Documents). Its frames count presents. |
| `DCUSA_PAD_SCRIPT_FROM=attract` | Frame 1 of the script is the first present of attract, so one script fits both a paced and an unpaced boot. |
| `DCUSA_AUDIO=0` | No speakers (silent output, paced in real time). |
| `DCUSA_AUDIO_WAV=<file>` | Record the mix to a WAV file instead of playing it. |
| `DCUSA_INI=Section.Key=Value;...` | Override GameData/config.ini keys (above). |
| `DCUSA_RESOLUTION=<w>x<h>` | Display size the game is told about (default 1920x1080). |
| `DCUSA_MSAA=4\|2\|0` | Multisampling (default 4). With 0, FXAA is on instead. |
| `DCUSA_AA=1\|0\|split` | FXAA on, off, or on the right half only (default: on only without MSAA). |
| `DCUSA_FRAME_SKIP=0\|1\|2` | Leave the scene render out of late ticks: 0 off (Mac default), 1 when late (iOS default), 2 every other tick (a test). |
| `DCUSA_NOPACE=1` | No 60 Hz pacing (runs as fast as it can). |
| `DCUSA_PACE_IDLE=0` | While waiting for the next frame, sleep instead of giving guest threads their turn. |
| `DCUSA_BOOT_PACE=1\|0` | Pace the boot's loading screen too (default: only with a pad script). |
| `DCUSA_BOOT_UNPACE_UNTIL=<percent>` | Leave the boot unpaced only below this loading percentage. |
| `DCUSA_BOOT_GIVE=0` | During an unpaced boot, do not give the loader threads their turns first (the old behaviour). |
| `DCUSA_BOOT_SCREEN=1` | The attract title with a progress bar during boot (Playing, above). |
| `DCUSA_BOOT_SCREEN_IMAGE=<png>` | Title image (default `<prepared>/boot/title.png`). |
| `DCUSA_BOOT_SCREEN_FONT=<ttf>` | Label font (default `font.ttf` beside the image, else Helvetica Neue Bold). |
| `DCUSA_TEXTURE_LIMIT=<px>` | Largest texture size kept (default 1024 on iOS, none on the Mac). |
| `DCUSA_BC_DECODE=1` | Decode BC textures on upload even where the GPU samples them (as on iOS). |
| `DCUSA_METAL=0` | The 2D software reference instead of Metal (on by default in dcrun and on iOS). |
| `DCUSA_SHADERS=<dir>` | Shader folder (default `<cache folder>/shaders`; `Documents/shaders` on iOS). |
| `DCUSA_TRANSLATE=1` | Translate shaders that have no translation yet, during the run. dcrun always sets it (any value counts as on); iOS never translates. |

### Environment variables: diagnostics and tests

| Variable | Effect |
| --- | --- |
| `DCUSA_STATS=1` | Draw, texture and frame-time reports at presents 1, 30 and every `DCUSA_EVERY`; also host API logging. |
| `DCUSA_EVERY=<n>` | Report (and with `DCUSA_FRAME`, write) every nth present. |
| `DCUSA_FRAME=<prefix>` | Write frames as `<prefix>.<n>.ppm` (presents 1, 30, every `DCUSA_EVERY`). |
| `DCUSA_FRAME_RANGE=<a>-<b>` | Write every present from a to b. |
| `DCUSA_READBACK=1` | Read every frame back from the GPU. |
| `DCUSA_EXIT_AT=<n>` | Exit cleanly after n presents (an instrumented build writes its profile). |
| `DCUSA_HITCH=<ms>` | One line for each frame slower than this, with where the time went. |
| `DCUSA_SKIP_LOG=<n>` | Every n presents, ticks per second and how many skipped the render. |
| `DCUSA_MEM=<n>` | Every n presents, host memory by kind (with `DCUSA_GPU_THREAD=0`). |
| `DCUSA_MEM_LIST=1` | List the Metal allocations in the memory report. |
| `DCUSA_MAP_STATS=<n>` | Every n presents, D3D Map calls by type and bytes. |
| `DCUSA_GPU_PASSES=1` | GPU time per render pass; `DCUSA_GPU_PASSES_LOG=1` also prints draws per frame. |
| `DCUSA_GPU_QUERIES=0` | Zeros for GPU timestamp queries (turns off dynamic resolution). |
| `DCUSA_GPU_QUERY_LAG=<k>` | Hold GPU times back k presents, as on a slower GPU. |
| `DCUSA_GPU_TIME_SCALE=<k>` | Report GPU times k times longer (a phone's dynamic resolution on a Mac). |
| `DCUSA_GPU_THREAD=0` | Encode Metal on the main thread instead of the render thread. |
| `DCUSA_FRAMES_IN_FLIGHT=<1-4>` | Frames queued to the GPU (default 2). |
| `DCUSA_ARENA=0` | New Metal storage for every buffer upload instead of the arenas. |
| `DCUSA_METAL_TRACE=<n>` | One line per draw of frame n; `DCUSA_METAL_TRACE_FRAMES=<k>` for k frames; `DCUSA_METAL_TRACE_CB=1` command buffers. |
| `DCUSA_METAL_SCAN=<n>` | Scan the render targets at present n. |
| `DCUSA_SHADER_TRACE=1`, `DCUSA_QUERY_TRACE=1`, `DCUSA_SHOW_TRACE=1` | Trace shader binds, queries, presents and the boot screen. |
| `DCUSA_SKIP_PS`, `DCUSA_SKIP_SRV`, `DCUSA_SKIP_PS_RT`, `DCUSA_SKIP_WIDTH`, `DCUSA_SKIP_SIZES`, `DCUSA_SKIP_TRACE` | Leave out matching overlay draws (used to capture the boot title without its text). |
| `DCUSA_BOOT_SCREEN_DUMP=<prefix>` | Write the boot screen at 0, 50 and 100% and at the network check. |
| `DCUSA_PAD_TRACE=1` | The pad snapshot and what the game made of it. |
| `DCUSA_WATCH=<addr>[,...]` | Print guest memory at presents 1 and every `DCUSA_EVERY` (default 100). |
| `DCUSA_IO=1` | Log guest file access; `DCUSA_IO_STACK=<text>` adds the call stack for paths containing it. |
| `DCUSA_DEBUG_OUTPUT=1` | All of the game's OutputDebugString lines (with `DCUSA_STATS`; otherwise only the first 8). |
| `DCUSA_DSOUND_TRACE=1` | DirectSound calls per buffer. |
| `DCUSA_AUDIO_STATS=1`, `DCUSA_AUDIO_DUMP=<dir>` | Mixer statistics; dump each voice's buffer. |
| `DCUSA_PARALLEL_THREADS=<hex entry>[,...]` | Run these guest threads in parallel. |
| `DCUSA_SLICE=<ms>` | Time slice at a frame yield (default 0). |
| `DCUSA_RELEASE_PAGES=0\|zero` | Keep freed heap pages, or clear them at once (a use-after-free test). |
| `DCUSA_SELFTEST_MATH=1`, `DCUSA_SELFTEST_BC=1` | Compare the translated CRT maths, and the BC decoder, with the host. |

### Build options (CMake)

| Option | Effect |
| --- | --- |
| `DCUSA_GAME_LIBRARY` | Game folder for the tests (default `../Daytona Championship USA`). |
| `DCUSA_PREPARED_CACHE` | Prepared image; the build translates it. |
| `DCUSA_GENERATED_DIR` | A folder translated by hand, instead. |
| `DCUSA_PYTHONPATH` | Python packages for the translator (default `build/pylib`). |
| `DCUSA_GENERATED_OPT` | Optimisation level of the translated code (default `-O2`). |
| `DCUSA_GENERATED_LINES` | Translated files built with line tables, or `all` (for profiling). |
| `DCUSA_PGO` | `generate`, or a merged `.profdata` to optimise with. |
| `DCUSA_ORDER_FILE` | Linker order file for dcrun. |
| `DCUSA_IOS_TEAM` | Development team for a device build. |
| `DCUSA_IOS_BUNDLE_ID` | iOS bundle identifier (default `local.dcusa.DaytonaUSA`). |
| `DCUSA_BOOT_TITLE` | Title image put in the iOS app (default `<prepared>/boot/title.png`). |

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

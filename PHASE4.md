# Phase 4 — audio, video, input, arcade services

Phase 4 from [PHASE0.md](PHASE0.md). It can run beside [Phase 3](PHASE3.md).

**Exit criterion:** every Win32 import used by translated code is native or fails
closed with a report.

The fail-closed half is already true. A handler answers the imports below, and
any other import aborts with the DLL and the name. File reads, the wait on a
guest thread, the message queue, DirectInput, a silent DirectSound device, and
`FindWindow` of `NRT Application` are in place. ADX, CSB, Sofdec, AVI, and the
shell security and credit messages are not answered as a service. The exit
criterion is not met until ADX, CSB, Sofdec, and AVI are native on a run that
needs them. The cabinet check at `0x577622` now sees localhost as `127.0.0.1`,
and with `NumCabinets=1` that sets the ping ready byte. After the first
outbound `DYT` packet, one reply is delivered through `select` and `recv`
with no host socket: id `0x2C`, the message that sets the link ready byte.
`Freeplay` then reads as 1. The same run opens `rom/sound/snd_db.txt`,
`rom/sprite/sprdb.bin`, `rom/auth2d/aetdb.bin`, and the two haar cascades.
Guest threads now run (PHASE2 item 12): `CreateThread` through the CRT wrapper
`0xC215BB` starts the procedure on its own `Cpu`, TEB and stack, and threads take
turns at blocking calls. The sound server `0xB66530`, the CRI workers `0xBFF0EB`,
the NRT IO thread and the resource worker run their own loops. The earlier
shortcuts (alive words written by `CreateThread`, waits that always returned 0,
the CRI pump in `f_00B50540` and in the frame function) are gone. Events,
semaphores and mutexes are real; `timeSetEvent` pulses its auto-reset event on
the host clock (`0x59DD26` passes `TIME_PERIODIC | TIME_CALLBACK_EVENT_PULSE`).
`CoCreateInstance` returns `REGDB_E_CLASSNOTREG`, so WIC and the camera stay
closed. The process heap stays 64 MB because the memory manager `VirtualAlloc`s
100 MB from the arena that starts there; further CRT allocations use
`0x37000000`, then a region from `0x80000000` that commits as it grows.
`VirtualAlloc` continues at `0x40000000` once its first region is full. The run
builds a GDI font DC (no host font). `GetFileTime`, `FileTimeToLocalFileTime`,
and `FileTimeToSystemTime` return success with a fixed stamp. A 90 s run opens
243 files and keeps drawing and presenting with no unported import. New
handlers it needed: `PathStripToRootW`, `GetDiskFreeSpaceW` (64 GB free of 128),
`GetWindowLongW` (with `SetWindowLongW` storing per index), `DestroyWindow` and
`UnregisterClassA` for the helper window `0x59E423` makes, `GetModuleHandleExW`
for the CRT's thread exit, and `WSACleanup` (#116). The DirectSound buffer now
follows the `dsound.h` slot order and reports `DSBSTATUS_PLAYING | LOOPING`; it is
still silent. ADX, CSB, Sofdec, and AVI are still not decoded. The presented
image is still black.

The game folder is read-only. Writes go to a container next to the cache.
No host socket is opened. With an empty queue, `PeekMessageW` returns 0, so the
frame loop does not quit. `dcrun --content <folder>` finds the edition under
that folder, reads it, and stays in the loop.

## The shell's security challenge (answered)

Every 10 s the game sends DYT message `0x2A` with a 4-byte challenge and counts
it unanswered (`[link+0x30]`); after five it asks for a reboot (`0x584B20` →
`0x5BE410` → `[0x17353E0]`) and the boot ends in `ExitProcess`. The shell's
answer is message `0x2E` whose 4-byte payload, run through the game's check
`0x584600` (XOR `0xB1`, twelve rounds of a 32-bit shift register with fixed
feedback taps, a bit reversal of byte 0, XOR `0xC0`, XOR `0x77`), gives the
challenge back. The check is affine over GF(2), so `src/recomp/shell.cpp`
inverts it exactly; `send` queues the answer and `select` / `recv` (with
`MSG_PEEK`) deliver it. CTest `test_shell` checks 20,000 challenges. With it the
boot reaches attract (PHASE3).

Also answered on the way: `CreateProcessA` (the boot starts
`c:\sega\shell\InpWrapper.exe`, the parallel-port force-feedback helper, and
skips it on failure: FALSE, `ERROR_FILE_NOT_FOUND`), `closesocket` (#3),
`DeleteDC`, `DeleteObject`, `IDirectSoundBuffer::GetFormat` / `SetFormat` (the
buffer keeps its `WAVEFORMATEX`), `QueryInterface` for `IDirectSoundBuffer8`,
`ID3D11Buffer::GetDesc`. The file table holds 65,536 handles (file handles at
`0x100000`, find handles at `0x200000`), `VirtualFree(MEM_RELEASE)` makes a
region reusable, and the heap continues at `0x80000000` past its two fixed
regions.

## Operator settings (landed)

The cabinet's Test and Service buttons open the shell's operator menu
(`Shell.exe` with `shell/GameTest*.ini` and `GameAdvancedTest*.ini`), which
does not run here. The game's own path for them is closed in this release.
No device profile maps actions 0 / 1 (Test / Service) or `0x12` / `0x13`
(SW1 / SW2); the keyboard source `0x47CB80` returns false; and no shell message
sets them. So the game's test-mode classes are unreachable, as on the cabinet
without the shell.

What the menu changes is `ShellData.ini`, which the game reads itself:
`[Credit] Freeplay`, `[Audio]` volumes, `[CalibrationData]`. `dcrun
--shell-setting Section.Key=Value` (repeatable) edits the container's copy,
made from the install's the first time, and the game reads that copy in place
of the install's. Checked with `Credit.Freeplay=0` and a pad script:

- Attract shows "INSERT CREDIT(S) TO START / CREDIT(S) 0", as in the Windows
  capture.
- Start with no credit is ignored.
- A coin (`SHELL_MSG_SETCREDIT`) shows "CREDIT(S) 1" and "PRESS START BUTTON".
- Start then opens Single Player.

`Credit.Freeplay=1` restores free play. The setting stays in the container
until it is changed.

## Controls (landed)

The cabinet's controls are a USB IO board the game reads through DirectInput.
`0x5DEBA0` enumerates game controllers (callback `0x5DE930`) and keeps a device
only when its instance name is `SEGA IO Controller`. It enumerates the axes
(callback `0x5DE8B0` sets each to -1000..1000), sets `c_dfDIJoystick` and
acquires it. Each frame `0x47CDF0` polls up to two boards (player slots at
`0x15964A0`, stride `0x90`):

- `lX` is steering: `(lX + 1000) / 2000 * 255`, then the test-menu calibration
  (`0x518470` / `0x518490` / `0x518420`, defaults 0 / 255 / 127), stored as
  0..0xFF00.
- `lRx` is the accelerator and `lZ` the brake: -1000 released, +1000 full.
- Buttons go through a per-device profile at `0x116D490` (five profiles of
  `0xE0` bytes chosen by name; twelve button → action pairs at `+0x80`). The
  `SEGA IO Controller` profile: button 0 start (action 2), 8..11 VR / view 1..4
  (3..6), 3 shift up (14), 4 shift down (13), 1 / 2 / 6 / 7 gear 1..4 (7..10).
- `0x5E2D80` turns the actions into the race's switch image (`0x19C87F8`) and
  the gear index `0x19C880C` (0..3). Shift up / down are edge-triggered (one
  gear per press) and clamped to 1..4 by the game; the H-pattern lever is the
  second board's buttons 8 / 9 / 10 (actions 3 / 4 / 5), shown as SHIFT1..4 in
  the input test (`0x5D2660`). The second board is not reported, which reads
  as the lever in neutral.
- Coin is not on the board. The shell sends `SHELL_MSG_SETCREDIT` (DYT `0x2D`,
  one byte: the credit count, which `0x48CE80` stores at `0x15278F8 + 7`), so a
  coin is the game's current count plus one. Message ids are the name table
  at `0x1543830` plus 20: `0x2C` `SHELL_MSG_SET_SKU` (the greeting), `0x2D`
  `SETCREDIT`, `0x2E` `SECURITY_AUTHENTICATE`.
- Test and service (actions 0 / 1) are not on the board profile either.

The host side is `PadSnapshot`, sampled once per present
(`set_pad_source` / `sample_pad`). `src/runtime/pad_mapping.cpp` is the default
gamepad layout:

| Game action | Controller |
|---|---|
| Coin | Back / Share |
| Start | Start / Options |
| Shift up / down | Right / left shoulder |
| View 1 / 2 / 3 / 4 | Y / X / B / A (Triangle / Square / Circle / Cross) |
| Steering | Left stick X, 10% dead zone, rescaled to full lock |
| Gas / brake | Right / left trigger, analog |
| Gear 1..4 | Unassigned |

`src/platform/macos/gamecontroller_pad.mm` reads the first extended gamepad
through GameController.framework (`shouldMonitorBackgroundEvents`; the main run
loop is serviced from the sample, since the game holds the main thread).
`DCUSA_PAD=0` turns it off. `DCUSA_PAD_SCRIPT=<file>` plays a script instead
(`src/runtime/pad_script.hpp`: `<frame>[-<last>] <control>[=<value>]`), and
`DCUSA_PAD_TRACE=1` prints the snapshot next to what the game decoded. CTest
`test_pad` covers the layout and the script; `test_imports` the board.

Checked in the game with a script, from attract into a race at Daytona:

- Coin: the credit count goes from unset to 1.
- Start: leaves attract for Single Player.
- The throttle confirms each menu: Quick Race, track, car. Shift down at car
  select picks MT.
- In the race, every control decodes as expected:
  - Throttle 0.5 / 1.0 → `0x8000` / `0x10000`.
  - Brake 0.6 / 1.0 → `0x9999` / `0x10000`.
  - Steering is proportional both ways around the calibrated centre; full lock
    reads 0 / `0xFF00`.
  - The four views switch between bumper, hood, near chase and high chase.
  - Five shift-ups from first stop at fourth; five shift-downs stop at first. A
    held shoulder button counts once.

Starting a race also needed `GlobalMemoryStatus` (logged only), the `PSFD00`
section's functions, and the CRT `fmod` / `atan2` handlers (PHASE2, PHASE3).

## Landed

`dcrun --content` calls `set_guest_files` with the install root (the folder that
holds `Daytona/` and `GameData/`), the current directory `Daytona`, and a
writable container. Guest paths use the `D:` drive. `..` cannot leave the
install. A read opens the container copy when one exists, otherwise the
install. A write never modifies the install. `CreateFileA` / `CreateFileW`,
`ReadFile`, `WriteFile`, `SetFilePointer`, `SetFilePointerEx`, `GetFileSize`,
`GetFileSizeEx`, `GetFileAttributesA`, `GetFileAttributesExW`, the
`FindFirstFile` / `FindNextFile` calls, `CloseHandle`, `GetCurrentDirectoryW`,
`SetCurrentDirectoryW`, `GetFullPathNameA` / `W`, `GetFileTime`,
`FileTimeToLocalFileTime`, and `FileTimeToSystemTime` are that layer. The
time calls use a fixed FILETIME / SYSTEMTIME; they do not read host mtimes.
`GetPrivateProfileIntA` and `GetPrivateProfileStringA` read an ini when the
file and the key exist, and otherwise return the caller's default.
`WaitForSingleObject` returns 0. The wait at `0x4881C4` treats that as
finished, and the wait at `0x4FC81D` continues only for 0. `GetExitCodeThread`
writes 0, not `STILL_ACTIVE` (259).

`PeekMessageW` at `0x41ECF6` returns a posted message, or 0 when the queue is
empty. `PostQuitMessage` posts `0x12`. `GetClientRect` writes a 1920×1080
client. `XInputGetState` always returns 1167, which `0x6A0281` counts as no
pad: the controls reach the game through the IO board (see Controls).

`FindWindowA` and `FindWindowW` return `0x2001` when the class or the title
contains `NRT`, which is the class at `0x1415734` and `0x1417FF8`. Any other
name returns 0, so the `SendMessage` of `0x8064` stays on the path that already
returns success. `DirectInput8Create` returns an `IDirectInput8` that reports
the SEGA IO board (see Controls). `DirectSoundCreate8` (ordinal 11) returns a
device. `CreateSoundBuffer` returns a buffer whose `Lock` points at guest
memory at `0x36000000` and whose `Play`, `Stop`, and `GetCurrentPosition`
succeed. No samples are mixed.

## Already answered

Startup, the window, the clock, the heap, and the first frame's graphics calls
are handled. The ones this phase inherits:

- Paths. `PathFileExistsA` and `PathFileExistsW` return false. `PathIsDirectoryW`
  is true only for an empty root or a drive root, so the walk in `0x6C0210`
  stops. `GetPrivateProfileStringA` and `GetPrivateProfileIntA` return the
  caller's default. The first integer read is `Credit/Freeplay`, default 0.
- The window is the handle `0x2001`. No host window is created.
  `GetForegroundWindow` returns it. `SetWindowLongW` installs `0x6A0740`.
- `XInputGetState` returns `ERROR_DEVICE_NOT_CONNECTED` (1167). `0x6A0281`
  counts that as no pad and init still succeeds.
- Winsock ordinals that startup uses return success and transmit nothing:
  `WSAStartup` (#115), `socket` (#23), `bind` (#2), `listen` (#13), `connect`
  (#4), `send` (#19), `select` (#18, which returns 0 so the caller skips
  `recv`), `inet_addr`, `htons`, `ntohs`, `htonl`, `ioctlsocket`, `setsockopt`,
  `gethostname` (fails, and `0x5641A8` keeps the address it already parsed).
  The listen address is `192.168.1.x:27000`.
- `timeGetTime` and the multimedia timer period calls. `timeSetEvent` arms a
  host-clock timer that pulses its event.
- `CreateThread` at `0x591C63` starts the body at `0x56AE20`, which loops while
  byte `+0x1C` is set and sleeps between passes. It runs as a guest thread.

## What is still fail-closed

By DLL, the imports with no handler:

| DLL | Count | What a run needs from it |
| --- | --- | --- |
| KERNEL32 | 136 | `CreateFileA` / `CreateFileW`, `ReadFile`, `WriteFile`, `SetFilePointer`, `FindFirstFile` / `FindNextFile`, `GetFileSize`, `CloseHandle`, `Sleep`, `LoadLibrary` |
| USER32 | 43 | `GetClientRect`, `DispatchMessageW`, `SendMessageW`, `PostMessageW`, `DefWindowProcW`, `PeekMessageA` |
| WS2_32 | 20 | The ordinals not in the list above, including `recv` (#16) and `recvfrom` (#17) |
| AVIFIL32 | 14 | AVI read and write |
| GDI32 | 8 | Fonts and a memory DC. `GetStockObject` is already handled |
| SHELL32 | 6 | `ShellExecuteA`, drag-drop, the tray icon |
| WINHTTP | 5 | Proxy discovery. No request is sent |
| ole32 | 5 | `CoCreateInstance`, structured storage |
| MSVFW32 | 4 | `ICOpen` / `ICDecompress` |
| SHLWAPI | 4 | `PathStripPathW` and three neighbours. The six path handlers above exist |
| OLEAUT32 | 3 | Three ordinals |
| AVICAP32 | 2 | `capCreateCaptureWindowA` |
| IPHLPAPI | 2 | `GetAdaptersAddresses`, `GetIfTable` |
| DINPUT8 | 1 | `DirectInput8Create` is handled: one `SEGA IO Controller` with four axes. An unknown method on the device still fails closed |
| DSOUND | 1 | Ordinal 11, `DirectSoundCreate8`, is a silent device. ADX is not decoded |
| DWrite | 1 | `DWriteCreateFactory` |
| ADVAPI32 | 1 | `GetUserNameA` |
| NETAPI32 | 1 | `Netbios` |
| WINMM | 1 | `timeSetEvent` |

`XINPUT9_1_0.dll` has one import, `XInputGetState`, and it is handled.

## Work

### 1. Files

Content is about 5.2 GB for 2019, addressed by path from the game folder.
`nrt/dx11/` is NRFC (magic `NRFC`, version 20150701): `car/` and `course/`,
with course `.nds` files of 370–400 MB that are streamed. Sprites are FARC.
Nothing is one archive index, so a path map is enough.

`CreateFileA`, `CreateFileW`, `ReadFile`, `SetFilePointer`, `GetFileSize`,
`GetFileAttributesExW`, `FindFirstFile` / `FindNextFile`, and `CloseHandle`
read that tree. A missing file is a failed open, not a throw. Writes
(`WriteFile`, `WritePrivateProfileStringA`, `CreateDirectoryA`, `DeleteFile`)
go to the container copy of `GameData/`. The player's folder is not modified.

`LoadLibrary` returns 0. The game then uses the imports it already has.

NRT containers, FARC archives and loose DDS now open from the IO and loader
threads. Phase 3 still has no course or car to draw; boot has not yet reached the
course load.

### 2. The message queue and the pad

`PeekMessageW` is called at `0x41ECF6` with the filter open and `PM_REMOVE`.
It has to return a message when one is posted, and 0 when the queue is empty.
`0x41ECFA` skips the rest on 0. A message of `0x12` (`WM_QUIT`) at `0x41ED00`
leaves the loop. `GetClientRect` returns the 1920×1080 client the swap chain
described. `DispatchMessageW` calls the procedure `SetWindowLongW` installed
(`0x6A0740`). `PostQuitMessage` is the way a run ends. Until something posts
it, the loop keeps spinning, which is the Phase 2 stop.

`DirectInput8Create` and `XInputGetState` read one `PadSnapshot`: wheel, gas,
brake, view, gear, start, test, service, coin. No pad still returns
`ERROR_DEVICE_NOT_CONNECTED` from XInput, so the current init path stays valid.
Force feedback is computed by `soc::ForceFeedbackImpl` and was sent to a
parallel-port motor. The snapshot records the request. Core Haptics and wheel
motors are Phase 7 and Phase 8.

Autoplay (`AutoPlaySetting.ini`, or a scripted snapshot) is what Phase 5 drives.
The handler has to accept that source as well as a live pad.

### 3. The shell service

The cabinet split is `Shell.exe` on one side and the game on the other. The
game sends window messages. Each known message gets a native answer. An unknown
message fails closed.

- Security. `GAME_MSG_SECURITY_CHALLENGE` is answered authenticated. Any other
  security call fails closed. There is no dongle and no I/O-board handshake.
- Credits. `SHELL_MSG_SETCREDIT` and the free-play default. A coin button posts
  the same message.
- Test and service. `test_mode::` is 34 classes. The test button reaches that
  menu. Bookkeeping is stored in the container.
- Settings. `config.ini`, `GameSettings.ini`, `HighScoreData.xml`, and the
  shell variable files are read-write copies in the container. The profile
  reads that already return the caller's default keep doing so when the copy
  has no key.

`SendMessageW` and `PostMessageW` are how these arrive. They share the queue
from item 2.

### 4. Audio

DirectSound is ordinal 11, `DirectSoundCreate8`, under CRI ADX 10.71c and CRI
CSB. Sound is `rom/sound/`: 58 ADX streams and `SE.csb` (a `@UTF` table, 15.8
MB). WinMM `timeSetEvent` is the mixer tick.

That plan (native CRI entry points) turned out not to be needed: the
translated CRI runs correctly, so emulating DirectSound faithfully is enough.

**Landed.** The game makes three secondary buffers, all 48 kHz 16-bit:

- A 48 KB 5.1 stream (`WAVE_FORMAT_EXTENSIBLE`, mask `0x3F`). The game's
  stream object (`0x59E660`) calls CRI Atom's mixer (`0xBC4CA0`, which
  decodes `SE.csb` cues) through a callback, converts its floats to int16,
  and keeps the ring fed from `GetCurrentPosition`.
- A 128 KB stereo buffer: CRI ADX's own output, music and the rest of the ADX
  streams, played at 44.1 kHz (`SetFrequency`) with `SetVolume(-380)`.
- A 64 KB mono buffer CRI starts and leaves silent.

`src/audio/ds_mixer` gives each buffer guest memory (`Lock` hands out the
range, split where it wraps), a play cursor that advances as the output takes
samples (the write cursor 10 ms ahead), looping and one-shot play, volume, pan
and frequency (linear resampling), and folds 5.1 to stereo. The fold-down is
normalised so a full-scale mix cannot clip. A windowed `dcrun` plays through
Core Audio (`src/platform/macos/core_audio_output.mm`). Headless runs and
`DCUSA_AUDIO=0` use a device-less output paced in real time (`src/audio/null_output`);
`DCUSA_AUDIO_WAV=<file>` records it. `DCUSA_AUDIO_STATS=1` prints each buffer's
level every 10 s; `DCUSA_AUDIO_DUMP=<dir>` writes their memory; and
`DCUSA_DSOUND_TRACE=1` logs the DirectSound calls.

Checks:

- **Attract music.** The ADX buffer, resampled to 44.1 kHz, correlates 0.92
  with `d1a_bgm08_adv.adx` decoded on its own, and matches it within 0.3 dB.
  CRI mixes about 3.8 dB hot (the source touches full scale) and then sets the
  buffer to -3.8 dB.
- **Race.** Before the fix, the 5.1 stream was all -32768: CRI Atom's ADX
  decoder (`0xBFF940`) overflowed to NaN. Its prediction coefficients come from
  `0xC00050` (`√2 − cos(…)` and a square root), and the CRT's sqrt returned its
  argument because of returned flags (PHASE2: `find_flag_returning`). Now the
  stream is around -14 dBFS RMS in a race, and the mixed output clips on 0.005%
  of samples.
- **CRT maths.** `DCUSA_SELFTEST_MATH=1` runs the translated cos, sin, tan,
  log, sqrt and pow at the first present against the host's: all exact.

`timeSetEvent` still drives CRI's server thread as before.

### 5. Video

Movies are `rom/movie/`: 24 Sofdec `.sfd` files (MPEG program stream, 481 MB)
and 2 AVI files (173 MB).

**Landed: Sofdec plays as translated code.** At track select, CRI's thread opens
`d1a_new_bgn_1.sfd` and the next one (the course fly-overs behind the course
cards), decodes them, and uploads the frames to a texture the menu draws. A
scripted run shows the background moving from frame to frame: infield, then pit
lane. The image is clean at full size, with no block errors and the right
colours. The 24 files are four fly-overs for each of the six course and
difficulty sets (`bgn` / `int` / `exp`, old and `new`).

**The AVIs are camera stand-ins.** `screen demo.avi` (432×240, Microsoft
Video 1) and `cmn_rslt_name_entry_chara.avi` (1644×196) are opened only by the
OpenCV Video-for-Windows capture code (`0xA56B20`, `0xA56FB0`; thunks at
`0xA7ED00`–`0xA7ED44`) in `OpenCVMgrImpl`, the cabinet camera manager, which
runs only with capture enabled (`m_bEnableCapture`). It is off by default, as on
the cabinet the capture came from, whose "YOU" box is black. `AVIFIL32` and
`MSVFW32` stay fail-closed. The camera feature is out of scope (PHASE0).

Correction: attract also opens `screen demo.avi`, after its first full loop
(about present 40,000 at 60 Hz). OpenCV first asks for
`opencv_ffmpeg2410.dll`, which the install does not ship: `LoadLibraryA` now
returns NULL with `ERROR_MOD_NOT_FOUND` for a DLL that is not there (one that
is there still fails closed). OpenCV then falls back to Video for Windows:
`AVIFileInit` succeeds and `AVIFileOpenA` fails with `AVIERR_FILEOPEN`, so the
video is not shown and the game carries on. Before this, every run stopped
there. Decoding the AVI (Microsoft Video 1) is still not done; the other
`AVIFIL32` entries stay fail-closed.

### 6. Left fail-closed on purpose

- Networking. `connect` already returns success and `select` already times out.
  The remaining Winsock ordinals, `WINHTTP`, `IPHLPAPI`, and `Netbios` stay
  fail-closed unless a call site on the standalone path requires a specific
  success value. Standalone is `NETWORK_TYPE=2`. Linked cabinets, rank, and
  the satellite modes are not ported.
- The camera. `capCreateCaptureWindowA` fails closed. OpenCV is not run. The
  face billboard has no camera.
- `CreateProcessA`, `ShellExecuteA`, the tray icon, and drag-drop. The game
  does not spawn a process.
- `LoadLibrary` of a DLL that is not the ones already imported.

### 7. EEPROM

The i960 side has `_Check_EEPROM` and `EEPROM_to_DRAM`. Backup RAM is a file in
the container, loaded into the guest buffer the Model 2 program reads. It is
written back when the game writes it. The player's ROM images are not modified.

## Suggested order

1. File reads, then container writes. **Done** for the handlers the run uses
   (including the file-time stubs and `OVERLAPPED` reads, which complete in the
   call and set their event).
2. The message queue, `GetClientRect`, and quit. Then the pad snapshot, with
   no pad still meaning "not connected". **Done.**
3. The shell messages: security answer, free play, test, service. **Partly.**
   Freeplay, the `0x2C` reply and the security challenge (`0x2A` → `0x2E`) are
   answered; credits, test and service as a full service are not.
4. ADX and CSB at the CRI boundary, and `timeSetEvent` as a host tick.
   **Partly.** `timeSetEvent` pulses its event on the host clock and the CRI
   threads run; ADX and CSB are not decoded.
5. Sofdec, then the two AVI files. **Not done.**
6. EEPROM for the Model 2 backup. **Not done.**

Camera, HTTP, NetBIOS, and linked play stay fail-closed.

## Out of this phase

- Pixels. Phase 3.
- A controller UI and haptics. Phases 7 and 8. A window landed early: `dcrun`
  opens one (`src/platform/macos/game_window.mm`, a `CAMetalLayer` the
  Metal device draws each frame into with `gpu::show`; AppKit events are
  drained once per frame from Present, since the game keeps the main thread).
- Opening a socket.

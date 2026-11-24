# Working on this repository

A static recompilation of Daytona Championship USA (2017 and 2019 PC builds)
to native arm64 macOS. Start with [README.md](README.md), then the phase
documents ([PHASE0.md](PHASE0.md) for the plan, PHASE2–4 for the translator,
graphics and services).

## Rules

- Never hand-edit generated code (`build/x86tr/*/gen`). Fixes go in the
  translator (`tools/x86tr`, `SITE_OVERRIDES` / `OVERRIDES`) or in
  `src/recomp`. A temporary probe must reproduce the flags the next
  instruction reads.
- Nothing from the game folder is committed (shader source, frames, audio).
- Unattended runs: `dcrun --headless` (or `DCUSA_WINDOW=0`) and `DCUSA_PAD=0`,
  so no window opens and a connected controller does not drive the game.
  `DCUSA_AUDIO_WAV=<file>` records the sound instead of playing it.

## Work queue

Work through these in order without waiting for input. When an item is
verified fixed (checked in a run, not only built), mark it **verified** here
with the evidence. Commit and push to `main` after each stage (the user
agreed to this).

1. **Audio output.** DirectSound buffers emulated and mixed to Core Audio
   (`src/audio/ds_mixer`, `src/platform/macos/core_audio_output.mm`).
   **Verified** (see below).
2. **ADX music and voices.** Played by the translated CRI ADX library into its
   stereo buffer; native decoding was not needed. **Verified** for music (see
   below). Voice lines in a race come through CRI Atom (item 3).
3. **CSB sound effects.** `SE.csb` cues play through the translated CRI Atom
   mixer into the game's 5.1 stream. **Verified** (see below).
4. **Movies.** **Verified** (see below). Sofdec plays through the translated
   CRI code. Attract opens `screen demo.avi` through OpenCV after its first
   loop; the open fails cleanly and the video is not shown (PHASE4, "Video").
   Decoding it is not done.
5. **Frame rate.** Mostly done; the target is now iOS and Vision Pro, so
   lower is still better. Metal encodes on a render thread, the translator
   inlines small functions and the cookie check, clears fold into the next
   pass, and waiting guest threads are parked instead of woken at every
   preemption (PHASE3, "Frame-rate profile"). Unthrottled, race frames are
   about 10.1 ms mean and the heaviest scenes (2,300+ draws) about 15 ms,
   within 60 Hz. Those are CPU-bound: the engine's translated per-draw code
   (82% of the main thread, about 6 us a draw, no single hot spot). PGO was
   tried and gained nothing measurable. A heavy frame is about 7 ms of GPU
   time on this Mac (`DCUSA_GPU_PASSES`). Code layout and size (order file,
   `-Os`) and inlining the whole draw path were also neutral: the remaining
   cost is the engine's memory work. The scene-change stalls (100 to 380 ms
   frames, `DCUSA_HITCH`) all fall on static loading screens (white fade,
   CRIWARE logo, "Data Loading"), so they do not show as stutter.
6. **Cabinet settings.** **Verified** for credits (see below). Operator
   settings go through `dcrun --shell-setting Section.Key=Value`, since the
   operator menu is the shell's, and in this release the game has no input path
   for Test / Service (PHASE4 "Operator settings").
7. **Phase 5.** **Verified** (see below). CTest `phase5_attract` and
   `phase5_controls` (label `phase5`; [PHASE5.md](PHASE5.md)).

8. **iOS and iPadOS.** **Verified in the Simulator and on an iPhone 15 Pro
   Max** (see below). Still to do:
   - **iPad mini 6 (4 GB):** killed for memory 15 to 30 s into boot (limit
     about 2.8 GB). Its A15 has no BC textures, so they are decoded to plain
     pixels, 4 to 8 times larger: 1.7 GB after 400 of about 2,300 textures.
     A 512 cap, no MSAA and a 1280x720 display did not get it past 2.4 GB at
     10 s. Next: transcode BC to ASTC (same size as BC7/BC3, sampled by A15),
     best done on the Mac ahead of time and shipped beside the shaders.
   - **Speed:** races run at about 20.6 ms a frame on the phone (25 before;
     PHASE3 "Frame rate on an iPhone"). The limit is the game's main thread,
     about 16 ms of its own code a race frame, plus the render thread waiting
     on the GPU. Next: the main thread's object traversal (0x4423B0, 14%),
     the D3D layer's per-draw work (12%), the GPU's 6.5 ms of back-pressure.
   - **Touch controls.**
   - **Unexplained GPU memory:** about 0.9 GB of Metal driver memory beyond
     its allocations (render passes hold transient memory while the game
     renders; PHASE3 "Memory").

## Future work

- **Boot "Data Loading" time.** Boot takes about 7,500 presents (about two
  minutes at the 60 Hz pacing) before attract, plus a single 1.0 to 1.3 s
  frame near present 6,000. Find out why, and whether it can be cached.
  Leads: the boot drains its resource queue on the main thread once per frame
  (threads.cpp `sched_frame_yield`), so pacing may be what limits it
  (`DCUSA_NOPACE=1` boots faster); `DCUSA_HITCH` and `DCUSA_IO=1` show where
  the time goes. Caching ideas, once the cost is known: skip the pacing
  while the loading screen is up, keep decompressed or decoded data on disk
  between runs, or snapshot guest memory after boot.
- **Service / test mode.** On the cabinet the Test and Service buttons open
  the operator menu of `Shell.exe` (driven by `shell/GameTest*.ini` and
  `GameAdvancedTest*.ini`), a separate program that does not run here. The
  game's own path is closed in this release: no input profile maps actions
  0 / 1 or SW1 / SW2, the keyboard source `0x47CB80` returns false, and no
  shell message sets them (PHASE4 "Operator settings"). Today settings go
  through `dcrun --shell-setting Section.Key=Value`, which edits
  `ShellData.ini`. Options: rebuild the shell's menu natively from those
  .ini files (a host-side menu that writes `ShellData.ini`), translate and
  run `Shell.exe` itself, or look for test-mode classes in the game that a
  host-side switch could still reach.

## Known issues

- With `DCUSA_METAL=0` (the 2D software reference instead of Metal), a run
  stops at about present 120 on an `int3` at `0x6D14B8`, the end of
  `0x6D1460`. The Metal path, the default, is unaffected. Not investigated.

- GPU memory reached 4.3 GB (5,455 Metal allocations) in a windowed session
  after several races. Partly explained since: upload arenas pinned by a few
  long-lived buffers grew without bound (fixed, compact_parked), and the first
  bytes of every buffer took a Metal buffer of their own (35,000 of them;
  fixed). Not remeasured across several races.

## Verified fixed

- **Audio output** (item 1). A headless run records the mix (`DCUSA_AUDIO_WAV`).
  Attract has sound from the moment attract starts. A scripted race
  (`DCUSA_PAD_SCRIPT`) has the 5.1 effects stream at about -14 dBFS RMS, with
  0.005% of samples clipped. A windowed run starts Core Audio without error.
  The race fix was a translator bug: flags returned across `ret`
  (`find_flag_returning`). It made the CRT's sqrt return its argument, which
  wrecked CRI's ADX coefficients and drove the decoder to NaN. PHASE4 "Audio"
  and PHASE2 have the details.
- **ADX music** (item 2). The attract music buffer, set against the decoded
  `d1a_bgm08_adv.adx`: correlation 0.92, level within 0.3 dB.
- **CSB sound effects** (item 3). Race levels about -14 dBFS RMS, no NaN,
  0.005% clipped; the user listened to the recorded race clips and they
  sounded right.
- **Movies** (item 4). In a scripted run to track select, the game opens
  `d1a_new_bgn_1.sfd` and `_2.sfd`. The fly-over plays behind the course cards
  (the background changes between frames), and a full-size frame is clean.
  PHASE4 "Video" has the details.
- **Credits** (item 6). `--shell-setting Credit.Freeplay=0`: attract shows
  INSERT CREDIT(S) / CREDIT(S) 0, Start is ignored without credit, a coin
  gives CREDIT(S) 1, and Start opens Single Player.
- **Phase 5** (item 7). `ctest -L phase5`. `phase5_attract` passes on the
  current build (95th-percentile colour distance 0.451, medians within
  tolerance) and fails on the magenta-bloom build (0.808, 32 magenta frames).
  `phase5_controls` passes all 15 checks.
- **Cube maps** (car reflections). Cars flashed from frame to frame because
  cube maps were created as 2D textures. Consecutive headless frames now keep
  the cars' shading, Phase 5 passes (colour p95 0.432), and the user
  confirmed in the window that the flashing is gone. PHASE3 "Cube maps".
- **Image quality** (mips, trees, anti-aliasing). Textures keep every mip
  level the game uploads (road sheen and distant shimmer), foliage uses alpha
  to coverage, and 4x MSAA is native and on by default (`DCUSA_MSAA`, FXAA via
  `DCUSA_AA` as an option). Phase 5 passes (colour p95 0.413) and the user
  confirmed the road and fence look right in the window. PHASE3 "Cube maps",
  "Anti-aliasing".
- **Name entry and the next race.** The letter strip (an MS Video 1 AVI read
  through Video for Windows) now shows, and the race after name entry starts
  (the guest heap's free list had made the load crawl). The user confirmed
  both in the window. PHASE4 "Video" correction and "Guest heap".
- **iOS Simulator** (item 8). `cmake --preset ios-sim`; with the cache, shaders
  and game folder in the app's Documents, the iPad Pro 13-inch Simulator boots
  through Data Loading to attract and draws the 3D demo with the ranking board.
  The Simulator has no BC textures; they are decoded on upload, and
  `test_bc_decode` matches the Mac GPU (BC1-3, BC6H and BC7 exactly, BC4/5 to the
  nearest value). With the decoder forced on the Mac (`DCUSA_BC_DECODE=1`)
  Phase 5 attract passes (colour p95 0.429, median 0.212); before BC6H was
  decoded it failed (saturation 0.071 over).
- **iPhone 15 Pro Max** (item 8). The `ios-device` build boots to attract and
  races (`DCUSA_PAD_SCRIPT=race.txt` from Documents) at about 4.6 GB
  footprint, where it was killed at 5.4 GB before the memory work. A race frame
  matches the Mac's: shadows, the fence, the course map. PHASE3 "Memory" and
  "Shadow maps on iOS".
- **Dynamic resolution and timestamps.** The game's GPU timestamp queries
  report each frame's command buffer times, so its dynamic resolution works:
  a Mac renders races at 1920 wide (1280 before, with zeros), the iPhone
  settles at 960. Phase 5 attract passes (colour p95 0.428; median luma 102.0
  against the capture's 101.6, saturation 0.336 against 0.336).
- **CRT maths.** `DCUSA_SELFTEST_MATH=1`: the translated cos, sin, tan, log,
  sqrt and pow match the host's exactly.

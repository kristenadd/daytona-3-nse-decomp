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
   CRI code. The two AVIs belong to the camera feature (OpenCV capture, off
   by default) and stay fail-closed.
5. **Frame rate.** Mostly done; two cases left. Metal encoding now runs on
   a render thread and the translator emits faster code. The unthrottled race
   frame went from 15.0 to 9.7 ms mean (PHASE3, "Frame-rate profile"). Paced,
   370 of 383 one-second race intervals hold 58 to 60 fps. Left: the heaviest
   scenes (2,300+ draws) are GPU-bound at 15 to 16 ms (54 to 57 fps), and
   scene changes stall for a second at 21 to 28 ms with the GPU idle.
6. **Cabinet settings.** **Verified** for credits (see below). Operator
   settings go through `dcrun --shell-setting Section.Key=Value`, since the
   operator menu is the shell's, and in this release the game has no input path
   for Test / Service (PHASE4 "Operator settings").
7. **Phase 5.** **Verified** (see below). CTest `phase5_attract` and
   `phase5_controls` (label `phase5`; [PHASE5.md](PHASE5.md)).

## Known issues

- With `DCUSA_METAL=0` (the 2D software reference instead of Metal), a run
  stops at about present 120 on an `int3` at `0x6D14B8`, the end of
  `0x6D1460`. The Metal path, the default, is unaffected. Not investigated.

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
- **CRT maths.** `DCUSA_SELFTEST_MATH=1`: the translated cos, sin, tan, log,
  sqrt and pow match the host's exactly.

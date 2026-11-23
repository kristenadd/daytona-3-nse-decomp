# Phase 5 — headless runner and comparison against Windows

Phase 5 from [PHASE0.md](PHASE0.md): run the game headless, autoplay it, and
check the result against the Windows original. What exists now is two CTest
tests under the label `phase5`. Each takes minutes, so routine runs leave them
out with `ctest -LE phase5`; `ctest -L phase5` runs them.

## phase5_attract

`tools/phase5/attract_compare.py` runs `dcrun --headless` for 690 s (boot plus
a full attract loop) and writes a frame every 120 presents (2 s at 60 Hz). It
then compares the frames with a Windows capture of the attract loop
(`DCUSA_REFERENCE_VIDEO`, a 9-minute 1918×1078 H.264 recording made under
TeknoParrot), sampled every 2 s. Boot frames (black, text on black) are left
out on both sides.

The two loops are not frame-synchronous, so each of our frames is matched to
the most similar reference frame, using 32×18 thumbnails:

- **Colour.** A 48-bin RGB histogram; L1 distance of the normalised channel
  histograms, averaged (0 to 2). It picks the five candidates, and it is the
  measure for 3D scenes, whose camera differs from run to run.
- **Picture.** Mean absolute difference against the closest of those
  candidates. Reported, not judged: the cameras differ.
- **Sanity.** No frame after attract starts is black or has a magenta region
  (the NaN bloom of PHASE3).
- **Overall.** Median luma and saturation of all attract frames, ours against
  the capture's.

| Check | Tolerance | Current build (CTest run) | Magenta-bloom build |
|---|---|---|---|
| 95th-percentile colour distance | 0.60 | 0.451 | 0.808 (fails) |
| Worst colour distance | 0.90 | 0.669 | 0.889 |
| Median colour distance | 0.30 | 0.225 | 0.246 |
| Median luma | ±12 | 99.8 vs 101.6 | 96.3 |
| Median saturation | ±0.06 | 0.370 vs 0.336 | 0.386 |
| Attract frames | at least 150 | 274 | 223 |
| Magenta frames | none | 0 | 32 (fails) |

The single worst frame is not the main measure (its limit went from 0.80 to
0.90 when a run caught a close-up along a car's side at 0.86, correctly
coloured). A few camera shots, such as a
wheel filling the frame, have no counterpart in a capture sampled every 2 s,
and they score about 0.6 to 0.7 with correct colours. The 95th percentile
separates the builds: 0.45 for the matching build, 0.81 for the magenta one.

`report.json` holds every frame's numbers, and `worst.png` sets the six worst
frames beside their reference matches.

## phase5_controls

`tools/phase5/controls_smoke.py` plays a pad script (`runtime/pad_script.hpp`)
from attract into a race at Daytona: coin, Start, the gas through the menus
(MT picked with a shift down), then every mapped control. `DCUSA_PAD_TRACE`
prints the game's own decode. The test checks:

- the coin adds a credit, and Start sets action 2;
- gas 0.5 and 1.0, and brake 0.6, decode proportionally;
- full steering lock reads 0 / 0xFF00, and -0.4 lies between;
- views 1 to 4 set actions 3 to 6;
- five shift-ups from first stop at fourth, and five downs stop at first, one
  gear per press;
- the run has no fail-closed stop.

All 15 checks pass.

## Not done

PHASE0's oracle (guest-RAM and i960 hashes per frame, against Windows traces)
needs traces from a Windows run, and there are none yet. A full race to the
finish line is not scripted; the controls test drives for about 30 s of a
race.

# Phase 3 — graphics

Phase 3 from [PHASE0.md](PHASE0.md): a D3D11 device that draws, a Metal backend, and
shaders translated from the player's own HLSL.

**Exit criterion:** attract-mode frames match Windows captures within tolerance.

The tolerance and the capture set belong to Phase 5. This phase produces the frames.
Nothing from the game folder is committed. Shader source stays in the player's EXE and
is translated on the machine that prepares the cache.

Phase 2 left a device that answers every call the first frame makes. Buffers and
textures keep the bytes `CreateBuffer`, `CreateTexture2D`, `UpdateSubresource`,
and `Map` / `Unmap` move. `ClearRenderTargetView` fills a stored target from
the four floats. `DrawIndexed` fills a triangle into the bound render target
when the vertices are in screen pixels or in NDC, and samples a bound pixel-shader
texture when the layout has a texcoord. RGBA8, BGRA8, BC1, and BC3 are sampled
as color. R16G16 float, RGBA16 float, R16 float, and R32 float are sampled as
grayscale or two channels. An untextured triangle larger than 64 pixels is left
alone: those are the 3D scene, and they are not in screen space until a vertex
shader runs. `Present` copies that target. `DCUSA_FRAME` writes present 1 and
present 30 as PPM files next to the path it names. `DCUSA_STATS` prints the
draw counts.

`D3DCompile` reads `Texture2D` and `SamplerState` names out of the HLSL and
ignores `#define` lines, so `#define Texture2D sampler2D` is not a binding.
`D3DReflect` reports those names. `GetOutputParameterDesc` returns a failure
HRESULT so the output-signature loop stops, and constant buffers are not
reported, so the variable walk that follows a cbuffer is not entered. The
first drawn shader's only texture is `g_Texture` at register t0. The draws
before the first present are fullscreen quads in NDC, blitting one target
into the next, and that chain starts from a 512×512 R32 texture that is still
all zeros. A 256×256 RGBA upload does land: it is a font atlas, and no draw
in the first 30 presents binds it. One mesh is submitted in that window, and
30 by present 30. Its shader's texture is `g_VelocityMap`. The vertices are a
sphere of radius one million. A perspective matrix unmapped into a 272-byte
constant buffer (x scale about 1.79, y scale about 3.19, the 16:9 projection)
is applied to those positions, and they land in NDC around the center of the
view. The pass binds no vertex constant buffer of its own, and the texture it
samples is empty, so the triangles take their vertex color. The next draw
is a fullscreen quad that samples that target into the following one. Draw 20
of that frame is the fullscreen blit into the back buffer. Its pixel shader
is the glare composite: it samples `g_Texture`, `g_LightShaft` and three glare
maps. `g_Texture` is a 1920×1080 RGBA16F target that is still all zeros, and
so are the other full-size inputs. The only mesh that paints is the velocity
sphere, 128 triangles into a target that composite does not sample. No draw
in the first 30 presents binds an RGBA texture, so the font atlas is loaded
and never sampled. Clears and triangle writes of float targets store
half-floats and R32 floats.

The passes that do run are Shadow, PreScene, Main, PostProcess, Motion Blur,
Glare, and Gamma. EnvCube, Reflect, DOF, and Radiation Blur do not. Main is
entered and still submits no course mesh.

The load stall is past. It was not one missing call: the NRT IO thread
(`f_00781BB0`), the resource worker (`f_0075C930`), and the CRI file, sound and
stream workers never ran, and earlier runs drained their queues with hand edits
inside the generated files. Guest threads now run cooperatively (PHASE2 item 12),
and those edits are gone. With the threads running, a translator bug showed up:
flags were treated as dead when control passed into the next discovered piece of
a function. zlib's `inflate` is split into 109 pieces, and every `FArC`
(gzip) archive failed with "invalid bit length repeat". The real `aet.bin` offsets
were small, but the relocated pointers were garbage. With flags kept live across
those exits, the guest's own zlib decodes stored, fixed, dynamic, Huffman-only and
RLE gzip streams byte for byte.

A 90 s content run now starts 14 guest threads and opens 243 files with no
fail-closed stop: `GameData/.shaders/shader_cache.bin`, `NRTBox.dx11.nrfc`, the
pit-crew and crowd robots, `ND_M_Bgc_O_kb_eye_down`, the `rom/auth2d` and
`rom/sprite` FARCs, minimap, lightprobe, network, 166 sidebar DDS and the
wait-scene DDS. Textures created reach 292 by present 300. `ReadFile` with an
`OVERLAPPED` completes in the call and sets its event. Other fixes on the way:
`GetModuleFileName` returns `D:\Daytona\Daytona.exe`, so "exe folder + `\nrt\...`"
stays inside the game folder. `VirtualAlloc` continues at `0x40000000`, and the heap
continues at `0x80000000`; the boot asks for single 53 MB blocks, and running out of
heap was a clean shutdown, not a crash. The `IDirectSoundBuffer` slots now follow
`dsound.h` (Lock `+0x2C`, Play `+0x30`, GetStatus `+0x24`, Stop `+0x48`, Unlock
`+0x4C`); before, only `+0x10` was right.

The D3D11 objects (vtables, singletons, the object pool) used to sit at
`0x25100000`, 17 MB into the process heap. Once the boot filled the heap, heap
blocks and D3D objects overwrote each other, and every `D3DCompile` rewrote the
vtables over live heap data. They are at `0x100000`–`0x3F0000` now, below the
image, with the reflection name pool.

The presented image is still black. The glare composite's inputs are still empty,
because no course or car is drawn yet. The run is slow: about 1.7 presents a second,
and half the CPU is the 2D reference device painting full-screen float targets
(`draw_indexed` / `store_color`). Boot state `[0x1735608]`, the five subsystem
states, and the 778-entry resource manager have not been re-read since the threads
started running.

`dcrun --content <folder>` mounts that install and stays in the frame loop.
`DCUSA_STATS=1 DCUSA_EVERY=100` prints draw counts every 100 presents.
`DCUSA_FRAME=<path>` writes present 1, 30, and every `DCUSA_EVERY`th as PPM.
`DCUSA_IO=1` logs every open and read with the thread that did it.
**Attract mode renders.** With `DCUSA_METAL=1`, `DCUSA_SHADERS` and
`DCUSA_TRANSLATE=1`, a run boots in about 8,000 presents (70 s on an M3 Max):
boot state `[0x1735608]` goes 0 → 5 → 8, the five subsystems reach state 2, net
status `[0x15FF5DC]+0x74` becomes 1, and the resource queue drains to 0. From
there the game plays its attract loop: a race with cars, track, trees and sky
behind the FREE PLAY / PRESS START BUTTON overlay, and the "NEW LIVE CAMERA
SYSTEM" card with driver portraits. A frame is about 1,300 draws (436 depth-only,
403 scene, 354 blur, shadows, the post chain); 397 shaders are translated, 185
pipelines built, nothing skipped. Colours are not yet checked against Windows.
What it took, in order of discovery:

- Preemption at loop back-edges (PHASE2 item 12). The boot's resource walk
  (`f_004FDF60` → `f_0051E7F0`) loops without a blocking call; with purely
  cooperative switching the loader threads it waits on never ran.
- The guest's file table held 256 handles; the boot keeps several hundred
  containers open, and after that every open failed with
  `ERROR_TOO_MANY_OPEN_FILES`. A failed read leaves a resource in state 3, and
  the walk spins forever on such an entry (the "state 3" path does not advance
  its iterator). The table is 65,536 now.
- `UpdateSubresource` honours its box and only keeps subresource 0;
  `VirtualFree(MEM_RELEASE)` returns regions for reuse (53-64 MB streaming
  buffers come and go).
- D3D11 objects are reference counted and released objects' slots reused; the
  pool moved to `0xE0000000` with room for a million (the boot keeps over
  12,000 buffers alive).
- The player's `GameData/.shaders/shader_cache.bin` is hidden: with it the game
  creates shaders from Microsoft DXBC instead of compiling its HLSL. Our
  `D3DCompile` bytecode is `DXBC`, `DCUS`, then the request key, so the cache
  the game writes into the container still finds the translation.
- Shaders a new scene compiles are translated during the run
  (`DCUSA_TRANSLATE=1` runs `translate_shaders.py --only <key>`).
- Constant buffers keep their explicit registers (`CB_PER_SCENE` is `b9`, bound
  once per frame by the game at that number); only unpinned resources are
  numbered by the lowest free slot. Without this every 3D draw read zeros for
  its scene constants and the frame was white.
- Reflection reports array lengths and matrix orientation
  (`D3D_SVC_MATRIX_COLUMNS` for HLSL's default column-major).
- Draws with no pixel shader (depth-only shadow passes) get a pipeline with no
  fragment function.
- The shell's security challenge is answered (PHASE4): without it the game
  asked for a reboot 50 s in.

**Long runs and frame rate.** Attract used to slow to a crawl after about
12,500 presents and stop at 14,000. Causes, in order:

- The NRT heap ran out of address space. Failed 16 MB and 1.3 MB allocations
  (`nrt_texture.cpp`, `resource_creator.cpp`) were retried forever by a worker
  thread, each writing to `debug.txt` (about 30,000 opens a second).
  `VirtualAlloc` now uses `0x40000000`–`0xB0000000` and the third heap region is
  `0xB0000000`–`0xE0000000`. Path lookups below the install root are cached.
- The CRT's x87 math descriptors (`fmod` at `0x13DE170`, `atan2` at `0x13F3DD0`:
  a name, then sixteen handlers at `+0x10` that `0xC44D50` / `0xC44DCB` jump
  through) are discovery roots now (`CRT_DISPATCH_TABLES`). Nothing else points at
  the handlers (`0xC450D4`, `0xC4510F`, the `fprem` loop `0xC1E804`, `0xC50388`),
  so none of them were translated. `0xC5030C` stays native.
- `fnsave` / `frstor` / `fnstenv` / `fldenv` are translated (`fpu_save` /
  `fpu_restore`): the CRT x87 exception helper (`0xC3946D`) runs in attract.
- The Metal backend: the pipeline cache key was built with `ostringstream` on
  every draw (about 1,200 a frame in the race), and every buffer update made a
  new `MTLBuffer`. The key is raw bytes now. Repeat buffer updates go into 8 MB
  shared upload arenas, which are reused once the GPU is done with them and no
  buffer's current bytes live there (a buffer's first upload gets its own
  `MTLBuffer`). A constant buffer shorter than the function reads gets a
  zero-padded copy, as D3D11 reads zeros past the end.
- `Present` read the frame back and waited for the GPU every frame. It now
  commits without waiting (at most two frames queued) and reads back only the
  frames `DCUSA_FRAME` writes (`DCUSA_READBACK=1` for every frame). A shared
  texture the GPU may still read gets a new Metal texture before a CPU write.
- The game presents with SyncInterval 0, but the cabinet runs at 60 Hz, so
  presents are paced to 60 Hz (`DCUSA_NOPACE=1` to run unthrottled).

**Colours against a Windows capture.** A 9-minute attract recording from the
cabinet (TeknoParrot, 1918×1078, 30 fps, H.264) was sampled every 2 s and set
against our attract loop at the same cadence (every 120 presents). The loops
run the same sequence: Lakeside by day, the speedway, Lakeside at dusk, Metro
City, the canyon, with ranking and "new live camera" cards and the title in
between. Two bugs turned the dusk sky and its glare magenta:

- Bloom and glare render each mip level of a texture while reading the level
  above. Views ignored their mip range: render targets had one level, every
  render target view wrote level 0, and every shader resource view read the
  whole texture. Reading and writing the same level gave NaN in red and blue.
  Render targets now keep their mip chain. RTVs write their `MipSlice`
  (`rtv_mip`), and SRVs sample a Metal texture view over `MostDetailedMip` /
  `MipLevels`.
- D3D11 `min` / `max` are IEEE minNum / maxNum: a NaN operand gives the other
  one. The lit shaders end in `min(colour, 65500)`, so a stray NaN on Windows
  becomes a bright value the tonemap absorbs. spirv-cross writes `fast::min` /
  `fast::max` / `fast::clamp`, whose NaN result is undefined. These are now
  `d3d_min` / `d3d_max` / `d3d_clamp` (`precise::fmin` / `fmax`; done in
  `translate_shaders.py`), and the runtime compiles with `MTLMathModeRelaxed`,
  which keeps NaN semantics.

`DCUSA_METAL_SCAN` reports NaN texels per channel and their bounding box; that
is how the mip feedback was found. After both fixes, none of 313 attract frames
has a magenta region, and no render target holds a NaN. Matched scenes, mean
over the frames (0..255; p10 / p90 are luma percentiles):

| Scene | Luma ref / ours | p10 ref / ours | p90 ref / ours | Saturation ref / ours |
|---|---|---|---|---|
| Title (2D) | 202 / 205 | 50 / 64 | 253 / 255 | 0.19 / 0.18 |
| Ranking card | 98 / 97 | 42 / 41 | 190 / 187 | 0.51 / 0.54 |
| Lakeside day | 91 / 83 | 27 / 24 | 176 / 169 | 0.40 / 0.44 |
| Lakeside dusk | 66 / 70 | 14 / 25 | 130 / 119 | 0.56 / 0.59 |
| Metro City | 100 / 108 | 41 / 57 | 184 / 194 | 0.30 / 0.37 |
| Canyon | 97 / 96 | 21 / 32 | 179 / 180 | 0.58 / 0.59 |

The 2D screens show identical content and match within three levels. The 3D
scenes differ by camera position as much as by colour. Ours has slightly lifted
shadows (p10), which shows on the title too, so it is likely the capture's
encoder. Frame-exact tolerances are Phase 5's.

Differences that are not colour: the capture runs with credits ("INSERT
CREDIT(S)"), ours in free play. The capture's ranking table holds that
machine's saved times. The "YOU" box on the live camera card is black there
(no camera) and shows the default helmet here.

A 10-minute attract run now completes without a fail-closed stop. Boot reaches
attract in about 7,500 presents (125 s at 60 Hz).

**Cube maps.** The Metal backend made every texture 2D, so the game's cube
maps were not cubes: the car reflection map (`e0146ec0`, 256x256, eight
levels, rendered one face per frame through render target views whose
`FirstArraySlice` was dropped) collected all six faces in one slice, the 14
static environment maps (BC6H, seven levels) got only face 0, level 0, and
the 213 shaders that sample `texturecube` (`g_EnvMap`, `g_EnvMapUser`) were
handed a 2D texture. Cars changed shading from frame to frame. Now a texture
with `MISC_TEXTURECUBE` and a multiple of six slices is an `MTLTextureTypeCube`
(or cube array) with all its levels; initial data is uploaded per
subresource (`upload_texture_sub`); a `TEXTURE2DARRAY` render target view
keeps its first slice (view +20, `DrawState::rtv_slice`) for draws and clears;
mip views keep all six faces; and an empty `texturecube` slot gets a 1x1 zero
cube. 2D textures created with initial data now get all their mip levels
too (they used to get level 0 only, so distant fences, signs and markings
shimmered with moire); `UpdateSubresource` of a level other than 0 goes
straight to that level, and a texture replaced while the GPU still reads it
carries its smaller levels over.
`DCUSA_FRAME_RANGE=<a>-<b>` writes every present in a range, and
`DCUSA_METAL_TRACE_FRAMES=<k>` traces k frames, which is how it was found.

**Frame-rate profile.** `DCUSA_STATS` now prints, per report interval, the
wall time per frame, the GPU time per frame (from each command buffer's
`GPUStartTime` / `GPUEndTime`), and draws per frame. The GPU never limits the
frame rate: at most about 12 ms in the heaviest dusk scenes (1,500 to 1,750
draws). The CPU does, and fixing it took four changes:

- Indirect and virtual calls did a binary search over about 107,000 entries,
  15% of the main thread. A 16,384-entry direct-mapped cache now sits in front
  of it (`find_entry_cached`).
- Each draw bound every constant buffer, texture and sampler slot. Now only the
  slots the functions read are bound (the reflection bitmasks), and nothing
  already bound on the encoder is set again (`Device::bound`).
- `publish_frame` copied the 8 MB back buffer every present. It now copies
  only on frames that are read back.
- `Map` called `mprotect` on every map, and copied old contents in for
  `WRITE_DISCARD`. Now the region is committed once and discards skip the copy.

The heaviest dusk seconds of the attract race went from 24 to 29 ms to 19 to
21 ms (35 to 40 fps became 48 to 52). The rest of the loop runs at 60. What
remains on the main thread is guest code itself (about 53%), plus guest threads
that run one at a time: another thread's game work (`0x426890`) waits its turn
instead of running on another core.

A second pass, profiled unthrottled (`DCUSA_NOPACE=1`) in the attract race:
the main thread was busy 93% of the time, the other guest threads together
about 10%, so running them in parallel would gain little. Of the main thread,
translated code was 60%, Metal encoding 19%, indirect-call dispatch 6%. Three
changes:

- The translator inlines `__security_check_cookie` and keeps xmm registers in
  locals (PHASE2 "Native overrides"): about 7%.
- Metal runs on a render thread (`src/gpu/gpu_queue.mm`). The guest's `gpu::`
  calls are recorded with copies of the bytes and draw state they point at,
  and handed over at `submit()`; the render thread replays them through
  `gpu::direct` (metal.mm) in order while the guest runs the next frame. The
  guest stays at most one frame ahead of the encoder. `read_rgba8`,
  `scan_targets` and `stats` wait for the render thread to go idle first.
  `DCUSA_GPU_THREAD=0` runs everything on the guest's thread as before.

Unthrottled, the mean race frame went from 15.0 ms to 9.7 ms, the 90th
percentile from 20.0 to 14.1 ms, the worst from 26 to 19 ms. At the normal
60 Hz pacing, 370 of the 383 one-second intervals of the attract race hold 58
to 60 fps. The 13 that don't are two kinds:

- The heaviest scenes, 2,300 to 2,600 draws, where the GPU now takes 15 to
  16 ms a frame: 54 to 57 fps, GPU-bound.
- Scene changes, 500 to 1,400 draws at 21 to 28 ms with the GPU at 5 to 7 ms:
  one-off stalls while a scene loads (not profiled yet).

Both Phase 5 tests pass with the render thread (attract colour p95 0.471,
median 0.216).

**The heaviest scenes** (2,300 to 2,600 draws) were measured again, with an
eye on iOS and Vision Pro, where the budget is far tighter than the Mac's
60 Hz.

- `DCUSA_GPU_PASSES=d<draws>` times every render and blit pass of a frame with
  at least that many draws (Metal timestamp counters at stage boundaries) and
  prints a table: target size and format, draws, distinct pixel shaders, the
  most used one. `DCUSA_GPU_PASSES=<n>,<n>` picks frames by number, and
  `DCUSA_GPU_PASSES_LOG=1` prints each frame's draw count.
- Timed alone, a heavy frame is about 7 ms of GPU time. The main scene pass
  (978 draws into 1920x1080 RGBA16F) is the largest at about 2.5 ms; the
  bloom chain and the HUD follow. The "15 ms" in the `DCUSA_STATS` line sums
  each command buffer's GPU start-to-end time, which overlaps between
  consecutive frames; it overstates GPU load.
- So the heavy scenes were CPU-bound: the guest's main thread ran 92% busy,
  most of it the engine's draw submission (`0x6CFDB0` and its helpers), about
  7 us per draw.

Changes:

- The translator inlines small functions (PHASE2 "Native overrides"): the
  engine's reference-counted pointers, accessors and the like, 55 call sites
  in `0x6CFDB0` alone. `InterlockedIncrement` / `Decrement` called through the
  IAT are done in place.
- `call_indirect` kept its rare error paths in the same function, so every
  virtual call and import paid for a 288-byte frame, ten saved registers and a
  stack-protector check. They are out of line now. `call_com` finds the
  context methods with a switch instead of a chain of 30 compares.
- Clears are deferred (`flush_clears` in metal.mm): a D3D clear becomes the
  load action of the next pass that renders to the target, instead of an
  empty pass that writes the whole surface out for the next pass to read
  back. Anything else that reads the target first encodes the clear on its
  own. A heavy frame went from 64 passes to 54. On this Mac the GPU time is
  the same within noise (its bandwidth is ample); on a tile-based phone GPU
  each full-screen clear saved is two full-surface memory transfers. The
  depth-as-colour copy reuses its staging buffer instead of allocating one
  each time.

Unthrottled over seven minutes of attract (same build otherwise), race frames
went from 11.4 to 10.5 ms mean, 90th percentile 14.7 to 13.4 ms, and the
heavy intervals from 17.3 to 15.7 ms mean (worst 18.6 to 16.9). Those heavy
seconds now fit in 16.7 ms on the Mac; for the phone and headset budgets the
guest's per-draw cost is still the limit.

**Per-draw CPU cost**, a third pass. In a heavy scene, of the main thread:
the engine's render walk (`0x43FA90`) 71%, its draw-list walk (`0x733490`,
`0x76D510`) 42%, the draw call itself (`0x6CFDB0`) 23%; and 9% in the
cooperative scheduler.

- Preemption (a 2 ms timer, honoured at loop back-edges) gave every other
  guest thread a turn, and nearly all of them sit in waits: each turn was a
  condition-variable hand-off to a thread that found nothing to do. Now a
  thread blocked in a wait is parked (`pick_after` in threads.cpp) until
  something that can end a wait happens (event set, semaphore or mutex
  released, critical section left, timer fired, thread finished or resumed,
  handle closed: `g_version`) or its timeout comes, with a turn at least every
  20 ms as a safety net. The scheduler's share went from 9% to 3.5%; race
  frames 10.6 to 10.1 ms mean, heavy 15.4 to 15.0 ms. Attract audio recorded
  with and without it has the same levels and the same gaps.
- A direct call writes the flags back only when the callee reads flags on
  entry (`SYNC_TO`, resolved when the shards are written), and a function
  that reads none on entry does not load them (`PROLOGUE_NF`). Measured as
  neutral; kept, since it is six stores per call and six loads per entry less.
- Profile-guided optimisation of the generated code and runtime
  (`DCUSA_PGO=generate`, a run with `DCUSA_EXIT_AT=<presents>` and
  `LLVM_PROFILE_FILE`, `llvm-profdata merge`, then `DCUSA_PGO=<profdata>`):
  heavy 14.7 vs 14.6 ms, race 10.4 vs 10.3 ms. Within noise, so not used by
  default; the option stays.
- Guest code is now 82% of the main thread in a heavy scene, spread over the
  engine's functions with no single hot spot; host D3D handling is 7%,
  dispatch 2%. What is left is the translated code doing the engine's work:
  about 6 us per draw.

Two directions were then measured and set aside:

- **A native draw path.** The draw path is the engine's material system:
  `0x76D510` walks render items with a virtual call each, `0x733490` runs a
  pass per material (a virtual call, `0x458BC0`, 15% of the main thread; the
  draw `0x6CFDB0`, 21%; then a virtual setter per parameter, `0x735260` /
  `0x735380` / `0x735430`, 16%). `0x6CFDB0` alone reaches 318 functions and
  about 17,000 instructions by direct calls. A faithful native port is far
  larger than its likely gain; a native fast path for `0x6CFDB0`'s common
  case remains an option.
- **Smaller or better-placed code.** A linker order file of the functions a
  run executes (`DCUSA_ORDER_FILE`, 26,305 functions from the PGO profile) and
  `-Os` for the generated code (`DCUSA_GENERATED_OPT`, text 38.9 to 38.0 MB)
  were both within noise. Instruments' CPU Counters, checked against a
  compute loop and a pointer chase, put about a quarter of the main thread in
  memory stalls and under a tenth in mispredictions, with no sign that
  instruction fetch is the limit on this Mac. Both options stay in the build
  for devices with smaller caches.
- **The draw path inlined whole.** Every direct call in `0x6CFDB0` (the
  remaining helpers were up to 287 instructions) and in `0x733490` inlined
  (`INLINE_LIMITS` in translate.py). Two seven-minute runs: race 9.91 vs
  9.89 ms, heavy intervals at about 2,600 draws 15.5 vs 15.6 ms. No gain: the
  draw path's cost is its memory work, not its calls. The table is empty.

**Scene-change stalls.** `DCUSA_HITCH=<ms>` prints every frame longer than
that, with the time host code spent in it on file reads, the main thread's
guest waits, texture and buffer creation, shader creation, waiting for the
render thread, and MSL compiles and pipeline builds (`runtime/hitch.hpp`).
With `DCUSA_FRAME` set it also writes out the two frames presented after
any hitch over twice the limit. Over a paced attract loop:

- Each segment change has a few slow frames: one of 100 to 260 ms with no
  host time in it, one of 240 to 380 ms in which the game creates 6,700 to
  12,000 textures and 9,000 to 16,000 buffers (about 5 ms of it host time),
  and one or two of about 100 to 140 ms. Boot has a 1.0 to 1.3 s load.
- The time is the game's own scene setup on the main thread: in one such
  frame, 37% of a one-second sample was `0x459580` called for object after
  object, each first touch of an object a cache miss.
- Every frame captured right after these hitches is a static screen: the
  "Data Loading" text at boot, black, a white fade, or the CRIWARE logo. The
  pauses happen while nothing moves, so they do not show as stutter, and
  there is nothing to fix in the frame loop.
- Two things were ruled out on the way. Running the loader's LZ decompressor
  (`0x426890`) on its own core while the main thread kept the CPU barely ever
  overlapped (17 times, at most 1.2 ms in a loop) and was removed. A
  two-second run of 50 to 110 ms frames seen once was caused by the profiler
  (`sample` suspends the process); unprofiled runs do not show it.

`dcrun --content <folder>` mounts that install and stays in the frame loop.
`DCUSA_STATS=1 DCUSA_EVERY=100` prints draw counts every 100 presents.
`DCUSA_FRAME=<path>` writes present 1, 30, and every `DCUSA_EVERY`th as PPM.
`DCUSA_IO=1` logs every open and read with the thread that did it.
The HLSL the boot compiles is translated to MSL (item 4), and with
`DCUSA_METAL=1` an offscreen Metal device draws every call (item 5). The
presented frame is still black, and that now reflects what the game draws: at
this point of boot each frame is only the post-process chain over an empty scene
(no sprite, text or mesh pass yet), so every HDR input is RGB 0. The exit
criterion is not met: attract has not been reached, and the Windows captures
belong to Phase 5.
An unknown COM slot still fails closed as `not ported (service) SLOT:
Interface+XX`.

## What the first frame already calls

The objects live in `src/recomp/d3d11.cpp`. Vtables are 256 slots because
`ClearState` is context `+0x1B8`. Kinds, in order: Device, Context, Dxgi, Adapter,
Factory, Output, Swap, Texture, Srv, Sampler, Rtv, Query, Blob, Shader, Reflector,
Buffer, Dsv, Layout, Raster, DepthState, Blend, then DirectInput and DirectSound.

`D3D11CreateDevice` (`0x6B967F`) returns feature level `0xB000`. The startup path
then calls:

- Device: `CheckFeatureSupport` (threading flags both false),
  `CheckMultisampleQualityLevels` (one sample), `CreateBuffer`, `CreateTexture2D`,
  `CreateShaderResourceView`, `CreateRenderTargetView`, `CreateSamplerState`,
  `CreateQuery` (562 of them at `0x6B28C0`), `UpdateSubresource` (the bytes are
  dropped), `CreateVertexShader`, `CreatePixelShader`, `CreateComputeShader`,
  `CreateInputLayout`, `CreateRasterizerState`, `CreateDepthStencilState`,
  `CreateBlendState`, `CreateDepthStencilView`, `SetPrivateData`.
- Context, from the draw at `0x6CFDB0`: `VSSetConstantBuffers` `+0x1C`,
  `PSSetShaderResources` `+0x20`, `PSSetShader` `+0x24` (pops 16; the caller pushes
  an extra zero that `mov esp, ebp` drops), `PSSetSamplers` `+0x28`, `VSSetShader`
  `+0x2C`, `DrawIndexed` `+0x30`, `PSSetConstantBuffers` `+0x40`,
  `IASetInputLayout` `+0x44`, `IASetVertexBuffers` `+0x48` (five arguments),
  `IASetIndexBuffer` `+0x4C`, `IASetPrimitiveTopology` `+0x60`,
  `VSSetSamplers` `+0x68`, `OMSetBlendState` `+0x8C`, `OMSetDepthStencilState`
  `+0x90`, `RSSetState` `+0xAC`, plus `OMSetRenderTargets`, `RSSetViewports`,
  `RSSetScissorRects`, `ClearRenderTargetView`, `ClearDepthStencilView`,
  `ClearState`, `GenerateMips`, `Map` / `Unmap`, and query `Begin` / `End` /
  `GetData`.
- `Map` returns a 64 MB scratch at `0x31000000`. `Unmap` pops 12. `GetData`
  returns `S_OK` and zeroes the caller's buffer; any other HRESULT makes `0x6B89F6`
  skip the copy.
- DXGI: one adapter ("Port", 512 MB), one output, a 1920×1080 mode, a factory, a
  swap chain. `GetBuffer` is an `ID3D11Texture2D` and keeps a 1920×1080 RGBA
  target. `Present` (`0x6BBA27`, `0x6BBA31`) copies the bound target. Its
  callers ignore the HRESULT. `GetFullscreenState` writes windowed.
  `GetDesc` writes 1920×1080 and a 60/1 refresh, because `0x6BB6F4` divides the
  denominator by the numerator. `GetContainingOutput` returns the output.
  `WaitForVBlank` returns `S_OK`.
- `D3DCompile` (`0x6DB5CF`) returns a blob and does not compile. `D3DReflect`
  reports no bound resources, which `0x6DC860` treats as success.
- `D3DPERF_BeginEvent` and `D3DPERF_EndEvent` return 0. `d3d9.dll` exports nothing
  else. `DWriteCreateFactory` is imported and has no literal call site in the
  generated code.

`CreateComputeShader` is accepted. Whether a geometry shader, a hull shader, or a
domain shader is created is not yet counted. The profiles in the EXE are
`vs/ps_4_0` and `vs/ps/gs/cs_5_0`.

## Work

### 1. Keep the same entries, store the resources

The guest-visible objects stay where they are. A create call records the
description and the bytes. `Map` still returns a pointer in the scratch at
`0x31000000`. `Unmap` copies a write back. The copy at `0x4092A1` therefore
lands in the resource. A texture bigger than the 64 MB scratch is not stored.

Constant buffers, vertex buffers, index buffers, and textures are the four the
draw at `0x6CFDB0` binds. Samplers, blend, depth, and rasterizer state are
recorded the same way. Views point at the resource they were created from.

### 2. Count the shader stages

Before a Metal pipeline is built, count creations and draws of each stage on a
run that reaches attract:

- vertex, pixel (already created on the first frame)
- geometry
- compute (created; not yet dispatched on the first frame)
- hull and domain, if any

A stage with a zero count is not implemented. Geometry and compute are expected.
Tessellation is not, until a count says otherwise.

### 3. Shaders, at prepare time

Sixty HLSL modules are embedded in the EXE (`#ifdef HLSL`, about 1,300 `float4`,
321 `SV_Position`). The game compiles them at run time. `GameData/.shaders/shader_cache.bin`
holds 434 DXBC blobs from compiler 9.30.9200.20499. The player may not have that
file. Model 2 uses `rom/shader/*.fxo` (DXBC plus DX9 bytecode) and `m2*.glsl`.

Translation runs while the cache is prepared, on the machine that has the EXE.
The HLSL is compiled with DXC to SPIR-V and SPIRV-Cross to MSL, and the MSL is
stored in the cache under the shader's hash. A hash-keyed override replaces one
module when the cross-compile is wrong. The override is written to match
behaviour. The original HLSL is not committed.

`D3DCompile` then finds the cached module instead of returning an empty blob.
`D3DReflect` reports the bindings that module actually has, so `0x6DC860` sets
up the constant buffers and textures the shader uses. A shader with no cache
entry fails closed with the hash.

The `.fxo` path is separate and smaller. It is DXBC, or the GLSL beside it, not
the embedded HLSL.

### 4. An offscreen Metal device

The first backend draws into a texture, not a window. The window is the Phase 7
shell. `Present` keeps the contract it has now (success, callers ignore the
HRESULT) and publishes the texture for the Phase 5 compare.

The device needs buffers, 2D textures including BCn, samplers, blend and depth
state, multiple render targets, and the stages the count in item 2 found.
iPhone and visionOS do not all support BCn
(`MTLDevice.supportsBCTextureCompression`). Where it is missing, prepare
transcodes to ASTC. The Mac used for the oracle can sample BCn directly.

`rsqrtss` is used 85 times in reachable code, plus one `rcpps` and one `rcpss`.
They are exact today. Hardware is about 12 bits. A table waits until a captured
frame shows the difference.

### 5. What a frame is made of

NRT draws the HD scene from `nrt/dx11/` (NRFC containers, DDS, zlib). The race
the player recognises is the i960 program in `rom/daytona_rom/`, interpreted by
m2e, with NRT models substituted by model id. Model 2 polygons and the `.fxo`
shaders are the fallback path, not the HD path.

Attract has to get far enough to issue the draws. That needs the files Phase 4
opens. Until those reads exist, this phase can be driven by a captured command
list rather than by `dcrun`.

### 6. Text

`DWriteCreateFactory` is the only DirectWrite import. GDI text imports that are
still fail-closed are `CreateFontW`, `GetGlyphOutlineW`, `GetGlyphIndicesW`,
`GetTextMetricsW`, `CreateCompatibleDC`, `SelectObject`, `DeleteDC`, and
`DeleteObject`. They are Phase 4 imports. This phase consumes the glyphs if the
game rasters text into a texture, and uses a Metal font path only if DirectWrite
is actually called.

### 7. The 2D reference

A full software rasterizer is larger than the attract comparison needs. The
reference device covers the 2D and UI draws: colour, alpha, a texture sample,
scissor. The 3D comparison is captured Windows frames, numbered under autoplay,
with a perceptual tolerance. Phase 5 owns the diff. This phase names the frame
image and the tolerance the diff will use.

## Suggested order

1. Record resource bytes and descriptions behind the existing create and `Map`
   calls. **Done.** The guest ABI does not change. Float clears and triangle
   writes store native half / R32.
2. Count shader stages on a run that draws. **Partly.** Passes that run are
   known; GS/HS/DS census is not. Blocked on a run that loads the course.
3. Finish resource loads so Main has a course mesh and the glare composite's
   `g_Texture` is not empty. **Done: boot completes and attract renders.** (The
   notes below are from before that.) The IO, resource and CRI
   threads run, the guest's zlib decodes the archives, and boot loads 243 files
   in 90 s. Next: confirm the boot state machine advances, then find what still
   stands between the wait scene and the course load (`f_005825A0`, net status
   `[0x15FF5DC]+0x74`). Speeding up the 2D reference (or skipping paints of
   targets nothing samples) would shorten each try.
4. Prepare-time HLSL to MSL, keyed by hash, and make `D3DCompile` / `D3DReflect`
   return that module. **Done for every shader the boot compiles.** The game
   assembles each source at run time (`#define HLSL`, `#define ENABLE_CBUFFER 1`,
   per-shader macros, then module text; only 2 of 78 are verbatim in the EXE),
   so the input is the request itself. `DCUSA_SHADERS=<dir>` makes `D3DCompile`
   write `src/<key>.hlsl` and `src/<key>.json` (key: FNV-1a 64 of source,
   macros, entry, profile). `tools/shaderprep/translate_shaders.py <dir>` runs
   glslang (HLSL front end; Homebrew has no DXC) and spirv-cross, and writes
   `msl/<key>.metal`, `<key>.json` and `<key>.refl`. A stage counts only when
   `spirv-val` accepts it: glslang as is, then with local texture arrays
   unrolled, then without glslang's optimizer. The 3 `g_PreShadowPS` variants
   build a local `Texture2DShadow[4]` from four globals, which glslang turns
   into invalid SPIR-V; the unroll stage drops that array and turns each
   `tex2DCmp*(TexShadow[i], ...)` into a `?:` over the same call on each global
   (same value: texture reads have no side effects and `?:` is not control
   flow). The module's own `#ifndef GLSL` alternative is not equivalent (it
   always samples shadow map 0), so it is not used.
   Textures, samplers and constant buffers are numbered from 0 per class, like
   `t#` / `s#` / `b#`, by rewriting the SPIR-V binding decorations, and the MSL
   uses those numbers (`--msl-decoration-binding`). Constant buffer offsets and
   sizes come from the same SPIR-V, so they need not match D3D's packing: the
   game finds every variable through reflection. `override/<key>.metal` +
   `.json` replace one module; none is needed now.
   Boot compiles 78 requests (70 `ps_4_0`, 8 `vs_4_0`). All 78 translate, and
   all 78 compile and expose `main0` on the Metal device (`mslcheck`, CTest
   `test_msl`). With a translation present, `D3DReflect` answers from it:
   `GetDesc` (bound resources and constant buffers), `GetResourceBindingDesc`
   (cbuffers included) and `…ByName`, `GetConstantBufferByIndex` / `ByName`,
   `GetVariableByName`, and the constant buffer, variable and type interfaces
   (`GetDesc`, `GetVariableByIndex` / `ByName`, `GetType`, `GetBuffer`). Out of
   range gives D3D's invalid object, whose `GetDesc` fails. By present 30 the
   game has read 81 buffer, 1,034 variable and 3,914 type descriptions with no
   unported slot. Later scenes will compile requests the boot does not; a run
   captures them, and the next translate picks them up.
5. Offscreen Metal for the stages that count is non-zero. BCn where the device
   has it, ASTC where it does not. **Running for vertex and pixel shaders.**
   `src/gpu/metal.{hpp,mm}`, on with `DCUSA_METAL=1` plus `DCUSA_SHADERS`.
   `d3d11.cpp` keeps a full state mirror (`gpu::DrawState`): every VS / PS
   constant buffer, view and sampler slot, up to 8 render targets and the depth
   view, input layout (semantic, index, format, slot, offset,
   `APPEND_ALIGNED`, per-instance step), vertex / index buffers, topology,
   viewport, scissor, and blend / depth / rasterizer descriptions. It feeds the
   backend at each guest write (create with data, `UpdateSubresource`, `Unmap`),
   clear and draw; the 2D software reference is skipped then. Every upload is a
   new Metal object, so draws already encoded keep the bytes they saw (D3D's
   contract for constant buffers rewritten between draws).
   - Pipelines: translated VS + PS, vertex descriptor built by matching the VS
     inputs' semantics (`input` lines in the `.refl`, from glslang's
     `-fhlsl_functionality1`) to the layout, colour formats, blend, depth
     format; cached. Stage-interface locations are fixed per semantic
     (`TEXCOORDn` = n, `COLORn` = 16 + n, ...) so any VS/PS pair links the way
     D3D links them. Constant buffers keep their `b#` index; vertex streams are
     Metal buffers 16+.
   - Metal binding reflection of each pipeline gives what each stage reads. A
     buffer, texture or sampler the game left empty gets a zero stand-in (D3D
     reads zeros there). A depth texture bound where the shader reads
     `Texture2D` is read through an `R32Float` copy taken after each pass or
     clear that wrote the depth.
   - New render targets and depth textures are cleared to zero, as D3D creates
     them; Metal leaves private textures undefined, and the NaN in them spread
     through the whole composite.
   - Formats: RGBA8 / BGRA8 (sRGB too), RGBA16F/Unorm, RG16F, R16F, R32F,
     RGB10A2, RG11B10, R8 / A8 / RG8, BC1-BC7 including BC6H, and depth (D32 and
     D24S8 as `Depth32Float(_Stencil8)`). Only mip 0 is uploaded; a texture has
     more levels only when it is created for `GenerateMips`. `Present` commits
     the frame, waits, and reads the back buffer back for the frame image.
   - `Draw` (`+0x34`) is answered with Metal. Geometry, hull, domain and
     compute stages, stencil, depth bias, MSAA, texture arrays / cubes / 3D,
     non-zero mips from the guest, typed views that differ from the resource
     format, and ASTC for devices without BCn are not done. Boot compiles only
     `vs_4_0` / `ps_4_0`, so the stage census stays open until attract.
   - A 90 s content run under `MTL_DEBUG_LAYER=1`: 25,548 draws, 15 pipelines,
     none skipped, no validation error, about 13 presents a second (the 2D
     reference managed 1.7). CTest `test_metal` draws through the backend and
     checks pixels: layout by semantic, texture and point sampler, a constant
     buffer changed between draws, viewport, clear, depth read as colour, and a
     zero stand-in.
   - Debugging: `DCUSA_METAL_TRACE=<n>` prints every draw and clear of present
     n with shaders, targets, state and the first pixel-shader constants;
     `DCUSA_METAL_SCAN=<n>` reads back every colour target and prints
     non-zero, NaN and per-channel maxima.
6. `.fxo` for the Model 2 path. **Not started.**
7. The 2D reference and the frame image Phase 5 will diff. **2D reference
   written and unit-tested.** Presented frames are still black.

## Out of this phase

- A host window and `CAMetalLayer`. Phase 7.
- Opening the game's files. Phase 4. The game folder stays read-only.
- The i960 hot loop. It stays translated until Phase 5 profiles it.
- Committing shaders, ROM, or DDS.

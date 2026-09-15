# The Q16.16 walk set up without floats — 2026-09-04

Render path `displaylist-full` = span writer + exact integer transform + the
Q16.16 walk's start and step derived from that transform, with **`D ≈ 2^30`**.
This is the step that removes `exactReciprocal` and `invSf` — the per-item and
per-scanline float divisions, the actual callers of `__divsf3` — from the fill
loops.

## The precision trade, stated once

The inverse of the rigid transform is the transpose over `D = C² + S²`, and a
Q15 table `(C,S)` has `D = 2^30 (1 ± 6e-5)`. Treating `D` **as** `2^30` turns
the division into a shift:

```
base_x · 2^16  =  (C·dxN + S·dyN) · 2^16 / (D · s)   ≈   (C·dxN + S·dyN) >> 14 / s
step per pixel =  (2C) / s
```

One int32 division per row per axis for the start, none per pixel. The error is
6e-5 relative on a pattern coordinate — a hundredth of a pixel at the far edge of
a 960-wide canvas — and it does **not compound**: the angle accumulator rebuilds
the transform from an integer angle every time. This is generative art, and the
decision was to spend that. It is the same shape as the bare-transpose tried and
reverted on 2026-09-03; it lost then against a "match `sinf`" metric that no
longer applies.

## What it moves

`compare-paths displaylist displaylist-full`, at 960×540:

| corpus | differing renders | pixels |
|---|---|---|
| main (21) | 3 | 221 |
| ops (30) | 6 | 9,018 — the rotated-pattern probes, whole-canvas fills |
| real (36) | 18 | 5,944 |

At device size (200×200), looked at rather than counted: `art_deco_4` **5 px**,
`seascape_4` **4 px**, `grid` **21 px**, rotated pattern probe 35 px. Boundary
pixels where a pattern cell edge sits within 6e-5 of a pixel centre.

## What it costs — nothing, except for circles

Watchy, `full` against `span` (the same renderer minus this change):

| | span | full | |
|---|---|---|---|
| `op_fill_rect_pattern` | 9.74 | 9.70 | flat |
| `op_fill_rect_pattern_rot` | 20.21 | 19.98 | −1% |
| `op_draw_asset` | 10.44 | 10.38 | flat |
| `reconnected` | 17.36 | 16.87 | −3% |
| `thunderstorms` | 52.30 | 50.43 | −4% |
| `disconnected` | 27.65 | 26.86 | −3% |
| `seascape_4` | 81.78 | 79.11 | −3% |
| **`art_deco_4`** | 87.34 | **100.02** | **+14%** |
| `seascape_2` | 30.76 | 33.31 | +8% |
| `op_fill_circle` | 5.76 | 6.40 | +11% |

The setup itself is flat to slightly better. The regression is entirely the
**exact integer circle span** that the `-int` path already carried — an int64
multiply and a 31-iteration integer root per scanline — and it lands on the two
circle-heavy scripts, as it did before. Next step.

## The first version was four libgcc calls per row

`intDdaRow` first divided `nx`, `ny`, `2C` and `2S` by `s` as `int64` — four
`__divdi3` calls per scanline. Caught by a report-parsing mistake rather than by
the measurement: the collector dropped the new column and I read the no-map
column as +62% for it. The int64 divisions were real regardless; `nx` and `ny`
fit `int32` on any on-screen row, so the divide is one Xtensa instruction now.

## Step 3b — the circle span in int32 at 1/16 pixel

The exact integer span (Q15, int64 multiply, 31-iteration root per scanline)
was the whole `+14%` on `art_deco_4`. Under the approximate flag it is now Q4
int32: every quantity fits for radii up to 8000, the root is 16 iterations, and
the endpoint can sit one pixel off the exact one where the true edge is within
1/16 px of a pixel centre. Same trade as `D ≈ 2^30`; +~200 px on the real
corpus at 960×540 (6,142 vs 5,944).

| | span | full (exact span) | full (Q4 span) | |
|---|---|---|---|---|
| `op_fill_circle` | 5.77 | 6.40 | **5.76** | equal to the float `sqrtf` span |
| `op_fill_circle_pattern_rot` | 12.58 | 13.38 | 12.35 | −2% |
| `art_deco_4` | 86.23 | 100.02 | **81.34** | **−5.7%** vs span |
| `seascape_2` | 30.62 | 33.31 | 29.81 | −3% |
| `seascape_4` | 80.03 | 79.11 | 77.89 | −3% |
| `thunderstorms` | 52.39 | 50.43 | 50.53 | −4% |
| pattern and DRAW probes | | | | flat |

`displaylist-full` is now flat-to-better than the span path on every script and
every probe. Against the float renderer of two days ago, `art_deco_4` is
**518 → 81 ms**.

Still float in this path: `narrowSpan` (per-row bisection in `fillRect` and
`drawAsset`), the display-list bounds pass (per item), `exactReciprocal`
(per item, computed and unused by the integer branches), and
`matrix_set_rigid`'s `1/det` (per transform command).

## Step 4 — the last two float sites: `narrowSpan` and the bounds pass

**Bounds pass in Q15.** `calculateScreenBoundsQ15` mirrors the float function
case for case — four corners, a line, the unit square, centre ± `lr × scale` —
from the exact integer transform; floor and ceil are shifts, the half-pixel
widening is `± ONE/2`. It is per item, so it shows where items are many:
`op_fill_pixel` −17%, `seascape_4` −12%, `art_deco_4` −10%, `op_draw_asset` −7%.

**`narrowSpan` on integers — first shape regressed.** Its threshold type now
follows `g`'s return type, and the integer path bisects on the same int64
expression the walk is set up from, so boundary and interior agree by
construction. Evaluating that expression per probe — two 64-bit multiplies,
~40 probes a row — cost `disconnected` **+21%**, `reconnected` +12%, `grid` +6%
against the previous full path. FILL_RECT-heavy scripts, all of them.

**The fix is exact and free.** `(C·dxN(x) + S·dyN) >> 14` is affine in `x`
with an integer slope: it equals `g(x0) + 2C·(x − x0)` exactly, because
`C·(x−x0)·2¹⁵` is a multiple of `2¹⁴`. One int64 evaluation per row; each probe
is a 32-bit multiply and an add. The pixel distance came back identical —
245 / 9,018 / 6,168 — which is the proof it is the same formula.

`full` against `span`, Watchy, after both:

| | span | full | |
|---|---|---|---|
| `thunderstorms` | 52.29 | **41.35** | **−21%** |
| `eyes` | 22.44 | 17.84 | −21% |
| `seascape_4` | 74.44 | 60.89 | −18% |
| `art_deco_4` | 88.21 | 73.72 | −16% |
| `confetti` | 20.46 | 17.25 | −16% |
| `seascape_2` | 33.28 | 29.89 | −10% |
| `disconnected`, `reconnected`, `grid` | | | −1…−3% |
| `op_draw_asset` | 10.68 | 9.44 | −12% |
| every other probe | | | flat or better |

Against the float renderer of 2026-09-02: `art_deco_4` **518 → 74 ms**,
`thunderstorms` 124 → 41, `seascape_4` 148 → 61.

Anomaly on the record: `op_fill_pixel`'s **baseline** read 6.23 ms this run
against 2.04 in every earlier run, for `fixed` and `span` alike — paths that do
not execute any new code. A 3x swing on a 120-item probe looks like an
instruction-cache layout effect from the binary growing (the ESP32 executes
from flash through a 32 KB cache). Re-checked on the next flash, not explained
away.

What is still float on the full path: `exactReciprocal` (per item, its result
unused by the integer branches), `matrix_set_rigid`'s `1/det` (per transform
command), `mp_sin_deg` for the float matrix (per command), and the float
fallback loops themselves. All per-item or per-command; none per pixel or per
row. They go when the float path is compiled out rather than selected.

## Step 5 — float that was executed and discarded, and the PIXEL loop

Asked "where are the floats in the pipeline, then?" after the flip, the honest
map had two things wrong with the claim that everything between a display list
and the pixels was integer:

**Executed, not used.** In `fillRect`, `fillCircle` and `drawAsset` the float
DDA setup — `invSf` (a float DIVISION when `SCALE` is not a power of two),
`bx0`, `dbx`, the pattern-row hoist with its `v / sf` — was still computed
every scanline and then lost the ternary to the integer setup. `exactReciprocal`
did the same once per item. The compiler could not remove them because the
float fallback branch reads them. They now run only when the integer setup
declines the row. Pixel-identical to the pre-change default on all three corpora
(21 / 30 / 36). Speed: flat — a float divide on the FPU was never the cost —
which is exactly why no gate and no measurement had noticed.

**`PIXEL` / `FILL_PIXEL` were still float per pixel.** Their inner loop walked
the item's AABB with `im0*fx + m2y + im4` and a float range test — at `SCALE 5`
that is 25 float tests per item. Treated all week as "one pixel per item",
which is only true at `SCALE 1`. Now the same integer walk the fills use:
scaled-logical Q16 from the exact transform, affine along the row, tested
against `(lx*s) << 16` edges; the patterned form takes its base coordinate from
`intDdaRow`. Three shapes were measured on `op_fill_pixel` (120 one-pixel
items, the probe that isolates per-item cost):

| shape | vs span |
|---|---|
| int64 accumulators | +10.4% |
| int32 accumulators | +12.5% — so the loop width was never it |
| item constants hoisted above the row loop | **+1.8%**, noise |

The setup was the whole cost, on rows one pixel long. Fills flat throughout;
pixel identity held on every corpus, including for `PIXEL`.

## What is still float, precisely

- **Display-list generation, per `TRANSLATE`/`ROTATE`/`RESET`**: the float
  matrices in `TransformSnapshot` are still built (`mp_sin_deg`,
  `matrix_set_rigid` with its `1/det`) alongside the exact integer state,
  because the selectable float path reads them. ~100 per render.
- **The float fallback branches** in every primitive, and the float bounds pass:
  compiled in, never executed on the default path.
- Nothing per pixel. Nothing per row.

Removing the last two is a build-configuration change — compile the float path
out rather than select it at run time — and it is what will finally make `nm`
show `__divsf3` and `sqrtf` gone from the binary.

## Step 6 — what the objects actually import, function by function

Read from `nm` and `objdump -dr` on the Watchy build. First correction: an
earlier pass reported "no renderer object references any float helper" — that
was my search missing the objects, which PlatformIO places at
`.pio/build/M5Paper_MicroPatterns/src/` (the `../..` in `build_src_filter`
normalised away), **outside** the env directory and **shared between the
normal, bench and profile envs**. SCons recompiles on flag change, so it is
correct, and it is why alternating envs rebuilds the renderer every time.

The renderer objects DO import float helpers — from the compiled-in float path:

| object | imports | from |
|---|---|---|
| `micropatterns_drawing` | `__divsf3` ×17 | float DDA setups, `narrowSpan` float lambdas, `exactReciprocal`, `fillColorFromScaled`, `screenToLogicalBase` |
| | `sqrtf` | the float circle span |
| | `roundf` ×15 | float endpoints in `drawRect`, `drawLine`, `drawCircle` |
| | `lrintf` | `fxFrom` |
| | **`__divdi3` ×3** | **the unrotated DRAW row clip — on the default path** |
| `matrix_utils` | `__divsf3` | `1/det` |
| `display_list_renderer` | `ceilf`, `floorf` | the float bounds pass |

Everything in that table but one row is the selectable float path: compiled in,
never executed on the default. The exception was the step-5 row clip, written in
`int64` and measured flat — three libgcc calls per row of every unrotated `DRAW`,
the only library call left inside a default-path loop. Its operands are bounded
by the `fxFits` guard, so `int32` gives the same answers:

| | before | int32 clip | |
|---|---|---|---|
| `op_draw_asset` | 9.44 | **8.90** | −6% |
| `seascape_4` | 62.18 | 59.88 | −4% |
| pixel identity, three corpora | | 21 / 30 / 36 | |

After it, the drawing object imports `__divsf3` and `sqrtf` only, both from the
float path. **The default path makes no library call in any loop.**

What compiling the float path out would change: `sqrtf`, `roundf`, `lrintf`,
`ceilf`, `floorf` and ~380 FPU instructions of dead code leave the renderer
objects; `sqrtf` likely leaves the binary. `__divsf3` stays regardless — the
Arduino framework's `ColorFormat.c` imports it — so "no soft-float in flash" is
not reachable from the renderer side on this platform, only "no soft-float in
the renderer".

## Step 7 — no float executed per transform command or per item (2026-09-05)

Asked whether "compiled in, never executed" was true, the exact answer was: true
for the fallback branches, **false** for two things that ran on the device with
nothing reading their results.

- **Per `TRANSLATE`/`ROTATE`/`RESET`** the runtime built the float `matrix`,
  `inverseMatrix` and `tx/ty` — `mp_sin_deg`, float accumulation, and
  `matrix_set_rigid`'s `1/det`, a real `__divsf3` call — alongside the exact
  integer state. ~100 executions per render, discarded. Now built only when a
  path asks (`MicroPatternsRuntime::setFloatTransformEnabled`); the float path
  and the historical partial paths ask, the default does not. The snapshot
  records whether it was built (`hasFloat`).
- **Per item**, every fill hoisted `im0..im5` from that matrix. They still are
  hoisted, as per-item consts exactly as before, but from a zero matrix when
  there is none; nothing on the default path reads them (see below).

Two readers of the float matrix turned up **on the default path** the moment it
stopped being built, neither of which any counter or gate had ever flagged
because doing the work twice gives the same picture:

1. The **rotation selector** `if (im1 == 0.0f)` in `fillRect` and `drawAsset`
   chose row-hoist vs rotated walk from the float matrix. With no matrix, every
   rotated fill took the unrotated path: 12 of 21 goldens failed. Now
   `sinQ15 == 0` when there is no float matrix; the float test when there is,
   so the float path stays identical.
2. The **asset row index** in `drawAsset`'s row hoist came from `im1`/`im5`;
   `fillRect`'s equivalent had been converted, `drawAsset`'s had not. Every
   `DRAW` case failed — `op_draw_asset`, `city`, `nest`. Now from the integer
   setup's `y0`, computed before the hoist as in `fillRect`.

A row the integer setup declines (Q16.16 overflow, pattern coordinates beyond
±2^14 units) needs a matrix for its float fallback. It materialises one from the
integer state into a scratch member (`floatFallbackMatrix`, out of line), then
**re-runs the whole item** with that matrix in place, so every coefficient the
item reads is the materialised one and the hot code keeps its per-item consts.
The re-run is idempotent: the same pixels in the same colours, and with the
occupancy map the rows already painted are skipped. It is counted
(`float-fallback rows`, printed by `compare-paths`): **0** on every corpus at
960x540 and at the Watchy's 200x200.

Gates: default goldens 21/21; `golden-float` 21/21 untouched; pixel identity
against a HEAD build 21/21 + 3/3; partial paths 21/21 identical to each other;
sanitizer clean.

### The device A/B measured the linker, not the change

The first device run said PIXEL was **2.75x slower** on the default path and
scripts carrying PIXEL 5–25% slower, with the map-off path unchanged. Three
different shapes of the drawing code gave the same number. Forcing the float
matrices back on (same code, HEAD's data) gave the same number. The profiling
build showed the extra time **inside and outside** the rasteriser: the
renderer's untouched bounds and occlusion code was 2.4x slower per item too.
Every untouched function was byte-identical in size; the whole image had
shifted by at most 0x400 bytes.

So: HEAD's code plus ~1KB of dead padding, linked and referenced so the linker
kept it. The hot functions moved by **8 bytes**. Real scripts moved by up to
**±35%** (`seascape_4` +30% on `fixed`, +35% on `nomap`, −12% on `float`;
`nest` ±15%; `thunderstorms` ±14%), same code, same data, deterministic to the
microsecond. The ESP32 runs code from flash through a small cache, and where the
linker put each function decides what that cache does. **Every device A/B in
this document that resolved a delta under ~35% on flash builds was comparing
placements as much as algorithms.** The goldens and the pixel-identity gates
are unaffected; only the timings are.

**Instrument:** `MP_HOT` (`mp_attr.h`) marks the rasteriser's hot functions —
the five fills/pixels, the outline ops, the renderer's per-item path, the
occlusion buffer — and the bench environments define `MP_HOT_IRAM` to place
them in IRAM, where there is no cache. Cost: 21.5KB IRAM (62.0 → 83.6KB in the
bench). With the hot code in IRAM the same 8-byte shift moves the median by
**0.0%** (worst script 8%, the two ~0.5ms probes). The shipped firmware cannot
use it: the Watchy firmware's IRAM is at 128.0KB of 128KB (WiFi/BLE) and would
overflow by 19.8KB, the M5Paper by 14.3KB. So the flash build still has
placement variance in the field; the IRAM build is how a change is judged.

Against HEAD on flash, the IRAM build of this step is 10–34% faster on the big
scripts and never worse than 3% — that is IRAM removing cache misses, not this
step, and it is what the watch would get if it had the IRAM to spare.

### The runs, in order (raster µs, median of 7, deterministic to ±1µs)

The raw captures of this day were kept in a session scratch directory that did
not survive; these are the figures as reported at the time. `full` is the
default path; `nomap` is the default with the occupancy map off; `fixed`, `span`
and `float` are the historical partial paths.

| run | build | `op_fill_pixel` full | `nest` full | `seascape_4` fixed | `seascape_4` nomap | what it showed |
|---|---|---|---|---|---|---|
| w29 | HEAD (747d413), 2026-09-04 | 2250 | 11562 | 118521 | 150715 | baseline |
| w32 | HEAD rebuilt 2026-09-05 | 2250 | 11562 | 118521 | 150715 | builds reproduce to the µs |
| w30 | step, shape 1: coefficients as mutable locals | 6203 | 14554 | 100716 | 125557 | "PIXEL 2.75x", DRAW-heavy −15% |
| w33 | step, shape 2: per-row loads, row restart | 6611 | 12110 | 100381 | 124344 | same PIXEL; partial paths ±40% |
| w34 | step, shape 3: per-item consts, item re-run (kept) | 6197 | 14213 | 101457 | 126433 | same PIXEL: the shape is not the cause |
| w35 | w34 code, float matrices forced on everywhere | 6699 | 14299 | 100128 | 125009 | same: the data is not the cause |
| w36 | HEAD + 1KB pad, not linked (GC'd; +4 bytes) | 2250 | 11562 | 118513 | 150693 | null result |
| w37 | HEAD + 1KB pad, linked (+8 bytes on hot code) | 2213 | 10511 | 154711 | 204104 | **±35% from an 8-byte shift** |
| w38 | step (w34 code), hot code in IRAM | 2092 | 9630 | 100238 | 125014 | IRAM: 10–34% faster than HEAD-flash |
| w39 | w38 + the pad | ≈w38 | ≈w38 | ≈w38 | ≈w38 | median 0.0%, worst 8% |
| w40/w40b | HEAD, hot code in IRAM (stopped at 599 / 688 of 1015) | 2077 | — | 98152 | — | **step vs HEAD, IRAM: flat** |

Profile (env `watchy2-profile`, default path, `op_fill_pixel`, 120 items):
HEAD total 2421µs of which 1347 inside `drawFilledPixel`; the w34 code total
6930µs of which 4368 inside — and 2562 vs 1074 **outside**, in the renderer's
bounds/occlusion code that had not changed. `nest`: DRAW 2937→4727µs,
FILL_RECT 6534→7002µs. That split is what pointed away from the change.

Symbol sizes (HEAD → w34): every untouched function identical;
`drawPixel` +152B, `drawFilledPixel` +117B, `fillRect` +296B, `fillCircle` +137B,
`floatFallbackMatrix` +74B (new, out of line); `.flash.text` +1084B; addresses of
the renderer shifted by 8 bytes, the drawing functions by ~0x100, the runtime by
0x410. IRAM: bench 62.0KB → 83.6KB with the hot set; Watchy firmware 128.0KB of
128KB without it (overflow 19.8KB with); M5Paper overflow 14.3KB.

### Hypotheses tried and killed, in order

1. *The materialiser runs per item because the integer walk declines rows at
   200x200.* Counter at 200x200: 0 on every corpus. Dead.
2. *Mutable float locals inside the row loop wreck register allocation.* Shape
   2 (per-row const loads) and shape 3 (per-item consts, re-run the item) gave
   the same PIXEL number. Dead — though shape 3 is kept, since it restores the
   exact HEAD code in the hot path and is the right shape regardless.
3. *Five inlined copies of a sin/cos/divide blow the inlining budget.* Made it
   `noinline`: same number. Dead.
4. *The snapshot's new `bool` adds padding that breaks the `memcmp` dedupe and
   grows the pool.* The constructor memsets the whole struct; the pool is a
   deque. Dead on inspection.
5. *The renderer reads the stale float matrix for PIXEL bounds.* It uses the
   Q15 bounds on the default path. Dead on inspection.
6. *It's the data: identity matrices where HEAD had real ones.* Forced the
   float build on everywhere (w35): same number. Dead.
7. *Placement.* HEAD + 8 bytes: ±35%. Alive, and confirmed by IRAM removing it.

The first pad attempt (w36) was garbage-collected by the linker and shifted
nothing; the run looked like a refutation and was a null result. Reference the
pad from live code or it does not exist.

**This step, IRAM vs IRAM** (HEAD carrying the same hot set): raster median
**0.0%** on every path (default worst +3.3%, best −6.7%; float path within
±1.6%, untouched as intended), `op_fill_pixel` **+0.7%** — the "2.75x" was
placement. Display-list phase on the default path **−3.0%** median, −12% best:
the float work that no longer runs per command. Coverage: 21 of 29 scripts —
the HEAD+IRAM firmware stopped reporting partway through both of its runs
(599 and 688 of 1015 samples, at different points); this step's IRAM builds
completed all three of theirs. Not investigated; noted.

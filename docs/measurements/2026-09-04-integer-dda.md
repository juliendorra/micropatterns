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

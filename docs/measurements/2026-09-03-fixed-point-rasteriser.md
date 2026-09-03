# Q16.16 fixed-point rasteriser — 2026-09-03

The default rasteriser since 2026-09-03. The superseded float path stays
selectable as `displaylist-float`, with its own golden set. Measured on a Watchy (ESP32-PICO-D4, 240 MHz) with both paths
alternating inside one firmware run.

**Result on real hardware: −42% on `DRAW`, −41% on rotated patterned disk, −33%
on rotated pattern fill, −31% on a solid disk, −15% on unrotated pattern fill,
and exactly 0.0% on every path it does not touch.** Whole scripts:
`artdeco_default` −27.1%, `emulator_welcome` −14.3%, `city` −11.5%, `nest`
−11.3%.

**It is the default, and the goldens were rebaked for it.** The float path is
not deleted: it remains selectable as `displaylist-float`, keeps its own goldens
in `golden-float/`, and is gated by `make verify-float` inside `make ci`. A
superseded implementation that still runs is a comparison that can be re-made;
one that has been deleted is an argument.

## I predicted this would lose, and the reason I gave was wrong

The earlier argument in `2026-09-03-sine-table.md` was: both targets are ESP32
with a single-precision FPU, so Q16.16 trades a one-cycle `MUL.S` for a
32x32->64 multiply and a shift, and cannot win.

That framing assumed fixed point means *doing the same arithmetic in integers*.
It does not have to. A pattern coordinate along a scanline is affine in x:

```
b(x+1) = b(x) + d          d constant for the whole row
```

In float that recurrence is unusable — repeated addition drifts — so the float
loop recomputes `im0*x + m2y + im4` from scratch at every pixel, then multiplies
by the reciprocal of the scale, then converts to int. **In fixed point the
recurrence is exact**, because integer addition is exact, and `>> 16` is an
exact floor for negative values too, so the conversion disappears with it.

Per pixel, per axis:

| | float path | Q16.16 path |
|---|---|---|
| multiply | 2 (`im0*fx`, `*rcp`) | 0 |
| add | 2 | 1 |
| float->int | 1 | 0 (arithmetic shift) |

The win is not a faster multiply. It is **not multiplying**, and not moving
values between the FPU and integer register files 40,000 times a frame. An FPU
does not help with work you no longer do.

The device deltas are *larger* than the host's (−42.4% vs −35.9% on
`op_draw_asset`), which is the same story from the other side: on x86-64 the
eliminated operations are cheap, on Xtensa they are not.

## Method

- `DisplayListRenderer::setFixedPointEnabled(bool)`, off by default, plumbed to
  `MicroPatternsDrawing`. Registered as host render path `displaylist-fixed`, so
  `compare-paths` and `bench --path` work on it with no further wiring — the
  seam `tools/host_harness/src/render_path.h` was built for.
- Converted: the two inner loops of `fillRect`, the two of `drawAsset`, and
  `fillCircle` — the last by a different route, see below.
- In `fillRect` and `drawAsset`, span narrowing stays in float. It is O(log n)
  per scanline, not per pixel, and leaving it alone means **the set of pixels
  visited is identical** — the change is isolated to what happens inside the
  span. `fillCircle` is the exception, and deliberately so: there the span
  itself was the opportunity.
- Untouched: `FILL_PIXEL`, `LINE`, `RECT`, `CIRCLE` outlines. They rasterise
  through integer Bresenham/midpoint code already. They are the control group.
- Device numbers: both paths alternate per rep inside ONE firmware run. Two
  flashes could not rule out a difference in clock, temperature, heap layout or
  flash cache; alternating inside one run puts both under identical conditions.

### Proving the fast path actually runs

`compare-paths` reported 21/21 identical on the first attempt — which is also
exactly what a fixed path that silently never executed would report. Proving
which world we were in meant swapping `patOn`/`patOff` inside both fixed loops
and re-running: `op_fill_rect_pattern_rot` then differed by **517,506 pixels**.
The loops run. The sabotage was reverted and equivalence re-checked.

An equivalence gate that cannot fail is not a gate — but "go and sabotage it" is
not a procedure anyone will repeat, so it is not a fix either. **The renderer now
counts the pixels it emits through a fixed-point inner loop**
(`DisplayListRenderer::getFixedPointPixels()`, accumulated per span so it costs
nothing in the loop it measures), `compare-paths` prints it for both paths, and a
path whose name advertises fixed point while reporting zero **fails** instead of
passing:

```
18 identical, 3 differing
fixed-point pixels: displaylist=0  displaylist-fixed=4981473
```

That gate was itself verified by forcing every range check to fail, so the fixed
path always fell back. It printed `21 identical, 0 differing` — a clean false
pass under the old gate — followed by:

```
fixed-point pixels: displaylist=0  displaylist-fixed=0
FAIL: path "displaylist-fixed" advertises fixed point and executed none.
      Every comparison above is vacuous.
```

## Result — device, per operation

Watchy, 200x200, min of 7 reps, milliseconds of rasterisation:

| operation | float | fixed | delta |
|---|---|---|---|
| `op_draw_asset` | 36.17 | 20.82 | **−42.4%** |
| `op_fill_circle_pattern_rot` | 49.51 | 29.43 | **−40.6%** |
| `op_fill_rect_pattern_rot` | 59.21 | 39.75 | **−32.9%** |
| `op_fill_circle` | 35.59 | 24.59 | **−30.9%** |
| `op_fill_rect_pattern` | 42.21 | 35.90 | **−15.0%** |
| `op_fill_rect_solid` | 32.97 | 32.97 | +0.0% |
| `op_fill_pixel` | 2.05 | 2.05 | +0.0% |
| `op_line` | 5.53 | 5.53 | +0.0% |
| `op_rect_outline` | 2.35 | 2.35 | +0.0% |
| `op_circle_outline` | 4.12 | 4.12 | +0.0% |

### fillCircle: the win is not fixed point

`fillCircle` is the one place where the biggest saving came from deleting work
rather than from cheaper arithmetic, and it is worth separating.

The float path inverse-transforms **every pixel in the span** for the sole
purpose of asking "is this inside the circle?", then discards the coordinates
unless the fill is patterned. It does not have to. The matrix is rigid and the
scale uniform, so a circle maps to a circle: `|base - centre| <= r` is exactly
`|screen - Centre| <= r*SCALE`, answerable in screen space with no transform.

And it is answerable per SCANLINE, not per pixel: a horizontal line crosses a
circle exactly twice, so a row's inside-pixels are one contiguous run of
half-width `sqrt(R^2 - dy^2)`. One square root per row, against a transform plus
two multiplies plus a compare per pixel. The old `narrowSpan` bisection goes with
it — it produced a *conservative* span (`|dx| <= r` and `|dy| <= r`, necessary
but not sufficient) and still tested every pixel inside it. The new span is
exact, so nothing inside it is tested at all.

Hence `op_fill_circle` (solid, no pattern) at **−30.9%** despite containing no
pattern arithmetic to speed up. The Q16.16 DDA then supplies the pattern lookup
on top, taking `op_fill_circle_pattern_rot` to −40.6%.

How close is the new span to the old test? Probed across rotation, scale, radius,
clipping and sub-pixel centre placement, 9 solid-disk configurations:
**8 byte-identical, 1 differing by 2 px** (an off-centre disk translated to
(481,271), rotated 7 degrees, radius 177). The two are mathematically equivalent
for a similarity transform and differ only in where a boundary pixel rounds.

An earlier version of this file claimed the span was byte-identical outright, on
the strength of one unrotated case. It is not, and one configuration is not a
proof — the same generalising-from-one-sample error as the "every changed pixel
is an edge pixel" claim in `2026-09-03-sine-table.md`.

One root per row was left as a `sqrtf`. At most 540 rows against ~40,000 pixels,
it is already below the noise; a table or an incremental integer half-width would
be optimising the 0.1%.

Five of ten read **+0.0%**, to the microsecond. Those are the paths the change
does not touch, and they are the control that makes the other five credible:
against a ±5% noise floor established by the `parse` phase, an untouched path
reading exactly zero says the measurement is clean and the change is isolated.

## Result — device, whole scripts

| script | float | fixed | delta |
|---|---|---|---|
| `artdeco_default` | 59.43 | 43.33 | −27.1% |
| `emulator_welcome` | 43.98 | 37.67 | −14.3% |
| `city` | 56.96 | 50.42 | −11.5% |
| `nest` | 12.88 | 11.42 | −11.3% |
| `prims` | 9.58 | 9.58 | +0.0% |
| `bounds` | 0.67 | 0.67 | +0.0% |
| `i32` | 0.44 | 0.44 | +0.0% |

A quarter off the rasterisation of the default art-deco script, on the hardware
that actually draws it. For context, `docs/measurements/2026-08-27-m5paper-baseline.md`
puts rasterisation at 91% of compute.

## What it costs

**The two paths are not byte-identical, and cannot be made so.** Not because of
accumulated drift — that part is small and boundable — but because Q16.16
quantises a coordinate to 1/65536, and wherever the float value sits closer than
that to an integer, the two floors disagree. Bit-exactness with a float path is
unavailable at any fixed-point precision short of replicating float's own
rounding, which would forfeit the entire win.

Measured divergence, `compare-paths displaylist displaylist-fixed`:

| corpus | result |
|---|---|
| `tools/host_harness/corpus` (21 cases) | 3 differ: `artdeco_default`, 34 / 34 / 31 px of 518,400 — **0.0066%** |
| `tools/device_bench/ops` (30 cases) | 6 differ: `op_fill_rect_pattern_rot` 894 px (**0.17%**), `op_fill_circle_pattern_rot` 664 px (**0.13%**) |

Solid fills and solid disks are byte-identical; only patterned surfaces move.

**Each renderer has its own goldens.** `golden/` pins the default (fixed);
`golden-float/` pins the float path, baked with `make bake-float` and gated by
`make verify-float`, which is part of `make ci`. One shared set could not serve
both: it would force a choice between declaring one renderer broken and leaving
the other ungated. `make compare-float` reports the distance between them and
exits non-zero because they do differ — a report to read, not a gate to keep
green.

The 894-pixel case is a deliberately adversarial probe: a full-canvas 4x4
pattern rotated 23 degrees, i.e. the maximum possible density of pattern-cell
boundaries for the drift to flip. Every divergence is a pattern cell resolving
one way rather than the other at its own edge; none is a shape in the wrong
place.

Where they differ, the fixed path is arguably the *more* correct of the two: the
DDA evaluates the affine function exactly, while the float path accumulates its
own rounding through a multiply, two adds and a reciprocal multiply per pixel.
"Differs from float" is not the same as "wrong", and neither is it a reason on
its own to prefer float.

But by this project's own rule — *an optimisation that changes the output is not
an optimisation, it is a different renderer* — that makes this a change to
adopt deliberately, with the goldens rebaked, or not at all. It is left
selectable and off precisely so that decision can be made on these numbers
instead of in advance of them.

## What is left

- **`FILL_PIXEL`, `LINE`, outlines**: already integer, nothing to convert.
- **`fillRect` solid** is unchanged at 32.97 ms and is now the most expensive
  operation with nothing done to it. It has no coordinate work left to remove --
  it is `emitPixel` across a span -- so the next saving there is in `emitPixel`
  and the occupancy map, not in arithmetic.
- **An exactly-correct pure-integer DDA is possible** and would remove the
  quantisation divergence entirely. See `docs/analysis/is-q16-16-integer-math.md`:
  the sine table made every per-pixel step an exact rational, which is the
  precondition for a Bresenham-style carry. Roughly double the Q16.16 inner loop,
  still far below float, and bit-identical across every toolchain.
- **The transform state itself** (`tx`/`ty`, the matrices) is still float. This
  change converts the per-pixel *consumer*, not the producer.

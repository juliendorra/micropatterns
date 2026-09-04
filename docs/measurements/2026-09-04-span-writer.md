# Writing spans instead of pixels — 2026-09-04

The per-operation profile (`2026-09-04-art-deco-profile.md`) found that
`op_fill_rect_solid` — no coordinates, no pattern, nothing but `emitPixel`
across a span — cost 33 ms for 40,000 pixels: **~200 cycles to set one bit.**
Almost all of it was `GxEPD2_BW::drawPixel` re-deriving rotation, mirror,
window origin, page and stride for every pixel, plus this renderer's own
per-pixel occupancy test, when every one of those answers is constant along a
scanline.

## The change

- The occupancy map is now **MSB-first**, the same bit order as a 1-bpp
  framebuffer. That is what lets a span's occupancy bytes and framebuffer bytes
  combine with one AND: `paint = mask & ~occupied; occupied |= mask`.
- `emitSolidSpan` builds a run's cover mask a byte at a time, folds the map in
  byte-wise, and hands the canvas one row of bytes through a new primitive,
  `mp_canvas_fill_mask_row`. It is a free function overloaded per canvas
  (the M5Paper binds the library's `M5EPD_Canvas` directly and cannot take a
  method), so there is still no vtable in the inner loop.
- The Watchy canvas writes the bytes directly into `GxEPD2_BW::_buffer`, which
  this fork makes public. The window fields it would need to trust are private,
  so instead of assuming them it **probes** at every canvas clear: two pixels
  through the library's own `drawPixel`, check the exact bytes changed, restore.
  Rotation, mirror, reverse, window origin, stride and page count are all
  validated by that one test; if it fails, the row goes through `drawPixel`
  per set bit.
- The host shim implements the primitive nibble-wise so the equivalence gate
  exercises the real span path. `compare-paths` refuses to pass a `-span` path
  that wrote no spans, and a non-span path that wrote any.

Selectable as `displaylist-span`; byte-identical to the default over the corpus.

## Step 1 — solid runs only, Watchy, ms of rasterisation

| | default | span | |
|---|---|---|---|
| `op_fill_rect_solid` | 33.40 | **8.21** | **−75.4%** |
| `op_fill_circle` | 25.52 | **5.82** | **−77.2%** |
| `confetti` | 43.85 | 19.64 | −55.2% |
| `city_by_telohtrab` | 39.22 | 18.31 | −53.3% |
| `eyes` | 45.74 | 24.04 | −47.5% |
| `city_2_by_telohtrab` | 43.84 | 25.50 | −41.8% |
| `circuits` | 44.42 | 28.24 | −36.4% |
| `disconnected` | 56.08 | 39.46 | −29.6% |
| `seascape_2` | 60.45 | 42.51 | −29.7% |
| `seascape_4` | 96.25 | 79.35 | −17.6% |
| `thunderstorms` | 74.76 | 62.42 | −16.5% |
| `art_deco_4` | 133.74 | 128.31 | −4.1% |
| `grid`, `reconnected`, every untouched probe | | | 0.0% |

The ones that did not move say what step 2 is: `grid` and `reconnected` are
patterned `FILL_RECT`, `art_deco_4` is 54 patterned circles, and
`op_fill_rect_pattern` / `op_fill_circle_pattern_rot` both read −0.0%. Step 1
touched solid runs only, on purpose, so that this table is attributable.

## Why the occupancy map stays

Measured first, because the plan assumed it earned its keep and that needed
checking. Disabling it (`nomap`) is 12–17% *faster* on every single-primitive
probe — that is the raw per-pixel cost of the test — and **+62% slower on
`art_deco_4`**, +26% on `seascape_4`, +21% on `seascape_2`. Wherever art
overlaps, the pixels it skips cost far more than the test. It loses only on
scripts that are one full-canvas fill. Folding it in byte-wise keeps the saving
and removes most of the cost.

## Step 2 — pattern runs and DRAW, Watchy, ms of rasterisation

Each pixel of a pattern loop now sets a bit in an ink mask instead of calling
`emitPixel`; the row is then written as two masked blits, `patOn` and `patOff`.
`DRAW` builds a cover mask from the asset's set bits and blits once. Output
byte-identical to the default over both corpora (21/21, 30/30); span rows
8,323 → 36,711.

| | default | span | |
|---|---|---|---|
| `op_fill_rect_pattern` | 37.31 | **14.89** | **−60.1%** |
| `op_fill_circle_pattern_rot` | 30.42 | 12.81 | −57.9% |
| `op_fill_rect_pattern_rot` | 40.98 | 18.41 | −55.1% |
| `op_draw_asset` | 20.23 | 11.00 | −45.6% |
| `city_by_telohtrab` | 38.92 | 16.09 | −58.7% |
| `grid` | 39.73 | 17.96 | −54.8% |
| `confetti` | 43.48 | 20.45 | −53.0% |
| `reconnected` | 44.11 | 21.35 | −51.6% |
| `circuits` | 43.84 | 21.39 | −51.2% |
| `city_2_by_telohtrab` | 43.30 | 21.41 | −50.6% |
| `eyes` | 45.41 | 22.50 | −50.4% |
| `disconnected` | 56.76 | 30.71 | −45.9% |
| `seascape_2` | 60.46 | 33.88 | −44.0% |
| `thunderstorms` | 74.67 | 51.95 | −30.4% |
| `seascape_4` | 94.48 | 76.22 | −19.3% |
| `art_deco_4` | 135.32 | 110.35 | −18.4% |
| untouched probes (`line`, outlines, `fill_pixel`) | | | +0.0% |

Against the float renderer of two days ago: `art_deco_4` 518 → 110 ms,
`grid` 59 → 18 ms.

The two laggards say where the remaining cost is. `seascape_4` is 117 `DRAW`s
and `op_draw_asset` only gained 45%: an asset's cover mask is sparse, and the
per-pixel bounds test and table read still run for every pixel of the AABB.
`art_deco_4` is 54 small patterned circles, and with `emitPixel` gone the per
pixel work that remains is the Q16.16 step, two integer `%` and a bit set — so
the modulo's share has grown. That is step 3.

## Step 3a — power-of-two masking: a regression, reverted

With `emitPixel` gone, the pattern loops' two integer `%` per pixel looked like
the next cost. First attempt replaced them with `v & (n-1)` when `n` is a power
of two and `((v % n) + n) % n` otherwise. Measured on the Watchy it was WORSE,
and worse on the default path too, since the loops are shared:

| | before | after | |
|---|---|---|---|
| `art_deco_4` (default) | 135.32 | 163.72 | **+21%** |
| `art_deco_4` (span) | 110.35 | 143.53 | +30% |
| `reconnected` (default) | 44.11 | 47.99 | +9% |
| `op_fill_circle_pattern_rot` (span) | 12.81 | 14.80 | +16% |

The fact that explains it, read from the twelve real scripts afterwards rather
than before: **patterns are 20×20 almost everywhere.** Eleven of twelve use the
recommended maximum; `grid` is 8×8 and the seascapes mix in 4×4. The mask never
fires, and the general case had gone from one division to two.

The regression is also a clean measurement of what the divisions cost: doubling
them added 21% to `art_deco_4`, so removing them entirely is worth about that.

The right trick is the older one. The coordinate advances by a constant
`|d| <= 1 cell` per pixel, so reduce it once per span into `[0, patW<<16)` and
keep it there with a compare-and-subtract per step. No division, and exact —
subtracting whole multiples of the modulus leaves `floor(v) mod patW`
unchanged. That is step 3b.

## Step 3b — wrap by subtraction: also slower, also reverted

Reduce the Q16.16 pattern coordinate once per span, then keep it in range with
a compare-and-subtract per step instead of `%`. Exact, and byte-identical on
the main corpus. On the Watchy:

| | step 2 | step 3b | |
|---|---|---|---|
| `op_fill_circle_pattern_rot` (span) | 12.81 | 15.49 | **+21%** |
| `op_fill_rect_pattern` (span) | 14.89 | 16.59 | +11% |
| `art_deco_4` (span) | 110.35 | 124.78 | +13% |

**Two different ways of removing the per-pixel division have now both lost.**
That is the measurement, and it overrides the reasoning that motivated both:
on this chip the integer divide in the pattern loop is not the cost it was
assumed to be, and a compare-and-branch pair per axis costs more than it does.
The 3a regression is better read as "adding a division and a test costs 21%"
than as "a division costs 21%". Not pursued further; both attempts kept out.

**And 3b was wrong as well as slow.** The ops corpus reported
`op_fill_rect_pattern_rot` differing by 27,985 pixels: the edit had wrapped `bx`
but not `by` in the rotated rect loop, so the pattern row index ran out of
range. `verify` passed and the main corpus compared identical, because the main
corpus contains no rotated patterned `FILL_RECT`. The ops corpus caught it.
`make compare-span` now checks the span path on BOTH corpora and is part of
`ci`; a gate is only as good as the cases in it.

## Step 4 — tile the unrotated pattern span instead of walking it

The Q16.16 walk is exact integer arithmetic, so along an unrotated row the ink
sequence repeats EXACTLY every `P = (patW<<16) / gcd(patW<<16, |dx|)` pixels —
for any scale, not only powers of two; only the size of `P` depends on it. The
mask bytes therefore repeat every `P / gcd(P, 8)` bytes. Build those by walking
(from the byte boundary, so every bit of them is right; the cover mask trims
`[x0,x1)` later), then copy. For a 20-wide pattern at `SCALE 1` that is 5 bytes
built and the rest memcpy'd. Falls back to the walk when the period is too long
to pay for itself. Byte-identical on both corpora; tiled rows 3,150 / 1,620.

| | step 2 | tiled | |
|---|---|---|---|
| `op_fill_rect_pattern` | 14.89 | **9.71** | **−35%** (solid is 8.17) |
| `reconnected` | 21.35 | 17.06 | −20% |
| `seascape_2` | 33.88 | 30.85 | −9% |
| `grid` | 17.96 | 18.73 | +4% — one 8x8 fill; unexplained, watch it |
| rotated, circle and DRAW probes | | | unchanged |

This is the answer to "would forcing power-of-two patterns help": the period
trick does not need it. 20x20 gives a 5-byte period; 8x8 gives 1. The language
stays as it is.

## Step 5 — clip the DRAW row to the asset's in-range run: exact, and flat

`ix` advances by a constant, so `0 <= ix < aw` holds on one contiguous run whose
ends solve in closed form from the same integer arithmetic as the walk. Applied
to the unrotated (row-hoisted) DRAW loop. Byte-identical; 21,012 rows clipped.

| | tiled | clipped | |
|---|---|---|---|
| `op_draw_asset` | 11.00 | 10.68 | −3% |
| `seascape_4` | 76.23 | 77.60 | noise |

Flat, and the reason is obvious afterwards: an UNROTATED asset's bounding box is
the asset. There were no out-of-range pixels to skip. The excess exists only for
rotated assets, which use the 2D loop this step did not touch. Kept, because it
is cheaper and exact; recorded as flat because it was.

## Step 6 — skip bytes the occupancy map has already painted

In the three 2D loops (rotated FILL_RECT, FILL_CIRCLE, rotated DRAW) every ink
bit computed for a pixel the map already holds is masked off in the emit. So:
walk the span a byte at a time and, when the occupancy byte is `0xFF`, advance
the accumulators by eight steps and compute nothing. Exact by construction.

**First version regressed the scripts it could not help.** Per-byte chunking
costs a compare and a loop setup per byte whether or not there is anything to
skip: `op_fill_circle_pattern_rot` +24%, `grid` +10%, `eyes` +5% — rows with no
occupied byte paying for a skip that never fires. Not committed.

Second version adds a one-pass scan per span; rows with no `0xFF` byte run the
plain walk untouched. Byte-identical on both corpora.

| | before | byte-skip v2 | |
|---|---|---|---|
| `art_deco_4` | 110.46 | **87.03** | **−21%** — paints 3.75x its pixels |
| `disconnected` | 31.29 | 27.65 | −12% |
| `seascape_4` | 77.60 | 75.56 | −3% |
| `op_fill_circle_pattern_rot` | 12.81 | 12.82 | flat |
| `grid` | 18.71 | 18.47 | flat |
| `eyes`, `confetti` | | | +2% after normalising a ~1% warmer run |

Against the float renderer of two days ago, `art_deco_4` is now **520 → 87 ms**.

## Step 7 — clip rotated DRAW rows: flat, reverted

Same closed-form clip as step 5, on the 2D loop, as the intersection of the `ix`
and `iy` runs. First version used int64 division (a libgcc call, four per row)
and made `seascape_4` **+11%**: its 117 stars are ~30 rows each, and per-row
setup outweighed the few excess pixels removed. int32 arithmetic plus a 24-px
short-row bypass brought it to `op_draw_asset` −3%, `seascape_4` +2%, all else
noise. Flat is not better; reverted.

What this step did leave behind: the pixel gate now also runs on the twelve
real scripts (`tools/device_bench/real/`, laid out by `gen_ops_corpus.py`).
Until now they had only ever been timed, never pixel-compared. 36/36 identical.

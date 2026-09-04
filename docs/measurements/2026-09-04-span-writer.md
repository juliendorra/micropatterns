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

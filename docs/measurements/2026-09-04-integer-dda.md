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

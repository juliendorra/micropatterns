# Two bounds bugs, and the one-line test that found them — 2026-09-03

Both bugs were in the code that computes a shape's screen-space extent. Neither
was a rounding artifact: one clipped every rotated filled disk, and the other
made **every axis-aligned `LINE` draw nothing at all**. Both had been in the tree
for as long as the display-list renderer, and no golden caught either.

Gated now by `tools/host_harness/corpus/bounds.mp`.

## The test

A shape's ink area is invariant under rotation. A circle is a circle at any
angle; a rotated rectangle covers the same number of pixels. So: render one
primitive at a spread of angles and count non-white pixels. Any primitive whose
count moves is losing coverage somewhere.

```
primitive                    0       5      11      17      22      23    spread
circle_outline            1020    1020    1020    1020    1020    1020    0.00%
draw_asset               24320   24318   24322   24320   24321   24323    0.02%
fill_rect                60000   60002   59998   59998   59998   60000    0.01%
fill_rect_pattern        30000   30003   29983   29988   30002   30004    0.07%
line                         0     399     393     383     371     369  100.00%  <== SUSPECT
rect_outline               996     994     978     952     925     917    7.93%  <== SUSPECT
fill_circle (before)    125676  125676  125116  125116  120132  120132    4.41%  <== SUSPECT
```

Two of the three suspects were real. The third was not, and saying why matters:

**`rect_outline` is not a bug.** A Bresenham line of length L at angle θ paints
`max(|dx|,|dy|) ≈ L·max(|cosθ|,|sinθ|)` pixels. At 22.5° that is L·0.924, so a
1000-pixel perimeter legitimately becomes 924. Measured: 925. The `line` row's
non-zero values are the same effect (401·cos 23° = 369, measured 369). This is
inherent to 8-connected thin-line rasterisation, not lost coverage — a spread
here is expected and the invariance test has to be read with that in mind.

## Bug 1 — every axis-aligned LINE was culled

`LINE` has no area, so a horizontal one has an AABB of exactly zero height. In
`display_list_renderer.cpp`:

```cpp
bounds.minY = floor(max(0.0f, unclippedVisualMinY));   // 270.0 -> 270
bounds.maxY = ceil (min(H,    unclippedVisualMaxY));   // 270.0 -> 270
if (bounds.minX >= bounds.maxX || bounds.minY >= bounds.maxY)
    bounds.isOffScreen = true;                          // 270 >= 270 -> culled
```

The test exists to catch shapes clipped entirely off-canvas. A zero-thickness
box trips it too, and the item is discarded before `drawLine` ever runs.

Measured on a 400-pixel line: **ROTATE 0, 90 and 180 rendered 0 pixels; ROTATE
1, 89 and 91 rendered all 401.** Nothing subtle — horizontal and vertical lines
simply did not exist.

Why no golden caught it: `prims.mp` is the only corpus script drawing `LINE`,
and it does so inside a `ROTATE $i` loop over `$INDEX` 0..11 — so its lines are
axis-aligned only at `$i = 0`, where the line it draws is `X1=0 Y1=10 X2=200
Y2=0`, which is not axis-aligned anyway. The fix changed **zero** goldens, which
is the clearest possible statement of the coverage gap.

**Fix:** give a degenerate dimension the half pixel on each side that the
rasteriser will actually touch, before the off-screen test runs. Safe for
occlusion, because marking reads the exact occupancy bitmap and these bounds only
delimit the region it scans.

## Bug 2 — rotated filled disks were clipped to octagons

`fillCircle` bounded itself by transforming **eight** points on the circle — four
cardinal, four diagonal via a `logical_radius * 0.7071f` offset — and taking
their min/max. That is the AABB of an *inscribed octagon*, which undershoots the
circle by `r·(1 − cos 22.5°)` = 7.6% of the radius.

Measured on a radius-200 disk (πr² = 125,663):

| ROTATE | ink px | width | |
|---|---|---|---|
| 0° | 125,676 | 400 | correct |
| 11° | 125,116 | 394 | |
| 22° | **120,132** | **372** | 4.4% of the area gone, eight flat sides |

The inside test was always the exact disk test `dx² + dy² ≤ r²`, and `narrowSpan`
narrows conservatively — the entire bug lived in those outer bounds.

**Fix:** centre plus radius, on all four sides. The transform matrix is rigid
(only `TRANSLATE` and `ROTATE` reach it; `SCALE` lives in a separate factor), so
a circle stays a circle and the screen radius is exactly `lr * scale`.

**Both neighbours already did it the right way**, which is what makes this a
consistency bug rather than a hard geometry problem:

- `drawCircle` (the `CIRCLE` outline) took the centre and derived the radius from
  the matrix column norms.
- `display_list_renderer.cpp` took the centre and the transformed radius length
  via `std::hypot`.

Only the filled path sampled. Three implementations of one question, and the odd
one out was wrong.

## A side effect worth having

Because the matrix is provably rigid, its columns are unit vectors, so every
"how long is the transformed radius" computation has the closed-form answer
`lr * scale`. That retired:

- 8 `transformPoint` calls per `FILL_CIRCLE`, down to 1
- 2 `sqrtf` per `CIRCLE` (they were computing √(c²+s²) on unit columns — 1.0 to
  within 1.5e-5)
- 2 `std::hypot` per circle in the bounds pass, same reason

Correctness and cost moved the same direction here, which is not usually the
deal on offer.

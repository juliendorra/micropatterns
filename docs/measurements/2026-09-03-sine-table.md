# The ROTATE sine table and the angle accumulator — 2026-09-03

`matrix_make_rotation` no longer calls `sinf`/`cosf`, and the runtime no longer
composes rotation matrices. This file is the measurement behind both changes and
behind the goldens they required rebaking.

> **Later that day** this turned out to matter for a reason nobody predicted.
> Making rotation a table of exact rationals is what makes a pure-integer DDA
> possible at all, and the rasteriser that followed is 15-42% faster per
> operation on hardware. See `2026-09-03-fixed-point-rasteriser.md` and
> `../analysis/is-q16-16-integer-math.md`. The numbers below are still scoped to
> the sine table alone -- binary sizes in particular were measured before the
> rasteriser work.

**Read the verdict first: this is not an optimisation.** It was adopted for
consistency and binary size. The speed effect is unmeasurable, and "Why it cannot
be faster" below says why that was predictable rather than surprising. The
accuracy effect is real, and took two attempts to get right.

## Method

- The same seven `.cpp` files from `M5Paper_MicroPatterns/src/`, built three
  ways: the original `sinf`/`cosf` renderer (the reference every number below is
  measured against), the first table version, and the shipped version.
- Renders from `mpharness` at `-O2`, 960x540, three fixed seeds (`counter=0
  12:34:56`, `counter=7 00:00:00`, `counter=42 23:59:59`).
- Corpus: the six `tools/host_harness/corpus/` scripts, the six
  `examples/scripts/` files, and the six scripts from
  `tools/device/backups/2026-08-27-143524/`. 18 scripts x 3 seeds = 54 renders.
- Rotation counts came from a build instrumented with a counter at
  `matrix_make_rotation`; determinant drift from a build instrumented at
  `matrix_invert`. Both were scaffolding and are not in the tree.
- The whole A/B was repeated through the WebAssembly build the online editor
  loads (`make verify-wasm` confirms it is byte-identical to the native
  goldens). Both toolchains flagged the same renders and the same first differing
  pixel in each.

**Host timings are not device timings.** See `tools/host_harness/README.md`. The
aggregate below is evidence that an effect is absent, not an ESP32 number.

## Why it cannot be faster

`ROTATE` is the only command needing trigonometry, and it needs it once, to fill
two slots of a 2x3 affine matrix. The floats that cost real time are the four
multiplies and four adds of the *inverse* transform, evaluated per pixel in
`micropatterns_drawing.cpp`. Neither change touches them.

| | |
|---|---|
| Rotation matrices built, worst of 54 renders | 109 |
| Renders building zero | 27 of 54 |
| Canvas pixels in that same worst render | 518,400 |
| Rasterisation share of device compute (2026-08-27 baseline) | 91% |

The 2026-08-27 baseline measured `circuits` at 2,350 ms of rasterisation out of
2,583 ms of compute. `circuits` calls `matrix_make_rotation` **zero** times.

Host benchmark, 9 reps over all 54 renders: **+0.76% aggregate**. Over the same
runs the parse phase — which contains no trigonometry at all — moved by up to
±10%. The signal is smaller than the noise floor of the instrument. A
microbenchmark of `matrix_make_rotation` alone, 2M calls, gave 4.3–5.0 ns
(`sinf`) against 4.0–4.3 ns (table): also inside run-to-run spread, because on
x86-64 `sinf` is cheap and `matrix_identity` dominates either way. On an ESP32
the ratio moves in the table's favour — there is no hardware sine — but it moves
a term called at most 109 times.

## Attempt 1: the table alone, and why it was not enough

A 360-entry Q15 table indexed by whole degrees. Not an approximation of a
continuous sine: `ROTATE`'s operand comes from `resolve()`, which returns an
`int`, so 360 entries cover the entire input domain with nothing to interpolate.

It had two defects, neither visible in a single-lookup error bound.

**It drifted.** `ROTATE` composed cumulatively — `matrix = matrix * R(d)` — and a
rounded `(c, s)` has `c² + s² = 1 ± 6e-5`, so every composition scaled the matrix
slightly and the error compounded with the rotation count. Measured at
`matrix_invert` across the corpus:

| Rotation source | Inversions | Worst \|det − 1\| |
|---|---|---|
| `sinf`/`cosf` | 1,382 | 1.3e-6 |
| Q15 table, composed | 1,382 | **6.6e-3** |

6.6e-3 is a 0.66% scale error — up to ~6 px at the far edge of a 960 px canvas,
and it appeared in exactly the render with the most rotations. In
`thunderstorms__c42` differing pixels reached 22 px from any silhouette edge: the
shapes held, but the phase of a diagonal pattern fill inside a rotated region
slid, because the inverse transform maps screen space into pattern space and the
drift landed elsewhere in the tile.

**The identity rotation was not the identity.** Q15 puts 1.0 at 32768, which does
not fit an `int16_t`, so `sin(90°)` was clamped to 32767 and `cos(0)` came back
as 0.99997. Every `TRANSLATE` issued at angle 0 — most of them — was quietly
scaled by that. This one is easy to miss because it is invisible in a
worst-case-error bound and shows up only as small diffs on scripts that barely
rotate.

## Attempt 2: what shipped

**Carry the angle, not the matrix.** The transform is only ever built from
`matrix_make_translation` and `matrix_make_rotation` — `CMD_SCALE` sets a
separate integer `scale` and never touches it — so it is always a rigid
transform, exactly and completely described by `(angleDeg, tx, ty)`:

- `ROTATE d` → `angleDeg = (angleDeg + d) mod 360`, exact integer arithmetic.
- `TRANSLATE dx,dy` → `t += R(angleDeg) · (dx,dy)`.

Both matrices are then rebuilt from that state. The linear part never compounds,
so the error stays bounded at one table entry however many `ROTATE`s a script
issues. `matrix_multiply` and `matrix_invert` have no callers left and are gone;
the inverse is the transpose over a closed-form determinant `c² + s²`, which
needs no 2x2 minor and can never be zero, so the "not invertible" branch is gone
with them.

The determinant division is **not** optional, and dropping it was the third thing
this got wrong before it got right. A bare transpose is only the inverse of a
perfectly orthonormal matrix; a table `(c, s)` is orthonormal to one ulp, and the
rasteriser uses both matrices — the forward one to place a shape, the inverse one
to decide which pattern pixel each screen pixel samples. They have to agree.

**Store the table as `int32_t`.** ±32768 is then representable, so the four
cardinal angles are exact (0, +1, 0, −1) and the interior keeps full Q15
precision. Worst-case deviation from `sinf` drops from 3.05e-5 (with the clamp)
to 1.52e-5. The extra 720 bytes are noise against the 4,585 saved by dropping
libm's sine.

## Result

Pixels differing from the original `sinf` renderer, across all 54 renders:

| Build | Differing px | Renders identical to `sinf` |
|---|---|---|
| Table, composed matrices (attempt 1) | 3,701 | 39 of 54 |
| Table + angle accumulator (shipped) | **279** | **41 of 54** |

A 13x reduction. The worst render went from 1,777 px to 30; the 109-rotation
render that showed the pattern-phase slide went to **0**. One render moved the
other way, `thunderstorms__c7`, 23 px to 33 — noise at that scale.

Worst of the three seeds per script:

| Script | Rotations | Attempt 1 | Shipped |
|---|---|---|---|
| `device backup s5 (Thunderstorms)` | 109 | 1432 | 33 |
| `examples/scripts/seascape3.mp` | 64 | 98 | 98 |
| `device backup s4 (Re/Connected)` | 58 | 1777 | 30 |
| `examples/scripts/eyes.mp` | 40 | 14 | 14 |
| `examples/scripts/seascape2.mp` | 36 | 2 | 0 |
| `corpus/prims.mp` | 12 | 0 | 0 |
| `corpus/artdeco_default.mp` | 7 | 4 | 4 |
| `device backup s3 (Eyes)` | 7 | 3 | 3 |
| `corpus/emulator_welcome.mp` | 1 | 0 | 0 |
| the nine scripts with no `ROTATE` | 0 | 0 | 0 |

Binary size, against the original `sinf` build:

| Artifact | Before | After | Delta |
|---|---|---|---|
| `mp_render.wasm` | 105,721 | 101,136 | −4,585 |
| `mp_render_m5paper.wasm` | 155,827 | 151,533 | −4,294 |
| `mp_render_watchy.wasm` | 154,642 | 150,342 | −4,300 |

Three goldens were rebaked: `artdeco_default` at all three seeds, 4 + 4 + 1
pixels, every one of them on a silhouette edge. This is the case
`tools/host_harness/README.md` calls "intended, understood, and reviewed".

## What this does NOT do

It does not make the renderer integer-math. `angleDeg` is exactly integer and
`scale` always was, but `tx`/`ty`, the two matrices, and every per-pixel
transform remain `float`. Going further means Q16.16 through the scanline loop,
where the argument is much weaker: both current targets (M5Paper and Watchy) are
ESP32 with a single-precision FPU, so it would trade a single-cycle `MUL.S` for a
32x32→64 multiply and a shift, on an inner loop already span-narrowed and
row-hoisted. The case for it is portability to an FPU-less target, not speed on
this one.

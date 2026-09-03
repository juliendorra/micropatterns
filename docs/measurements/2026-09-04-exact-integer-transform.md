# The exact-integer transform, measured — 2026-09-04

Removing the last floats from the rasteriser's *forward* path: line and rect
endpoints, circle centres, and the filled-circle scanline span including its
`sqrtf`. Selectable as render path `displaylist-int`.

**Result: byte-identical output, and between +0.0% and +0.9% on whole scripts —
free within measurement error.** The isolated operations it touches cost between
+0.3% and +2.6%. It is not a speedup and was never going to be; what it buys is
exactness and a step toward not linking soft-float at all.

Measured on a Watchy with all three paths alternating inside one firmware run.

## Why this is exactly representable

Every input is already a whole number, and the sine table gave them all the same
denominator:

- `SCALE` is an integer `>= 1`; `TRANSLATE` operands are integers (`resolve()`
  returns `int32_t`).
- `cos` and `sin` are table entries `C/32768`, `S/32768` with `C`, `S` exact
  integers.
- The angle accumulator means the linear part is always ONE table entry.
- So `tx`/`ty`, accumulating `R(angle) * (dx,dy)`, stay whole numbers over
  32768 forever: adding fractions that share a denominator never changes it.
- A circle's screen radius is `lr * SCALE` — both whole, so `R` is exact and
  `R^2 - dy^2` is an exactly representable whole number whose root is an integer
  square root.

Nothing rounds until a pixel index is finally needed, which is why the output is
identical rather than merely close.

## Result

| operation | float | Q16.16 | vs float | + integer xform | vs Q16.16 |
|---|---|---|---|---|---|
| `op_draw_asset` | 36.18 | 20.85 | −42.4% | 20.85 | +0.0% |
| `op_fill_circle_pattern_rot` | 49.55 | 29.36 | −40.7% | 30.00 | +2.2% |
| `op_fill_rect_pattern_rot` | 59.23 | 39.77 | −32.9% | 39.77 | +0.0% |
| `op_fill_circle` | 35.63 | 24.66 | −30.8% | 25.30 | **+2.6%** |
| `op_fill_rect_pattern` | 42.26 | 35.95 | −14.9% | 35.95 | +0.0% |
| `op_rect_outline` | 2.32 | 2.32 | +0.0% | 2.36 | +2.0% |
| `op_line` | 5.50 | 5.50 | +0.0% | 5.54 | +0.7% |
| `op_circle_outline` | 4.11 | 4.11 | +0.0% | 4.12 | +0.3% |
| `op_fill_rect_solid` | 33.02 | 33.02 | +0.0% | 33.02 | +0.0% |
| `op_fill_pixel` | 2.07 | 2.07 | +0.0% | 2.07 | +0.0% |

Whole scripts — the number that decides whether this matters:

| script | Q16.16 | + integer xform |
|---|---|---|
| `city` | 50.53 | +0.0% |
| `emulator_welcome` | 37.71 | +0.0% |
| `nest` | 12.53 | +0.0% |
| `i32` | 0.46 | +0.0% |
| `artdeco_default` | 43.41 | +0.2% |
| `prims` | 9.52 | +0.5% |
| `bounds` | 0.65 | +0.9% |

**The overhead is per-item and per-row, never per-pixel**, which is why a probe
built to hammer one primitive shows 2% and a real script shows nothing. It is
also constant: the work does not depend on the data, only on the item count.

## The two things that were tried and did NOT help

Recorded because both looked obviously right.

**A division-free integer square root.** `mp_isqrt64` first used Newton's
method, which needs a division per iteration, and 64-bit division on Xtensa is a
libgcc call — precisely the kind of library dependency this work exists to
remove. Replacing it with a restoring binary square root (shifts, adds and
compares only, no division) changed `op_fill_circle` from +2.4% to +2.5%:
**nothing**. The square root was never the cost. The division-free version is
kept anyway, because it is the one that does not call libgcc, which is the point.

**A 32-bit fast path in the point transform.** `xformPointQ15` uses `int64`
because a script may `TRANSLATE` by any `int32`. A guarded 32-bit path for the
overwhelmingly common case (`|X|,|Y| <= 2^14`, `|t| <= 2^26`, where the whole
expression provably cannot overflow) also changed nothing measurable. Kept for
the same reason: it is correct, and it is what an FPU-less target would want.

## The one thing that DID help, which was a bug of mine

`op_rect_outline` was +4.1%. The integer branch in `drawRect` had been inserted
*after* the four float `transformPoint` calls, so the integer path was computing
every corner **twice** — once in float, once in integers — and the float work was
then thrown away. `drawCircle` and `fillCircle` had the same shape.

Guarding the float computations took `op_rect_outline` from +4.1% to **+2.0%**.
Half the regression was self-inflicted, and it was invisible in the equivalence
gate because doing the work twice gives the same answer.

`op_fill_circle` did not move (+2.6% before and after), which is the cross-check:
its centre is computed once against 200 scanlines, so removing one duplicate
transform could not have mattered. The remaining cost there is the genuine
per-row `dyN * dyN` and the root.

## What is left, and what would actually pay

- **The filled-circle span still costs 2.6%.** It could avoid both the 64-bit
  multiply and the root entirely by stepping the half-width down the rows the way
  Bresenham draws a circle — additions and comparisons only. Not built, because
  the whole-script effect is 0.0% and the risk is in bounds arithmetic, which is
  where both of 2026-09-03's real bugs lived.
- **Float is not gone.** The display-list bounds pass, `fillRect`/`drawAsset`
  AABB corners, and the inverse matrix are all still float. Until every one is
  converted the binary still links soft-float, so the *dependency* argument —
  the actual reason to do this on an ESP32-C3 or RP2040 — is not yet cashed in.
  This change is one step of several, not the finish line.
- `displaylist-int` is byte-identical to the default, so it needs no goldens of
  its own. `make compare-int` is in `ci`: if the two ever stop agreeing, that is
  a bug rather than a trade-off, unlike `compare-float`.

## The pattern worth keeping

2026-09-03 predicted fixed point would lose because an FPU makes integer
arithmetic no cheaper. That prediction was **wrong about the Q16.16 fills** —
which won 15–42% by not multiplying at all — and **right about this change**,
which does the same arithmetic in integers and duly costs 0–2.6%.

The lesson is not "integers are fast" or "integers are slow". It is that
substituting a representation buys nothing, and removing work buys everything.
Both experiments were needed to say that with numbers instead of opinions.

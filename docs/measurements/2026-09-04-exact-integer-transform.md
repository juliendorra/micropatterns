# The exact-integer transform, measured — 2026-09-04

Removing the last floats from the rasteriser's *forward* path: line and rect
endpoints, circle centres, and the filled-circle scanline span including its
`sqrtf`. Selectable as render path `displaylist-int`.

**Result: byte-identical output, and it costs up to 13% on the heaviest real
script.** Do not turn it on for an ESP32. It exists for a target without an FPU.

That verdict is the opposite of what this file said after two rounds, and the
reason matters more than the verdict: the synthetic corpus said −0.5% to +0.2%,
"free within measurement error". The twelve scripts actually on the device said
`art_deco_4` **+13.2%** and `seascape_2` **+5.3%**. See "The corpus was lying".

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

Final, milliseconds of rasterisation, min of 7 reps:

| operation | float | Q16.16 | vs float | + integer xform | vs Q16.16 |
|---|---|---|---|---|---|
| `op_draw_asset` | 36.22 | 20.87 | −42.4% | 20.89 | +0.1% |
| `op_fill_circle_pattern_rot` | 49.43 | 29.38 | −40.6% | 30.03 | +2.2% |
| `op_fill_rect_pattern_rot` | 59.26 | 39.79 | −32.8% | 39.79 | −0.0% |
| `op_fill_circle` | 35.48 | 24.66 | −30.5% | 25.31 | **+2.6%** |
| `op_fill_rect_pattern` | 42.29 | 35.98 | −14.9% | 35.98 | +0.0% |
| `op_line` | 5.52 | 5.52 | +0.0% | 5.54 | +0.3% |
| `op_rect_outline` | 2.35 | 2.35 | +0.0% | 2.36 | +0.4% |
| `op_circle_outline` | 4.11 | 4.11 | +0.0% | 4.11 | +0.0% |
| `op_fill_rect_solid` | 33.04 | 33.04 | +0.0% | 33.04 | +0.0% |
| `op_fill_pixel` | 2.08 | 2.08 | +0.0% | 1.98 | **−4.9%** |

Whole scripts — the number that decides whether this matters:

| script | Q16.16 | + integer xform |
|---|---|---|
| `nest` | 12.42 | −0.5% |
| `emulator_welcome` | 37.75 | −0.2% |
| `prims` | 9.52 | −0.0% |
| `city` | 50.49 | +0.0% |
| `artdeco_default` | 43.43 | +0.2% |
| `i32` | 0.46 | +1.1% |
| `bounds` | 0.69 | +1.8% |

`op_fill_pixel` ending up 4.9% FASTER than float is the one genuine win: it is
120 one-pixel items, so it is dominated by per-item transform cost, and an
integer multiply-add beats a float one once the divisions are gone.

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

## Round two: the AABB corners, and a 50% regression that taught the most

Converting the remaining per-item corner transforms -- `fillRect`, `drawAsset`,
`drawPixel`, `drawFilledPixel` -- made `op_fill_pixel` **50% SLOWER** (2.80 ms to
4.20 ms) while every other probe stayed flat.

`op_fill_pixel` is 120 one-pixel items that paint 33 ink pixels between them, so
it is almost pure transform cost with no drawing to hide behind. That is what
made it the probe that caught this, and it is why a corpus needs a case with a
bad work-to-overhead ratio in it.

The cause was not int64 and not the corner count:

```c
int32_t mp_sin_q15(int deg) {
    deg %= 360;                 // <- an integer DIVISION
    ...
}
```

`xformPointQ15` called it **twice per point** for cos and sin, so a four-corner
AABB paid eight integer divisions per item. The angle cannot change within a
transform snapshot, so both entries now resolve once, in `TransformSnapshot`.

| probe | corners per item | before hoist | after hoist |
|---|---|---|---|
| `op_fill_pixel` | 4 | **+50.0%** | **−4.9%** |
| `op_rect_outline` | 4 | +2.1% | +0.4% |
| `op_line` | 2 | +0.6% | +0.3% |
| `op_circle_outline` | 1 | +0.2% | +0.0% |

After the fix the integer transform is at parity or slightly ahead everywhere
except the filled-circle span, and whole scripts come out at −0.5% to +0.2%.

Note that the earlier "the cost scales with the number of int64 multiplies"
reading was wrong twice over: first it was double work (round one), then it was
integer division. The corner count correlated with both, which is exactly how a
plausible wrong cause survives two rounds of measurement.

## What actually links, and what "remove the last floats" really means

`xtensa-esp32-elf-nm` on the firmware, reading the symbol TYPES rather than
assuming:

| symbol | address | type | |
|---|---|---|---|
| `__addsf3`, `__subsf3`, `__mulsf3` | `0x4000…` | **A** | ROM, costs no flash |
| `__floatsisf`, `__fixsfsi`, `__floatunsisf` | `0x4000…` | **A** | ROM |
| `__divsf3` | `0x40255a9c` | **T** | **linked into flash** |
| `sqrtf`, `__ieee754_sqrtf` | `0x4021…` | **T** | **linked into flash** |

The ESP32's FPU does add, multiply and int conversion in hardware, and the ROM
carries the helpers anyway. **Only float division and square root cost flash.**
"The binary still links soft-float" was too broad; the real target is two
symbols, and they come from:

- `sqrtf` -- the float circle span, which the integer path already avoids.
- `__divsf3` -- `exactReciprocal` (once per fill item), `invSf` (once per
  scanline), the `v / sf` fallbacks, and `1.0f / det` in `matrix_set_rigid`.

And the architectural point that follows: **as long as `displaylist-float`
remains selectable at runtime, both symbols stay in the binary no matter what
the integer path does.** A real FPU-less port has to compile the float path out
with `#if`, not choose between them at run time. That is a build-configuration
decision, not a rasteriser one, and it is worth knowing before more of the
renderer is rewritten in pursuit of it.

## The corpus was lying, and only the real art caught it

Everything above was measured on ten synthetic probes and seven small corpus
scripts. Adding the **twelve scripts actually on the device** — from the
2026-09-03 SPIFFS backup, compiled into the bench — changed the conclusion.

Q16.16 against float, on real art, milliseconds of rasterisation:

| script | float | Q16.16 | |
|---|---|---|---|
| `art_deco_4` | 514.75 | 132.99 | **−74.2%** |
| `thunderstorms` | 123.22 | 74.13 | −39.8% |
| `disconnected` | 86.81 | 55.57 | −36.0% |
| `grid` | 57.42 | 38.58 | −32.8% |
| `seascape_4` | 140.34 | 96.28 | −31.4% |
| `confetti` | 59.55 | 43.44 | −27.1% |
| `city_2_by_telohtrab` | 58.70 | 43.41 | −26.0% |
| `circuits` | 59.21 | 44.01 | −25.7% |
| `eyes` | 58.06 | 45.38 | −21.8% |
| `seascape_2` | 75.15 | 58.93 | −21.6% |
| `reconnected` | 52.42 | 42.29 | −19.3% |
| `city_by_telohtrab` | 47.99 | 38.82 | −19.1% |

**The Q16.16 win is much bigger on real work than the corpus suggested** — −19%
to −74%, against the corpus's −11% to −27%. `art_deco_4` alone goes from 515 ms
to 133 ms.

The exact-integer transform on top of that:

| script | Q16.16 | + integer xform | |
|---|---|---|---|
| `art_deco_4` | 132.99 | 150.54 | **+13.2%** |
| `seascape_2` | 58.93 | 62.05 | **+5.3%** |
| the other ten | — | — | −0.1% to +0.2% |

**The two that regress are the only two that use `FILL_CIRCLE`.** Every script
without it is flat to the tenth of a percent. So the +2.6% measured on
`op_fill_circle` in isolation was not a rounding curiosity: in a script made of
filled circles it compounds into 13%.

The synthetic corpus could not have found this. `op_fill_circle` was in it and
did report +2.6% — what was missing was any script *made of* filled circles, so
the per-op cost never carried the weight it carries in real work.

> A probe tells you what an operation costs. Only the real scripts tell you how
> much of that operation there is.

## What is left, and what would actually pay

- **The filled-circle span is now the whole problem, not a curiosity.** At +2.6%
  per operation it costs 13% on `art_deco_4`, and it is the only reason
  `displaylist-int` cannot be adopted. It could avoid both the 64-bit multiply
  and the root by stepping the half-width down the rows the way Bresenham draws
  a circle — additions and comparisons only. Now clearly worth building; it was
  dismissed one round earlier on the strength of a corpus showing 0.0%.
- **Float is not gone.** Still float: the display-list bounds pass, the Q16.16
  DDA setup (which derives its start and increment from the float inverse
  matrix, `invSf` included), `narrowSpan`, and `matrix_set_rigid`'s `1.0f/det`.
  The AABB corners in `fillRect`/`drawAsset`/`drawPixel`/`drawFilledPixel` ARE
  converted as of round two.
- **The DDA setup is the last hard one, and it is not mechanical.** Its start
  and increment are exactly `(C*dxN + S*dyN) / (D*s)` and `C*32768 / (D*s)` with
  `D = C^2 + S^2` — exact rationals, so it is doable. But rendering them as
  Q16.16 needs a 64-bit division, which trades `__divsf3` for `__udivdi3`: a
  different library call, not no library call. Removing BOTH means either an
  approximation (`D ~= 32768^2`, a 6e-5 relative error that WOULD change output)
  or a reciprocal computed once per item and multiplied thereafter. That is a
  precision decision, not a conversion — and worth taking deliberately, given
  every pixel difference this project has shipped so far came from exactly such
  a trade.
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

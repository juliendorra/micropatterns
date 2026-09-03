# Is Q16.16 "integer math"? And could we do it without fixed point?

Two separate questions, and the second has a better answer than I expected.

## 1. Is Q16.16 integer math?

**At the machine level, unambiguously yes.** Every operation in the Q16.16 inner
loops is `int32` add, arithmetic shift, and compare. No `float` type appears, no
FPU register is touched, no `libm` symbol is reachable. On an FPU-less target —
ESP32-C3, RP2040, Cortex-M0 — it runs at full speed where the float path would
fall back to soft-float.

**At the semantic level, it is integers standing in for fractions.** Q16.16
represents 0.5 as the integer 32768 and agrees to treat the low 16 bits as a
fraction. That is a *representation* choice, not a change of number system. So if
"integer math" means "no fractional quantities anywhere", Q16.16 does not
qualify.

**But that is not what the MicroPatterns spec means.** `README.md` says: "All
coordinates, parameters, and calculations use integers. Division truncates
towards zero." That is a statement about the **language** — what a script can
express and what `LET` computes. It is true today and none of this work touches
it. `resolve()` returns `int32_t`; a script cannot produce, store or observe a
fractional value.

The renderer is a different layer. It has to answer "which pattern cell does the
centre of screen pixel (37, 40) land in, under a rotation of 23 degrees" — and
that question has a fractional answer no matter how the language is specified.
The honest framing:

> The language is integer. The renderer's *arithmetic* is now integer too. The
> *quantities* it computes are still fractional, because the geometry is.

## 2. Could it be done without fixed point at all?

Yes — and specifically because of the sine table.

The classic pure-integer technique is Bresenham's: rather than representing a
fractional value at all, keep an integer part and an integer **error numerator**
over a fixed denominator, and carry:

```
bi += q;                       // integer part of the step
e  += r;                       // remainder numerator
if (e >= D) { e -= D; bi++; }  // carry, exactly, forever
```

This is exact for all time provided the step is a **rational number with an
integer numerator and denominator**. So: is ours?

**It is, and it was not before.** Walk the chain:

- `SCALE` is an integer `>= 1` (`micropatterns_runtime.cpp`, `CMD_SCALE`).
- `TRANSLATE` operands are integers (`resolve()` returns `int32_t`).
- Rotation used to be `sinf`/`cosf` — transcendental, not rational. Since
  2026-09-03 it is a **360-entry table of int32 values over 32768**, so
  `cos = C/32768` and `sin = S/32768` with `C`, `S` exact integers.
- The angle accumulator means the matrix's linear part is always ONE table
  entry, never a product of several.
- Therefore `tx` and `ty`, which accumulate `t += R(angle) * (dx, dy)` with
  integer `(dx, dy)`, are always exact multiples of `1/32768`.

The inverse's linear part is `A^-1 = 32768 * [[C, S], [-S, C]] / (C^2 + S^2)`, so
the per-pixel step along a scanline is

```
step = 32768 * C / ((C^2 + S^2) * SCALE)
```

— an exact rational. `32768 * C` and `C^2 + S^2` each peak at 2^30, inside
`int32`; the `* SCALE` in the denominator is what overflows once `SCALE > 2`, so
a real implementation needs either a 64-bit denominator or a gcd reduction
first.

**So a pure-integer, exactly-correct DDA is available here, and the sine table is
what made it available.** Replacing `sinf` with a table was argued for on
consistency and 5 KB of flash; it turns out to have been the enabling step for
this as well, which nobody predicted at the time.

### Why Q16.16 was still the right first move

Cost per pixel per axis:

| | float | Q16.16 | exact integer DDA |
|---|---|---|---|
| multiply | 2 | 0 | 0 |
| add | 2 | 1 | 2 |
| compare + conditional | 0 | 0 | 1 + 1 |
| float→int convert | 1 | 0 | 0 |
| exact? | no (its own rounding) | no (1/65536) | **yes** |

The exact DDA is roughly double the Q16.16 inner loop but still far below float.
What it buys is not speed — it is that the answer becomes **canonical**: bit-identical
on every compiler, optimisation level and architecture, forever. The current
float path is not even bit-identical to itself across toolchains (measured:
`-Os` WASM and `-O2` native disagreed by 21 px out of 3,701 on the same corpus).

That property is worth more than it sounds for a project whose test strategy is
golden images shared between a device, a native harness and a browser.

**It does not, however, remove the adoption decision.** An exact integer DDA
computes the *mathematically correct* affine value; the float path computes a
rounded one. They still differ from each other. Exactness buys reproducibility
across platforms, not agreement with the renderer that shipped yesterday.

## Recommendation

1. Q16.16 is now the default rasteriser — measured, gated, and 15–42% faster per
   operation on real hardware. The float path stays selectable as
   `displaylist-float` with its own goldens.
2. Treat the exact integer DDA as the thing to build **if and when** either the
   cross-toolchain reproducibility matters, or an FPU-less target appears. It is
   a contained change: same loop shape, different accumulator.
3. Do not describe either as making the language integer-math. The language
   already was. What changed is that the renderer stopped needing an FPU to
   agree with it.

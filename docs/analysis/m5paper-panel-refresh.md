# M5Paper: where panel refresh time actually goes

Written after the rasterizer work (`m5paper-responsiveness-work.md`) moved the
bottleneck off rasterization. It **corrects an earlier claim in that document.**

## The correction

> That document called the ~0.7 s panel refresh "a hardware floor: no amount of
> rasterizer optimisation removes it." The second half is true; **the first half
> is wrong.** It is not a hardware floor. It is mostly a software inefficiency
> in the M5EPD library, and the waveform mode barely affects it.

`M5EPD_Canvas::pushCanvas()` does two distinct things that were being conflated:

1. **`WritePartGram4bpp()` — a synchronous SPI transfer.** For the full 540x960
   panel this runs **129,600 iterations** of
   `digitalWrite(CS,0); spi->write32(word); digitalWrite(CS,1)` at **10 MHz**
   (`M5EPD_Driver.cpp`). Each iteration ships **32 SPI bits to carry 16 bits of
   pixel data** — 2x wire overhead — plus two GPIO toggles. That is ~415 ms of
   pure SPI clock before GPIO overhead, and it accounts for essentially all of
   the measured ~666 ms.
2. **`UpdateArea()`** — waits on `CheckAFSR()` for the *previous* waveform
   (normally already finished), fires the display command, and returns. It does
   not wait for the new waveform to complete.

So the number in `../measurements/2026-08-27-m5paper-baseline.md` is a **transfer
time, not a waveform time**, and changing the waveform will leave it unchanged.
Anyone who switches modes and then watches that timer will wrongly conclude the
change did nothing.

## What the waveform change does buy

It shortens the panel's **settle time after the push returns** — the interval
where the user watches a half-flipped screen — and the `CheckAFSR()` wait the
*next* push inherits.

IT8951 modes available through M5EPD (`M5EPD_Driver.h`):

| mode | greys | ghosting | update | verdict for us |
|---|---|---|---|---|
| `INIT` | — | — | 2000 ms | full erase to white |
| **`DU`** | **B/W only** | **Low** | **260 ms** | **chosen** — contract is exactly our content |
| `GC16` | 16 | Very low | 450 ms | what we used: 16 greys for content with none |
| `GL16`/`GLR16`/`GLD16` | 16 | Low-Med | 450 ms | need image preprocessing; buy nothing |
| `DU4` | 4 | Medium | 120 ms | legal for us (0 and 15 are two of its four states); faster but riskier on dense full-screen art |
| `A2` | B/W only | Medium | 290 ms | *slower* than DU here, and needs white-image entry/exit sequences |

**Why this is available at all:** the DSL is 1bpp. `micropatterns_drawing.h`
defines `DRAWING_COLOR_WHITE = 0` and `DRAWING_COLOR_BLACK = 15` and no script
ever produces anything else. Pushing a 16-grey waveform for two-tone content was
pure waste.

Expected effect: ~450 ms -> ~260 ms of settle, so roughly **1.1 s -> 0.93 s
end-to-end**. Real, but modest — and much smaller than the transfer.

### De-ghosting policy

Fast waveforms accumulate artefacts, so every non-GC16 panel update increments a
counter under `_panelMutex`; every `SCRIPT_DEGHOST_INTERVAL`-th (8) script push
uses GC16 instead and resets it. Indicator/banner DU4 region pushes count toward
the same budget, and the eventual GC16 is full-screen so it cleans their regions
too. The counter is seeded to the interval so the **first push after boot is
always GC16** — the boot path deliberately does not `Clear()`, so the panel's
prior contents are unknown. Amortised cost ~450/8 ≈ 56 ms per render against
~190 ms saved on each of the other seven.

**8 is a defensible starting point, not a measured one.** If residue is visible
by push 5, lower it; if the panel still looks clean at 8, raise it.

## The real prize, and why it is blocked

The gram transfer is where ~500 ms of the ~666 ms actually lives, and three
things are wrong with it for our use case:

1. **4bpp for 1bpp content** — we ship 4x the bytes we need.
2. **32 SPI bits per 16 bits of payload** — 2x wire overhead on top of that.
3. **10 MHz, with a CS toggle per 32-bit word** — no bulk transfer.

Fixing all three plausibly turns ~666 ms into roughly ~150 ms, which dwarfs the
waveform win. It is blocked on library internals: `IT8951_Defines.h` defines
only 2/3/4/8 BPP with no 1bpp path, and `_pix_bpp`, `_spi_freq`, `StartSPI()`,
`SetArea()` and `SetTargetMemoryAddr()` are all private. `M5.EPD.GetSPI()` is
public, but the address/area setup around the transfer is not, so the transfer
cannot be driven from outside.

**Decision taken:** vendor M5EPD into the repo and patch it. Not yet done.

## Rejected: partial / regional updates

`pushCanvas(x, y, mode)` pushes the *whole* canvas at an offset — there is no
sub-rectangle variant. Pushing only a dirty region would require a second canvas
plus a blit, costing more than the SPI it saves unless the dirty rect is small.
It cannot be small: `DisplayListRenderer::render()` clears and redraws
everything. Real value here needs a previous-frame copy in PSRAM (259 KB) plus a
dirty-rect diff, and only pays for scripts whose frames barely change. Not worth
doing before the gram transfer, which helps every script unconditionally.

## Needs on-device confirmation

1. **DU contrast on our content** — expect solid black/white, but dense 1px
   dither patterns are the thing to eyeball against the GC16 baseline.
2. **Whether the interval of 8 is right** (see above).
3. **The actual settle saving.** The push timer will be *unchanged* — that is
   expected, per the correction above. Measure it by timing
   `M5.EPD.CheckAFSR()` right after the push; `bench/mp_bench.cpp` already has
   this instrumentation as `push_wait_us`.
4. **DU script pushes interacting with DU4 indicator region pushes.** Two
   different fast waveforms over the same pixels was never exercised before —
   GC16 used to clean up after every render.

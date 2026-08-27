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

**Decision taken:** vendor M5EPD into the repo and patch it. **Done** —
`M5Paper_MicroPatterns/lib/M5EPD/`, see `README-VENDORED.md` there for the full
description of every local modification and `upstream-0.1.5.patch` for the diff.

### What was actually changed

1. **Bulk SPI transfer.** One CS assertion, one `0x0000` pack-write preamble,
   one `writeBytes()` for the whole payload, instead of 129,600 × {CS low,
   `write32`, CS high}. Halves the wire traffic (32 bits per 16 bits of payload
   → 16) and removes 259,200 GPIO toggles. **This alone should take the
   full-screen 4bpp transfer from ~666 ms to near its ~207 ms clock floor.**
2. **1bpp path.** `M5EPD_Canvas::pushCanvas1bpp()` +
   `M5EPD_Driver::Write1bppGram()` / `Update1bppArea()`. 259,200 bytes →
   **64,800**, so ~207 ms → **~52 ms** of clock. Wired into
   `DisplayManager::pushScriptCanvasLocked()` behind
   `DisplayManager::SCRIPT_PUSH_1BPP` (default true), except on the periodic
   de-ghost push, which stays 4bpp/GC16 on purpose.
3. **`M5EPD_SPI_FREQ_HZ`**, still 10 MHz. One-line change to raise; corruption
   is the expected failure mode, and it is now also the expected failure mode of
   the flow-control-free packed burst.

### The wrinkle the plan missed: rotation forces a host-side transpose

The IT8951 applies `SetRotation()` **during the load** (the rotate field is in
the `LD_IMG_AREA` info word), so controller DRAM is always panel-native. At 4bpp
that is fine. At 1bpp it is incoherent: we smuggle 8 pixels through as one "8bpp
pixel", so the controller would rotate whole *bytes*.

Independently, our canvas is **540** wide and 540 is not a multiple of 8 — there
is no way to pack 540-pixel rows at all, never mind the 32-pixel alignment some
LUT versions demand.

Both are solved the same way: transpose to panel-native **960×540** on the host
(960 = 30 × 32) and hand the driver panel-native coordinates. Cost is a pack pass
over 518,400 pixels; the loop is written so the only PSRAM-hostile access is one
strided byte write per output byte (64,800 of them). Budget ~20–30 ms, which
still leaves the 1bpp push far cheaper than the 4bpp one.

### Device verification checklist for the vendored transfer path

Ordered, because each step's failure explains the next.

1. **Flash `-e m5stack-fire`, render any script.** Watch the serial line
   `DisplayManager: script push mode=DU bpp=1, gram transfer N ms`. Expect
   `bpp=1` and N well under 150. `bpp=4` means `pushCanvas1bpp()` bailed —
   the reason is logged.
2. **Look at the image.** In order of what each symptom means:
   inverted → swap `M5EPD_1BPP_FRONT_GREY`/`M5EPD_1BPP_BACK_GREY`;
   upside down → the transpose reversal, see `README-VENDORED.md`;
   8-px columns swapped in pairs → flip `M5EPD_1BPP_ENDIAN`;
   torn bands / noise → SPI or FIFO, lower `M5EPD_SPI_FREQ_HZ`;
   nothing displayed → this firmware's LUT does not do 1bpp, set
   `SCRIPT_PUSH_1BPP = false`.
3. **Check the 4bpp path still works** — it is what indicators, banners and the
   periodic de-ghost use, and it got the bulk-transfer rewrite too. Press a
   button mid-render (activity indicator, DU4 region push) and let the de-ghost
   counter reach 8 (GC16 full-screen). Both must look right.
4. **Check `Clear()`** — `FillGramBulk()` replaced its loop as well. It runs on
   `clearScreen()`.
5. **Then, and only then, try raising `M5EPD_SPI_FREQ_HZ`.** 20 MHz first. If
   the panel corrupts, put it back; there is no datasheet number to appeal to.
6. **Numbers:** `-e m5paper-bench` reports `push_xfer_us`. Note that with
   `MP_BENCH_PUSH_1BPP=1` (default) that figure *includes* the host-side pack,
   which the 4bpp path does not do. A/B with `-DMP_BENCH_PUSH_1BPP=0`.

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

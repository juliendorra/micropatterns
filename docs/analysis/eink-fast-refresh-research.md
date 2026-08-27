# Fast e-ink: what transfers from paperboy/Modos to an IT8951 M5Paper

Research prompted by zephray's 57 fps Game Boy emulator on the M5PaperS3
(<https://gitlab.com/zephray/paperboy>) and Modos Labs' e-ink drive work.
Question asked: which techniques port to our M5Paper v1.1?

## 0. The honest answer: the central technique does NOT port

Paperboy is not a waveform trick, a mode selection, or a dithering choice. **It
replaces the display controller.** The ESP32-S3 acts as the panel's timing
controller: it bit-bangs the gate driver, DMAs 2-bit-per-pixel source-driver
commands over an `esp_lcd` i80/LCD_CAM bus at 24 MHz, and runs its own
per-pixel waveform state machine in SRAM.

That is possible because **the M5PaperS3 has no EPD controller** — the panel is
wired straight to the S3. Our M5Paper v1.1 has an **IT8951E** between the ESP32
and the panel. We cannot reach the source/gate drivers at all; we can only send
high-level commands over SPI and wait for the IT8951's own hard-coded waveform
LUTs. The author's framing:

> "What makes it special compared to other small Eink devkits is that it has got
> a screen with raw row/column driver interface … This enabled me to drive this
> screen while bypassing the normal waveform method."

Modos reached the same conclusion by design: *"A critical design choice in the
Glider was to avoid e-ink panels with built-in controllers."*

**Realistic ceiling for M5Paper v1.1: ~5-8 fps, versus paperboy's 57.** That gap
is structural. Do not plan around closing it.

## 1. The idea behind paperboy (worth understanding even though it does not port)

Conventional fast-refresh reduces *drive time*, trading contrast for speed, and
tops out around 15 fps on this panel generation. Paperboy instead removes the
**global lock**: normally the controller accepts a new image only once per
complete waveform. Paperboy accepts a new image *every* frame and tracks each
pixel's own waveform clock independently.

The state machine is one byte per two pixels: a 3-bit drive counter plus a 1-bit
target colour each. Per frame, per pixel: XOR stored target against incoming
target; if changed, reset that pixel's counter and flip direction; emit a drive
command while the counter is below saturation, else emit NOP. Unchanged pixels
emit `00` and cost nothing electrically — per-pixel partial update, not
per-region.

Modos' Caster does the same in FPGA, with an extra refinement paperboy lacks: a
reversing pixel gets the *complement* of its elapsed drive time.

**The numbers that explain why binary content is fast** (from Caster's HDL and a
decoded GC16 LUT row): a binary transition is **9 panel frames**; one GC16
greyscale transition is **38 frames**. A waveform file is a 3-input LUT —
frame x source grey x destination grey -> 2-bit drive — and temperature
compensation is just selecting a different table.

## 2. Technique portability

| Technique | Ports to IT8951? |
|---|---|
| S3 as TCON via LCD_CAM + DMA | **No** — no such peripheral, and the panel pins terminate at the IT8951 |
| Direct 2-bit source-driver commands | **No** — IT8951 generates these internally |
| Bit-banged gate driver | **No** — not wired to the MCU |
| Per-pixel waveform state machine | **No** — the IT8951 owns that layer |
| Free-running scan + VSYNC double-buffer + frameskip | **Partially** — the *pattern* maps onto `DPY_BUF_AREA` |
| **1bpp end to end** | **YES — the main available win** (section 3) |
| Spatial dither instead of greyscale | Yes, but irrelevant: our content is already binary |
| Dirty-line/rect change detection | **Yes**, pairs with `UpdateArea()` bounding boxes |
| Manual de-ghost (N black + N white frames) | **Yes** |

## 3. The actionable win: 1bpp transfers on IT8951

M5EPD ships every host->controller transfer at **4 bpp** (`_pix_bpp =
IT8951_4BPP`, hard-coded in all three `*Gram4bpp` paths) at a fixed **10 MHz**.
Our content is 1bpp. Transfer cost at 960x540:

| payload | bytes | SPI @ 10 MHz |
|---|---|---|
| full screen 4bpp (today) | 259,200 | ~207 ms |
| **full screen 1bpp** | **64,800** | **~52 ms** |
| 432x480 window, 1bpp | 25,920 | ~21 ms |

**The registers already exist in M5EPD** (`IT8951_Defines.h`):

```c
#define IT8951_UP1SR (IT8951_DISPLAY_REG_BASE + 0x138)  // Update Parameter1
#define IT8951_BGVR  (IT8951_DISPLAY_REG_BASE + 0x250)  // 1bpp colour table
```

Recipe, from Waveshare's `EPD_IT8951.c` (`EPD_IT8951_Display_1bp`):

1. Load the image declaring `Pixel_Format = IT8951_8BPP` but with
   `Area_X = X/8` and `Area_W = W/8`. Their comment is literally
   `//Use 8bpp to set 1bpp`. Packed 8-pixels-per-byte data goes over the wire.
2. `WriteReg(UP1SR+2, ReadReg(UP1SR+2) | (1<<2))` — enable 1bpp display mode.
3. `WriteReg(BGVR, (front_grey<<8) | back_grey)` — for us `0xF0` / `0x00`.
4. Issue `DPY_AREA` / `DPY_BUF_AREA`, wait, then clear the `UP1SR` bit again.

**Caveat:** on at least some LUT versions X and W must be **32-pixel aligned**,
or the image does not display. Assume this applies and align.

### Relation to our own measurement

Our measured transfer is **~666 ms**, against a ~207 ms raw-SPI-clock floor for
4bpp. The extra ~3x is M5EPD's transfer loop: 32 SPI bits shipped per 16 bits of
payload, plus a CS toggle per word. So the two fixes are **multiplicative** —
fix the loop *and* go 1bpp. See `m5paper-panel-refresh.md`.

### Other wins worth taking

- **Pipeline the upload against the waveform.** The IT8951 has its own DRAM and
  `DPY_BUF_AREA` displays from an arbitrary address. Load frame N+1 while frame
  N displays, so SPI time stops stacking on waveform time.
- **Dirty-rectangle tracking** -> `UpdateArea()` on the bounding box; update time
  scales with area.
- **Try SPI above 10 MHz.** M5EPD picks 10 MHz with no comment explaining why.
  No documented IT8951 maximum was found; verify empirically, expect corruption
  as the failure mode.

## 4. Numbers, with provenance — read the labels

**From source (trustworthy):** paperboy 17.7 ms/frame ≈ **57 fps** (not 60);
24 MHz pixel clock; 4 drive frames per transition; 9 frames for a binary
transition and **38 for GC16** (Caster/Glider); M5EPD SPI = 10 MHz, all
transfers 4bpp.

**Vendor claims (treat with suspicion):** the IT8951 mode times in
`M5EPD_Driver.h` (120/260/290/450/2000 ms) are **E Ink app-note "typical"
figures reproduced in a comment, not M5 measurements**. The **A2 = 290 ms entry
is internally inconsistent** — A2 is normally the *fastest* mode, and being
slower than DU4 makes no physical sense; that comment block reads like a
partially mis-transcribed app note. **Benchmark A2/DU/DU4 on our own unit before
trusting any of it.** Waveshare separately claims a **~7 fps IT8951 ceiling**
with frames precached in controller DRAM and no host transfer at all.

## 5. Not found / not verified

1. Whether the IT8951 accepts a **custom waveform**. `LUT0BADDR`, `LUT0MFN`,
   `UPBBADDR` exist in M5EPD's header and IT8951 boards load waveforms from SPI
   flash, so it is plausible — but no working example, no documented `.wbf`
   layout, no report of anyone doing it. **Do not plan around this.**
2. Which modes M5Paper's specific IT8951 firmware actually implements. M5's
   numbering does not match Waveshare's, and there are reports of DU4/A2/GL*16
   rendering white as grey on some panels.
3. The IT8951's maximum SPI clock. Not stated anywhere found.
4. **Any measured M5Paper refresh timing at all** — nobody has published one.
   Our own measurements would be novel.
5. Ghosting/panel-wear quantification for high-refresh use.

## 6. If the paperboy result is ever actually wanted

Not on an M5Paper v1.1. It needs a raw-interface panel plus an MCU with a fast
parallel output peripheral: an ESP32-S3 board with a directly-wired panel
(M5PaperS3, a Lilygo T5 variant, any epdiy-supported board), where epdiy already
provides `MODE_EPDIY_MONOCHROME` and 8-pixels-per-byte packing — and where our
all-binary content is exactly the case that runs fastest.

On the hardware we have, the plan is: **1bpp packed transfers + a true binary
waveform + dirty-rect `UpdateArea` + pipelined loads**, aiming at the ~7 fps
IT8951 ceiling rather than fighting it.

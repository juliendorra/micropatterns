# M5EPD — vendored and patched

**This is our code now.** It is no longer a dependency we can bump. If upstream
ever ships something we want, someone has to re-apply the modifications listed
below by hand. That trade was made deliberately: the entire host→controller
transfer path is the M5Paper's dominant cost and none of it was reachable from
outside the library.

## Provenance

| | |
|---|---|
| Upstream | <https://github.com/m5stack/M5EPD> |
| Version | **0.1.5** (`library.json` / `library.properties`) |
| Obtained from | PlatformIO registry package `m5stack/M5EPD`, registry id 11542 |
| Vendored on | 2026-08-27 |

No git commit hash is recorded because the artefact we consumed is the
PlatformIO registry tarball for 0.1.5, not a git checkout. `pio pkg install
--global --library m5stack/M5EPD@0.1.5` reproduces the exact pristine tree that
`upstream-0.1.5.patch` was diffed against.

`upstream-0.1.5.patch` in this directory is the **complete** diff of the four
modified files against that pristine tree. It is the audit trail: if it and the
list below ever disagree, the patch is the truth.

## What was deleted from the upstream tree

Nothing that affects the build. Removed to keep the repo small and quiet:

- `examples/` (3.6 MB, mostly a bundled `.ttf`)
- `tools/` (upstream's `image2gray` / `ttf2bin` Python helpers)
- `.piopm` (PlatformIO package manifest — its presence makes PlatformIO treat
  the directory as a managed package)
- `.clang-format` (would fight the project's own formatting)

Everything under `src/` is untouched except the four files below. That includes
the bundled FreeType, the fonts, `In_eSPI`, `Sprite`, `GT911`, `BM8563`,
`SHT3x`, `pngle` and `qrcode`.

## Local modifications

Four files: `src/M5EPD_Driver.h`, `src/M5EPD_Driver.cpp`,
`src/M5EPD_Canvas.h`, `src/M5EPD_Canvas.cpp`.

Every addition is marked `LOCAL` in the source. **No public API was removed or
changed.** `pushCanvas()`, `WritePartGram4bpp()`, `FillPartGram4bpp()`,
`Clear()`, `SetArea()`, `UpdateArea()` all keep their signatures and their
observable behaviour; other firmware code and the `m5paper-bench` build call
them unchanged.

### 1. Bulk SPI transfer, replacing the per-word CS toggle

*(`M5EPD_Driver.cpp`: `WriteGramBulk()`, `FillGramBulk()`, and the rewritten
loops in `Clear()`, `WritePartGram4bpp()`, `FillPartGram4bpp()`)*

Upstream shipped **one 16-bit payload word per CS assertion**:

```c
digitalWrite(_pin_cs, 0);
_epd_spi->write32(word);   // word is uint16_t -> emits 0x0000 then the 16 bits
digitalWrite(_pin_cs, 1);
```

`write32()` of a 16-bit value emits the IT8951 "write data" preamble `0x0000`
followed by the payload, so **every 16 bits of image cost 32 bits on the wire**,
plus two GPIO toggles. A full 540×960 4bpp frame was 129,600 of those.

IT8951 pack-write mode (`IT8951_I80CPCR` bit 0 — upstream *already* enables it
in `begin()`) accepts **one preamble per CS assertion** followed by an arbitrary
number of payload words. That is what Waveshare's
`EPD_IT8951_HostAreaPackedPixelWrite()` does. So the burst is now: CS low, one
`write16(0x0000)`, one `writeBytes()` of the whole payload, CS high.

**New risk this introduces:** upstream's per-word CS toggle also gave the
controller a natural gap between words. A packed burst has no flow control at
all — that is the point of `I80CPCR` pack-write, and it is what Waveshare does —
but if this panel's IT8951 write FIFO ever under-runs, it will show up as
**corruption in bands**, not as a hang. Same symptom, same first response, as
too high an SPI clock.

The wire bytes are bit-identical to what the old loop produced. Upstream built
each word as `gram[pos] << 8 | gram[pos+1]` and sent it MSB-first, which is
exactly `gram[]` in buffer order; the `0xFFFF - word` colour inversion is
`~word` for a full 16-bit word, i.e. a byte-wise complement, done here 32 bits
at a time into a 2 KB staging buffer (`_xfer_buf`, a member — internal DRAM,
+2048 bytes of static RAM).

### 2. Named, single-point SPI clock

*(`M5EPD_Driver.h`: `M5EPD_SPI_FREQ_HZ`)*

Upstream hard-coded `10000000` in two places (the constructor and `begin()`)
with no comment. Both now read the constant. **It is deliberately still 10 MHz.**
No IT8951 datasheet maximum could be found, so raising it is an empirical change
someone has to validate on the panel. It is a one-line edit.

Expected failure mode if raised too far: **corruption** — torn or garbled bands,
shifted pixels, or a screen of noise, because the controller mis-samples MOSI.
Not a blank screen, not a hang.

### 3. 1bpp transfer + IT8951 1bpp display mode

*(`M5EPD_Driver.h/.cpp`: `Write1bppGram()`, `Update1bppArea()`,
`LeaveDisplay1bpp()`, `ReadReg()`, `SetLoadArea()`, `Is1bppDisplayLatched()`,
the `M5EPD_1BPP_*` constants; `M5EPD_Canvas.h/.cpp`: `pushCanvas1bpp()`,
`free1bppBuffer()`, `_bitbuf`)*

The recipe is Waveshare's `EPD_IT8951_1bp_Refresh()` / `EPD_IT8951_Display_1bp()`
from `EPD_IT8951.c`:

1. Load declaring `Pixel_Format = IT8951_8BPP` but with `Area_X = X/8` and
   `Area_W = W/8`, shipping packed 8-pixels-per-byte data. Their own comment is
   *"Use 8bpp to set 1bpp"*. The controller stores each byte as one "8bpp pixel"
   which is really 8 packed 1bpp pixels.
2. `WriteReg(UP1SR+2, ReadReg(UP1SR+2) | (1<<2))` — enable 1bpp display mode.
3. `WriteReg(BGVR, (front_grey<<8) | back_grey)` — the two greys the 0/1 bits
   map to. We use `0xF0` / `0x00`, Waveshare's defaults, with **set bit = white**.
4. `DPY_BUF_AREA`, then eventually clear the `UP1SR` bit again.

`SetArea()`'s body moved into a new private `SetLoadArea(x, y, w, h, bpp,
rotate)` so the 1bpp load can override the bpp and rotate fields without
disturbing the members; `SetArea()` now just delegates and behaves identically.

#### Assumption: 32-pixel alignment (`M5EPD_1BPP_ALIGN`)

Packing alone only needs 8. Waveshare's notes and several IT8951 reports say
some LUT versions need X and W **32**-pixel aligned or nothing is displayed.
**We assume that applies and enforce 32.** Our only real caller is full-panel
(`x=0, w=960`), which satisfies either. Lower the constant to 8 only if you
have evidence this LUT does not care.

#### The rotation problem, and why the canvas is transposed on the host

This is the part the original plan did not anticipate, and it is load-bearing.

The IT8951 applies rotation **while it loads an image** — the rotate field lives
in the `LD_IMG_AREA` info word, so controller DRAM is always panel-native. That
is fine at 4bpp, where a byte is two pixels and the controller can rotate pixel
by pixel. It is **incoherent at 1bpp**, where we smuggle 8 pixels through as one
"8bpp pixel": the controller would rotate whole bytes, not pixels.

It is also unavoidable for a second reason. Our script canvas is **540×960**
portrait pushed with `ROTATE_90`, and **540 is not a multiple of 8**, let alone
32 — there is literally no way to pack 540-pixel rows. Panel-native the same
image is 960×540, and 960 = 30 × 32.

So `M5EPD_Canvas::pushCanvas1bpp()` transposes on the host and
`M5EPD_Driver::Write1bppGram()` / `Update1bppArea()` take **panel-native
(rotate-0) coordinates**, unlike every other method on the driver. The mapping
is derived from `UpdateArea()`'s own `ROTATE_90` formula
(`args = {y, PANEL_H - w - x, h, w}`):

```
panel_x = y + cy
panel_y = (PANEL_H - w - x) + (w - 1 - cx)     // canvas column -> panel row, reversed
```

The packing loop walks eight canvas rows at a time so the PSRAM reads stay
sequential; the only PSRAM-hostile access is one strided byte write per output
byte (64,800 of them for a full panel).

The packed buffer (`_bitbuf`, 64,800 bytes full-panel) is allocated lazily on
first use and **prefers PSRAM** — 64 KB of internal DRAM would be a permanent
bite out of the pool WiFi needs. It is freed by `deleteCanvas()`.

#### Lazy exit from 1bpp display mode

Waveshare clears the `UP1SR` bit immediately after the display command, which
means blocking on the whole waveform. We do not want that: upstream
`UpdateArea()` is fire-and-forget and the rest of the firmware depends on it.

Instead `Update1bppArea()` **latches** the mode on, and `LeaveDisplay1bpp()` —
called at the top of `Clear()`, `WritePartGram4bpp()` and `FillPartGram4bpp()` —
does `CheckAFSR()` and then clears the bit. That wait is exactly where upstream
already paid one (`UpdateArea()` starts with `CheckAFSR()`), so nothing gets
slower, and **back-to-back 1bpp pushes pay nothing at all**.

Clearing the bit while a waveform is still running would make the display engine
re-read the same DRAM as 4bpp mid-update and garble the frame; hence the wait.

`Is1bppDisplayLatched()` exposes the flag for debugging.

#### Failure modes, and what each one means

| What you see | Cause | Fix |
|---|---|---|
| Sharp image, **colours inverted** | `BGVR` byte order is the other way round on this firmware | swap `M5EPD_1BPP_FRONT_GREY` / `M5EPD_1BPP_BACK_GREY` |
| Sharp image, **upside down / 180° rotated** | the column reversal in the transpose is backwards | in `pushCanvas1bpp()`'s `ROTATE_90` branch, replace `(_iwidth - 1 - cx)` with `cx` |
| Image reads as **8-pixel columns swapped in pairs** | wrong load endianness | flip `M5EPD_1BPP_ENDIAN` to `IT8951_LDIMG_L_ENDIAN` |
| **Nothing displays at all** (panel keeps the old image) | the LUT rejected the area, or 1bpp display mode is not supported by this firmware | it is not recoverable by tuning — set `DisplayManager::SCRIPT_PUSH_1BPP = false` |
| Torn / garbled bands, noise | SPI clock too high | lower `M5EPD_SPI_FREQ_HZ` |

`pushCanvas1bpp()` returns **false and does nothing** if the geometry is
unsupported (rotation other than 0/90, misaligned X or W, off-panel, allocation
failure), so callers always have the 4bpp path to fall back to. The 4bpp path is
never dead code.


### 5. Removed the bundled HZK16 Chinese font (2026-08-27)

`src/Fonts/HZK16` (261 KB) and `src/Fonts/HZK16.h` (1.6 MB) were deleted.
Nothing in the library references them outside the font files themselves
(`grep -rn HZK16 src | grep -v Fonts/HZK16` is empty), the firmware never
renders CJK text, and 1.9 MB of unused data does not belong in the repository
permanently. The rest of `src/Fonts/` IS required -- `utility/In_eSPI.h`
includes `Fonts/glcdfont.c`, so the directory cannot be dropped wholesale.

Restore both files from upstream 0.1.5 if CJK glyphs are ever needed.

## Re-vendoring checklist

If you ever pull a newer upstream:

1. `pio pkg install --global --library m5stack/M5EPD@<new>` into a scratch dir.
2. `diff -u` the four modified files against **0.1.5** (use
   `upstream-0.1.5.patch`) to see what is ours.
3. Re-apply by hand. Do not `patch -p1` blindly — the bulk-transfer rewrite
   replaces whole loop bodies.
4. Delete `examples/`, `tools/`, `.piopm`, `.clang-format` again.
5. Regenerate `upstream-<new>.patch` and update this file.


---

## Device test result, 2026-08-27: 1bpp produces VERTICAL SHEARING

First contact with the panel. Outcome:

- **4bpp bulk path: CORRECT and fast.** ~666 ms -> ~207 ms, no visual defect,
  buttons and interaction normal. This is a real, verified win and is the
  current default.
- **1bpp path: WRONG.** Real script output rendered with **vertical shearing** —
  a consistent progressive horizontal slant down the image. `SCRIPT_PUSH_1BPP`
  is now `false` pending a fix.

### Reading the symptom

The symptom table in this file predicted *torn bands / noise* for a clock or
FIFO problem. **Shear is not that**, and the distinction matters:

| symptom | meaning |
|---|---|
| torn bands, noise, random | timing: SPI clock too high, FIFO under-run |
| **consistent progressive slant** | **row-stride mismatch** — each row starts at the wrong offset |

A slant is deterministic and geometric, so it is a addressing bug, not a
signal-integrity one. Raising or lowering the SPI clock will not touch it.

### Where to look

`pushCanvas1bpp()` transposes the 540x960 canvas to panel-native 960x540 and
packs 8 pixels per byte. A shear means the output row stride does not match the
row length actually written — the classic cause is computing the stride from the
wrong dimension (540 vs 960) or from the unpacked rather than packed width
(960/8 = 120 bytes per row).

Worth checking in order:
1. Output row stride: must be **120 bytes** (960 packed pixels), not 540/8 = 67.5
   (not an integer — see the original note on why 540 is unpackable) nor 240.
2. The `Area_W = W/8` value handed to the controller against the stride actually
   used when filling the buffer.
3. Whether the transpose's inner loop advances the destination by the stride or
   by the source width.

### Do not re-enable without panel verification

The transfer being 4x faster was measured and is not in doubt. Correctness was
assumed and was wrong. Any fix must be looked at on the panel before the flag
goes back to `true`.

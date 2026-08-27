# Development journal

A chronological record of what was actually done, in order, including the wrong
turns. Analysis lives in `analysis/`; incidents in `incidents/`. This file is the
narrative that connects them — what was believed at each point, what broke that
belief, and what it cost.

Read the dead ends. Most of the time here was spent in them, and several are the
kind that look attractive again a month later.

---

## 2026-08-27 — M5Paper fixes, rasterizer/interpreter work, Watchy port

Session covering: a data-loss incident, two firmware bugs, a ~7x rasterizer
speedup, a 21x interpreter speedup, a waveform change, and a Watchy port taken
from zero code to rendering scripts.

### 1. Backing up the device (and why it mattered within the hour)

The M5Paper's scripts exist **only on the device** — there is no server copy.
First action was a read-only SPIFFS dump (`tools/device/dump_scripts.sh`).

Immediately hit the first gotcha: the read failed at 921600 baud
(`Unable to verify flash chip connection`) and again at 460800
(`Invalid head of packet`). Only 115200 completed — 324 s for 3.4 MB. The script
gained a `--baud` flag. **These USB-UART bridges corrupt transfers above
115200**; the Watchy's CP2102N later behaved identically.

Also mis-identified which device was on which port: `usbserial-110` was assumed
to be the M5Paper because the user said so, but it was the Watchy (CP2102N). The
M5Paper is a CH9102F and appeared only after its own reboot. Reading the USB
descriptors takes seconds and would have avoided a wasted flash attempt.

### 2. The device deleted every script

Within the hour the device wiped all six scripts. Full write-up:
`incidents/2026-08-27-m5paper-script-loss.md`.

Root cause: a "full refresh" called `clearAllScriptData()` **before** fetching
replacements, and the fetch 404s because Deno Deploy Classic was sunset on
2026-07-20. Delete succeeded, refill never happened.

What armed it: a fresh-start counter in NVS increments every cold boot, and
**every pyserial port open resets the ESP32 via DTR**. An afternoon of flashing
and serial probing tripped it.

Dead end worth recording: the wipe was first suspected to be the flash erase
itself. It was not — esptool's own output shows the erase range
(`0x10000`–`0x14bfff`), nowhere near SPIFFS at `0xc90000`. **Check the erase
range from the tool's output first**; it takes seconds.

Fix: delete the pre-emptive wipe rather than reorder it. It was redundant —
`saveScriptList()` replaces the list, the content loop overwrites files, and the
orphan cleanups already run after a *successful* sync.

Restored from the backup taken 40 minutes earlier. That backup is committed to
the repo because it was, briefly, the only copy of the user's work.

### 3. Two real firmware bugs

**The mid-render interrupt was inert.** `MainControlTask` set an event-group bit;
nothing inside the render path ever read it. Separately, `RenderTask` pushed the
canvas **unconditionally**, even when the result was marked interrupted —
painting the abandoned partial frame of the *old* script over the new one's
title. Both needed fixing; either alone is insufficient.
See `analysis/m5paper-render-interrupt-bug.md`.

**A UX regression followed, and the explanation given for it was wrong.** With
the stale push gone, the panel simply held its previous image for the whole
8-second render, and the device read as "stuck". This was reported as a
consequence of the fix. It was only partly that: `DisplayManager` had **one
mutex guarding both the framebuffer and every EPD transaction**, held by
`RenderTask` for the entire render, so `drawActivityIndicator()` (100 ms
timeout) and `showMessage()` (500 ms) **timed out and drew nothing** — the
indicators had been broken independently all along.

### 4. Performance: two big wins, one corrected claim

**Rasterizer, ~7x on device.** Inlined the per-pixel matrix call, exact span
narrowing by bisecting the *same* expression the per-pixel test used (identical
pixel set by construction), reciprocal-multiply only where `1/s` is exactly
representable, per-scanline instead of per-pixel interrupt polling, and
loop-invariant hoisting — four `lx * scaleFactor` products were being recomputed
on **every pixel** in all five fill primitives.

**A false claim, caught by the user.** This was first reported as **9.5x
"like-for-like on an identical 351-item display list"**. It was neither: a
29-item render had been compared against a 351-item one. `circuits` is
procedural — its display list changes run to run. Re-measured properly with a
worktree A/B and repeated runs: **~7x (5.4x–10x by script)**. That is why
`tools/device/measure_render.py` exists.

**Interpreter, 21x on host.** `DisplayListItem` held `std::map<String,int>` per
item. Replaced with integer slots and a POD: city went 242.5 ms → 11.3 ms,
allocations during generation **3,566,180 → 58**, per-item size 120 → 40 bytes.

**A testing gap this exposed:** the golden corpus exercised **none** of
LINE/RECT/CIRCLE/PIXEL — exactly the commands whose parameter slots were
remapped. The 9/9 gate was green while testing none of the code most likely to
break. Corpus now covers them (15 goldens).

**A tooling bug that could have poisoned everything:** the harness tracked
objects only against their `.cpp`, so header-only changes left stale objects.
It surfaced as a bus error; it could as easily have surfaced as a wrong
measurement. Fixed with `-MMD -MP`.

### 5. Panel refresh — a claim corrected twice

First called "a hardware floor". **Wrong.** The ~666 ms is almost entirely the
synchronous SPI gram transfer — 129,600 word writes at 10 MHz, shipping 1bpp
content as 4bpp, 32 SPI bits per 16 bits of payload, a CS toggle per word — not
waveform time. See `analysis/m5paper-panel-refresh.md`.

Switching script pushes from `GC16` (16 greys, for content with none) to `DU`
(1-bit) buys ~450 ms → ~260 ms of *settle*, and **does not move the push timer
at all**. Anyone who changes the mode and watches that timer will wrongly
conclude nothing happened.

Researched zephray's 57 fps paperboy and Modos' work. Honest conclusion: **the
central technique does not port** — it replaces the display controller, which
requires a raw-interface panel. Our IT8951 is in the way; ceiling here is ~5–8
fps, structurally. But the research found a documented **1bpp transfer path**
whose registers M5EPD already defines. See
`analysis/eink-fast-refresh-research.md`.

### 6. The Watchy port, and the day's worst reasoning

Started from `Watchy_MicroPatterns/` containing one `.DS_Store`. The design docs
were good; there was no code.

The extraction was genuinely cheap, exactly as designed: the rasterizer's whole
platform surface is **four canvas methods across six call sites**. A compile-time
typedef (not a virtual base — `drawPixel` is the innermost loop) plus removing a
`DisplayManager&` the renderer stored and never read meant **both firmwares
compile the same sources**.

Then the firmware flashed with verified hashes and did nothing. What followed
was hours of wrong reasoning, all from one root cause:

> **Zero serial output is not diagnostic on this device.** The official
> InkWatchy image runs visibly while emitting zero bytes over 60 seconds.

Everything built on that silence was unsupported:

- A bare `Serial.println()` firmware was silent, and this was reported as
  proving the fault was outside the port code. It proved nothing.
- `--before no_reset` connecting was reported as "the chip sits in download
  mode". It was an **artifact of the immediately preceding command**, which used
  `--after no_reset` — measuring the state my own flag had created.
- Reset-line polarity was concluded twice, both times wrong. The single ROM
  banner observed was `rst:0x10 (RTCWDT_RTC_RESET)` — a watchdog reset, not a
  pin reset. **Read the `rst:` cause instead of assuming the pulse worked.**

Other dead ends: an ESP32-C3 bootloader grabbed by mistake (esptool caught it —
`Unexpected chip id in image`); an unpinned framework triggering a second
toolchain download into a full disk; `erase_flash` as a recovery step (no
effect); `verify_flash` confirming the image was always correct.

**What actually unblocked it: a 20-line firmware that only buzzed the vibration
motor.** It buzzed first try, proving the device runs code we compile — so
toolchain, bootloader, flash mode and partition layout were all fine, and the
fault was inside the firmware. **That should have been the first thing flashed.**
Two display fixes were applied before it, both guesses on an unverified premise.

The motor then became the telemetry channel, with a FreeRTOS task repeating the
furthest boot stage forever so it kept reporting even while `setup()` was
blocked. Kept behind `-DMP_STAGE_BUZZ=1`, default off.

The display fixes themselves came from InkWatchy's known-good path:
`Szybet/GxEPD2-watchy` and `init(0, initial, 10, true)` — `pulldown_rst_mode`
**true** (was false), 10 ms reset (was 2 ms). A red herring avoided: `EPD_BUSY`
is GPIO19, also VSPI's default MISO, but InkWatchy deliberately does not call
`SPI.begin()` there.

**Still unknown:** which change fixed the display. The successful flash carried
four changes at once, and the earlier "no change" report came from a period with
no working instrument. `analysis/watchy-port-attempt-log.md` §5.3 records the
bisect that would settle it.

### 7. The lesson both sessions learned independently

Another session, working on the same watch, lost hours to the same class of
error — including `timeout` not existing on macOS, producing empty output that
nearly read as "chip unresponsive".

> **Verify the instrument reads a known-good state before trusting it to report
> a bad one.**

Flashing the official working image, or the buzzing firmware, invalidates a dead
channel in minutes. `tools/device/buzz_watchy/` exists so this is one command.

### 8. State at end of session

- **M5Paper**: wipe fixed, interrupt fixed, serial console added, rasterizer ~7x,
  interpreter 21x (host), DU waveform. All flashed and verified on device except
  the last two, verified only by build + goldens at time of writing.
- **Watchy**: renders scripts on its panel, sharing the M5Paper's renderer core.
- **Gates**: 15 golden images; both firmwares build.
- **Approved, not started**: vendoring M5EPD to fix the SPI gram transfer —
  the largest remaining win (~666 ms → possibly ~150 ms).

### 9. M5EPD vendored and the transfer path patched (same day, later)

Done. `M5Paper_MicroPatterns/lib/M5EPD/` is now ours; `m5stack/M5EPD` is out of
`lib_deps`. Provenance, every local modification, and a complete diff against
pristine 0.1.5 live in `lib/M5EPD/README-VENDORED.md` and
`lib/M5EPD/upstream-0.1.5.patch`.

Three fixes, multiplicative:

1. **Bulk SPI.** Upstream shipped one 16-bit word per CS assertion, and
   `write32(uint16)` emits a `0x0000` preamble first — 32 wire bits per 16 bits
   of payload, plus two GPIO toggles, 129,600 times per frame. IT8951 pack-write
   (`I80CPCR` bit 0, which upstream *already enabled*) allows one preamble per CS
   assertion. Now: CS low, one preamble, one `writeBytes()`, CS high. Wire bytes
   are bit-identical.
2. **1bpp.** Load declared `IT8951_8BPP` with `Area_X = X/8`, `Area_W = W/8`;
   `UP1SR+2 |= 1<<2`; `BGVR = 0xF0<<8 | 0x00`. A quarter of the bytes.
3. **`M5EPD_SPI_FREQ_HZ`** — still 10 MHz, but now one named constant instead of
   two magic numbers. Raising it is deliberately left as a measured experiment.

**The thing the plan did not anticipate:** the IT8951 applies rotation *during
the load*, which is incoherent with 8-pixels-per-byte packing — it would rotate
whole bytes. And our canvas is 540 wide, which is not a multiple of 8, let alone
the 32 some LUTs demand. Both problems have the same answer: transpose on the
host into panel-native 960×540 (960 = 30 × 32) and give the driver panel-native
coordinates. `M5EPD_Canvas::pushCanvas1bpp()` does that; it returns false and
changes nothing if the geometry is unsupported, so the 4bpp path is a live
fallback, not dead code.

Also: `Update1bppArea()` latches 1bpp display mode on and stays fire-and-forget;
the mode bit is cleared lazily by the next 4bpp transfer, which waits on
`CheckAFSR()` — exactly where upstream already paid that wait. Waveshare clears
it inline and blocks on the whole waveform; we would rather not.

**Not verified on device at time of writing.** Builds + 15/15 goldens only. The
device checklist is in `analysis/m5paper-panel-refresh.md`.

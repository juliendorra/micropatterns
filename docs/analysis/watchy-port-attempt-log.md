# Watchy port: first implementation attempt (2026-08-27)

Companion to `watchy-port-design.md` (the plan) and
`watchy-hardware-and-references.md` (the hardware identification). This file is
the **record of actually doing it**: what got built, what worked, and — at
length — the rabbit hole that followed, because that hole cost more time than
the port itself and is not obvious from the resulting code.

**Status: code complete, execution unproven.** The firmware builds, flashes with
verified hashes, and shares the M5Paper's renderer core. It has never been
observed running. See §4.

## 1. What was built

`Watchy_MicroPatterns/` — a standalone Arduino + GxEPD2 firmware:

- `platformio.ini` — one environment (`watchy2`, board `esp32dev`), not the
  six-way board matrix InkWatchy carries. Watchy 2.0 is the only target, per the
  hardware identification.
- `src/watchy_canvas.h` — the entire platform surface, ~30 lines.
- `src/main.cpp` — boot, render, cycle scripts by button or serial command.
- `src/embedded_scripts.h` — four real scripts compiled into flash, taken from
  the device backup. No WiFi, no filesystem, no server: it boots and draws.

Build cost: **RAM 6.6% (21,756 B static), Flash 11.4% (359,569 B)**, leaving
~300 KB of heap for the display list — the main RAM risk on a no-PSRAM PICO-D4
looks survivable, though this is a static figure and not a measurement under
load.

## 2. The shared-core extraction

The design doc's central claim held up exactly: the rasterizer's whole platform
surface is **four methods across six call sites** (`_canvas->width/height/
drawPixel/fillCanvas`). Verified by `grep`, not assumed.

Two changes made the core platform-agnostic, and the Watchy firmware compiles it
**straight out of the M5Paper tree — no copies, one codebase**:

1. **`mp_canvas.h`** selects the canvas by **compile-time typedef**, not an
   abstract base class. This is deliberate: `drawPixel` is the innermost loop of
   every fill, and a virtual call per pixel would cost real time on a 240 MHz
   ESP32. `-DMP_PLATFORM_WATCHY` picks `WatchyCanvas`; the M5Paper is the
   default so its build is unchanged.
2. **`DisplayListRenderer` now takes `MPCanvas*`** instead of `DisplayManager&`.
   The `DisplayManager&` member was stored in the initializer list and then
   **never read again** (`grep -n '_displayMgr' display_list_renderer.cpp`
   returns exactly one line — the initializer). It coupled the renderer to
   M5Paper-only display code for no benefit.

Also removed: a stray `#include <M5EPD.h>` in `micropatterns_runtime.h` that no
symbol in that file used.

**Regression evidence:** `tools/host_harness` `make verify` passes **9/9 golden
images** after the refactor. The rendering is pixel-identical. The harness
itself needed a one-line update for the new constructor signature.

## 3. Colour: confirmed a non-issue

The DSL only ever emits `DRAWING_COLOR_WHITE = 0` and `DRAWING_COLOR_BLACK = 15`.
The M5Paper's "4bpp" is nominal; 15 is just what M5EPD wants for black. Mapping
to `GxEPD_BLACK`/`GxEPD_WHITE` loses **nothing** — the 14 intermediate greys the
M5Paper hardware can display are never requested by any script. Pattern fills
get their texture from pixel-level dithering in the asset bitmaps, not grey
values, so they survive intact.

Terminology note: neither device is colour. Calling this a "colour port" is
wrong; the axis is *greyscale depth*, and the DSL never used it.

## 4. The rabbit hole: flashed successfully, never ran

**Symptom:** esptool connects, identifies the chip (ESP32-PICO-D4 rev v1.0, MAC
`4c:75:25:a7:65:c0`), and writes flash with every hash verified. After any
reset the chip produces **zero serial bytes at 115200** — no application output
and, critically, **no ROM banner**. The e-ink never changes, which is expected:
e-ink retains its last image indefinitely, so an unchanged panel means nothing
was ever drawn, not that drawing failed.

### 4.1 What was ruled out, and how

- **Not the port code.** A firmware whose entire content was
  `Serial.println()` in `loop()` was flashed the same way and was **also
  completely silent**. This is the single most useful datapoint in this file: it
  moves the fault out of Micropatterns entirely. The bisect firmware is kept at
  `tools/device/hello_watchy/` precisely so this test is one command next time.
- **Not the pinout.** Checked against InkWatchy's own `src/defines/condition.h`,
  which demonstrably drives this exact panel on this exact watch:
  `EPD_CS 5, EPD_DC 10, EPD_RESET 9, EPD_BUSY 19`; buttons `MENU 26, BACK 25,
  DOWN 4, UP 32`. All match.
- **Not the flash layout.** Bootloader `0x1000`, partition table `0x8000`,
  `boot_app0` `0xe000`, app `0x10000` with `huge_app.csv` is internally
  consistent for a stock Arduino build. Confirmed by the session that later
  reflashed this device: InkWatchy's app offset of `0x20000` comes with its own
  `partitions.csv`, and **copying that offset without that table would break
  things**. Do not cargo-cult it.
- **Not the display init.** Boot markers were added on both sides of
  `g_display.init()` plus a 5-second heartbeat in `loop()`, specifically so that
  "hung in display init" and "never started" become distinguishable. Neither
  marker ever appeared.

### 4.2 Dead ends, in the order they were tried

1. **Baud rate.** `pio run -t upload` at `460800` failed with
   `Failed to connect ... No serial data received`; plain esptool at `115200`
   connected immediately. `upload_speed` is now pinned to 115200 with a comment.
   Same class of failure as the M5Paper's CH9102 (see the incident doc).
2. **Wrong bootloader path.** Grabbed
   `tools/sdk/esp32c3/bin/bootloader_dio_40m.bin` — the **ESP32-C3** one. esptool
   caught it: `Unexpected chip id in image. Expected 0 but value was 5`. The
   correct path is `tools/sdk/esp32/bin/`. A good example of a tool refusing to
   do the wrong thing.
3. **Reset-line polarity — asserted twice, wrong both times.** First concluded
   RTS→EN, then concluded DTR→EN after seeing a ROM banner following a DTR
   pulse. That banner was `rst:0x10 (RTCWDT_RTC_RESET)` — an **RTC watchdog**
   reset from esptool's stub, not a pin reset. Reading the reset *cause* rather
   than assuming the pulse worked would have avoided this entirely.
   **Lesson: `rst:0x...` in the ROM banner tells you what actually reset the
   chip. Read it.**
4. **"Held in reset" theory.** Several listeners held `rts=True` for their whole
   run, which on the standard circuit pins EN low and clamps the chip in reset.
   That was a genuine bug in the *test rig* and explains some of the silence —
   but not all of it, because runs with both lines released were also silent.
5. **Missing framework pin.** The first Watchy build tried to download a second
   Arduino framework (`~3.20006.0`) because `platform_packages` was unpinned.
   This collided with a full disk. Pinning to the M5Paper's `3.20004.0` avoids
   the download entirely and keeps both firmwares on one toolchain.
6. **The peer session's sequence did not reproduce either.** The session that
   later revived this watch supplied a verbatim working sequence
   (`setDTR(False); setRTS(True); sleep(0.3); setRTS(False)` — RTS→EN,
   DTR→GPIO0, RTS released at the end). Run here, with `lsof` confirming no
   process held the port, it produced **0 bytes**. So the "your monitor clamped
   EN" explanation is plausible for some runs but does not close the case.

### 4.2b Second investigation round (same day, after a full erase)

Everything in this round is negative evidence, which is why it is recorded:
each item removes a hypothesis that would otherwise be re-tried.

- **`erase_flash` did NOT help.** This was the top untried recovery step
  suggested by the session that later revived the watch. Full chip erase, then
  reflash: still zero serial bytes. Rules out a stale partition table or stale
  NVS confusing the bootloader.
- **The flash contents are correct.** `esptool verify_flash` digest-matched both
  the bootloader at `0x1000` and the app at `0x10000`. So this was never a
  corrupt, truncated or mis-offset image, and "reflash it again" is not the fix.
- **The panel is proof of ABSENCE, not presence.** After the erase the e-ink
  still showed InkWatchy's last frame. That is expected — e-ink holds its image
  with no power behind it — and it means nothing has drawn since InkWatchy last
  ran. Do not read an unchanged panel as "the app drew something wrong"; read it
  as "nothing drew at all".
- **No static line state runs the app.** With a 5-second heartbeat compiled into
  the firmware (the reason that heartbeat exists), all four static DTR/RTS
  combinations were held for 11 s each. Silence in all four. A static line state
  cannot reboot the chip, so this test was only meaningful once the heartbeat
  existed — before that, silence was ambiguous.
- **No reset pulse runs the app either.** For each boot-mode line held in each
  state, the other line was pulsed as EN. All four combinations: silence.

### 4.2c The most concrete lead: the chip sits in DOWNLOAD MODE

```
esptool.py --port /dev/cu.usbserial-110 --before no_reset flash_id   # connects instantly
```

`--before no_reset` connecting means the chip is **already in the ROM loader**,
with no reset performed. An ESP32 does that when **GPIO0 is held low**.

This reframes the entire investigation and is the single best lead:

> Every flash "succeeded" because esptool kept finding the chip already sitting
> in the ROM loader — not because any host-issued reset ever reached it. The
> application was therefore never once started, which is exactly consistent with
> no serial output, no ROM banner, no panel update, and no crash-loop banners
> (a crash loop would produce a *flood* of banners, not silence).

If true, the remaining question is what holds GPIO0 low — the USB bridge's
auto-reset circuit, or something physical such as a stuck or case-pressed
button. A cold power-cycle with no button contact would distinguish these; it
had not been performed at the time of writing.

**Caveat, stated because it matters:** this is a hypothesis that fits all the
evidence, not a proven cause. It is contradicted by the fact that another
session flashed AND ran InkWatchy on this same device on the same day. Whatever
that session does differently is the answer, and it is not yet known.

### 4.3 Unresolved

The device was subsequently unbricked and reflashed to InkWatchy by another
session, so the failing state no longer exists to probe. Whether the fault was
the test rig, a recovery step never performed (e.g. `erase_flash` clearing a
stale partition table or NVS), or something else is **not established**. Do not
record this as solved.

Two candidate next steps if this is picked up again:
- Try `erase_flash` before writing, then flash the full set. The peer session
  flashed bootloader + table + app + littlefs together and did not isolate the
  minimum, so there is no clean evidence about whether the erase is required.
- Switch from upstream GxEPD2 to the `Szybet/GxEPD2-watchy` fork InkWatchy
  vendors, which carries fixes for this SSD1681/GDEH0154D67 pairing including a
  first-boot grey/ghosting workaround. Not attempted — changing libraries on a
  hunch, before a boot log exists, would only add variables.

## 5. Settled product question

`watchy-port-design.md` §7.2/§9 lists coordinate mapping (960x540 scripts on a
200x200 panel) as an open product decision. **It is not open.** Per the project
owner: *no scaling, one pixel is one pixel* — that is a core principle of
Micropatterns, and the web editor already previews both screens. Scripts adapt
by reading `$WIDTH`/`$HEIGHT` (Circuits documents this in its own header);
scripts with hardcoded coordinates will simply be wrong at 200x200, and that is
acceptable and intended. The implementation renders at true 200x200 with no
scaling anywhere.

## 6. Environment gotchas hit on this machine

- **Disk at 100%.** Broke PlatformIO in confusing ways
  (`[Errno 28] No space left on device` surfacing as "Package Mirror" warnings
  and a `PackageException`). `~/.platformio/.cache` had grown to 1.5 GB of
  download archives and failed partial installs; clearing it is safe and it
  re-downloads on demand. **Run `df -h` before trusting any weird tool failure.**
- **`.DS_Store` files** break the ESP-IDF component manager with
  `NotADirectoryError` once Finder has browsed a project.
  `find . -name .DS_Store -delete` before building.

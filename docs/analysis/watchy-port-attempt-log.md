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

### 4.2c FALSIFIED: "the chip sits in DOWNLOAD MODE"

> **This section is WRONG and is kept only as a record of the mistake. The
> hypothesis below was falsified, and the observation that produced it was an
> artifact of my own tooling. Read §4.2d before believing anything here.**

### 4.2c (original text, retained)

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

### 4.2d The correction: silence is NOT diagnostic, and 4.2c was self-inflicted

Two independent findings demolish the reasoning above.

**1. The download-mode observation was an artifact of my own command.** The
esptool invocation immediately preceding that test was:

```
esptool.py ... --before default_reset --after no_reset verify_flash ...
```

`--after no_reset` **leaves the chip sitting in the ROM loader**. The very next
command then "discovered" the chip in the ROM loader. I measured the state my
previous flag had created and reported it as a property of the device. An
independent check by the other session, with no such flag in play, gives the
opposite result: `--before no_reset` FAILS and `--before default_reset`
succeeds — i.e. the chip requires a host-issued reset and is **not** idling in
the loader. The hypothesis is falsified.

**2. Zero serial output is not diagnostic on this hardware.** The other session
flashed the official prebuilt `Watchy_2-demo.bin` (InkWatchy v2.1.0, full 4 MB
image, hash verified). The watch **visibly works** — display updating, running
normally — and that same known-good image produced **zero serial bytes** in a
60-second capture at 115200. Not a partial log: zero.

The consequence is severe and applies to everything above:

> **Every conclusion in §4.1-4.2c that rests on "it was silent, therefore it was
> not running" is unsupported.** That includes the bisect result. The bare
> `Serial.println()` firmware being silent very likely did NOT mean it failed to
> execute. The instrument was dead; the chip may have been fine throughout.

Neither session has a confirmed mechanism for why run-mode UART does not reach
the host while esptool's stub communicates fine over the same pins. Nobody
should invent one.

Also corrected: **the ROM banner's absence diagnoses nothing here.** The other
session never saw one either, including while the app was demonstrably running
and emitting its own log lines. InkWatchy additionally sets
`CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y`, disabling second-stage bootloader output
by design.

**And the partition layout is confirmed fine.** The official v2.1.0 release puts
the factory app at **0x10000** — the same offset used here. The earlier
"InkWatchy uses 0x20000" is true of the repo's generated `partitions.csv` but
not of the shipped release.

### 4.2e Why the panel stayed blank for the other session's builds

Not the same fault as ours, but it explains the device's behaviour after this
session's work: the `erase_flash` performed here wiped the **littlefs**
partition, and the other session subsequently flashed only `firmware.bin`,
never `fs.bin`. So InkWatchy booted and ran correctly with every font and image
missing, drew nothing, and the panel held its last latched frame. The tell,
misread as harmless at the time:

```
[E][vfs_api.cpp:99] open(): /littlefs/weather/hourly/315446400 does not exist
```

This does **not** explain the Micropatterns firmware's behaviour — that build
embeds its scripts in flash and uses no filesystem at all.

### 4.2f What this leaves as the actual open question

The Micropatterns firmware may well have been executing the whole time. What is
established is only that **the panel never changed**. With serial removed as
evidence, the prime suspect becomes the display path itself:

- Upstream `zinggjm/GxEPD2` is used here. InkWatchy vendors
  `Szybet/GxEPD2-watchy`, a fork carrying fixes for this exact
  SSD1681/GDEH0154D67 pairing, including a first-boot grey/ghosting workaround
  and a documented border-waveform quirk. Trying that fork is now the obvious
  next step, where before it would have been changing libraries on a hunch.
- `g_display.init()` blocking on the BUSY line remains possible and, without
  serial, is invisible.

**Any further debugging must not depend on UART.** Use the panel itself as the
output channel — e.g. fill the screen with a distinctive pattern as the very
first action after `init()`, before any script work — or drive a GPIO. A
"silent" reading proves nothing on this device.

### 4.2g Instrument discipline (the actual lesson)

Both sessions lost hours to the same class of error: **an instrument returning
nothing for reasons unrelated to the device under test.**

- Here: silence read as "not running", and `--after no_reset` read as device state.
- The other session: `timeout` does not exist on macOS, so two esptool tests
  produced no output and were nearly read as "chip unresponsive".

> **Verify the instrument reads a known-good state before trusting it to report
> a bad one.** Flashing the official working image FIRST — and discovering it is
> also silent — would have invalidated the serial channel on day one and saved
> this entire investigation.

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


---

# 5. RESOLVED (2026-08-27)

The watch displays scripts. The renderer core is shared with the M5Paper, one
pixel per pixel at 200x200, no scaling.

## 5.1 The step that actually unblocked this

Not a display change — **a 20-line firmware that did nothing but buzz the
vibration motor.**

For hours the working assumption was that the app was running and only the
display failed. That was never tested. The motor-only firmware (no display, no
libraries, no globals with constructors, just `digitalWrite(GPIO 13)` in
`loop()`) buzzed immediately, proving the toolchain, bootloader, flash mode,
partition layout and boot path were all fine and that the fault was inside the
Micropatterns firmware.

**That test should have come first.** Two display fixes were applied before it,
both guesses layered on an unverified premise. The correct order is: prove the
device runs code you compiled, *then* debug what that code does.

The motor then became the telemetry channel, since serial is worthless here and
the panel cannot report progress that happens before the panel works. A separate
FreeRTOS task repeated the furthest boot stage reached, forever, so it kept
reporting even while `setup()` was blocked — a one-shot buzz per stage is not
enough, because a hang leaves you with silence and no count. Kept in the tree
behind `-DMP_STAGE_BUZZ=1`, default off.

## 5.2 What changed on the display side

Two things went in, both derived from InkWatchy's known-good Watchy-2 path
rather than invented:

1. **`Szybet/GxEPD2-watchy`** instead of upstream `zinggjm/GxEPD2`, pinned to
   the commit InkWatchy pins. The fork carries fixes for this exact
   SSD1681/GDEH0154D67 pairing.
2. **Panel bring-up matched to `initDisplay()`** in InkWatchy:
   `init(0, initial, 10, true)` — note **`pulldown_rst_mode = true`** (was
   `false`) and **reset duration 10 ms** (was 2 ms) — plus explicit `pinMode()`s
   with `BUSY` as INPUT, and `selectSPI(SPI, 20 MHz, MODE0)`.

A tempting red herring, recorded because "fix the obvious pin clash" would have
been wrong: `EPD_BUSY` is GPIO19, which is *also* VSPI's default MISO. InkWatchy
deliberately does **not** call `SPI.begin()` on Watchy 2 because the default
VSPI pins already match the wiring, so neither do we.

## 5.3 What is NOT known, and should not be claimed

**Which change fixed it is undetermined.** The flash that first showed a working
panel contained the fork, the InkWatchy init parameters, the probe pattern *and*
the stage reporter. An earlier flash contained the fork and the init parameters
and was reported as "no change" — but that report cannot be fully trusted as
evidence of failure, because on this device we had no working instrument at the
time and the observation window was not controlled.

So there are two live possibilities and no evidence separating them:

- the init parameters (most likely `pulldown_rst_mode`) were decisive; or
- the earlier flash also worked and was simply not observed working.

Bisecting this would mean reverting one change at a time and re-testing with the
motor telemetry enabled. **Not done.** If anyone needs certainty — for example
before dropping the fork and returning to upstream GxEPD2 — that is the
experiment to run.

## 5.4 The lesson, stated plainly

Both sessions working on this watch lost hours to the same error: **an
instrument returning nothing for reasons unrelated to the device under test.**
Serial silence was read as "not running"; `--after no_reset` was read as device
state; on the other session, macOS lacking `timeout` produced empty output that
nearly read as "chip unresponsive".

> Verify the instrument reads a known-good state before trusting it to report a
> bad one. A cheap positive control — the buzzing firmware, or flashing the
> official working image — invalidates a dead channel in minutes.

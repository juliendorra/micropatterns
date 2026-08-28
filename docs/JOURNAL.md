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


---

## 2026-08-27 (later) — Watchy UX, and a crash that only bites one device

### Buttons

`BTN_UP` was GPIO **32**, which is Watchy **1/1.5**. Watchy 2.0 is **35**. That
button had never worked. Found by reading InkWatchy's `condition.h` rather than
by testing — the failure was silent because a dead button is indistinguishable
from an unpressed one.

The corner mapping was then wrong in a second way: the pin *names* do not match
the physical layout. First attempt put MENU top-left; on the device the
indicator showed MENU is **bottom-left** and BACK is top-left. Fixed by
dispatching actions on **corner** rather than pin name, so the layout lives in
exactly one table and cannot drift from the indicator again.

The corner indicator was designed as its own verification: press a button, see
which corner lights. That is how the inversion was caught in one round.

### The script name was never implemented

Reported as "maybe it is offscreen?" — it was not offscreen, it was absent. The
Watchy firmware had only ever rendered the script itself. Now shown on its own
cleared frame, and only when the script actually changes.

### Flashing

Every render was a full refresh (2600 ms, flashes) where a fast partial update
(500 ms, no flash) would do — safe because the content is pure black and white.
See `analysis/watchy-panel-updates.md`. De-ghost interval measured on device at
24, unlike the M5Paper's equivalent which is still a guess at 8.

### The thunderstorms crash, and what the Watchy told us

`thunderstorms` hangs the **M5Paper** with a task-watchdog abort and a corrupted
backtrace, with **both cores idle**. Bisected on hardware: clean 3/3 on the
waveform commit, reliable failure on the interpreter commit that followed.

Then the useful negative result: **the Watchy cannot be made to crash on the
same script**, despite compiling the *same* core sources — same
`DisplayListItem`, same slot refactor, same snapshot pool.

That reshapes the diagnosis. A plain "dangling pointer in shared code" does not
fit; something M5Paper-specific is required. Ranked candidates:

1. **Stack overflow in `RenderTask`** (bounded 8192 words). The Watchy renders
   from `setup()`/`loop()` on a much larger stack. This fits every symptom —
   corrupted backtrace, both cores idle, one device immune.
2. **Canvas size.** 540x960 vs 200x200; thunderstorms is procedural and emits a
   larger list on the M5Paper.
3. **PSRAM.** The M5Paper's canvas and occlusion buffer are PSRAM-backed; an
   overrun there behaves differently from one on the heap.

Recorded because it is easy to misread: **"could not reproduce" is not proof of
absence.** The Watchy runs the same suspect code and may simply not be hitting
the trigger.

---

## 2026-08-28 -- Server sync on the Watchy, and three things it uncovered

The Watchy's scripts were compiled in (`EMBEDDED_SCRIPTS[]`, 1018 lines of
header). Replacing that with a real sync was meant to be plumbing. It was not.

### The unification actually happened

The instinct was to write a small fetch loop for the Watchy. That would have
been a second copy of a procedure whose subtleties were paid for by the script
loss incident -- above all the `clearAllScriptData()` that used to run BEFORE
the fetch, so a 404 left the device with nothing. Instead `mp_sync_scripts()`
now holds the whole procedure and knows nothing about tasks, queues or
displays. `FetchTask` is down to the queue, the watchdog and the interrupt
flag; the Watchy calls the same function from `loop()`.

`ScriptManager` and `MPNetworkManager` are compiled straight out of the M5Paper
tree, like the renderer already was. `MPNetworkManager` holds a
`SystemManager*` it never dereferences, so the Watchy passes `nullptr` rather
than porting a class that pulls in `M5EPD.h`.

### Dead end 1: `lib_ldf_mode = deep+`

The shared managers arrive via `build_src_filter` from another directory, and
PlatformIO's dependency finder only scans sources under `src_dir`. So the build
failed with `HTTPClient.h: No such file or directory`.

`deep+` is the documented answer and it is wrong here. It makes the framework's
**own** WiFi library fail to build -- `Network.h: No such file or directory` --
because `WiFi/library.properties` declares no dependency on `Networking`, and
`deep+` stops PlatformIO from wiring the framework libraries' include paths to
each other. Reproduced on a bare project containing nothing but
`#include <WiFi.h>`, which is the only reason this was believed rather than
blamed on the port.

Naming the libraries in `lib_deps` (`WiFi`, `Networking`, `HTTPClient`, ...)
does not help either: they appear in the dependency graph and the build still
fails identically.

What works is four `#include` lines in the Watchy's own `main.cpp`. They are
not used there; they exist to put the libraries in the graph.

### Dead end 2: `class NetworkManager`

Arduino 3.1 / IDF 5.3 -- the platform the Watchy moved to for the NVS fix --
ships its own global `class NetworkManager` in the Network library. Ours
collided with it the moment the Watchy compiled `network_manager.cpp`. The
M5Paper's Arduino 2.0.4 has no such class, so this was invisible until now.
Renamed to `MPNetworkManager`.

Worth noting the shape of the error: `redefinition of 'class NetworkManager'`
pointing at our own header, with the "previous definition" note fifteen lines
further down in the framework. Easy to read as a broken include guard, which is
what was assumed first.

### The crash the refactor caused, and what it exposed

After extracting the sync, the M5Paper crash-looped on boot:

    Guru Meditation Error: Core 1 panic'ed (Unhandled debug exception).
    Debug exception reason: Stack canary watchpoint triggered (MainCtrlTask)

Nothing in the change touched `MainCtrlTask`. Confirmed a real regression by
building and flashing the previous commit from a git worktree -- clean, zero
panics -- rather than assuming it was pre-existing.

The cause: removing the large `FetchTask_Function` from `main.cpp` changed
inlining in that translation unit, and `MainControlTask_Function`'s stack frame
grew from **1072 to 1232 bytes** (read out of both ELFs with `objdump`, from
the `entry a1, 0x430` / `entry a1, 0x4d0` instructions).

`MAIN_CONTROL_TASK_STACK_SIZE` was 4096 with a comment claiming `// Words`.
`xTaskCreate()` on ESP-IDF takes bytes. So a task doing ArduinoJson parsing and
SPIFFS calls had 4KB, and had been surviving on **under 160 bytes of margin**.

The lesson is not "be careful when refactoring". It is that the margin was
invisible: nothing reported it, and the failure mode was a boot loop rather
than a warning. It is 8192 now, matching RenderTask and FetchTask, and the
high-water mark is logged past the task's deepest call.

### The finding that is not fixed: TLS cannot allocate

With the crash gone, the sync runs end to end and fails at the last step:

    [V][ssl_client.cpp:62] Free internal heap before TLS 23212
    [E][ssl_client.cpp:37] (-32512) SSL - Memory allocation failed

WiFi associates, DHCP completes, the URL is right. mbedTLS then cannot get its
handshake buffers out of **23KB of free internal RAM**. Total free heap reads
3.8MB, which is PSRAM and no use to mbedTLS.

The 23KB is the point. BLE is built at boot (commit e45a995, because lazy init
from `loop()` crashed the Watchy) and the controller plus host hold roughly
90KB of internal DRAM for the life of the device. The WiFi driver wants ~40KB
more on top. The provisioning window and the sync are competing for the same
scarce pool, and provisioning currently wins permanently.

This is pre-existing, not caused by the sync work -- but the sync work is what
made it visible, because nothing before it exercised HTTPS with BLE resident.

Not fixed here because the obvious repair -- initialise BLE on demand -- is the
exact thing e45a995 moved AWAY from after it crashed the Watchy. The options
worth weighing, none yet tested:

  1. `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` at boot. Frees the
     Classic-BT half (tens of KB) and keeps BLE. Cheapest if it is not already
     being done.
  2. De-init BLE for the duration of a sync and rebuild it after. Reintroduces
     the failure mode e45a995 was written to avoid, on the device with less
     headroom.
  3. Route mbedTLS allocations to PSRAM on the M5Paper. Needs an sdkconfig
     change the precompiled Arduino libraries do not allow on this platform,
     and does nothing for the Watchy, which has no PSRAM.

The Watchy is the harder case either way: ~300KB total, no PSRAM, and it must
hold BLE, WiFi and TLS on the same pool.

---

## 2026-08-28 (later) -- Why fetching was broken on both devices

Two independent faults, and one wrong hypothesis discarded on the way.

### The wrong hypothesis: the root CA

The API's chain now reads leaf <- `YE2` <- `ISRG Root YE` <- `ISRG Root X2`,
while the firmware pins **ISRG Root X1**. That looks exactly like an expired
pin, and the Watchy's failure was fast enough to fit.

It is not the fault. `ISRG Root X2` is cross-signed by X1, so the pinned root
validates the whole chain -- confirmed against the live server with
`openssl s_client -CAfile`, using the PEM extracted from the firmware source
rather than a copy from the web. Worth recording precisely because the theory
was so plausible; pinning "both roots" would have shipped a change that fixed
nothing and hidden the real cause.

### Fault 1: BLE was holding the RAM that TLS needs

    [V][ssl_client.cpp:62] Free internal heap before TLS 23212
    [E][ssl_client.cpp:37] (-32512) SSL - Memory allocation failed

The M5Paper has 4MB of PSRAM and it is no help. mbedTLS, the WiFi driver and
the BLE controller all draw from **internal DRAM**, and `ESP.getFreeHeap()`
counts PSRAM -- so the device reported 3.8MB free while the pool that mattered
was nearly empty. That is how a hard wall stayed invisible: every number on
screen said there was plenty of memory.

Budget, roughly: a handshake wants ~45KB of internal DRAM in blocks up to
~17KB; the WiFi driver ~40KB; the BLE stack ~90KB for as long as it is up.

BLE is now built when a provisioning window opens and torn down when it
closes, and a sync tears it down first. Numbers after: internal heap before
TLS **23,212 -> 103,744** on the M5Paper; free heap after a sync **80KB ->
170KB** on the Watchy.

Building BLE at boot was itself a workaround (e45a995) for lazy init crashing
the Watchy -- and it was aimed at the wrong cause. The Watchy was on Arduino
2.0.4 then, where NVS is broken on this ESP32-PICO-D4, and `BLEDevice::init()`
reads NVS. The platform move had already fixed the real fault; the workaround
outlived it and cost every HTTPS fetch on both devices.

One detail that matters: `BLEDevice::deinit(false)`, never `deinit(true)`. The
`true` variant calls `esp_bt_controller_mem_release()`, which permanently
forfeits the controller's reserved region -- no later init can succeed until
the device reboots.

### Fault 2: the JSON parse was truncating over TLS

With memory fixed the M5Paper fetched 7/7 and the Watchy fetched **5/7**. The
two that failed were the two 15KB City scripts:

    Content length (15320) is large ...
    JSON parse error for 'city-2-by-telohtrab': IncompleteInput

`deserializeJson(doc, http.getStream())` looks like the memory-efficient
choice. Over TLS it silently truncates: `WiFiClientSecure::read()` returns -1
whenever no decrypted bytes happen to be buffered, ArduinoJson takes that for
end-of-stream, and the parse stops partway through. It only shows on payloads
spanning several TLS records, which is exactly why the five small scripts went
through and the two large ones did not -- and why this never surfaced on the
M5Paper, which was failing earlier for fault 1.

Reading the body with `getString()` first, then parsing, then freeing it, fixes
it. 7/7 on both.

Also dropped the `MAX_SCRIPT_CONTENT_LEN` (5600) heuristic around it.
ArduinoJson 7 grows on demand and ignores the fixed capacities the old
`DynamicJsonDocument` took, so quoting that number only implied a limit that
does not exist -- real scripts on the server reach 15KB.

### Watchy sleep, and two more serial dead ends

The Watchy loop spun at 20ms for the full 77s between renders. It now light
sleeps, waking on timer, buttons, or a provisioning window.

Deep sleep was considered and rejected on arithmetic, not taste: a wake costs
a full boot (SPIFFS mount, script load, parse, GxEPD2 init) measured at ~9s.
77s at ~0.8mA is about 62mA-seconds; 9s at ~100mA is about 900. Deep sleep is
worse at this cadence and only wins once the interval is minutes.

The serial console cost two attempts:

  - `esp_sleep_enable_uart_wakeup()` does nothing on Arduino 3.1. It fires only
    if UART0 is clocked from REF_TICK / XTAL, and Arduino 3.1 uses APB, which
    is gated in light sleep, with no supported way to change it after
    `Serial.begin()`. The M5Paper's works only because 2.0.4 defaulted to
    REF_TICK. The device woke on timer and buttons and stayed deaf to serial.
  - Sleeping in 4s chunks and polling on each wake does not recover it either.
    Bytes sent to a sleeping device are **dropped, not delayed** -- the RX FIFO
    is unclocked.

The working answer uses a property of the cable rather than the radio: opening
the port asserts DTR/RTS and resets the board, so a debugger always arrives at
a freshly booted device. The firmware stays awake 60s after boot and re-arms
that window on every byte received.

### Panel wording

The two firmwares had drifted into separate vocabularies for the same events
-- "NetMgr Fail!" / "Fetch: Fetch OK" / "Render Fail: eyes" against "Sync
failed" / "no WiFi" / "Render error". `mp_messages.h` now holds every
user-facing string and both compile it, and sync outcomes travel in
`ScriptSyncResult::message` so a sync reports itself identically on either
device.

The M5Paper's render error also had to be split across two lines: that panel
fits 30 glyphs at text size 3 (6x8 font x3 = 18px across 540px) and does not
wrap, so "Render error: city-2-by-telohtrab" was 33 glyphs and lost its ends.

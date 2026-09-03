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

---

## 2026-08-28 (later still) -- The fetch memory ceiling, and the tools kept

### How large a script can the devices actually fetch?

Reading the body in full before parsing fixed truncation over TLS but gave the
fetch a real ceiling, so it was measured rather than estimated. Watchy,
mid-sync, WiFi and TLS resident, internal heap:

| script | size | at start | after body | after parse | cost |
|---|---|---|---|---|---|
| art-deco-4 | 1,439 | 86,252 | 84,780 | 83,156 | 3.1K |
| circuits | 8,380 | 81,532 | 77,868 | 69,464 | 12.1K |
| city-by-telohtrab | 12,284 | 76,720 | 73,892 | 61,672 | 15.0K |
| city-2-by-telohtrab | 15,320 | 81,460 | 70,884 | 55,636 | **25.8K** |

Peak is about **1.7x the script size**: the raw body and the JsonDocument's copy
of the content string are both alive until the body is released after parsing.
A fetch starts with ~81KB free on the Watchy and ~104KB on the M5Paper, putting
the wall near **45KB and 60KB of script**. Comfortable to ~30KB. The largest
script on the server today is 15KB.

`fetchScriptContent()` now checks `2x + 8KB` against free internal heap before
allocating. Without it an over-large script would exhaust the heap somewhere
unpredictable -- possibly inside the TLS layer, taking the rest of the sync
with it -- rather than failing as one named script the caller reports and moves
past. The existing local copy is untouched either way.

The render is a separate and probably tighter limit, bounded by display-list
size rather than source bytes. Not measured.

### Tools kept, and why

Three diagnostics from this work existed only in a scratch directory, which is
the wrong place for the evidence behind decisions the codebase now depends on.
All are under `tools/device/probes/`, each with a README recording what it
proved:

- **`nvs-probe/`** -- the A/B that decided the Watchy's platform. Run it on any
  future platform bump before trusting NVS.
- **`ldf-wifi-probe/`** -- why `lib_ldf_mode = deep+` is banned in the Watchy's
  `platformio.ini`, in nine lines.
- **`serial-probe/`** -- separates "the app is not running" from "I cannot hear
  it". Deliberately left with `flash_mode = qio`, the build that proved qio
  stops this board executing entirely.

`tools/device/mpcon.py` replaces the throwaway serial script that was retyped a
dozen times during this work. Its `browse-timing` subcommand is a real
regression check: it reports the title-to-title interval and how many renders
started, where **exactly one** is correct.

Worth recording that the tool's own line-matching was wrong twice before it was
right, and both times it reported a regression that did not exist -- the
M5Paper emits both a console echo and a firmware log per step (double count),
and "Triggering render" also matches the 77s timer wake (false positive). A
measurement tool is not evidence until it has been checked against a case whose
answer is already known.

---

## 2026-09-01 -- The Watchy's clock, and $COUNTER per script

Two things the Watchy had been getting wrong quietly, both the same shape: the
shared runtime was fully wired, and the platform layer under it never supplied
the values.

### $HOUR/$MINUTE/$SECOND were always zero

`main.cpp` called `runtime.setTime(0, 0, 0)` on every render. Not a stub that
failed loudly -- a plausible-looking call that made every time-dependent script
draw midnight, forever, while the panel kept updating every 77s as if something
were happening. The parser, the slot table, the variables and `setTime()` are
all compiled from the M5Paper tree and were always correct; `system_manager` is
the file that reads the RTC there, and it was never ported (the port design doc
says as much: "Not ported at all -- nothing on the Watchy needed it").

`watchy_rtc.{h,cpp}` is the missing piece: probe I2C, read BCD time, done.

**It does not use SmallRTC, which the design doc named.** SmallRTC is a Watchy
library component and pulls that library's own GxEPD2 assumptions into a build
that already pins the Szybet fork; `Rtc_Pcf8563` is light but PCF8563-only, and
this watch's chip had never actually been identified -- the hardware doc records
DS3231 on v1.0 and PCF8563 on v1.5/2.0 *per SQFMI*, unverified, and says a bus
scan is the only way to settle it. The two chips answer at 0x51 and 0x68, which
do not overlap, so probing both identifies the part instead of guessing at it.
About 130 lines, two register maps, no new dependency.

**The bus scan settled the open hardware question: this watch has a PCF8563.**

Both chips carry a flag meaning "my oscillator stopped, do not trust me" --
PCF8563's VL bit, DS3231's OSF. `valid()` reports it, and that is what decides
whether boot goes to NTP. The M5Paper infers the same condition from the clock
reading exactly `00:00:00`, which is a guess that is wrong once a day.

NTP runs at boot only when the chip says it is untrustworthy, and again on the
manual sync gesture -- both parts drift minutes a month, nothing else corrects
them, and the sync is already bringing up WiFi. Timezone is a hardcoded `+1`
constant matching the M5Paper's `SystemManager` default, because this firmware
has no settings store to hold anything else. **Neither device handles DST**; the
M5Paper passes `daylightOffset_sec = 0` too, so both are an hour out in summer.

### $COUNTER was one number for the whole device

`static int g_counter` incremented once per render. It advanced correctly on
every path (manual, auto re-render, and exactly once per settled title-browse
rather than once per press) -- but it was **one sequence shared by every
script**, and it lived only in RAM, so leaving a script and coming back resumed
wherever the others had got to, and any reboot sent all of them to zero.

The M5Paper has had per-script persisted counters all along, in
`script_states.json` via `ScriptManager` -- and that file is already compiled
into the Watchy firmware. The fix is to use it: load the script's own state,
increment, render, save. `renderScript()` now takes the state in and reports the
time it used back out, the same in/out shape `RenderController` has on the
M5Paper. Same storage format, so a script moved between the two devices keeps
counting.

An abandoned render (button pressed mid-raster) does **not** save, so a frame
that was never shown does not consume a tick -- the same principle as winding
back the de-ghost counter for the same case.

### Verified on the device, not just built

Flashed to the watch over `/dev/cu.usbserial-10`, then driven from the serial
console:

- `MPCON|rtc PCF8563 ok 22:28:25` at boot -- chip identified, time trusted.
- `sync` re-fetched all 7 scripts (ok 7, failed 0) and set the RTC from NTP.
- Counter across a reboot: 0 -> 1 -> 2, then 3 on the 77s auto re-render.
- Per-script isolation, which is the actual claim: `run 0` twice took
  `art-deco-4` 0 -> 1 while `eyes` independently resumed 5 -> 6.

Building had to happen in a throwaway `git worktree` at HEAD: the working tree
carried unrelated in-progress edits to the shared renderer that did not compile
(`display_list_renderer.cpp` calling a then-private `occupancyBase()`). Worth
remembering as a technique -- it flashed exactly the change under test, with
none of the uncommitted work around it.

### `rtc-unset`, and the bit that would not be set (same day)

Testing the cold-clock path meant opening the watch and disconnecting the
battery, so the plan was a console command staging the same state: set the
chip's VL bit by hand, reboot, watch boot go to NTP.

**It does not work, and the device said so in one line.** Writing `0x80` to the
PCF8563's VL_seconds register (0x02) put the seconds through fine and left bit 7
clear -- the register read back `0x03`, then counted on. **VL is clear-only in
software on this part**: only the low-voltage detector raises it. The datasheet
wording ("remains set until overwritten") reads as though it were writable in
both directions, which is exactly the sort of thing worth checking on the chip
rather than in the PDF.

Finding that took one round trip because the `rtc` console command was extended
to print the raw flag register (`flagreg=0x03`) instead of only the interpreted
verdict. "The flag did not take" and "the flag is not what decides" produce the
same `ok`, and nothing else on this device can look at that register. The raw
byte stayed in the command for that reason.

So the override is ours, in NVS, honoured by `valid()`. That is a test hook in
production logic, which is a real cost -- but it buys the decision *and the
recovery*: `set()` lifts the override exactly where it clears the hardware flag,
and only on success, so one failed NTP reply cannot quietly end the test it was
staged for. Verified end to end on the watch:

```
rtc-unset            -> unset(forced) 00:00:03
reboot               -> RTC: PCF8563, NOT SET, 00:00:07
                        RTC not set; going to NTP
                        NTP: RTC set to 22:51:46 (UTC+1)
then                 -> rtc  ->  ok 22:52:58
```

DS3231's OSF *is* writable, so on that chip the honest version would have
worked -- but one mechanism that behaves the same on both parts beats two that
diverge on hardware nobody can see.

What this still does not cover, and what the battery pull remains the only test
for: what the time registers actually read after a true power loss. They are
undefined. `invalidate()` zeroes them, which is a guess at the pessimistic case.

## 2026-09-02 -- Watchy samples every second of the minute

The Watchy's automatic re-render interval changed from 77 to 83 seconds. Because
`gcd(83, 60) = 1`, each wake advances by 23 seconds modulo one minute and visits
all 60 second offsets before repeating. The order is deliberately scattered
rather than sequential: `0, 23, 46, 9, 32, ...`.

The previous value 77 was already coprime with 60 and therefore already had
full coverage, with a stride of 17. The change to 83 does not create that
mathematical property; it selects a different scattered traversal while
preserving it. Primality is useful shorthand here, but coprimality with 60 is
the property the schedule depends on.

Full coverage does not mean equally good short-run dispersion. With the old
17-second stride,

```text
7 * 17 = 119 = 2 * 60 - 1
```

so after only seven wakes the sequence lands one second before an offset it has
already sampled. The opening sequence `0, 17, 34, 51, 8, 25, 42, 59, 16, ...`
then resembles the previous batch shifted backwards by one second.

For the new 23-second stride, the first adjacent return is later:

```text
13 * 23 = 299 = 5 * 60 - 1
```

No smaller positive multiplier of 23 is congruent to `+1` or `-1` modulo 60.
It therefore takes 13 wakes before a sample lands next to an earlier one. The
ratio `23/60` is also close to the golden-ratio conjugate squared
(`1/phi^2`, approximately 0.382), which explains the more even-looking early
distribution; 23 is a near-golden-ratio stride, not itself a "golden ratio".

The complete second-offset cycles repeat after 60 wakes: 77 minutes for the old
cadence and 83 minutes for the new cadence. The benefit is thus the distribution
within that cycle, not eventual coverage.

This matters for scripts that branch on `$SECOND`. A cadence sharing a factor
with 60 would make some branches unreachable during automatic rendering. The
83-second cadence also remains close to the previous power and update budget;
only the Watchy changes, while the M5Paper retains its independent cadence.

## 2026-09-02 -- BLE belongs to the Watchy's Back button

BLE provisioning now opens only when the top-left physical Back button is
pressed. Top-right and bottom-right navigate scripts, and bottom-left re-renders
the current script, without starting BLE. A short top-left press opens the
20-second provisioning window; holding the same button for five seconds still
performs the explicit network sync and full panel refresh.

This is a memory-safety boundary, not merely a UI preference. The BLE stack
holds roughly 90 KB of internal DRAM while resident. Previously every navigation
press opened BLE immediately before the selected script was parsed and rendered,
making heap-heavy scripts such as City 2 substantially more likely to fail even
though their generated display lists are small. Restricting BLE to one explicit
gesture keeps routine rendering in the radio-off memory profile.

The long-press path remains safe: its NTP and script-sync functions stop BLE
before bringing up Wi-Fi/TLS, so the two large radio working sets are not kept
resident together.

---

## 2026-09-02 -- The web emulator now runs the firmware, and only the firmware

The audit in `docs/analysis/web-device-renderer-audit.md` closed the gap between
the JavaScript renderers and the device from 33,907 pixels to 1,228 -- and then
the firmware renderer was compiled to WebAssembly and the gap became zero by
construction: 18/18 golden images identical.

At that point the three JavaScript renderers (interpreter, compiler, display
list) had one remaining job, being a second implementation to audit against,
and that job was done. So they are removed. The editor renders with the
firmware, lints with the firmware's parser, and feeds its pattern previews and
pixel editor from the firmware's parsed assets, all exported through the WASM
module.

Checked in a browser before committing: rendering, diagnostics with line
numbers, the unquoted-`NAME` script the old parser rejected, pattern cloning,
and pixel editing writing back into the script text. All gates green with the
harness's JavaScript audit removed: verify 18/18, WASM 18/18, culling
neutrality, no-map fallback, `audit-sweep`.

What this buys beyond fidelity: there is now exactly one parser and one renderer
for the language. Every divergence in the audit came from there being two.


---

## 2026-09-03 -- The sine table, two bounds bugs, and a fixed-point rasteriser
                 that won for a reason I had explicitly ruled out

Session covering: a measurement that said "don't bother", a change made anyway
for consistency, three bugs introduced by it, two long-standing rendering bugs
found by accident, a documented conclusion about this hardware overturned, the
first per-operation on-device benchmark, and a 15-42% rasteriser speedup from an
idea I had argued against in writing two hours earlier.

Read the wrong turns. Four of the eight sections below are mistakes.

### 1. The question, and the answer nobody wanted

`ROTATE` used `sinf`/`cosf` while the language advertises integer math. Would a
sine table be faster on device?

Instrumented `matrix_make_rotation` and counted: **at most 109 calls in the
entire 54-render corpus, and zero in half of it.** Rasterisation is 91% of
device compute (2026-08-27 baseline) and the trig sits in the interpreter, once
per `ROTATE`, never per pixel. Host benchmark: +0.76% aggregate, against a
parse-phase control -- which contains no trigonometry -- that moved +/-10% over
the same runs. The signal was smaller than the instrument's noise.

So: no. Adopted anyway, for two reasons that are not speed. `README.md:179` had
claimed since forever that `ROTATE` "uses integer math internally (e.g.
precomputed sin/cos tables)" -- the spec had been describing a table the code
did not have. And dropping libm's sine saved 4-5 KB of flash on all three WASM
builds.

**Dead end worth recording:** the first version of the write-up said every
changed pixel was "an edge pixel, none is a shape in the wrong place". That was
checked by looking at 48x48 crops centred on the FIRST differing pixel, which is
a terrible sample. Measuring distance-from-nearest-edge properly showed
`thunderstorms__c42` had pixels **22 px from any edge**. The claim was wrong and
had already been published. See section 2 for why.

### 2. Three bugs in the sine table, each invisible to the previous test

**a. Composing rotation matrices compounds the table's error.** `ROTATE` did
`matrix = matrix * R(d)`, and a Q15-rounded `(c,s)` has `c^2 + s^2 = 1 +/- 6e-5`,
so every composition scaled the matrix slightly. Measured at `matrix_invert`
across the corpus: worst `|det - 1|` was **6.6e-3 with the table against 1.3e-6
with sinf** -- a 5,000x increase, and a 0.66% scale error. That is what put
pixels 22 px from an edge: the phase of a pattern fill inside a rotated region
slid several pixels.

Fix: carry the accumulated angle as an INTEGER and rebuild the matrix from it,
instead of multiplying matrices. `ROTATE d` becomes `angle = (angle + d) mod
360`, which cannot drift. The transform is only ever translations and rotations
(`CMD_SCALE` sets a separate integer factor and never touches the matrix), so
`(angleDeg, tx, ty)` is its exact and complete state.

**b. My first inverse was wrong, and "cleaner" was the reason.** Having made the
matrix rigid, I replaced `matrix_invert` with a bare transpose -- no determinant,
no division, very tidy. It made low-rotation scripts WORSE. A transpose is the
inverse of a perfectly orthonormal matrix; a table `(c,s)` is orthonormal to one
ulp, and the rasteriser uses both matrices -- the forward one to place a shape,
the inverse one to decide which pattern pixel each screen pixel samples. They
have to agree. The determinant division is not optional; what a rigid transform
buys is that the determinant is `c^2 + s^2` in closed form and can never be zero.

**c. Q15 cannot represent 1.0, so the identity rotation was not the identity.**
Q15 puts 1.0 at 32768, which does not fit an `int16_t`. Clamped to 32767, so
`cos(0) = 0.99997` and every `TRANSLATE` issued at angle 0 -- most of them -- was
quietly scaled. Invisible in a worst-case-error bound, visible only as small
diffs on scripts that barely rotate. Fixed by storing the table as `int32_t`,
which makes the four cardinal angles exact and costs 720 bytes.

Combined effect, measured against the original `sinf` renderer over 54 renders:
**3,701 differing pixels became 279**, and 41 of 54 renders became byte-identical
to the float original.

### 3. A test that found two bugs nobody was looking for

While answering "what is `0.7071f` for?", the answer turned out to be: it places
four diagonal sample points for `fillCircle`'s bounding box, and the whole
approach is wrong.

Generalised it into a test: **a shape's ink area is invariant under rotation.**
Render one primitive at a spread of angles, count non-white pixels, flag anything
that moves. Three suspects, two real:

- **`FILL_CIRCLE` bounded itself with the AABB of an inscribed OCTAGON.** Eight
  sampled points undershoot a rotated disk by `r*(1 - cos 22.5deg)` = 7.6% of the
  radius. Measured on radius 200: 400 px wide at ROTATE 0, **372 px at ROTATE
  22**, losing 4.4% of its area and drawing eight flat sides.
- **Every axis-aligned `LINE` drew NOTHING.** A zero-area shape has a
  zero-thickness AABB; `floor`/`ceil` collapsed it to `minY == maxY`, and the
  "clipped away to nothing" test culled the item. ROTATE 0/90/180 rendered **0
  pixels**; ROTATE 1/89/91 rendered all 401. The fix changed **zero goldens**,
  which is the most complete statement of a coverage gap available.
- **`rect_outline` was a FALSE POSITIVE** and saying so matters. Its 7.9% spread
  is Bresenham: a line of length L at angle t paints `L*max(|cos|,|sin|)` pixels.
  At 22.5deg that is 924 of 1000; measured 925. Not lost coverage.

Both real bugs predate the display-list renderer and neither had golden
coverage. `corpus/bounds.mp` now provokes both.

**A cascade nobody planned:** because the matrix is provably rigid, its columns
are unit vectors, so every "how long is the transformed radius" computation has
the closed form `lr * scale`. That retired 8 `transformPoint` calls per
`FILL_CIRCLE` down to 1, two `sqrtf` in `drawCircle`, and two `std::hypot` in the
bounds pass. Correctness and cost moved the same way, which is not the usual deal.

### 4. "Serial is worthless on this device" was wrong

`analysis/watchy-port-attempt-log.md` concluded the Watchy's serial output is
unusable and built a vibration-motor telemetry channel because of it.

**Serial works.** 115200, normal DTR/RTS polarity, and the firmware is chatty:
boot banner, heap, RTC, script list, per-render timings. What made it look dead
is that the **Watchy deep-sleeps between renders** -- a sleeping ESP32 transmits
nothing, so a listener attached at the wrong moment sees silence. Reset the board
and it talks immediately.

That is the THIRD instrument artifact on this one device, after `--after
no_reset` being read as device state and macOS lacking `timeout`. Section 5.4 of
the attempt log states the lesson and this is one more instance of it. The
lesson is apparently not learnable by writing it down once.

### 5. Building the on-device benchmark, and what it cost

- **`env:m5paper-bench` does not compile.** `mp_bench.cpp` still calls a
  four-argument `DisplayListRenderer` constructor that became three-argument. The
  bench nobody ran had rotted.
- Wrote a Watchy bench with its OWN `main()` and a `build_src_filter` that drops
  `src/*`. No SPIFFS, ScriptManager, WiFi, BLE or RTC: none is under measurement
  and all of it competes for a PICO-D4's ~180 KB.
- **That broke the normal firmware.** `env:watchy2` selects sources with `+<*>`,
  so merely creating the file gave a duplicate `setup`/`loop`/`g_display`. Fixed
  by guarding the file with `#if MP_BENCH`, the convention `mp_bench.h` already
  used -- not by editing the normal env.
- **First per-operation corpus was wrong twice.** Repeating a full-canvas fill 24
  times measured the same work as once: the occlusion buffer correctly culls
  every item after the first (24 items, 1 rendered). And a rotated rect
  translated to the corner rendered **0 items** -- entirely off-screen. Fixed by
  covering the canvas exactly once per script and taking statistics from reps,
  and by checking `rendered items`, not just that it parsed.

Result for the sine-table work, on hardware: rasterisation unchanged (as
predicted), **display-list generation 6-24% faster on every rotating script,
with `city` and `nest` -- which never rotate -- at 0.0% as the control.**

### 6. Q16.16: I predicted it would lose, in writing, and was wrong

The argument I made was: both targets are ESP32 with an FPU, so fixed point
trades a one-cycle `MUL.S` for a 32x32->64 multiply and a shift, and cannot win.
That framing **assumed fixed point means doing the same arithmetic in integers.**
It does not have to.

A pattern coordinate along a scanline is affine: `b(x+1) = b(x) + d`. In float
that recurrence is unusable because repeated addition drifts, so the float loop
recomputes `im0*x + m2y + im4` from scratch every pixel, multiplies by a
reciprocal, and converts to int. **In fixed point the recurrence is exact**, and
`>> 16` is an exact floor, so the conversion disappears with it. Per pixel per
axis: two multiplies become none, and the float->int conversion goes.

> The win is not a faster multiply. It is not multiplying. An FPU does not help
> with work you no longer do.

The device deltas are LARGER than the host's (-42.4% vs -35.9% on
`op_draw_asset`), which is the same story from the other side: the eliminated
operations are cheap on x86-64 and expensive on Xtensa.

**The generalisable error:** I evaluated a technique by its most obvious
implementation and reported the conclusion as a property of the technique. The
correct output would have been "here is what I would measure", not "here is why
it cannot work". The harness existed to answer it in ten minutes.

**And `fillCircle`'s share of the win is not fixed point at all.** The float path
inverse-transformed every pixel purely to ask "inside the circle?", then threw
the coordinates away. A circle under a rigid transform is a circle, so the test
is `|screen - Centre| <= r*SCALE` -- answerable per SCANLINE as a half-width
`sqrt(R^2 - dy^2)`. One root per row instead of a transform per pixel, and the
old `narrowSpan` bisection goes too because the new span is exact rather than
conservative. Solid `fill_circle` is **-30.9%** with no pattern arithmetic to
speed up.

### 7. The equivalence gate could not fail, and finding that out was luck

`compare-paths displaylist displaylist-fixed` printed **21/21 identical** on the
first run. That is the answer a correct fast path gives. It is also exactly the
answer a fast path that never executed gives, and there was no way to tell them
apart.

Establishing which world we were in meant deliberately corrupting the fixed loops
until the comparison failed (517,506 pixels). It ran. But **"go and sabotage it"
is not a procedure anyone will repeat**, so it is not a fix.

The renderer now counts pixels emitted through fixed-point inner loops --
accumulated per SPAN, so it costs nothing in the loop it measures -- and
`compare-paths` enforces it in both directions: a fast path reporting zero fails,
and a path named `*-float` reporting non-zero fails too, because a reference that
is secretly running the code it is a reference FOR is equally vacuous while
looking more convincing. The new gate was itself verified by forcing every range
check to fail: it printed `21 identical, 0 differing` -- a clean false pass under
the old gate -- followed by `FAIL: ... executed none. Every comparison above is
vacuous.`

This generalises past this change. `audit-sweep` already carries the same idea
("two cases must FAIL"). Any gate whose passing state is indistinguishable from
its not-running state needs a positive control, and it is worth asking of the
existing ones.

**Second overclaim caught by being asked:** I wrote that the screen-space circle
span produces byte-identical output, having checked exactly one configuration.
Probed properly across rotation, scale, radius, clipping and sub-pixel centre
placement: 8 of 9 identical, and an off-centre rotated disk differs by **2 px**.
Mathematically equivalent, differently rounded. Both overclaims this session came
from generalising a single sample.

### 8. Where it landed

Fixed point is now the **default**, goldens rebaked. The float rasteriser stays
selectable as `displaylist-float` with its own golden set in `golden-float/`,
gated by `make verify-float` in `ci` -- a superseded implementation that stays
runnable, so the comparison can be re-made rather than re-argued. `make
compare-float` reports the distance: 3 of 21 goldens differ, `artdeco_default` by
34 px of 518,400 (0.0066%).

The two are not byte-identical and cannot be made so: Q16.16 quantises to
1/65536, so wherever a float value sits closer than that to an integer, the two
floors disagree. **A pure-integer exact DDA is available, though, and the sine
table is what made it available** -- `SCALE` and `TRANSLATE` are integers and
rotation is now `C/32768` with `C` an exact integer, so every per-pixel step is
an exact rational and a Bresenham-style carry is possible. Roughly double the
Q16.16 inner loop, still far below float, and bit-identical across every
toolchain -- which the current float path is not, having been measured
disagreeing with itself by 21 px between `-Os` WASM and `-O2` native. See
`analysis/is-q16-16-integer-math.md`.

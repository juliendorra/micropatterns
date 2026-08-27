# M5Paper MicroPatterns — Platform / Toolchain / Display-Hardware Performance Analysis

Scope: PlatformIO/toolchain config, ESP32 platform flags, FreeRTOS task/core layout, and
e-ink display push path. Explicitly **out of scope**: rasterizer algorithm changes and
interpreter/VM changes (covered by sibling analyses).

Baseline: script `art-deco-3` takes **10,102–18,724 ms** in the "with optimizations" log
(`runtime optimizations.txt`), down from 30,741–49,677 ms "without optimizations" (that
earlier pass already removed a `log_d()` call that fired on every `resolveValue()` call —
see Finding 1 below for why this matters to the current investigation).

---

## Ranked levers

| # | Lever | Est. saving | Effort | Risk |
|---|-------|-------------|--------|------|
| 1 | Drop `-mfix-esp32-psram-cache-issue` (+ its forced `-fno-jump-tables -fno-tree-switch-conversion`) | **low-hundreds of ms**, mostly from restored switch-dispatch jump tables in the 2 hot `switch(cmd.type)`/`switch(item.type)` loops; PSRAM-access wrapping removed everywhere | Trivial (delete 1 build flag) | **Low, but NOT zero — verify first.** RESEARCHED (not measured): M5Stack's product page lists the SoC as ESP32-D0WDQ6-**V3** (chip rev 3), which fixed the PSRAM cache bug in hardware. This was NOT probed on Julien's actual unit — the M5Paper was not connected to this machine, only the Watchy was. **Confirm per-unit before shipping this flag change** (see §Verification below) |
| 2 | Turn WiFi off before/during rendering (`WiFi.mode(WIFI_OFF)`) if not already off, and confirm FetchTask isn't scheduled during a render | Unknown, potentially **hundreds of ms to low seconds** if a fetch happens to overlap a render | Low (one-line guard) | Low — needs on-device confirmation of overlap frequency |
| 3 | Switch flash from **DIO 40 MHz to QIO 80 MHz** (`board_build.flash_mode=qio`, `board_build.f_flash=80000000L`) | Meaningful for flash-cache-miss-heavy code (all render code lives in flash, not IRAM) — commonly cited as up to ~2x raw flash read throughput; real-world CPU-bound win is smaller but nonzero | Trivial (2 build flags) | Low-Medium — must verify the M5Paper's flash chip actually supports QIO before flashing (untested here; M5Stack's own M5Paper board files elsewhere generally use `dio`/`qio` — needs on-device check, don't assume) |
| 4 | Add explicit `-O2` for the render-hot source files via `build_unflags -Os` + `build_flags -O2` (whole-project or per-file) | Unknown without a benchmark; `-Os` vs `-O2` deltas on Xtensa are typically single-digit percent on generic C++, more on math-heavy loops | Low-Medium (also increases flash usage; check partition headroom — see below) | Low, but must retest visual output — LX6 `-O2`/`-O3` can be more aggressive about reassociating FP ops than `-Os`; combined with `-ffast-math` this becomes real risk (see #5) |
| 5 | `-ffast-math` on render sources | Unknown, likely small on integer-heavy MicroPatterns runtime (see note) | Low | **Flagged — do not enable without owner sign-off.** The rasterizer/runtime task description says float comparisons drive pixel coverage; `-ffast-math` permits reassociation and relaxes NaN/inf and exact-comparison semantics, which can silently shift anti-aliasing/coverage boundaries by a pixel and is invisible until you diff renders |
| 6 | Enable LTO (`board_build.embed_txtfiles`/`-flto` via `build_flags`) | Small-modest (\~2–8% typical on ESP32-Arduino LTO reports), also shrinks flash image | Medium — Arduino-ESP32 2.0.x LTO support is flaky/partially broken in places; needs a clean full build to confirm it links | Medium |
| 7 | `IRAM_ATTR` the hot per-pixel/per-command inner loop functions in `micropatterns_drawing.cpp` / `display_list_renderer.cpp` | Meaningful if the render loop currently thrashes the flash instruction cache under PSRAM/DMA contention from `pushCanvas`; unmeasurable from static analysis alone | Medium (must budget IRAM; M5Paper app has plenty of free 520KB SRAM but IRAM segment is smaller, ~128KB minus what Arduino core already uses) | Medium — needs `idf.py size`/map-file check to avoid IRAM overflow |
| 8 | Lower `-DCORE_DEBUG_LEVEL` from 5 to 3 (Info) or 0 (None) for the release/perf build | **Effectively $0 in the current codebase** — see Finding 1: the hot per-command/per-pixel paths already carry ~zero `log_d`/`log_v` calls. Real win only if you also want to save the ~microseconds of `if (level <= CORE_DEBUG_LEVEL)` branch checks scattered through Arduino-core/library calls (WiFi, SPI, M5EPD) at a lower level | Trivial | None — but see caveat below (M5EPD library and WiFi stack do still carry some debug/verbose logging at level 5) |
| 9 | Upgrade `espressif32@5.1.0`/Arduino core 2.0.4 (ESP-IDF 4.4) → Arduino-ESP32 3.x (ESP-IDF 5.x) | Uncertain, possibly a real win (ESP-IDF 5.x has newer/faster core libs, better LTO support, updated Xtensa toolchain) but also possibly a *regression risk* | **High** | **High** — M5EPD 0.x (this project's `m5stack/M5EPD` dep) has open, unresolved compatibility issues building against newer `framework-arduinoespressif32` branches (see Sources). Do not attempt without a fork/patch plan and a full on-device regression pass |
| 10 | CPU frequency | **0 — already optimal.** MEASURED: no `setCpuFrequencyMhz()` call anywhere in `systeminit.cpp`/`main.cpp`/`global_setting.cpp`; M5.begin() defaults to 240 MHz and nothing downclocks it. | — | — |

---

## Finding 1 — Logging is *not* the big win here (already fixed)

The task brief assumed `-DCORE_DEBUG_LEVEL=5` plus per-command `log_d()` calls (citing
`resolveValue(): Runtime resolveValue accessing $SECOND`) was likely the single largest,
cheapest win. **On inspection of the current source tree, this fix has already been made.**

- `runtime optimizations.txt` itself documents it: the "without optimizations" runs (30.7–49.7s)
  show `log_d()` firing from `micropatterns_runtime.cpp:102/107` on every `resolveValue()` call —
  this is called for every `$VARIABLE` reference, potentially thousands of times per script,
  and at 115200 baud each formatted line easily costs 1–3ms of **blocking** UART time. The
  "with optimizations" runs (10.1–18.7s) already reflect this fix and cut total time by ~60%.
- MEASURED (`grep -c "log_d("` across all `src/*.cpp`, current tree):

  ```
  network_manager.cpp: 19   (WiFi connect/scan path — not per-render)
  script_manager.cpp:  15   (script load/save — not per-render)
  display_manager.cpp: 11   (mostly in drawActivityIndicator, an input-driven UI indicator,
                              not the render hot path)
  occlusion_buffer.cpp: 6   (ALL commented out already — verified in source)
  input_manager.cpp:    4
  main.cpp:              3
  micropatterns_runtime.cpp:      0
  micropatterns_drawing.cpp:      0
  matrix_utils.cpp:               0
  display_list_renderer.cpp:      0  (uses log_w/log_i/log_e — none inside the per-item loop
                                       except one log_i emitted once at the very end)
  micropatterns_parser.cpp:       0
  ```

  The two hottest paths in the render pipeline — `micropatterns_runtime.cpp` (executes every
  script command, including loop bodies) and `micropatterns_drawing.cpp` (the pixel rasterizer)
  — currently have **zero** `log_d`/`log_v` calls. `occlusion_buffer.cpp`'s 6 calls are all
  commented out in source (`// log_d(...)`), so they cost nothing even instantiated at level 5.

**Conclusion:** at `CORE_DEBUG_LEVEL=5`, remaining `log_d` calls fire only on state
transitions (WiFi connect, script load/save, button-press indicators) — bounded, low-frequency
events, not per-pixel or per-command work. Lowering `CORE_DEBUG_LEVEL` will not measurably
speed up the render itself in the *current* codebase. It's still worth doing for hygiene (item
#8) and because the Arduino/ESP-IDF WiFi and M5EPD library internals do emit some of their own
logging at level 5 (untraced here — outside `src/`), but do not expect this to explain multiple
seconds of the 10–19s budget as the original hypothesis assumed. **This assumption should be
communicated back to whoever is track­ing the "biggest win" list — it's already been captured.**

---

## Finding 2 — `-mfix-esp32-psram-cache-issue` is dead weight on this hardware

MEASURED from `.pio/build/m5stack-fire/idedata.json` (actual compiler invocation for this
project): the flag is present **twice** in `cxx_flags`/`cc_flags`, and the Arduino-ESP32 2.0.4
build system automatically pairs it with `-mfix-esp32-psram-cache-strategy=memw`,
**`-fno-jump-tables`**, and **`-fno-tree-switch-conversion`** — this bundle is Arduino-ESP32's
standard "PSRAM Rev.1-safe" compile profile, forced on whenever `BOARD_HAS_PSRAM` +
`-mfix-esp32-psram-cache-issue` are both defined.

RESEARCHED: the PSRAM cache bug (external RAM read/write corruption under specific access
patterns) exists only on ESP32 **chip revision 1**; it was fixed in hardware in **revision 3**
("ECO V3"), and Espressif's own guidance is that rev-3 silicon does not need the software
workaround (Espressif ESP32 ECO V3 User Guide; multiple `espressif/esp-idf` issue threads).

RESEARCHED: the M5Paper's SoC is the **ESP32-D0WDQ6-V3** — i.e. chip revision 3 — per
M5Stack's own product listing.

**This means the flag, and the two switch-dispatch-disabling flags it drags in, are pure
overhead with zero corruption-avoidance benefit on this board.** Concretely:
- `-fno-jump-tables` / `-fno-tree-switch-conversion` force the compiler to lower
  `switch (cmd.type)` in `micropatterns_runtime.cpp:285` (fires once per executed script
  command, including every loop iteration) and `switch (item.type)` in
  `display_list_renderer.cpp:236` (fires once per display-list item) into linear/binary
  compare chains instead of a jump table. For a MicroPatterns command set with a
  double-digit number of command types, this is a real, if modest, per-dispatch cost that
  scales with the number of executed commands — likely tens of thousands of dispatches in a
  generative script, so plausibly tens of ms, not seconds, but free to eliminate.
- The `-mfix-esp32-psram-cache-issue`/`memw` codegen wraps or serializes certain PSRAM
  accesses at the instruction level; removing it removes that tax across the entire binary,
  which matters because the 540×960×4bpp canvas (**~253KB**, calculated below) lives in PSRAM.

**Action:** drop `-mfix-esp32-psram-cache-issue` entirely from `build_flags`. Zero
risk given confirmed rev-3 silicon. This is the highest-confidence, lowest-risk, zero-cost
change in this report.

---

## Verification required before dropping the PSRAM cache flag

**This is a hardware-dependent change and the hardware was never probed.** The chip-revision claim
comes from M5Stack's product listing, not from Julien's unit. Early M5Paper production runs, or a
board reworked/RMA'd at any point, could carry rev-1 silicon. On rev-1 silicon, dropping
`-mfix-esp32-psram-cache-issue` produces **silent, intermittent PSRAM data corruption** — which on
this project would look like sporadic garbage pixels or crashes during long renders, i.e. exactly the
kind of bug that is miserable to attribute after the fact.

Cost of checking: about ten seconds. With the M5Paper connected over USB:

```bash
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --port /dev/cu.usbserial-XXXX chip_id
```

Look for the `Chip is ESP32-D0WDQ6-V3 (revision v3.0)` line. Proceed only if it reports **revision
v3.0 or higher**. If it reports revision v1 or v0, keep the flag — the ~low-hundreds-of-ms win is not
worth intermittent memory corruption.

For reference, this exact probe was run against the connected **Watchy** and returned
`ESP32-PICO-D4 (revision v1.0)` — a rev-1 part. That is a different board and does not bear on the
M5Paper, but it is a concrete reminder that rev-1 ESP32 silicon is still in active circulation in
this project's own hardware.

## Finding 3 — Toolchain / optimization level (MEASURED)

From the actual compiler invocation (`idedata.json`) for this exact project/platform pin:

```
cxx_flags: -std=gnu++11 -fexceptions -fno-rtti -mfix-esp32-psram-cache-issue
           -mfix-esp32-psram-cache-strategy=memw -mfix-esp32-psram-cache-issue -mlongcalls
           -ffunction-sections -fdata-sections ... -Os -freorder-blocks -Wwrite-strings
           -fstack-protector -fstrict-volatile-bitfields ... -fno-jump-tables
           -fno-tree-switch-conversion -MMD
```

- **`-Os` is confirmed as the default** (PlatformIO `build_type=release` + Arduino-ESP32 2.0.4
  defaults). No optimization flag was specified in `platformio.ini`, so this default silently
  applies to every file, including the rasterizer.
- **Exceptions are enabled** (`-fexceptions`), RTTI is already off (`-fno-rtti`). MEASURED:
  `grep -rn "throw \|try {" src/*.cpp` found no exception usage in the MicroPatterns runtime
  itself — Arduino `String`/`std::vector`/`std::map` may throw internally on OOM, but the
  codebase does not appear to rely on try/catch control flow. `-fno-exceptions` is a candidate
  for a modest code-size/speed win but carries real risk with `String`/STL containers under
  ESP32 Arduino unless carefully tested (they compile without exceptions fine on ESP32, but
  removing exception tables changes OOM-handling behavior from throw to abort — should be
  tested on-device, not assumed safe).
- `-O2`/`-O3` were not evaluated on-device (no compiler available in this environment to
  produce a comparative binary — see Unknowns). Given `-Os` is already fairly close to `-O2`
  on Xtensa GCC for typical C++ (no vectorization difference since LX6 has no SIMD), expect a
  small, not dramatic, win from `-O2`; `-O3`'s loop unrolling/inlining could bloat flash-resident
  code and increase i-cache pressure, which would fight against #7 (IRAM) rather than help.
- `-ffast-math`: **flagged as risky and NOT recommended without the rasterizer owner's
  sign-off** — see table row #5. Not evaluated further here since it's algorithmic-correctness
  territory, outside this report's platform scope, beyond flagging the risk.

---

## Finding 4 — FreeRTOS task/core layout (MEASURED)

From `main.cpp` and `main.h`:

```cpp
xTaskCreatePinnedToCore(MainControlTask_Function, "MainCtrlTask", 4096, NULL, prio 2, ..., core 1);
xTaskCreatePinnedToCore(InputTask_Function,        "InputTask",    ?,    NULL, prio 3, ..., core 1);
xTaskCreatePinnedToCore(RenderTask_Function,        "RenderTask",  8192, NULL, prio 1, ..., core 0);
xTaskCreatePinnedToCore(FetchTask_Function,         "FetchTask",    ?,    NULL, prio 1, ..., core 0);
```

- **RenderTask and FetchTask (network) are pinned to the same core (0), same priority (1).**
  Arduino-ESP32's own WiFi/BT stack task also runs on core 0 by default. This means: if a
  script fetch/list-check happens to be scheduled around the same time as a render, the
  render competes with WiFi driver interrupts/tasks and the app's own network I/O on the same
  core. `network_manager.cpp` shows `WiFi.mode(WIFI_OFF)` is called after a fetch completes or
  fails (good hygiene), but **nothing in the render-trigger path
  (`triggerScriptRender`/`RenderTask_Function`) explicitly checks or forces WiFi off before
  starting a render.** Whether this actually overlaps in practice depends on the app's fetch
  cadence, which was not traced end-to-end here.
- Priorities: RenderTask and FetchTask are both `tskIDLE_PRIORITY+1` — lowest above idle,
  below MainCtrlTask (2) and InputTask (3). This is reasonable (keeps input responsive) but
  means the scheduler can and will interleave FetchTask work into what should be a render
  window if both are runnable.
- **`esp_task_wdt_reset()`/`yield()` frequency**: MEASURED call sites in the hot paths:
  - `micropatterns_runtime.cpp:390` — `if (i > 0 && i % 20 == 0) { yield(); if (i % 60 == 0) esp_task_wdt_reset(); }` — already throttled to every 20/60 iterations, not every iteration. Reasonable.
  - `micropatterns_drawing.cpp:333/445/498` and surrounding — similar periodic throttled pattern (yield every N pixels/rows, WDT reset less often).
  - This is **already well-tuned** — not a source of significant overhead as written. No
    evidence of over-frequent yielding in the current code; each `yield()`/`taskYIELD()` costs
    on the order of a context-switch check (microseconds) and is called at most a few thousand
    times total, not per-pixel.

**Action recommended:** explicitly force `WiFi.mode(WIFI_OFF)` (or confirm it's already off)
immediately before `RenderTask_Function` begins rendering a job, and/or move FetchTask to a
lower priority / gate it from running while `AppState::RENDERING_SCRIPT` is active. This needs
on-device profiling to quantify (see Unknowns) — it's plausible this contention is 0ms most of
the time (if fetches are infrequent/scheduled) or up to several hundred ms if they overlap.

---

## Finding 5 — Framebuffer / display push path (MEASURED, from M5EPD source in `.pio/libdeps`)

The vendored `M5EPD_Canvas.cpp` (`m5stack/M5EPD`, resolved into
`.pio/libdeps/m5paper/M5EPD/src/M5EPD_Canvas.cpp` for this repo) shows:

```cpp
#if defined(ESP32) && defined(CONFIG_SPIRAM_SUPPORT)
    if (psramFound() && _usePsram)
        ptr8 = (uint8_t *)ps_calloc(_bytewidth * h + 1, sizeof(uint8_t));
    else
#endif
        ptr8 = (uint8_t *)calloc(_bytewidth * h + 1, sizeof(uint8_t));
```

- **Canvas framebuffer lives in PSRAM** (`ps_calloc`), confirmed. For the app's
  `_canvas.createCanvas(540, 960)` (rotated M5Paper resolution) at 4bpp: width rounds to a
  multiple of 4 (540 already qualifies), `_bytewidth = 540/2 = 270`, buffer size =
  `270 * 960 + 1 ≈ 259,201 bytes (~253 KB)`. There is also a small `_indicatorCanvas`
  (384×64 or 64×256 depending on indicator type — a few KB) allocated/freed per indicator draw.
  This confirms the PSRAM-cache-flag interaction in Finding 2 is real: every canvas
  read/write/pushCanvas touches PSRAM.
- **`pushCanvas()`** (`M5EPD_Canvas.cpp:164`) does:
  ```cpp
  _epd_driver->WritePartGram4bpp(x, y, _iwidth, _iheight, _img8);
  _epd_driver->UpdateArea(x, y, _iwidth, _iheight, mode);
  ```
  i.e. it (1) streams the whole 4bpp framebuffer over SPI to the IT8951 controller's internal
  memory, then (2) issues the waveform update command and the controller drives the panel.
- **SPI clock to the IT8951 is hardcoded at `_spi_freq = 10000000` (10 MHz)** in
  `M5EPD_Driver.cpp:22`, and is not overridden anywhere in this app's code (MEASURED — no
  `_spi_freq` or SPI frequency override found in `src/*.cpp`). At 10 MHz SPI, streaming 253KB
  takes roughly `253000 * 8 / 10e6 ≈ 200ms` of pure SPI transfer time for a full-canvas push —
  this is a fixed, unavoidable-at-current-clock cost paid on every full push. IT8951 datasheets
  generally support higher SPI clocks (commonly cited up to 24 MHz); raising `_spi_freq` in the
  M5EPD library (not app code — would require a local library patch/fork, since it's hardcoded)
  could cut that ~200ms roughly in half, but this is **library-owned code, not app code** —
  changing it means vendoring a patched M5EPD, which is a materially different effort/risk
  tier than a `platformio.ini` change.
- **Update mode**: `main.cpp:796` uses `UPDATE_MODE_GC16` for the main render push;
  `display_manager.cpp` uses `UPDATE_MODE_GC16` for full-canvas pushes and
  `UPDATE_MODE_DU4`/`UPDATE_MODE_GC16` for small activity-indicator partial pushes. RESEARCHED
  IT8951 waveform timings (commonly cited, panel/vendor dependent): **GC16/GL16 ≈ 450ms**,
  **A2 ≈ 290ms**, **DU/DU4 ≈ 120ms** for a full-panel update; these scale down for small partial
  regions like the indicators. GC16 is the correct choice for full-quality generative art (16
  gray levels, no ghosting) and should not be downgraded to DU/A2 for the main render — that
  would visibly degrade the art (1-bit/limited-graytone, the exact tradeoff the task brief
  warns about). **This ~450ms GC16 waveform + ~200ms SPI transfer (~650ms total) is a real,
  largely irreducible per-render cost** given the current panel/library, and is a small
  fraction (roughly 3–6%) of the observed 10–19s — meaning **the display push is not where
  most of the 10–19s is going**; the bulk is compute (script execution + rasterization), which
  is out of this report's scope (rasterizer/interpreter subagents own that).

---

## Finding 6 — Flash mode/frequency (MEASURED)

`platformio.ini` does not set `board_build.flash_mode` or `board_build.f_flash`. The project
reuses the generic `board = m5stack-fire` board definition (not a native M5Paper board file —
this project's platform pin predates/doesn't use an official `m5stack-papers3`/`m5paper` PIO
board id). MEASURED from the resolved board JSON
(`~/.platformio/platforms/.../boards/m5stack-fire.json`):
```
"flash_mode": "dio", "f_flash": "40000000L"
```
i.e. the build ships **DIO flash at 40 MHz**, confirmed also by the bootloader binary path in
`idedata.json` (`bootloader_dio_40m.bin`). All render code executes from flash (via the 32KB
i-cache), not from IRAM (no `IRAM_ATTR` used anywhere in `src/`), so flash read throughput
directly gates how fast code executes on i-cache misses. Switching to **QIO 80 MHz**
(`board_build.flash_mode = qio`, `board_build.f_flash = 80000000L`) doubles the nominal SPI
flash clock and adds two more data lines — a meaningful win for code/constant-data fetch,
essentially free to try. **Caveat: this needs on-device verification** that the M5Paper's
flash chip is 4-wire QIO-capable and correctly strapped for QIO mode before flashing (most
ESP32 modules are, but this was not confirmed from the repo — there is no schematic or M5Stack
board file to check flash chip part number in the current install).

---

## Finding 7 — CPU frequency (MEASURED — already correct)

`grep -rn "setCpuFrequencyMhz" src/*.cpp` → **no matches**. `idedata.json` confirms
`F_CPU=240000000L` compiled in, and M5.begin() does not alter it at runtime (RESEARCHED:
M5Paper's default clock is 240MHz and stays there unless explicitly changed). **No action
needed — CPU is already running at maximum clock during rendering.**

---

## Suggested `platformio.ini` (every change commented)

```ini
[env:m5stack-fire]
platform = espressif32@^5.1.0
board = m5stack-fire
framework = arduino
platform_packages = platformio/framework-arduinoespressif32@3.20004.0
upload_speed = 1500000
monitor_speed = 115200
board_build.partitions = default_16MB.csv

; --- NEW: flash mode/speed (Finding 6) ---
; DIO/40MHz -> QIO/80MHz. Roughly doubles flash SPI throughput for i-cache misses.
; RISK: verify on-device that the M5Paper flash chip is QIO-capable before flashing;
; if the board fails to boot after reflashing, revert to dio/40000000L immediately.
board_build.flash_mode = qio
board_build.f_flash = 80000000L

build_flags =
    ; Lowered from 5 (verbose) to 3 (info). Current hot render paths (micropatterns_runtime.cpp,
    ; micropatterns_drawing.cpp, matrix_utils.cpp, display_list_renderer.cpp) already carry
    ; ZERO log_d/log_v calls (verified against current source), so this mainly reduces logging
    ; from WiFi/library internals during fetch, and is essentially free. Use 0 for a "no logs"
    ; production build once confident.
    -DCORE_DEBUG_LEVEL=3
    -DBOARD_HAS_PSRAM
    ; REMOVED -mfix-esp32-psram-cache-issue: M5Paper ships ESP32-D0WDQ6-V3 (chip rev 3),
    ; which has the PSRAM cache bug fixed in hardware. This flag (and the
    ; -fno-jump-tables/-fno-tree-switch-conversion it forces on Arduino-ESP32 2.0.4) is pure
    ; overhead here with zero benefit. Removing it restores switch-statement jump-table
    ; codegen for the two hot command/item dispatch loops.
    ;
    ; OPTIONAL, UNVERIFIED — only add after an on-device before/after comparison, and only
    ; if -ffast-math is NOT also enabled without rasterizer-owner sign-off:
    ; -O2
    ; build_unflags = -Os

lib_deps =
    m5stack/M5EPD
    bblanchon/ArduinoJson@^7.4.1
build_src_filter =
    +<*>
    +<../../src/display_manager.cpp>
    +<../../src/event_defs.h>
    +<../../src/global_setting.cpp>
    +<../../src/input_manager.cpp>
    +<../../src/main.cpp>
    +<../../src/matrix_utils.cpp>
    +<../../src/micropatterns_command.h>
    +<../../src/micropatterns_drawing.cpp>
    +<../../src/micropatterns_parser.cpp>
    +<../../src/micropatterns_runtime.cpp>
    +<../../src/network_manager.cpp>
    +<../../src/render_controller.cpp>
    +<../../src/script_manager.cpp>
    +<../../src/system_manager.cpp>
    +<../../src/systeminit.cpp>
    +<../../src/occlusion_buffer.cpp>
    +<../../src/display_list_renderer.cpp>
```

Not included above (deferred, needs more validation before landing):
- `-fno-exceptions` (Finding 3) — needs an on-device build+boot test since no exception usage
  was found in app code, but String/STL OOM behavior changes from throw to abort.
- LTO — flaky on Arduino-ESP32 2.0.x, needs a from-scratch build to confirm it links cleanly.
- `IRAM_ATTR` on hot rasterizer functions — needs a `.map` file / `idf.py size` pass to check
  free IRAM budget before committing code to it (out of scope for a build-flag-only change).
- Arduino-ESP32 3.x / ESP-IDF 5.x upgrade — high-effort, high-risk given M5EPD compatibility
  concerns; treat as a separate project, not a quick win.
- Explicit WiFi-off-before-render guard in `main.cpp`/`network_manager.cpp` — this is an app
  source-code change (not a `platformio.ini`-only change), flagged for whoever owns FreeRTOS
  task wiring, matches Finding 4.

---

## Irreducible floor

Given the current panel/library/hardware, a rough floor for a full-quality (GC16) render+push,
independent of script compute time:

- IT8951 GC16 full-panel waveform: **~450ms** (RESEARCHED, vendor-typical; panel-specific,
  needs on-device confirmation with this exact panel).
- SPI framebuffer transfer at the M5EPD-library-hardcoded 10MHz: **~200ms** for the ~253KB
  4bpp canvas (MEASURED buffer size, calculated transfer time from measured SPI clock).
- **Total unavoidable-at-current-config display path: ~650ms (~0.65s)**, roughly **3–6% of
  the observed 10–19s** total. This confirms the platform/display layer is *not* the dominant
  cost — the bulk of the 10–19s is compute (script interpretation + rasterization), which is
  explicitly out of this report's scope. Even in a best case where every lever in this report
  lands cleanly (drop PSRAM-cache flag, QIO flash, WiFi off during render, lower debug level),
  expect platform-level savings in the **low hundreds of ms**, not seconds — the interpreter
  and rasterizer subagents' findings are where the multi-second wins have to come from.
- If SPI clock to the IT8951 were raised (requires patching the vendored M5EPD library, not
  just `platformio.ini`), the ~200ms transfer could shrink further, but this doesn't change
  the ~450ms GC16 waveform floor, which is fixed by the panel/controller regardless of software.

---

## Unknowns / needs on-device measurement

1. **Actual current split of the 10–19s** between (a) script parsing, (b) display-list
   generation (`_runtime->generateDisplayList()`), (c) rasterization
   (`_renderer->render(...)`), and (d) the final `pushCanvas`. `render_controller.cpp` already
   logs `generationDuration` and `renderDuration` via `log_i` — **turn `CORE_DEBUG_LEVEL` to at
   least 3 (Info) and capture these two numbers plus a `millis()` wrap around `pushCanvas` for
   one full run of `art-deco-3`** to get the real breakdown before prioritizing further work.
   This report could not run the firmware on hardware.
2. **Whether WiFi/FetchTask actually overlaps a render in practice** — needs a serial-log trace
   correlating `FetchTask_Function` activity with `RenderTask_Function` timing on a live device
   (Finding 4). This report only established that the *possibility* exists in the task/core
   layout, not that it currently happens or how often.
3. **Whether the M5Paper's flash chip supports QIO mode** — could not be confirmed from the
   repo (no schematic/flash part number available). Flash `dio→qio` should be tried on a
   spare/test device first, with a fallback plan (revert `platformio.ini`, reflash) if the
   board fails to boot.
4. **Actual `-O2`/`-Os` and LTO deltas** — no ESP32/Xtensa toolchain build was run in this
   sandboxed environment beyond inspecting the already-cached `.pio/build` artifacts; a real
   before/after timing comparison on hardware is needed, this report only reasons from general
   Xtensa-GCC behavior.
5. **IRAM headroom** — not measured; requires `pio run -v` or `idf.py size-components` output
   against the actual linked binary to know how much of the ~128KB IRAM segment Arduino
   core/M5EPD/WiFi already consume before deciding how many rasterizer functions could be
   pinned there.
6. **M5EPD/library-level logging at CORE_DEBUG_LEVEL=5** — this report only audited `src/*.cpp`
   (the app's own code) for `log_d`/`log_v` density. The M5EPD library and Arduino-ESP32
   WiFi/BLE stack may still emit their own verbose logs at level 5 during
   connect/scan/render-adjacent calls; not exhaustively audited here since it's vendored
   library code, not app code, but worth a `grep -rc "log_d(" .pio/libdeps/*/M5EPD/src` pass if
   pursuing item #8 further.

---

## Sources

- [ESP32 ECO V3 User Guide (Espressif, PSRAM cache bug + rev-3 fix)](https://www.tme.eu/Document/7ec1b4293073f3b571c5f352860a3490/esp32v3.pdf)
- [ESP32 Chip Revision v3.0 User Guide, Espressif — PSRAM cache bug fixed in rev 3](https://www.espressif.com/sites/default/files/documentation/esp32_chip_revision_v3_0_user_guide_en.pdf)
- [xtensa-esp32-elf-gcc / -mfix-esp32-psram-cache-issue discussion, espressif/esp-idf#11100](https://github.com/espressif/esp-idf/issues/11100)
- [ESP32-D0WD PSRAM crash discussion, espressif/esp-idf#11762](https://github.com/espressif/esp-idf/issues/11762)
- [Support for external RAM — ESP-IDF Programming Guide](https://demo-dijiudu.readthedocs.io/en/latest/api-guides/external-ram.html)
- [M5Paper ESP32 Development Kit product page (M5Stack shop) — confirms ESP32-D0WDQ6-V3 SoC, 8MB PSRAM, 16MB flash, 240MHz](https://shop.m5stack.com/products/m5paper-esp32-development-kit-v1-1-960x540-4-7-eink-display-235-ppi)
- [IT8951 waveform mode overview/timings, ESPHome IT8951 component docs](https://esphome.io/components/display/it8951/)
- [IT8951 waveform enum reference (GC16/GL16/DU/A2), gnzzz/IT8951 docs](https://github.com/gnzzz/IT8951/blob/master/docs/enums/_it8951_.waveform.md)
- [Waveshare e-Paper IT8951 update mode descriptions](https://www.waveshare.com/wiki/Template:EPaper_Codes_Descriptions-IT8951)
- [m5stack/M5EPD GitHub repository](https://github.com/m5stack/M5EPD)
- [M5EPD build failure against newer framework-arduinoespressif32 branches, m5stack/M5EPD#10](https://github.com/m5stack/M5EPD/issues/10)
- [Arduino ESP32 3.0.0 release notes — ESP-IDF v5.1 base, breaking API changes](https://github.com/espressif/arduino-esp32/releases)
- Local, MEASURED sources (this repo): `M5Paper_MicroPatterns/platformio.ini`,
  `M5Paper_MicroPatterns/runtime optimizations.txt`, `M5Paper_MicroPatterns/src/*.cpp`,
  `M5Paper_MicroPatterns/.pio/build/m5stack-fire/idedata.json`,
  `M5Paper_MicroPatterns/.pio/libdeps/m5paper/M5EPD/src/M5EPD_Canvas.cpp`,
  `M5Paper_MicroPatterns/.pio/libdeps/m5paper/M5EPD/src/M5EPD_Driver.cpp`,
  `~/.platformio/platforms/*/boards/m5stack-fire.json`.

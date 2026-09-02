# Watchy Port Design — Micropatterns

Status: **SUPERSEDED as a plan, kept as history.** Written 2026-08-27, before any
code existed. The port was built on 2026-08-27/28 and diverged from this plan in
several load-bearing ways; the plan is preserved unchanged because the reasoning
behind the choices -- and behind the ones that turned out wrong -- is the useful
part. Read the reconciliation below before treating anything here as current.

### What this plan got right, and where the build went elsewhere

| This plan said | What was built | Why |
|---|---|---|
| Storage backend changes SPIFFS -> LittleFS | **SPIFFS kept** | The M5Paper's `script_manager.cpp` is compiled straight out of its tree, unmodified. Changing the backend would have forked it. |
| `network_manager` / `script_manager`: **ADAPT** | **Neither adapted -- both shared verbatim** | They needed only `#include` and one rename (`NetworkManager` -> `MPNetworkManager`, which collides with an Arduino 3.1 class). `MPNetworkManager` holds a `SystemManager*` it never dereferences, so the Watchy passes `nullptr`. |
| `main.cpp`: **REWRITE**. `setup()` = one wake cycle, then deep sleep. "No `loop()` to speak of." | **`loop()` with light sleep** | Deep sleep costs a full boot per wake -- SPIFFS mount, script load, parse, GxEPD2 init -- measured at ~9s. At the 77s cadence that is ~900mA-seconds against ~62 for light sleep. Deep sleep only wins once the interval is minutes. See `JOURNAL.md`. |
| A `wake_router` classifying RENDER / INTERACTIVE / FETCH | Not built | The single `loop()` with light sleep made it unnecessary. |
| `system_manager`: **REWRITE** for SmallRTC / PCF8563 | Not ported. Its *clock* was, later and much smaller: `watchy_rtc.{h,cpp}` (2026-09-01) | For a year the firmware passed `setTime(0,0,0)` and every time-dependent script drew midnight. The replacement is ~130 lines that probe I2C and read BCD time -- **not SmallRTC**, which drags the Watchy library and its own GxEPD2 assumptions into a build that pins the Szybet fork. Probing 0x51 vs 0x68 also settled this doc's open question (§8.2 of the hardware doc): **this watch has a PCF8563**. NVS settings, sleep policy and the rest of `system_manager` are still not ported, and nothing needs them. |
| `MAX_SCRIPT_CONTENT_LEN 5600` is "a RAM-relevant constant on Watchy" | **Removed from the fetch path** | ArduinoJson 7 grows on demand and ignores fixed capacities, so it only implied a limit that did not exist. Real scripts reach 15KB. The real ceiling is measured in `JOURNAL.md`. |
| "the root CA is directly reusable" | Correct, and worth stating loudly | The chain later moved to ISRG Root X2, which *looks* like an expired pin. X2 is cross-signed by X1, so the pinned root still validates. Verified against the live server before anything was changed. |

The single largest thing this plan did not anticipate: **NVS is broken on this
ESP32-PICO-D4 under Arduino 2.0.4**, which forced the whole platform onto
pioarduino (Arduino 3.1 / IDF 5.3). See
`docs/incidents/2026-08-28-watchy-nvs-postmortem.md` and the reproducer at
`tools/device/probes/nvs-probe/`.

Original status line follows.

Status: design document, no code committed to the device. Nothing was flashed, written or erased.
Date: 2026-08-27.
Companion docs: `docs/analysis/watchy-hardware-and-references.md` (hardware ID, measured panel
timings, power math), `docs/analysis/m5paper-interpreter-perf.md`,
`docs/analysis/m5paper-rasterizer-perf.md`, `docs/analysis/m5paper-platform-perf.md`.

> **Foundation is decided, by the project owner:** *"I want a standalone firmware"* / *"I don't care
> about the InkWatchy firmware apart from it being a well functioning example."*
> This document therefore designs a **standalone, dedicated Micropatterns firmware** for the Watchy,
> on plain Arduino + GxEPD2, mirroring the M5Paper client's module structure. InkWatchy is
> **reference only** — mined for specific hard-won fixes, cited by file:line, never inherited.
> The rejected alternative is recorded in §11.1 for the record.

---

## 0. Executive summary

**The port is much cheaper than it looks, and the reason is a single measured fact.**

`micropatterns_drawing.cpp` — the whole rasterizer, all transforms, all pattern-fill logic — compiles
on a desktop host against a canvas class with **exactly four methods**:

```cpp
class Canvas { int width(); int height(); void drawPixel(int,int,uint8_t); void fillCanvas(uint8_t); };
```

That is the entire platform surface of the renderer. Verified by host compilation (§3). The parser,
the runtime/display-list generator, the occlusion buffer and the matrix utils compile on the host with
**no platform surface at all** beyond a ~65-line `String` shim.

Second measured fact: **the DSL is already 1bpp.** `DRAWING_COLOR_WHITE = 0` and
`DRAWING_COLOR_BLACK = 15` (`micropatterns_drawing.h:11-12`) are the only two colour values that ever
exist. The "4bpp colour model" is nominal — 15 is just what M5EPD wants for black. Mapping to
GxEPD2's `GxEPD_BLACK`/`GxEPD_WHITE` is a one-line change with **zero fidelity loss**.

So the three things that sound like the hard parts of this port — the colour model, the rasterizer
coupling, the DSL semantics — are not hard. What *is* hard:

1. **Energy, not RAM, is the binding constraint.** From the parallel hardware study: one
   fetch+render+refresh cycle costs **~0.2–0.6 mAh** against a **~200 mAh** cell, which puts the
   sustainable autonomous cadence at roughly **hourly to half-hourly** — not per-minute, and
   certainly not continuous. Every architectural choice below is subordinate to that number. §7.4.
2. **RAM.** The display list is `std::map<String,int>` per item (~128 B struct + ~240 B of heap map
   nodes per item). On a no-PSRAM PICO-D4 this does not fit. The fix is not to shrink it — it is to
   **delete the display list on Watchy** and rasterize forward, in one pass, straight into the
   5,000-byte framebuffer. §7.3.
3. **Coordinate space.** Scripts authored for 960×540 are simply wrong at 200×200 and there is no
   automatic fix that preserves the art. This is a product decision, not a technical one. §7.2, §9.

**Recommendation, in one line:** extract a platform-agnostic `micropatterns_core/` library (proved
possible today), then build a small, standalone Arduino + GxEPD2 Watchy firmware around it that
mirrors the M5Paper client's module boundaries, borrowing three specific fixes from InkWatchy by
citation. §5.

**The one number that decides whether this is a delight or a curiosity:** render time. The parallel
rasterizer analysis estimates **10–20× achievable** on the M5Paper rasterizer hot path. On Watchy
that is the difference between a ~15 s render (unusable — 0.33 mAh of CPU alone, and the watch is
visibly stuck) and a sub-second one (0.02 mAh, invisible). §7.4 makes that sensitivity explicit.

---

## 1. M5Paper client architecture

`M5Paper_MicroPatterns/src/`, 7731 lines. Four FreeRTOS tasks (`main.h:23-35`) exchanging fixed-size
POD messages over queues (`event_defs.h`):

```
InputTask ──InputEvent──▶ MainControlTask ──RenderJobQueueItem──▶ RenderTask
   ▲ (GPIO ISR)                │      ▲                              │
   │                           │      └────RenderResultQueueItem─────┘
   │                           ├──FetchCommand──▶ FetchTask ──FetchStatus──┐
   │                           └◀───────────────────────────────────────────┘
```

The rendering pipeline itself is a clean three-stage design:

```
script text ──MicroPatternsParser──▶ std::list<MicroPatternsCommand> + assets
             ──MicroPatternsRuntime──▶ std::vector<DisplayListItem>   (resolves vars, loops, ifs,
                                                                       snapshots transform state)
             ──DisplayListRenderer──▶ (cull off-screen, cull occluded, back-to-front)
             ──MicroPatternsDrawing──▶ M5EPD_Canvas::drawPixel
```

Two culling layers exist purely as speed optimisations for M5Paper's 518,400-pixel 4bpp canvas:

- `OcclusionBuffer` — a 16×16-block coarse opacity grid (`occlusion_buffer.cpp:14`).
- `_pixelOccupationMap` — **one byte per pixel**, 518,400 bytes (`micropatterns_drawing.cpp:43`).

`DisplayListRenderer::render()` iterates the list **in reverse** (`display_list_renderer.cpp`,
`for (auto it = displayList.rbegin(); ...)`), i.e. front-to-back, so foreground pixels claim the
occupation map before background pixels are considered. **This is why the display list exists at
all** — reverse iteration requires the whole list to be materialised first. Remember this; it is the
lever that makes the Watchy RAM budget work (§7.3).

### 1.1 Per-file portability verdict

Legend: **CORE** = moves to the platform-agnostic library unchanged or near-unchanged ·
**ADAPT** = concept ports, implementation is per-device · **REWRITE** = M5Paper-only, Watchy needs
its own · **DROP** = not needed on Watchy.

| File | LOC | Verdict | Notes / what couples it |
|---|---:|---|---|
| `matrix_utils.h/.cpp` | 62+36 | **CORE, verbatim** | Pure `<cmath>`. Host-compiles clean. Zero changes. |
| `micropatterns_command.h` | 137 | **CORE, one edit** | Only coupling is `#include <Arduino.h>` for `String` and the `color = 15` default (`:89`, `:127`). Needs the POD display-list rework for Watchy (§7.3). |
| `micropatterns_parser.h/.cpp` | 53+1023 | **CORE, verbatim** | Only `Arduino.h`/`String`. Host-compiles clean. Uses exactly 8 `String` methods: `length, trim, substring, toUpperCase, indexOf, startsWith, c_str, lastIndexOf`. |
| `micropatterns_runtime.h/.cpp` | 70+410 | **CORE, two edits** | `#include <M5EPD.h>` (`micropatterns_runtime.h:4`) is **entirely unused** — delete it. `esp_task_wdt_reset()`/`yield()` need a `core::yield()` hook. Host-compiles clean otherwise. |
| `micropatterns_drawing.h/.cpp` | 62+527 | **CORE, behind an interface** | Uses only `width/height/drawPixel/fillCanvas` on the canvas (proved, §3). Swap `M5EPD_Canvas*` for `mp::ISurface*`. Colour constants become 0/1. |
| `occlusion_buffer.h/.cpp` | 33+65 | **CORE, verbatim** | `Arduino.h` only for `uint8_t`; `esp32-hal-log.h` include is dead (all uses commented out). Host-compiles clean. Probably **DROP** on Watchy anyway (§7.3). |
| `display_list_renderer.h/.cpp` | 59+312 | **CORE, one edit** | `#include "display_manager.h"` (`display_list_renderer.h:9`) is gratuitous — it only calls `getCanvas()`. Take an `ISurface&` in the ctor instead. Then fully portable. On Watchy this file is **DROP**ped in favour of the streaming renderer (§7.3), but kept for M5Paper. |
| `display_manager.h/.cpp` | 54+395 | **REWRITE** | `M5EPD_Canvas`, `M5.EPD.SetRotation`, `pushCanvas`, `UPDATE_MODE_GC16/DU4`, 540×960 hardcode, EPD mutex. Watchy equivalent is 40 lines over GxEPD2. |
| `system_manager.h/.cpp` | 83+294 | **REWRITE** | `M5.RTC` / `RTC_Time` / `RTC_Date` (BM8563), NVS, `esp_light_sleep`. Watchy uses PCF8563/DS3232 via `SmallRTC`, LittleFS, deep sleep. The *NVS-key/state-persistence concept* ports; the API does not. *(2026-09-01: only the clock was eventually needed, and it is `watchy_rtc.{h,cpp}` rather than SmallRTC -- see the reconciliation table above.)* |
| `input_manager.h/.cpp` | 46+178 | **REWRITE** | GPIO 37/38/39 (`input_manager.h:12-14`), M5Paper's 3-button layout. Watchy has 4 buttons on 26/25/4/35 and InkWatchy already owns the ISR + debounce. |
| `network_manager.h/.cpp` | 54+553 | **ADAPT** | `WiFi.h`/`HTTPClient`/`WiFiClientSecure`/`ArduinoJson` are all available on Watchy. The API-endpoint logic, the JSON shapes and the root CA are directly reusable. Timeouts and the *policy* of when to fetch must change completely (§7.4). |
| `script_manager.h/.cpp` | 98+1574 | **ADAPT** | Logic (list.json, fileId generation, per-script exec state, orphan cleanup) is portable and valuable. Storage backend changes SPIFFS → LittleFS. Largest single file; budget real time here. |
| `render_controller.h/.cpp` | 32+115 | **ADAPT** | Thin glue. Reconstruct for the streaming renderer; keep the `parse → generate → render → return final state` shape and the interrupt callback. |
| `event_defs.h` | 169 | **ADAPT** | Queue POD structs. `MAX_SCRIPT_CONTENT_LEN 5600` (`:10`) is a RAM-relevant constant on Watchy. |
| `main.h/.cpp` | 79+1032 | **REWRITE** | `M5.begin()` (`main.cpp:54`), four always-on FreeRTOS tasks with 4K/2K/8K/8K-word stacks (`main.h:29-32` = 88 KB of stack). Wrong shape for a deep-sleeping watch entirely (§7.4). |
| `systeminit.h/.cpp` | 26+39 | **DROP** | Pre-`M5.begin()` power-up. InkWatchy's `initHardware()` covers this. |
| `global_setting.h/.cpp` | 33+28 | **DROP** | Vestigial; only content is duplicate button pin `#define`s (`global_setting.h:22-24`). |

**Totals.** CORE ≈ **2,715 lines** (parser 1076, runtime 480, drawing 589, display-list renderer 371,
matrix 98, occlusion 98, command header 137). ADAPT ≈ 2,400 lines. REWRITE/DROP ≈ 2,100 lines.

Roughly **35 % of the M5Paper client — and 100 % of the interesting part, the language — moves
unchanged.**

---

## 2. Hardcoded-assumption inventory

All paths relative to `M5Paper_MicroPatterns/src/`.

### 2.1 Canvas dimensions

| Location | Assumption | Breaks on Watchy? |
|---|---|---|
| `display_manager.cpp:44` | `_canvas.createCanvas(540, 960)` | Yes — only hardcoded dimension in the tree. |
| `display_manager.cpp:353-360` | `getWidth/getHeight` delegate to the canvas | No — already dynamic. |
| `micropatterns_drawing.cpp:9-10, 20-21` | `_canvasWidth/_Height` read from the canvas | No. |
| `micropatterns_runtime.cpp` | `$WIDTH`/`$HEIGHT` env vars set from ctor args | No. |

**Verdict: the C++ is already dimension-agnostic.** One line. The 960×540 problem is entirely in the
*scripts*, not the firmware (§7.2).

### 2.2 Colour model

| Location | Assumption |
|---|---|
| `micropatterns_drawing.h:11` | `const uint8_t DRAWING_COLOR_WHITE = 0;` |
| `micropatterns_drawing.h:12` | `const uint8_t DRAWING_COLOR_BLACK = 15;` |
| `micropatterns_command.h:89` | `uint8_t color = 15; // ... (M5EPD uses 4bpp)` |
| `micropatterns_command.h:127` | `uint8_t color = 15;` on `DisplayListItem` |
| `micropatterns_drawing.cpp:155,168-174` | pattern-fill returns one of the two constants |

Grep proves it: **no value other than 0 and 15 is ever assigned to a colour.** Change the two
constants to `0` and `1` and the whole tree is 1bpp. The `uint8_t` field could become a bitfield.

### 2.3 M5-specific API calls

| Location | Call |
|---|---|
| `main.cpp:54` | `M5.begin(true,false,true,true,true)` |
| `display_manager.cpp:4` | canvases bound to `&M5.EPD` |
| `display_manager.cpp:39` | `M5.EPD.SetRotation(M5EPD_Driver::ROTATE_90)` |
| `display_manager.cpp:109,148,201,209,317,325` | `pushCanvas(..., UPDATE_MODE_GC16 / UPDATE_MODE_DU4)` |
| `system_manager.cpp:166,172,180,186` | `M5.RTC.setTime/setDate/getTime/getDate` |
| `system_manager.h:22-23` | returns `RTC_Time` / `RTC_Date` (M5 types) in its public API |
| `micropatterns_runtime.h:4`, `micropatterns_drawing.h:4`, `display_manager.h:4`, `system_manager.h:4`, `systeminit.h:4`, `main.h:4` | `#include <M5EPD.h>` |

Note `micropatterns_runtime.h:4` — a core file includes `<M5EPD.h>` and **never uses it**. Deleting
that one line is the single highest-leverage change in this whole refactor.

### 2.4 PSRAM

`M5Paper_MicroPatterns/platformio.ini`:
```
build_flags = -DCORE_DEBUG_LEVEL=5 -DBOARD_HAS_PSRAM -mfix-esp32-psram-cache-issue
board_build.partitions = default_16MB.csv
```
There are **zero explicit `ps_malloc` / `heap_caps_malloc` / `MALLOC_CAP_SPIRAM` calls** in the
source. The PSRAM dependency is entirely implicit: with `BOARD_HAS_PSRAM` under arduino-esp32 2.0.4,
`CONFIG_SPIRAM_USE_MALLOC` routes allocations above `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (16 KB) to
PSRAM. So `_pixelOccupationMap` (518 KB) and the `_displayList` vector silently land in PSRAM, while
the thousands of small `std::map` nodes stay in internal DRAM.

This is the dangerous kind of dependency: invisible in the code, fatal on removal.

### 2.5 Memory hogs

| Location | Structure | M5Paper cost | Naive Watchy cost |
|---|---|---:|---:|
| `micropatterns_drawing.cpp:43` | `_pixelOccupationMap` — 1 byte/pixel | 518,400 B | 40,000 B |
| — same, as 1bpp | | 64,800 B | **5,000 B** |
| `occlusion_buffer.cpp:14` | 16×16 block grid, 1 B/block | 2,040 B | 169 B |
| `micropatterns_command.h:113-134` | `DisplayListItem` struct | ~~**128 B each**~~ → **40 B** | ~~128 B~~ → **40 B** |
| — plus per-item `std::map` nodes | ~~~4 int params typical~~ | ~~**~240 B each**~~ → **0** | ~~~240 B~~ → **0** |
| `micropatterns_command.h:56-84` | `MicroPatternsCommand` | **256 B each** | 256 B each |
| `micropatterns_command.h` | `MicroPatternsAsset` | 80 B + `w*h` bytes data | same |
| `event_defs.h:10` | `MAX_SCRIPT_CONTENT_LEN` | 5,600 B per queue slot | 5,600 B |
| `script_manager.h:12-13` | JSON docs 1024 + 2048 B | 3,072 B | 3,072 B |
| `main.h:29-32` | task stacks 4K+2K+8K+8K **words** | 88,064 B | would not fit |

`sizeof` figures measured on host (clang, x86-64, `String`=24 B); on ESP32 Arduino `String` is 16 B so
`DisplayListItem` lands around 112–128 B. Same order of magnitude either way.

**A 2,000-item display list therefore costs roughly 740 KB of heap.** On a device with ~200 KB free.

> **Superseded, 2026-09-02.** Two of the rows above no longer describe the code, and the struck-through
> figures are kept only so the reasoning that followed from them stays readable.
>
> - `DisplayListItem` is now a **trivially-copyable 40-byte POD** (`micropatterns_command.h`). The two
>   `std::map`s per item are gone: parameters live in a fixed `int32_t p[4]`, the DRAW asset is a
>   resolved pointer, and the transform is a pointer into a pooled snapshot. The per-item heap
>   allocations — the 480 KB row in §7.3(a) — are **zero**.
> - `_pixelOccupationMap` is now **1 bit per pixel** (commit `14a5304`), i.e. the 5,000 B row above, not
>   the 40,000 B one. It also degrades instead of aborting when the allocation cannot be satisfied.
>
> Net effect: **§7.3(a) "does not fit" is no longer true, and the naive port is what actually
> shipped** — `Watchy_MicroPatterns/src/main.cpp` parses, generates a display list, and rasterizes it
> with the occupancy map, from the same sources as the M5Paper. The streaming renderer of §7.3(b) was
> never built. See the correction block in §7.3.

### 2.6 `std::map<String, ...>` heap churn

Every `DisplayListItem` carries `std::map<String,int> intParams` and `std::map<String,String>
stringParams`. Every parameter is a red-black-tree node allocation *plus* a separate `String` heap
buffer *plus* two malloc headers. For a script emitting 2,000 primitives with 4 params each that is
**16,000 heap allocations per render**. On M5Paper with PSRAM this is merely slow. On Watchy it is
fatal — both for capacity and for fragmentation.

### 2.7 Task stacks

`main.h:29-32` allocates 88 KB of FreeRTOS stack across four permanently-resident tasks. The Watchy
design must not have four permanently-resident tasks at all (§7.4).

---

## 3. Evidence: the core already compiles on a host

This was actually run, not asserted. Shim written to a scratch dir:

- `Arduino.h` — 65 lines: a `String` class over `std::string` implementing exactly the 8 methods the
  core uses, plus `operator+`, `operator<` (needed as a `std::map` key), `millis()`, `yield()`.
- `esp_task_wdt.h` — one inline no-op.
- `esp32-hal-log.h` — four no-op macros.
- `M5EPD.h` — a `M5EPD_Canvas` declaring **only** `width()`, `height()`, `drawPixel(int,int,uint8_t)`,
  `fillCanvas(uint8_t)`.

Result (`clang++ -std=c++17 -fsyntax-only`, macOS, Darwin 23.4):

```
--- matrix_utils            (clean)
--- micropatterns_parser    (clean)
--- micropatterns_runtime   (clean)
--- occlusion_buffer        (clean)
--- micropatterns_drawing   (clean, against the 4-method canvas)
```

Only two real code gaps surfaced and both are trivial: `micropatterns_runtime.cpp:250` and `:390` call
bare `yield()`, and `micropatterns_parser.cpp:511` range-for's over a `String` while `:609` uses the
`String(ptr,len)` constructor. Everything else was already portable.

`display_list_renderer.cpp` did **not** compile only because `display_list_renderer.h:9` pulls in
`display_manager.h`, which pulls `freertos/FreeRTOS.h`. Removing that include (it is only used to name
`M5EPD_Canvas`) is expected to make it compile too.

**This is the whole thesis of the refactor, demonstrated in about twenty minutes.** It also means a
host-side test harness and a golden-image regression suite are cheaply available (§8, M0).

---

## 4. InkWatchy as a reference — what the local source actually says

**Reference study only.** Per §5 we are not building on InkWatchy; this section exists so the three
things we *do* borrow (§5.3) are traceable to real code, and so the panel/power facts it encodes are
not lost. Source read at `/Users/julien/Documents/GitHub/InkWatchy/` (project name
`InkWatchy-personal`), 34,424 lines of C/C++ under `src/`.

### 4.1 Hardware identification — settled from source

`.pio/build/` contains exactly two entries: `Unknown` and **`Watchy_2`**, and `sdkconfig.Watchy_2`
(86 KB) is present alongside a `firmware.bin` dated 2026-08-27 13:36. Cross-referenced with the
confirmed ESP32-PICO-D4 rev v1.0 + CP2102N and `platformio.ini`'s comment *"It's the CP2102 IC"*:

> **The connected device is a Watchy v2.0, currently running a build of the user's local InkWatchy
> tree from the `Watchy_2` environment.**

Pin map for Watchy v2.0, from `src/defines/condition.h`:

| Function | Pin | Line |
|---|---:|---|
| EPD CS | 5 | `condition.h:46` |
| EPD DC | 10 | `condition.h:47` |
| EPD RESET | 9 | `condition.h:48` |
| EPD BUSY | 19 | `condition.h:49` |
| SPI SCK / MISO / MOSI / SS | 18 / 19 / 23 / 5 | `condition.h:65-68` |
| Button MENU | 26 | `condition.h:87` |
| Button BACK | 25 | `condition.h:88` |
| Button DOWN | 4 | `condition.h:89` |
| Button UP | **35** (v2 only; v1/v1.5 use 32) | `condition.h:91` |
| Button active level | `BUT_STATE LOW`, click `HIGH`, `RISING` interrupt | `condition.h:84-86` |
| Vibration motor | 13 | `condition.h:110` |
| RTC interrupt | 27 | `condition.h:111` |
| Battery ADC | **34** (v2 only; v1.5=35, v1=33) | `condition.h:116` |
| ADC divider | `500.0f` | `condition.h:114` |
| I²C SDA / SCL | 21 / 22 | `sleep.cpp:38-39` |
| BMA423 INT1 / INT2 | 14 / 12 | `sleep.cpp:48-49` |
| RTC chip | **external** (PCF8563 or DS3232 via `SmallRTC`) | `condition.h:154-156` |
| CPU speeds | 80 / 160 / 240 MHz | `condition.h:176-179` |

### 4.2 Framework — hybrid, not IDF-only

```ini
framework = espidf, arduino          ; hybrid
platform  = pioarduino platform-espressif32 53.03.13
platform_packages = framework-espidf @ v5.3.2
                    framework-arduino @ arduino-esp32 3.1.0
board = esp32dev
board_build.filesystem = littlefs
board_upload.offset_address = 0x20000
```

**The Arduino API is fully available inside InkWatchy.** `src/defines/defines.h:20` includes
`<Arduino.h>` directly and the codebase uses `String`, `pinMode`, `digitalWrite`, `SPI`, `WiFi.h`,
`ArduinoJson` throughout. Dropping Arduino-framework Micropatterns core files in is not a rewrite.

The genuine contrast worth recording: M5Paper is pinned to **arduino-esp32 2.0.4 / IDF 4.4**
(`platform_packages = framework-arduinoespressif32@3.20004.0`), InkWatchy to **3.1.0 / IDF 5.3.2**.
Practical consequences for the core: ArduinoJson v7 API is used on both sides (good); `String` is
unchanged; but IDF 5.x renames some headers and is stricter about `-Wall`. Expect an afternoon of
warnings, not a rewrite. This is another argument for keeping the core free of *any* framework header.

### 4.3 Display layer — as implemented

`src/hardware/display/display.h:3`:
```cpp
extern GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> *dis;
```

- Panel driver `GxEPD2_154_D67`, 200×200, 1bpp, via the fork `Szybet/GxEPD2-watchy` (pinned commit
  `1a15932`), plus Adafruit-GFX.
- The page-height template parameter is `::HEIGHT` — i.e. **a single full-screen page buffer,
  200×200/8 = 5,000 bytes**. No paging, no tiling. Draw anywhere, then `display()`.
- SPI at **20 MHz**, `SPI_MODE0` (`display.cpp:30`).
- `dis->init(0, !bootStatus.fromWakeup, 10, true)` — the second argument means *"do the slow
  initial-reset dance only on a cold boot, not on a deep-sleep wake"*.
- **The framebuffer lives in RTC slow memory.** `src/hardware/display/display.cpp:3` —
  `dis = &rM.display`, and `rM` is `RTC_DATA_ATTR rtcMem rM` (`rtcMem.cpp:3`), with the GxEPD2 object
  as a member (`rtcMem.h:23`). The map file confirms `.rtc.data = 6464` bytes. **The 5,000-byte
  framebuffer survives deep sleep**, which is what makes cross-wake partial refresh possible at all.
  RTC slow memory on ESP32 is 8 KB, so ~1.7 KB is left. *Micropatterns cannot claim RTC slow memory.*
- Refresh policy (`display.cpp:82-120`): a counter `rM.updateCounter`; every
  `FULL_DISPLAY_UPDATE_QUEUE` (**60**, `config.h:69`) partial updates, force one full update, then
  reset. Classic ghosting-vs-flash tradeoff — 60 partials between flashes.
- `deInitScreen()` calls `dis->hibernate()` before sleeping (`display.cpp:76-82`).
- Dirty rectangles: **GxEPD2's own** `setPartialWindow()`. InkWatchy calls it explicitly only in the
  cold-boot clear path (`display.cpp:57`); UI code generally redraws the whole buffer and lets the
  `display(PARTIAL_UPDATE)` path handle it. InkWatchy's stated optimisation is at a higher level —
  *"Most UI is rendered only when needed / values it's showing changed"* (README).

### 4.4 The panel workaround flags

- `SCREEN_PARTIAL_GREY_WORKAROUND=1` (default ON, `platformio.ini`) — used at `display.cpp:55-63`:
  **on cold boot only**, set a full-screen partial window, `clearScreen()`, `fillScreen(WHITE)`, then
  a `FULL_UPDATE`. Works around the D67 waking into a grey/half-latched state after a cold power-up,
  which otherwise poisons every subsequent partial refresh.
- `SCREEN_FULL_WHITE_WORKAROUND=0` (default OFF) — `display.cpp:220-231`: when a full update leaves
  the panel white, immediately re-issue a `PARTIAL_UPDATE` 50 ms later.
- `SCREEN_BLACK_BORDER=0` — the panel's border waveform. The comment at `display.cpp:36-45` documents
  the real fix: patch `GxEPD2_154_D67::_InitDisplay()` to write `0x00` after command `0x3C`
  (BorderWaveform). The Szybet fork carries this.
- README credit: *"Huge thanks for pointing out the direction for a potential fix for screen ghosting,
  I easily have spent 2 weeks on it"*.

**These are exactly the bugs a from-scratch Watchy firmware would rediscover.** Two weeks of someone
else's debugging, already paid for — which is exactly why §5.3 borrows these three fixes by citation
rather than letting a from-scratch firmware rediscover them.

### 4.5 Power / sleep — as implemented

`src/hardware/sleep/sleep.cpp`:

- `ForceInputs()` (`:4-63`) — before sleeping, forces ~24 GPIOs to `INPUT` (all button, SPI, I²C, EPD
  and BMA pins) and calls `Serial.end()`. Inline comment: **"Saves 70 uA"**.
- `goSleep()` (`:66-183`) — detach all four button interrupts, `deInitScreen()` (GxEPD2 hibernate),
  `LittleFS.end()`, then `esp_sleep_enable_ext1_wakeup(UP|DOWN|MENU|BACK, ESP_EXT1_WAKEUP_ANY_HIGH)`
  and `esp_deep_sleep_start()`.
- RTC alarm wake is handled by `SmallRTC` on `RTC_INT_PIN 27` (ext0 is noted as no longer needed since
  SmallRTC 2.3.7, `sleep.cpp:166`).
- `manageSleep()` (`:194+`) — sleep after `SLEEP_EVERY_MS` = **10 s** of idle (`config.h:183`), but
  first bounce back to the watchface; refuse to sleep while the WiFi task is running.
- Night mode: wake every `NIGHT_SLEEP_FOR_M` = **45 min** between hours 23 and 5 (`config.h:180-182`).
- `AVOID_SLEEPING_ON_FULL_MINUTE 4` (`config.h:89`) — don't sleep if a minute boundary is <4 s away.
- Default CPU speed `minimalSpeed` (80 MHz), `BUTTON_CPU_SPEED normalSpeed` (160 MHz) on button wake
  (`config.h:188-189`).
- Optional MD5-hashed RTC-memory backup to LittteFS on sleep (`sleep.cpp:135-155`) so state survives
  a full reset, not just deep sleep.
- The only other current figure in the tree: the ULP path costs **"+150uA"** (`sleep.cpp:107`).

The README contains **no measured mA/µA battery figures** — it describes battery *features* (night
wake interval, disable-wakeup, disable-vibration) rather than numbers. The two in-code figures above
(70 µA saved by `ForceInputs`, 150 µA for ULP) are all the source states. The parallel hardware doc
should own the power budget arithmetic.

### 4.6 Flash budget (InkWatchy's layout — ours is §7.6)

`resources/tools/fs/in/partitions.csv`. This is the table currently written to the device at the
InkWatchy-custom offset `0x19000`:

| Name | Type | Offset | Size |
|---|---|---:|---:|
| factory | app | `0x20000` | `0x1CD000` = **1,888,256 B** |
| littlefs | data | `0x1ED000` | `0x200000` = **2,097,152 B** |
| nvs | data | `0x3ED000` | 12,288 B |
| coredump | data | `0x3F0000` | 65,536 B |

Current `firmware.bin` = **1,370,768 B**. **Free in the app slot: ~517 KB.**

The Micropatterns core is ~2,700 lines of straightforward C++ with no big tables; expect **60–100 KB**
of flash. It fits comfortably. The 2 MB LittleFS partition is enormous for a handful of ~5 KB scripts.

Static RAM of the current build (`xtensa-esp32-elf-size` on `firmware.elf`):
`.dram0.data` 61,092 + `.dram0.bss` 37,856 = **98,948 B static DRAM**; `.iram0.text` 110,539;
`.rtc.data` 6,464.

---

## 5. Foundation: a standalone Micropatterns firmware (decided)

**Decision, by the project owner:** *"I want a standalone firmware"*; *"I don't care about the
InkWatchy firmware apart from it being a well functioning example."* This is not re-litigated here.
The weighing that preceded it, and the reasons the alternative had merit, are preserved in §11.1 so
that a future reader can see what was traded away rather than assuming the question was never asked.

The independently-produced hardware/reference study
(`docs/analysis/watchy-hardware-and-references.md` §6) reached the same conclusion on technical
grounds — *"build fresh on plain Arduino + GxEPD2 …, mirroring the M5Paper client's module
structure, and explicitly port over (not inherit wholesale) InkWatchy's sleep/`ForceInputs()` pin
hygiene, the dirty-flag/full-refresh-counter display pattern, and the three panel workaround flags."*
Owner decision and technical recommendation agree. Design to it.

### 5.1 What the firmware is

`Watchy_MicroPatterns/` — a plain **Arduino-framework PlatformIO project**, `board = esp32dev`,
targeting Watchy 2.0 only. No ESP-IDF/CMake, no `managed_components`, no asset pipeline, no UI
framework. Module boundaries deliberately mirror `M5Paper_MicroPatterns/src/` so that the two
clients stay legible as siblings:

```
Watchy_MicroPatterns/
  platformio.ini            [env:watchy_v2]  esp32dev, arduino, custom partitions
  src/
    main.cpp                setup() = one wake cycle, then deep sleep. No loop() to speak of.
    wake_router.{h,cpp}     classify wake cause -> RENDER | INTERACTIVE | FETCH   (§7.4)
    display_manager.{h,cpp} GxEPD2 wrapper + WatchySurface : mp::ISurface         (§5.3)
    input_manager.{h,cpp}   4 buttons, ext1 wake + in-session debounce
    system_manager.{h,cpp}  SmallRTC (or Rtc_Pcf8563), NVS state, deep sleep entry, ForceInputs()
    script_manager.{h,cpp}  LittleFS: list.json, script bodies, per-script exec state
    network_manager.{h,cpp} WiFi + HTTPS + ArduinoJson against the existing Deno API
    render_controller.{h,cpp} glue: load script -> mp::renderStreaming() -> refresh
    power.{h,cpp}           battery ADC (pin 34, /500.0f divider), gates and thresholds
  lib/ or lib_extra_dirs -> ../micropatterns_core
```

Roughly **1,200–1,800 lines of new firmware code**, against ~2,700 lines of core reused verbatim.

### 5.2 Dependency list — deliberately short

| Dependency | Why | Alternative considered |
|---|---|---|
| `Szybet/GxEPD2-watchy` (pinned commit `1a15932`) | The panel driver InkWatchy already debugged for this exact GDEH0154D67/SSD1681 pairing, including the `0x3C` border-waveform fix. Measured timings live here: `full_refresh_time = 2600`, `partial_refresh_time = 500`, `power_on_time = 100`, `power_off_time = 150`. | Upstream `ZinggJM/GxEPD2` — same code minus the Watchy-specific patches. Using the fork is close to a strict win; **pin the commit** so it cannot drift. |
| `adafruit/Adafruit-GFX-Library` | Required by GxEPD2. Used *only* for the fallback status/error screens — Micropatterns art never touches GFX. | None; it is a hard GxEPD2 dependency. |
| ~~`Szybet/SmallRTC` **or** `orbitalair/Rtc_Pcf8563`~~ **Neither: `watchy_rtc.{h,cpp}`, 2026-09-01** | External RTC on Watchy 2.0 (`condition.h:154-156` selects `EXTERNAL_RTC`). `SmallRTC` abstracts DS3232 vs PCF8563, which resolves open question §8.2 of the hardware doc at runtime rather than at compile time. | Internal ESP32 RTC — rejected: drifts badly across deep sleep and Watchy 2.0 has a real RTC chip on I²C (SDA 21 / SCL 22) with its interrupt on pin 27. **What was built instead:** a ~130-line driver doing the one thing needed of either library — probe 0x51/0x68, read BCD time, honour the chip's own VL/OSF "do not trust me" flag. SmallRTC would have brought the Watchy library and its GxEPD2 assumptions into a build that pins the Szybet fork. §8.2 is answered: **PCF8563**. |
| `bblanchon/ArduinoJson` (v7) | Same library and same major version the M5Paper client already uses, so `network_manager` and `script_manager` port with minimal edits. | Hand-rolled parsing — rejected, no upside. |
| Arduino core `WiFi` / `HTTPClient` / `WiFiClientSecure` / `LittleFS` | Stock. | — |

Explicitly **not** taken: StableBMA/BMA423 (no accelerometer feature in v1), Olson2POSIX, NTPClient
(a single SNTP call is ~15 lines), Time.h, MoonPhase, open-meteo, Grafici-GFX, BLE.

Copy one build-hygiene idea from InkWatchy verbatim: its `custom_component_remove` list exists to
strip unused IDF managed components. On a pure-Arduino project the equivalent is simply *not
including* those libraries, which the table above already does. But **do not copy** its
`-D BUILD_TIME=...$(date)` flag — it forces a full recompile every hour and is a development-speed
tax with no benefit here.

### 5.3 What is borrowed from InkWatchy, with provenance

Three things, each small, each cited. Written fresh — no InkWatchy files are copied.

1. **Dirty-flag + counted forced-full-refresh.**
   Source: `InkWatchy/src/hardware/display/display.cpp:82-120` (`disUp()`, `dUChange`,
   `rM.updateCounter >= FULL_DISPLAY_UPDATE_QUEUE`), constant at
   `InkWatchy/src/defines/config.h:69` (= 60).
   Our version: §7.5, with a much smaller counter because generative art ghosts far worse than
   InkWatchy's mostly-static UI text.

2. **The three panel workarounds.**
   - Cold-boot grey latch: `InkWatchy/src/hardware/display/display.cpp:55-63` — on cold boot only
     (`bootStatus.fromWakeup == false`), `setPartialWindow(0,0,200,200)` → `clearScreen()` →
     `fillScreen(WHITE)` → full update, *before* any partial update is attempted. **Adopt
     unconditionally.** This is the bug a from-scratch firmware hits on day one.
   - Full-update-goes-white: `InkWatchy/src/hardware/display/display.cpp:220-231` — after a full
     update, wait 50 ms and issue a partial update. **Ship as a compile-time flag, default off**,
     exactly as InkWatchy does; it is a per-unit panel defect, not a universal one.
   - Border waveform: `InkWatchy/src/hardware/display/display.cpp:36-45` documents the fix —
     command `0x3C` (BorderWaveform) must be followed by data `0x00` in
     `GxEPD2_154_D67::_InitDisplay()`. **Free if we use the Szybet fork**, which carries it.
   Rationale for taking these at all: `InkWatchy/README.md:136` — *"…pointing out the direction for a
   potential fix for screen ghosting, I easily have spend 2 weeks on it"*.

3. **Sleep-time GPIO leakage cleanup.**
   Source: `InkWatchy/src/hardware/sleep/sleep.cpp:4-63` (`ForceInputs()`), measured comment
   *"Saves 70 uA"*. Forces ~24 pins to `INPUT` and calls `Serial.end()` before deep sleep.
   Our version must be **re-derived**, not copied: our pin usage differs (no BMA423, no motor), so
   the list of "unused/floating" pins differs. Copying InkWatchy's list blindly would force pins we
   actually use. Derive it from our own pin map, keep the technique. 70 µA against a ~200 mAh cell
   is ~0.35 %/day — small but free.

Explicitly **out of scope**: the ULP/lpCore always-on clock path
(`InkWatchy/src/hardware/lpCore/`), the resources/asset pipeline, the `UiPlace` framework, button
chord detection, and the six-board compile matrix. Micropatterns targets **Watchy 2.0 only** for v1;
the pin map is `#define`d in one header and that is the entire portability story.

### 5.4 What is lost, honestly

Relative to living inside InkWatchy, this firmware gives up: a working watch (the watch becomes a
picture frame that also knows the time), the multi-credential WiFi ladder, timezone handling from an
Olson database, alarms, the accelerometer, battery-saving UI modes, and a mature button/menu system.
The owner has judged that trade. What it buys is a codebase small enough to reason about end to end,
a dependency list of five libraries, and no upstream fork to maintain forever.

**Reversibility:** because the language lives in `micropatterns_core/` (§6) and depends on neither
firmware, an InkWatchy integration remains possible later at the cost of only the adapter layer.
Nothing in this design forecloses it.
## 6. The shared-core refactor

### 6.1 What the empty `src/` at the repo root actually is — a dead end, resolved

`M5Paper_MicroPatterns/platformio.ini` ends with:

```ini
build_src_filter =
	+<*>
	+<../../src/display_manager.cpp>
	... 15 more ...
```

`build_src_filter` paths are relative to `src_dir` = `M5Paper_MicroPatterns/src`, so `../../src/`
resolves to **`<repo-root>/src/`**, which is empty.

Git history explains it exactly:

- `6c1b5cd` *"m5paper task and rtos rearchitecture"* — created the manager files at repo-root `src/`
  (`display_manager`, `input_manager`, `network_manager`, `render_controller`, `script_manager`,
  `system_manager`) **and** added the `../../src/...` filter lines.
- `19af41e` *"clean up scattered files"* (2025-05-21) — moved every one of them to
  `M5Paper_MicroPatterns/src/` and left the filter untouched.

So: **there was never a designed shared-core layout.** The root `src/` was an accident of where files
landed during a refactor, and the `../../src/` filter lines have been dead no-ops for over a year —
harmlessly, because `+<*>` already picks up the real files. They should be deleted; leaving them
invites exactly the misreading that they encode a shared layout.

Worth stating plainly because the alternative reading ("a shared core was designed and abandoned,
therefore it was tried and failed") would be wrong and would wrongly discourage §6.2.

### 6.2 Proposed layout

```
micropatterns_core/                  ← NEW. No Arduino, no IDF, no M5EPD, no GxEPD2.
  include/mp/
    string.h                         ← mp::Str  (typedef to Arduino String on-device, std::string on host)
    surface.h                        ← ISurface
    platform.h                       ← IClock, IStorage, INetwork, IInput, IPower, yield hook
    command.h                        ← ex micropatterns_command.h
    parser.h  runtime.h  drawing.h
    display_list_renderer.h  occlusion_buffer.h  matrix_utils.h
  src/
    parser.cpp runtime.cpp drawing.cpp
    display_list_renderer.cpp occlusion_buffer.cpp matrix_utils.cpp
  test/
    host_main.cpp                    ← runs a script, dumps PGM/PNG
    golden/                          ← reference renders per example script
  library.json                       ← so PlatformIO lib_deps can point at it by path

M5Paper_MicroPatterns/               ← unchanged app; gains lib_extra_dirs = ../
  src/platform/                      ← M5Paper adapters (M5EpdSurface, M5Clock, SpiffsStorage, ...)
Watchy_MicroPatterns/                ← THE standalone Watchy firmware (§5.1)
  src/                               ← Watchy adapters live here (WatchySurface, RtcClock, LittleFsStorage, ...)
```

Adapters live inside each firmware's own `src/` rather than in top-level `platform_*/` directories.
Two consumers is not enough to justify a third layer of indirection, and PlatformIO is happier when
board-specific code sits under the board's project.

Consumed from PlatformIO with `lib_extra_dirs = ../` + `lib_deps = micropatterns_core`, or simply
`build_src_filter = +<../../micropatterns_core/src/*>` plus an `-I`. (A CMake/IDF `CMakeLists.txt`
would be six `.cpp` files and one include dir — kept in mind only because it is what an InkWatchy
integration would need if that path is ever revisited; §11.1.)

### 6.3 Interface headers

**`include/mp/string.h`** — one indirection so the core never names `Arduino.h`:

```cpp
#pragma once
#if defined(MP_HOST_BUILD)
  #include "mp/host_string.h"      // the ~65-line shim proven in §3
#else
  #include <Arduino.h>
  namespace mp { using Str = ::String; }
#endif
```

**`include/mp/surface.h`** — the complete platform surface of the rasterizer. Four methods, because
that is empirically all `micropatterns_drawing.cpp` uses:

```cpp
#pragma once
#include <stdint.h>

namespace mp {

// 1bpp everywhere. The DSL only ever produces two colours; see §2.2.
enum Ink : uint8_t { INK_WHITE = 0, INK_BLACK = 1 };

class ISurface {
public:
    virtual ~ISurface() = default;

    virtual int  width()  const = 0;
    virtual int  height() const = 0;

    // (x,y) is guaranteed in-bounds by the caller. Hot path: keep it trivial.
    virtual void drawPixel(int x, int y, Ink c) = 0;

    // Clear the whole surface.
    virtual void fill(Ink c) = 0;

    // OPTIONAL fast path. Default falls back to drawPixel so adapters may ignore it.
    // Horizontal run [x, x+len) on row y. Worth ~5-10x on a packed 1bpp buffer.
    virtual void fillSpan(int x, int y, int len, Ink c) {
        for (int i = 0; i < len; ++i) drawPixel(x + i, y, c);
    }
};

} // namespace mp
```

A note on the virtual call in `drawPixel`. It is on the hottest path in the system — one indirect
call per pixel. Two mitigations, in preference order: (a) implement `fillSpan` in every adapter and
teach `fillRect`/`fillCircle`/`drawLine` to emit spans, which removes the per-pixel call for the vast
majority of pixels; (b) if that is still not enough, make `MicroPatternsDrawing` a template on the
surface type (`template<class S> class DrawingT`) and explicitly instantiate once per platform —
devirtualised, zero runtime cost, at the price of one `.cpp` per platform. Start with (a); measure;
escalate to (b) only with a number in hand.

**`include/mp/platform.h`** — everything *except* pixels:

```cpp
#pragma once
#include <stdint.h>
#include "mp/string.h"

namespace mp {

struct Clock {
    virtual ~Clock() = default;
    virtual void localTime(int& hour, int& minute, int& second) = 0;
    virtual uint32_t unixTime() = 0;       // 0 if the clock is not yet trusted
    virtual bool     isTimeValid() = 0;
};

// Two things persist: script bodies (a few KB, keyed by fileId) and per-script
// execution state ($COUNTER and friends). Deliberately not a filesystem API.
struct Storage {
    virtual ~Storage() = default;
    virtual bool read (const Str& key, Str& out) = 0;
    virtual bool write(const Str& key, const Str& value) = 0;
    virtual bool exists(const Str& key) = 0;
    virtual bool remove(const Str& key) = 0;
};

enum class FetchStatus { Ok, NoWifi, HttpError, ParseError, Interrupted, NotAttempted };

struct Network {
    virtual ~Network() = default;
    virtual bool        available() = 0;         // cheap: is a fetch even plausible right now?
    virtual FetchStatus fetchScriptList(Str& outJson) = 0;
    virtual FetchStatus fetchScript(const Str& humanId, Str& outJson) = 0;
};

enum class Button : uint8_t { None, Next, Prev, Select, Back };

struct Input {
    virtual ~Input() = default;
    virtual Button poll() = 0;                   // non-blocking; None if nothing pending
};

struct Power {
    virtual ~Power() = default;
    virtual int  batteryPercent() = 0;
    virtual bool isCharging() = 0;
    virtual bool lowPowerMode() = 0;             // renderer may reduce work / skip fetch
};

// The core cooperatively yields inside long loops. On FreeRTOS this feeds the
// watchdog; on host it is a no-op. Replaces the bare yield()/esp_task_wdt_reset()
// calls at micropatterns_runtime.cpp:250 and :390.
struct Host {
    virtual ~Host() = default;
    virtual void yieldNow() = 0;
    virtual bool interrupted() = 0;              // user pressed a button mid-render
    virtual uint32_t millisNow() = 0;
};

} // namespace mp
```

**`include/mp/renderer.h`** — the one entry point an app needs:

```cpp
#pragma once
#include "mp/surface.h"
#include "mp/platform.h"

namespace mp {

struct RenderEnv { int hour, minute, second, counter; };

struct RenderStats {
    bool     ok            = false;
    bool     interrupted   = false;
    Str      error;
    uint32_t parseMs       = 0;
    uint32_t rasterMs      = 0;
    uint32_t primitives    = 0;
    uint32_t peakHeapUsed  = 0;
};

// Streaming path (Watchy): parse, then interpret-and-rasterize in one forward
// pass. No display list is materialised. See §7.3.
RenderStats renderStreaming(const Str& script, ISurface& surface,
                            const RenderEnv& env, Host& host);

// Buffered path (M5Paper): parse, build a display list, cull, rasterize in
// reverse with a pixel-occupancy map. Needs O(primitives) memory.
RenderStats renderBuffered (const Str& script, ISurface& surface,
                            const RenderEnv& env, Host& host);

} // namespace mp
```

Both paths share the parser and the rasterizer. Only the middle differs. That is the whole design.

### 6.4 Migration order — M5Paper stays working at every step

Each step is independently committable and independently verifiable on the M5Paper.

| Step | Change | How you know M5Paper still works |
|---|---|---|
| **R0** | Delete the 17 dead `+<../../src/...>` lines from `M5Paper_MicroPatterns/platformio.ini`. | Build byte-size before/after is unchanged. |
| **R1** | Delete `#include <M5EPD.h>` from `micropatterns_runtime.h:4`. Add `mp/string.h`; replace `#include <Arduino.h>` in `micropatterns_command.h`/`micropatterns_parser.h`. | Compiles; run a script, image identical. |
| **R2** | Add `micropatterns_core/` containing **copies** of the 6 core `.cpp` + headers. Add a host `test/` target and a golden-image dump for 5 example scripts. Nothing on-device consumes it yet. | M5Paper untouched. Host tests pass. |
| **R3** | Introduce `mp::ISurface`. Add `M5EpdSurface : ISurface` in `platform_m5paper/`. Change `MicroPatternsDrawing` to hold `ISurface&`. Point M5Paper's build at `micropatterns_core/` and **delete** its local copies of the 6 files. | Render each example on the M5Paper and diff against the pre-R3 photo. This is the one risky step. Do it alone, commit alone. |
| **R4** | Move `RenderController` behind `mp::renderBuffered()`. Extract `mp::Clock/Storage/Network/Input/Power` with M5Paper adapters wrapping the existing managers verbatim. | Full device soak: boot, fetch, cycle scripts, sleep, wake. |
| **R5** | Add `renderStreaming()` to the core. Verify on **M5Paper first** — it must produce a visually equivalent image (ordering differs, so a byte-exact match is not expected where primitives overlap). | Side-by-side photos of the same script under both paths. |
| **R6** | Only now: the Watchy firmware and its adapters. | §8 (M2 onward). |

**Honest cost.** R0–R2 are near-zero risk (roughly a day). R3 is the real one: it touches the file
that draws every pixel, and a subtle regression there is a *visual* bug, not a compile error — which
is exactly why R2 (golden images on the host) comes before it and not after. Budget 2–3 days for R3
including verification. R4 is mechanical but touches 2,400 lines (3–4 days). R5 is new code, not a
refactor (2–3 days). **Call it 8–12 working days before a single Watchy line is written.**

If that is too much up front, the *legitimate* shortcut is: do R0–R2, then jump straight to the
Watchy work consuming the R2 **copies**, accepting a temporary fork of the core, and do R3/R4 later.
Duplication is a real cost but it is a visible, mechanical one. The forbidden shortcut is skipping
R2's golden images — that is the thing that makes R3 safe.

---

## 7. Watchy-specific decisions

### 7.1 Colour model — 1bpp, and nothing is lost

Change `micropatterns_drawing.h:11-12` from `{0, 15}` to `mp::INK_WHITE/INK_BLACK` and the tree is
1bpp. Justification, from §2.2: no third value ever exists.

Semantics carry over exactly as the README specifies them:

| DSL construct | 4bpp today | 1bpp Watchy |
|---|---|---|
| `COLOR NAME=BLACK` | 15 | `INK_BLACK` |
| `COLOR NAME=WHITE` | 0 | `INK_WHITE` |
| `DRAW` pattern, bit=1 | current colour | current ink |
| `DRAW` pattern, bit=0 | not drawn (transparent) | not drawn |
| `FILL_RECT`/`FILL_CIRCLE` pattern bit=1 | current colour | current ink |
| `FILL_RECT`/`FILL_CIRCLE` pattern bit=0 | inverse colour | inverse ink |
| `FILL NAME=SOLID` | current colour | current ink |

**Fidelity lost: none from the colour depth.** What *is* lost is 13× the pixels, so 1-pixel-period
patterns (fine checkerboards, hatches) that read as texture at 960×540 read as flat grey mush at
200×200 — but that is a resolution effect, not a depth effect, and it lands squarely on the
coordinate-space question.

There is one genuinely new hardware consideration: on a real e-ink panel with partial refresh, a
1px-period dither is the worst case for ghosting. Expect to force a full refresh after any
high-frequency-pattern render. §7.5.

**Emulator "Watchy preview": already half-built.** `micropatterns_emulator/index.html:166-169` already
has a `displaySizeSelect` offering `540x960 (M5Paper)` and `200x200 (Default)`, wired to
`updateCanvasDimensions()` (`simulator.js:192-215`), which sets `$WIDTH`/`$HEIGHT` and re-runs. What
is missing for real parity:

1. Rename the option to **`200x200 (Watchy)`** — "Default" tells the author nothing.
2. Render 200×200 **at 1:1 with `image-rendering: pixelated`**, and offer a 2×/3× zoom. Today
   `simulator.js:207-212` disables the zoom button for non-M5Paper sizes, so a Watchy preview is a
   postage stamp on a modern display. That single UI detail decides whether authors can actually see
   what they are making.
3. Mark the canvas visually as a *watch* (round bezel overlay / correct aspect), so authors feel the
   target.
4. Optional but high-value: a **ghosting/refresh preview** toggle that shows what a partial refresh
   does to a fine dither. Nothing else in the toolchain tells an author "this pattern will smear".

### 7.2 Coordinate space — flag to the user, but recommend per-device authoring

Scripts are authored against `$WIDTH`/`$HEIGHT`. The example in the README does
`LET $center_x = $WIDTH / 2`, which is resolution-independent. But `DEFINE PATTERN ... WIDTH=20
HEIGHT=20` is **pixel-exact and does not scale**, and literal coordinates (`FILL_RECT X=100 Y=400`)
are absolute.

Four options considered:

| Option | What it does | Verdict |
|---|---|---|
| **Scale** (÷2.7) | Multiply all coordinates by 200/540. | **Rejected.** Patterns are the heart of the language and they are bitmaps; scaling them by 0.37 destroys them. Also `SCALE FACTOR` is an integer ≥1 in the DSL, so there is not even a way to express a shrink. |
| **Crop** (top-left 200×200) | Show a corner. | **Rejected.** Generative art centred at `$WIDTH/2` lands off-screen. Arbitrary and ugly. |
| **Letterbox** (fit 540×960 → 112×200) | Preserve aspect, waste 44 % of a 200×200 screen. | **Rejected.** Wrong aspect for a watch, and still needs pattern scaling. |
| **Per-device authoring target** | Scripts declare (or are tagged with) their target size; the device fetches only matching scripts; the editor previews at that size. | **RECOMMENDED.** |

Why per-device authoring is right *for this project specifically*: the whole premise is that authors
write small generative scripts and see them on a device. The parity between editor and device is the
product. A silent geometric transform between the two breaks the one promise the project makes.
A 200×200 watch face is a genuinely different design brief from a 540×960 tablet — treating them as
the same artwork at different sizes is the mistake.

Concretely:
- Add `target: "200x200" | "540x960" | "any"` to the script metadata in `micropatterns_server`.
  Today `micropatterns_server/main.ts:41-84` has a single per-user device index
  (`{userId}-device.json`) with no device dimension at all — this is the gap.
- The device sends its dimensions on fetch (or the index is per-device); it receives only compatible
  scripts.
- The editor's size selector *is* the authoring target, and gets saved with the script.
- `"any"` is for genuinely resolution-independent scripts (those using only `$WIDTH`/`$HEIGHT` maths
  and `SOLID` fills). The editor could even detect and offer this automatically: a script with no
  coordinate literal above 199 and no `DEFINE PATTERN` is a good `"any"` candidate.

**Ship-fast fallback for M2/M3:** a `mp::RenderEnv` with `WIDTH=200, HEIGHT=200` and a hand-authored
starter set of Watchy scripts. No server change needed to prove the port. The server/editor work is
milestone M6, not a blocker.

**This is question 1 for the user (§9).**

### 7.3 RAM budget

Baseline. ESP32-PICO-D4 has 520 KB SRAM, of which ~320 KB is DRAM usable as heap (the rest is IRAM
for code). There is **no PSRAM** — confirmed both by `esptool chip_id` and by
`InkWatchy/sdkconfig.Watchy_2:1287` (`# CONFIG_SPIRAM is not set`).

The only *measured* static-DRAM figure available today is InkWatchy's, from
`xtensa-esp32-elf-size` on its `firmware.elf`: `.dram0.data 61,092 + .dram0.bss 37,856 = 98,948 B`.
**Our standalone firmware should land well below that** — no BLE stack, no accelerometer driver, no
asset/font tables, no UI framework, five libraries instead of fifteen. Call it **50–70 KB** as a
working estimate, with InkWatchy's 99 KB as a pessimistic ceiling.

```
DRAM usable as heap                            ~320,000 B
  − our static (.data + .bss), estimated        ~−60,000 B   (InkWatchy's 98,948 B = ceiling)
  − task stacks + Arduino/FreeRTOS runtime      ~−30,000 B   (we run far fewer tasks than M5Paper)
  = free heap, WiFi down                       ~230,000 B
  − WiFi / LWIP / mbedTLS when connected        ~−60,000 B
  = free heap, WiFi up                         ~170,000 B
```

*(Estimates from the ELF plus standard ESP32 figures. **Milestone M3 replaces them with a measured
`ESP.getFreeHeap()` on our own firmware on the actual watch.** Do not design against the estimate —
and log `heap_caps_get_largest_free_block()` alongside it, because fragmentation, not capacity, is
the failure mode here; risk §10.6.)*

Now the two candidate designs:

**(a) Naive port — display list, as on M5Paper.** *~~Does not fit.~~ **It fits, and it is what
shipped.** See the correction below the table.*

| Item | Bytes (as estimated) | Bytes (as built) |
|---|---:|---:|
| Framebuffer 200×200×1bpp (GxEPD2's page buffer, on the heap) | 5,000 | 5,000 |
| Parsed script: ~200 `MicroPatternsCommand` @ 256 B + map nodes | ~80,000 | ~80,000 |
| Display list: 2,000 `DisplayListItem` | 256,000 @ 128 B | **80,000 @ 40 B** |
| …plus ~4 map nodes each @ ~60 B | 480,000 | **0** |
| Pixel occupancy map | 40,000 @ 1 B/px | **5,000 @ 1 bpp** |
| Occlusion grid | 169 | 169 |
| **Total** | **~861,000** | **~170,000** |

> **Correction, 2026-09-02.** The 861 KB column was written against a `DisplayListItem` that carried two
> `std::map`s and an occupancy map at one byte per pixel. Neither survives: the item is a 40-byte POD
> and the map is 1bpp (§2.5). The overage was never fixed by *this* section's recommendation — it was
> fixed underneath it, and the naive port then fit on the first try.
>
> Measured on the built firmware (`watchy-port-attempt-log.md` §1): **static RAM 21,756 B**, well under
> the 50–70 KB working estimate above, leaving on the order of **300 KB** of heap rather than 230 KB.
>
> Two things this does *not* retire:
>
> 1. **The slope.** The display list is O(primitives drawn), and a `REPEAT` multiplies that without
>    lengthening the script. At 40 B/item the working set crosses ~300 KB somewhere north of 5,000
>    primitives. Streaming (b) is still the only change that flattens this; it is now a headroom
>    argument rather than a fits/does-not-fit one.
> 2. **Fragmentation, which is the failure actually observed.** `micropatterns_drawing.cpp:131` records
>    a real, reproducible Watchy abort: parsing the largest script fragmented the heap down to a
>    **~26 KB largest free block**, so the occupancy map's allocation called `abort()` mid-render and
>    the watch rebooted. The fix shipped was to check `heap_caps_get_largest_free_block()` and degrade
>    to painter's order. That is a graceful failure, not a solved problem. Exact constrained-WASM
>    reproduction later showed that Arduino String growth and the parse tree fragment the heap enough
>    for a subsequent **display-list** vector growth to fail. VM bytecode does not inherently fix that:
>    compiling the retained tree to bytecode can increase peak memory. A direct-to-compact-IR parser
>    would reduce parser nodes; streaming or a compact render list addresses display-list growth.
>    See `wasm-device-resource-fidelity.md`.

**(b) Streaming renderer — recommended.** Delete the display list. Delete the occupancy map. Delete
the occlusion buffer. Interpret the command tree and rasterize each primitive immediately into the
framebuffer, back-to-front (painter's algorithm — the natural script order).

| Item | Bytes | Note |
|---|---:|---|
| Framebuffer 200×200×1bpp | 5,000 | GxEPD2's own full-height page buffer; we allocate no second canvas |
| Script text (`MAX_SCRIPT_CONTENT_LEN`) | 5,600 | reuse `event_defs.h:10` |
| Parsed command tree, ~200 cmds @ 256 B | 51,200 | dominant cost — see below |
| …param `std::map` nodes, ~2 per cmd @ ~60 B | 24,000 | |
| Assets: 16 patterns max @ 80 B + 20×20 data | 7,680 | DSL caps at 16 patterns |
| Live `MicroPatternsState` + recursion stack | ~2,000 | REPEAT/IF nesting |
| Script list JSON | 1,024 | `script_manager.h:12` |
| Exec-state JSON | 2,048 | `script_manager.h:13` |
| Render task stack | 8,192 | 2K words; float-heavy, do not shrink blind |
| **Working total, WiFi down** | **~106,700** | fits in ~230 KB |
| Fetch: TLS + HTTP + JSON, transient | +60,000 | **never concurrent with a render** |

Fits, with roughly 120 KB of headroom — but only if fetch and render are **serialised**, which the
power design wants anyway (§7.4 rule 5). Enforce it structurally: the wake router runs *fetch, then
WiFi off, then render*, never overlapping. Peak concurrent demand is then ~107 KB (render, WiFi down)
or ~72 KB (fetch, no parsed script resident), not their sum.

Two follow-on optimisations, in order, if headroom proves tight at M3:

1. **Interned parameter keys.** `std::map<String,int>` → a fixed `struct { uint8_t key; int16_t val; }
   params[8]` with keys interned to an enum at parse time. Removes ~24 KB *and* thousands of
   allocations per parse. Also removes fragmentation, which matters more than the bytes on a device
   that never reboots.
2. **`MicroPatternsCommand` slimming.** 256 B is mostly two `std::list`s (nested/then/else) and three
   `std::vector`s of tokens. A flattened bytecode array with jump offsets would take a typical script
   from ~51 KB to ~8 KB. Bigger win, bigger change — a real rewrite of `micropatterns_parser.cpp`'s
   output stage. Defer until measured.

**What the streaming design costs:** the occlusion and overdraw culling go away, so overlapping
primitives are drawn more than once. On M5Paper that mattered (518,400 pixels, slow canvas). At
200×200 there are 40,000 pixels — 13× fewer — and the framebuffer write is a byte-mask on SRAM, not
a driver call — `docs/analysis/m5paper-rasterizer-perf.md` reads the actual Xtensa disassembly and
finds the per-pixel cost dominated by a windowed `callx8` to a `std::function` interrupt check, an
out-of-line `matrix_apply_to_point` call, and redundant `mul.s` — i.e. **per-pixel overhead, which
culling avoids and which optimisation removes outright**. The working assumption is that a Watchy
render is dominated by transform maths rather than pixel writes, so culling saves little.
**M1 (on the M5Paper, today) and M3 (on the watch) settle it — do not defend either design without
those numbers.**

**`fillSpan` matters more than culling here.** `fillRect`/`fillCircle` with `FILL NAME=SOLID` should
emit horizontal runs; on a packed 1bpp buffer a span write is a couple of masked byte stores instead
of 200 read-modify-writes.

### 7.4 Power and wake architecture — designed around 0.2–0.6 mAh per cycle

This is the section everything else is subordinate to.

**The budget, from `watchy-hardware-and-references.md` §4** (their arithmetic, restated so this
document is self-contained):

| Cost component | Energy | Source |
|---|---:|---|
| Battery capacity | **~200 mAh** | SQFMI docs, not independently measured |
| WiFi connect + small HTTPS fetch, ~4 s @ ~120 mA avg | **0.133 mAh** | estimate |
| CPU render, optimistic (~1.2 s @ 80 mA) | **0.027 mAh** | estimate |
| CPU render, pessimistic (~15 s @ 80 mA) | **0.333 mAh** | estimate |
| Full refresh, 2.6 s | **0.058–0.116 mAh** | 2600 ms **measured** in the vendored `GxEPD2_154_D67.h` (`full_refresh_time`, comment "e.g. 2509602us") |
| Partial refresh, 500 ms | **0.011–0.023 mAh** | 500 ms **measured** (`partial_refresh_time`, "e.g. 457282us") |
| Panel power on / off | 100 ms / 150 ms | measured, same header |
| Deep-sleep baseline | tens of µA (order of magnitude) | inferred from InkWatchy's *"Saves 70 uA"*, `sleep.cpp:12` |

**Per cycle: ~0.2 mAh (optimistic render, partial refresh) to ~0.6 mAh (pessimistic render, full
refresh).** Reserving ~100 mAh of a 200 mAh cell for Micropatterns over a 7-day charge cycle:

```
100 mAh / 0.2 mAh  = 500 cycles / 7 days = ~71 renders/day = every ~20 min
100 mAh / 0.6 mAh  = 167 cycles / 7 days = ~24 renders/day = every ~60 min
```

> **Sustainable autonomous cadence: hourly to half-hourly. Not per-minute.**
> A per-minute cadence is 1,440 cycles/day = **288–864 mAh/day**. That is 1.5× to 4× the entire
> battery, every day. Per-minute rendering is not a tuning question; it is arithmetically impossible.

#### Render-cost sensitivity — why the rasterizer work matters more than anything else here

The render term is the only one under our control (WiFi cost is fixed by protocol, refresh cost is
fixed by the panel). The parallel rasterizer analysis estimates **10–20× achievable** on the M5Paper
rasterization hot path — devirtualising the per-pixel `std::function` interrupt check, inlining
`matrix_apply_to_point`, hoisting loop-invariant `mul.s`, span-filling instead of per-pixel calls.
Those all apply verbatim to the Watchy rasterizer, because it is *the same file*.

Sensitivity, holding WiFi and refresh fixed, assuming a daily fetch (so most cycles are render-only,
partial refresh — WiFi's 0.133 mAh amortises to ~0.006 mAh/cycle at 24 renders/day):

| Render time | CPU energy/cycle | Total/cycle (partial) | Cycles per 100 mAh | Sustainable cadence |
|---:|---:|---:|---:|---|
| 15 s (today's M5Paper worst case, unimproved) | 0.333 mAh | ~0.35 mAh | 285 | ~every 35 min |
| 5 s (3× improvement) | 0.111 mAh | ~0.13 mAh | 770 | ~every 13 min |
| 1.2 s (13×, i.e. purely pixel-bound scaling) | 0.027 mAh | ~0.045 mAh | 2,200 | ~every 4.5 min |
| 0.4 s (rasterizer optimisations land, 10–20×) | 0.009 mAh | ~0.027 mAh | 3,700 | ~every 3 min |

Read that table as: **rasterizer optimisation buys back an order of magnitude of cadence.** At
15 s/render the watch updates about as often as a bus timetable; at 0.4 s it can update every few
minutes and *feel* alive. It also changes the felt experience on a button press — 15 s of a frozen
watch after pressing "next" is a broken product; 0.4 s is instant.

Two caveats kept honest:
- The 13× pixel reduction only applies to the **pixel-bound** portion. The interpreter/display-list
  half (loops, transform maths, `std::map` lookups) does **not** shrink with resolution — same core,
  same clock. The hardware doc flags this as its single most important unknown, and the streaming
  design (§7.3) removes the display-list half rather than shrinking it.
- Sub-second renders are a *goal*, not a measurement. M3 measures it.

#### The wake machine

`setup()` runs one cycle and calls `esp_deep_sleep_start()`. There is effectively no `loop()`.

```
                 ┌──────────────── deep sleep (the default state) ────────────────┐
                 │ ESP32 off. External RTC keeps time. ~tens of µA after         │
                 │ ForceInputs(). GxEPD2 hibernated; panel holds the last image. │
                 └───┬─────────────────────────┬──────────────────────────────┬───┘
   RTC alarm (pin 27)│           ext1: any of   │ UP 35 / DOWN 4 / MENU 26 /  │ cold boot
                     │           BACK 25        │                             │
                     ▼                          ▼                             ▼
      ┌───────────────────────┐   ┌────────────────────────────┐  ┌──────────────────────┐
      │ RENDER cycle          │   │ INTERACTIVE cycle          │  │ COLD BOOT            │
      │ CPU 80 MHz            │   │ CPU 160 MHz (responsive)   │  │ grey-latch workaround│
      │ read RTC, ++$COUNTER  │   │ UP/DOWN: prev/next script  │  │ (§5.3) + full refresh│
      │ load script (LittleFS)│   │ MENU: force fetch / info   │  │ + first render       │
      │ renderStreaming()     │   │ render + PARTIAL refresh   │  │ + full fetch         │
      │ PARTIAL refresh       │   │ 10 s idle → sleep          │  └──────────────────────┘
      │ arm next RTC alarm    │   │ NEVER fetches implicitly   │
      │ sleep                 │   └────────────────────────────┘
      └───────┬───────────────┘
              │ if (now - lastFetch > FETCH_INTERVAL_H) && battery > 25 %
              ▼
      ┌──────────────────────────────────────────────────────────┐
      │ FETCH cycle — the expensive one, ~0.133 mAh              │
      │ WiFi up → GET list → GET only changed scripts → LittleFS │
      │ → WiFi DOWN → then render → sleep                        │
      │ hard 20 s budget, then abort and retry next interval     │
      └──────────────────────────────────────────────────────────┘
```

Rules, each with its reason:

1. **Default render cadence: 30 min.** Configurable `MP_RENDER_INTERVAL_MIN`. Sits in the sustainable
   band even at pessimistic render cost, and drops toward 5 min automatically if M3 shows renders are
   fast (the interval should be a runtime value derived from measured render time, not a constant).
2. **Never fetch on a button press** unless the user explicitly chooses "fetch now" from a menu. The
   M5Paper client's instinct — fetch on boot and timer wake, never on button (README) — is right;
   make it stricter. A button press must feel instant, and WiFi association takes seconds.
3. **Fetch cadence: daily, jittered.** Scripts are generative: `$COUNTER`, `$MINUTE`, `$HOUR` produce
   new output with no new data. There is no reason to talk to the network often. `MP_FETCH_INTERVAL_H`,
   default 24. This is also §9 question 4 — "publish and it's on my wrist within the hour" costs ~24×
   the network energy (3.2 mAh/day vs 0.13, i.e. ~1.6 %/day of the cell).
4. **Battery gate:** no fetch below 25 %; below 10 %, stop autonomous renders entirely and only
   render on button press. Battery via ADC pin **34** with divider **500.0f** (`condition.h:114,116`).
5. **WiFi and render are never concurrent.** Required by the RAM budget (§7.3) and it also keeps the
   current peak down. Sequence is strictly: fetch → WiFi off → render.
6. **Per-script cadence heuristic — the cheapest big win available.** At parse time, scan the command
   tree for environment-variable references and set the wake interval accordingly:

   | Script references | Natural interval | Justification |
   |---|---|---|
   | `$SECOND` | clamp to `MP_RENDER_INTERVAL_MIN` | Sub-minute change is unaffordable; the script will look "wrong" but stepping is honest. |
   | `$MINUTE` or `$COUNTER` | `MP_RENDER_INTERVAL_MIN` (30 min) | |
   | `$HOUR` only | 60 min, aligned to the hour | Rendering more often produces a **byte-identical image** — pure waste. |
   | none of the above | **never** re-render autonomously | A static script only needs redrawing on script change. Costs zero. |

   This is ~20 lines in the parser walking `MicroPatternsCommand::params` for `TYPE_VARIABLE`
   entries, and for the last two rows it eliminates renders entirely rather than making them cheaper.
7. **Night quiet hours.** Adopt InkWatchy's shape (`config.h:180-182`): between 23:00 and 05:00, do
   not render autonomously at all (the watch is on a nightstand or a sleeping wrist; nobody is
   looking). That is 6/24 = 25 % of all cycles removed for free. InkWatchy still wakes every 45 min
   because it must show the time; we have no such obligation.
8. **Persist across sleep in RTC slow memory, cheaply.** We own all 8 KB now (no InkWatchy `rM`
   competing), but keep it to a small POD anyway — `RTC_DATA_ATTR struct { char fileId[16]; uint32_t
   counter; uint32_t lastFetchUnix; uint16_t renderIntervalMin; uint16_t partialsSinceFull; uint8_t
   cold; }` ≈ 32 bytes. **Do not** cache the framebuffer there: GxEPD2 will re-render into a fresh
   5 KB heap buffer each wake anyway, and the *panel* holds the image, not us. **Do not** cache script
   text there: a 5 KB LittleFS read is microamp-seconds against WiFi's milliamp-seconds.
9. **Mirror the DRAM copy to NVS on script change only**, so a battery-out event does not lose which
   script was selected. Not on every wake — NVS writes are flash wear.
10. **`ForceInputs()`-equivalent before every sleep** (§5.3 item 3). ~70 µA ≈ 0.35 %/day.

### 7.5 Display refresh strategy

Constants are **measured**, from the vendored `GxEPD2_154_D67.h` that ships in
`InkWatchy/.pio/libdeps/Watchy_2/GxEPD2/src/epd/`: full refresh **2600 ms**, partial **500 ms**,
power-on 100 ms, power-off 150 ms. Use these, not SSD1681 datasheet numbers.

- **Partial (`display(true)`) by default** on every render cycle. 500 ms, no flash, ~0.011–0.023 mAh.
- **Full (`display(false)`) every N partials.** InkWatchy uses N=60 (`config.h:69`) for a UI that is
  mostly stable black text on white. Micropatterns output is the opposite: high entropy, often ~50 %
  black, often finely patterned, and **every pixel changes every frame**. That is the ghosting worst
  case. **Start at N = 12** and tune against the M3 photographs. At a 30 min cadence, N=12 means one
  2.6 s flash every 6 hours — negligible energy (4 × 0.116 = 0.46 mAh/day) and rarely witnessed.
- **Force a full refresh** on: (a) cold boot, (b) script change, (c) after a render whose script used
  a `DEFINE PATTERN` of period ≤ 2 px (detectable at parse time — the same scan as §7.4 rule 6),
  (d) manual request from the menu.
- **Cold-boot grey-latch workaround is mandatory** (§5.3 item 2). It is the first bug a fresh
  firmware hits and the symptom — a permanently grey-tinged panel — looks like a hardware fault.
- **Dirty-flag gate.** Borrowed from `disUp()`/`dUChange`
  (`InkWatchy/src/hardware/display/display.cpp:82-120`): if the newly rendered framebuffer is
  byte-identical to the previous one, **skip the refresh entirely**. Cheap to check (memcmp of 5,000
  bytes, microseconds) and it saves 500 ms + 0.02 mAh whenever a script's output has not actually
  changed. This pairs with §7.4 rule 6 as a runtime backstop for the static-script case.
- **Do not attempt dirty-rectangle / partial-window tracking.** GxEPD2 holds a full-height page
  buffer for this panel; a generative render dirties most of the screen anyway; and the
  partial-window path is precisely where the panel's known artefacts live (`SCREEN_PARTIAL_GREY_
  WORKAROUND` exists because of it). A whole-screen partial update at 500 ms is affordable at a
  30 min cadence. Revisit only if the cadence ever drops below ~1 min, which the power budget says
  it cannot.

### 7.6 Partition table — we control it now

Stock InkWatchy uses every byte of the 4 MB with no slack and no OTA slot
(`InkWatchy/resources/tools/fs/in/partitions.csv`; hardware doc §3). Standalone means we choose.

Proposed `Watchy_MicroPatterns/partitions.csv`:

```csv
# Name,     Type, SubType, Offset,   Size,     Flags
nvs,        data, nvs,     0x9000,   0x5000,             # 20 KB  — settings, selected script
otadata,    data, ota,     0xE000,   0x2000,             # 8 KB
ota_0,      app,  ota_0,   0x10000,  0x1A0000,           # 1.625 MB
ota_1,      app,  ota_1,   0x1B0000, 0x1A0000,           # 1.625 MB
littlefs,   data, spiffs,  0x350000, 0xA0000,            # 640 KB — cached scripts
coredump,   data, coredump,0x3F0000, 0x10000,            # 64 KB
                                                          # total = 0x400000 exactly
```

Sizing rationale:

- **App slots 1.625 MB each.** The Watchy firmware is far smaller than InkWatchy's 1.37 MB — no
  asset pipeline, no games, no book reader, five libraries instead of fifteen. Expect **400–700 KB**.
  1.625 MB is comfortable even with WiFi + mbedTLS + ArduinoJson linked in.
- **LittleFS 640 KB.** Scripts are ~5 KB each (`MAX_SCRIPT_CONTENT_LEN 5600`, `event_defs.h:10`).
  640 KB holds **>100 scripts** plus `list.json` and exec state, with LittleFS overhead. InkWatchy
  needs 2 MB because it stores fonts, images, books and encrypted vault blobs; we store text.
- Partition table stays at the **default `0x8000`**, unlike InkWatchy's custom `0x19000`. One less
  bespoke thing.

**Is OTA worth a slot?** *Yes — and this is a change of position from "scripts are data, so who
cares".* The argument for OTA is not about scripts; it is about the watch:

1. Micropatterns scripts are indeed fetched data and need no OTA. But **the renderer is not.** Every
   rasterizer optimisation from §7.4's sensitivity table, every ghosting-constant tune from M3, every
   new DSL command, is a firmware change. This is a project that will iterate on the firmware for a
   long time.
2. The watch is a **wearable with a fiddly USB connection** and no dual-slot safety net today. A bad
   flash over serial on a device you wear is a much worse failure than on a dev board.
3. The cost is **1.625 MB of flash we have no other use for.** The alternative layout (single 3.25 MB
   factory app) would be 2.5 MB of dead space.
4. The firmware **already has WiFi, HTTPS and a server it talks to.** `Update.h` + a version check in
   the existing fetch cycle is perhaps 80 lines. The API already serves per-user JSON; a
   `firmware_version` field costs nothing.

Note the hardware doc's finding: InkWatchy has **no working OTA to borrow** — its `esp_https_ota`
component and OTA certs are vestigial, pulled in transitively, with no application code calling them
(`grep -rl esp_https_ota src/` → nothing). This must be built, not copied.

**But: OTA is not a v1 milestone.** Reserve the slots in the partition table from day one (changing
the table later means a full erase and losing cached scripts), implement OTA at M6 or later. Costs
nothing to reserve; costly to retrofit.

---

## 8. Phased implementation plan

Ordered so the earliest milestone kills the riskiest assumption. Each has a concrete
*look-at-the-watch* check. **The watch currently runs Julien's own InkWatchy build from today.
Nothing below is flashed without him explicitly asking, and M2 is where that decision arrives.**

### M0 — Host harness and golden images · *no device, no flash*
**Kills:** *"the core is genuinely platform-agnostic, and we can tell when we break it."*
Build `micropatterns_core/` plus `test/host_main.cpp` (the §3 shim, productised) that renders any
script to a PGM at any dimensions. Golden renders for ≥5 example scripts at both 540×960 and
200×200. Wire into CI.
**Verify:** `make test` green. Then **open the 200×200 PGMs and look at them.** If the existing
corpus is unrecognisable at watch size, §9 question 1 is answered empirically and M6 gets promoted
ahead of M5. This is the cheapest possible answer to the project's biggest product question, and it
needs no hardware at all.

### M1 — M5Paper render-cost split · *M5Paper only, no Watchy involvement*
**Kills:** *"is render time pixel-bound (shrinks ~13× on Watchy) or loop-bound (doesn't shrink at
all)?"* — the hardware doc's §8.4, its single most important unknown, and the input to every row of
§7.4's sensitivity table.
`render_controller.cpp:60-93` already logs the generation/rasterization split via `millis()`. Add one
bracket around `_parser.parse()` (currently unmeasured). Run 8–10 real scripts of varying complexity
on the M5Paper and correlate: does total time track *pixels drawn* or *primitive count*?
**Verify on device (M5Paper, not Watchy):** read the serial log. If rasterization dominates, the
Watchy port is comfortable and the rasterizer optimisations pay off directly. If display-list
generation dominates, §7.3's optimisation ladder (interned keys, then flattened bytecode) moves from
"later" to "before M3".

### M2 — Pixels on the panel · *first flash of the Watchy; requires explicit go-ahead*
**Kills:** *"the rasterizer produces the right image on real GxEPD2 hardware."*
Minimal `Watchy_MicroPatterns/` build: `esp32dev`, Watchy 2.0 pins (§4.1), pinned GxEPD2-watchy,
`WatchySurface : mp::ISurface`, one script compiled into flash, `renderStreaming()`, the cold-boot
grey-latch workaround, one full refresh, then permanent deep sleep. No WiFi, no LittleFS, no RTC, no
buttons.
**Before flashing:** back up the current firmware. `InkWatchy/.pio/build/Watchy_2/firmware.bin`
(1,370,768 B, built today 13:36) and `bootloader.bin` / `partitions.bin` are already on disk and
match the device — confirm with the user that this is a sufficient restore path, and note that our
new partition table at `0x8000` means restoring InkWatchy needs its table re-flashed at `0x19000`.
**Verify on device:** *the watch shows the pattern, and it matches the M0 golden PGM for the same
script at 200×200.* Hold the phone next to the PGM. Specifically check:
- Not inverted — a slip in the 0/15 → 0/1 mapping (§2.2) inverts the whole image.
- Not mirrored or rotated 90° — `setRotation(0)`.
- No black frame around the edge — the `0x3C` border waveform (§5.3).
- No grey haze — the cold-boot workaround worked.

### M3 — Timing, RAM and ghosting under load
**Kills:** *"streaming beats buffered, renders are fast enough, and partial refresh survives
generative art."*
Extend M2: cycle 5 scripts of rising complexity; log `parseMs`, `rasterMs`, primitive count,
`ESP.getFreeHeap()` before and after each; loop 60 renders with partial refresh.
**Verify on device:** read the serial log for the render time — *this is the number that fills in
§7.4's sensitivity table for real* — and check free heap is flat across 60 renders (no fragmentation
leak). Then **look at the screen after 1, 10, 30 and 60 partial refreshes** and photograph each. How
much ghosting? That series sets N in §7.5. If ghosting is bad by 12, N drops; if the screen still
looks clean at 60, N rises and we save energy.

### M4 — The wake machine
**Kills:** *"the power budget survives contact with reality."*
RTC (SmallRTC on I²C 21/22, interrupt pin 27), deep sleep with ext1 button wake (35/4/26/25) and RTC
alarm wake, the `ForceInputs()`-equivalent, battery ADC on pin 34, the RENDER/INTERACTIVE/FETCH wake
router, the RTC-slow-memory state POD, the §7.4 rule-6 cadence heuristic.
**Verify on device:** set the interval to 5 min, charge fully, put it on the wrist and leave it for
48 hours. Then: *(a)* does the image actually change every 5 minutes without touching it? *(b)* does
a button press wake it and respond in under a second? *(c)* **what does the battery percentage read
after 48 h?** Extrapolate; compare against §7.4's prediction. This is the milestone that either
confirms or destroys the whole cadence design, and it needs nothing but patience and a wrist.

### M5 — Storage, script cycling, fetch
**Kills:** *"the fetch path fits in both budgets."*
LittleFS `ScriptManager` (ported from `script_manager.cpp`, SPIFFS → LittleFS), `NetworkManager`
against the existing Deno API, daily jittered fetch with the 25 % battery gate and 20 s abort, and
UP/DOWN cycling that never touches WiFi.
**Verify on device:** UP/DOWN steps through the cached scripts *instantly* — if there is a
multi-second pause, something is fetching that shouldn't be. Then publish a new script from the web
editor, pick "fetch now" from the menu, and watch it appear. Then leave it another 48 h and check
the battery again — the delta versus M4's number is the true cost of one daily fetch.

### M6 — Editor/device parity and per-device authoring targets
**Kills:** *"authors can actually design for a watch."*
Emulator: rename the size option to **"200×200 (Watchy)"**, render 1:1 with `image-rendering:
pixelated` plus a real 2×/3× zoom (today `simulator.js:207-212` disables the zoom button for
non-M5Paper sizes, making the Watchy preview a postage stamp), a watch-shaped frame, and optionally
a ghosting preview. Server: a `target` field on script metadata and a per-device index — today
`micropatterns_server/main.ts:41-84` has one flat per-user device list with no dimension at all.
Firmware: send dimensions on fetch.
**Verify on device:** author a script in the browser at 200×200, mark it for the device, fetch —
*and the watch shows exactly what the browser showed.* Photograph both side by side. That photo is
the project's thesis.

### M7 — OTA *(optional, deferred)*
The partition table reserved the slots at M2 (§7.6). `Update.h` + a firmware-version field in the
existing fetch response. **Verify:** publish a firmware build, and the watch updates itself without
a cable. Nothing is copied from InkWatchy here — it has no working OTA.

---

## 9. Open product questions for the user

> **UPDATE 2026-08-27 — the coordinate-space question below is SETTLED and is no
> longer open.** Per the project owner: *no scaling, one pixel is one pixel.*
> That is a core principle of Micropatterns, and the web editor already previews
> both screen sizes. Scripts adapt via `$WIDTH`/`$HEIGHT`; scripts with
> hardcoded coordinates will be wrong at 200x200 and that is intended. The
> implemented firmware renders at true 200x200 with no scaling. See
> `watchy-port-attempt-log.md` §5. The rest of this section stands.


Foundation is no longer among these — it is decided (§5). What remains:

1. **Coordinate space — the big one.** Should existing 960×540 scripts appear on the watch at all
   (accepting they will be cropped and wrong), or is a Watchy script a *separate artwork* authored at
   200×200? Recommendation: the latter (§7.2) — but it means the editor and server grow a per-device
   notion, and the Watchy library starts from zero. **M0 gives you evidence to decide with, before
   any firmware is written**: it renders the existing corpus at 200×200 on a laptop so you can just
   look at it. *Sub-question:* is `"any"` — genuinely resolution-independent scripts — a category
   worth supporting, or an over-complication?

2. **What is the watch, exactly?** A generative pattern that never tells you the hour is a beautiful
   object and a poor watch. (a) Pure art, no time — honest to the project, useless as a watch.
   (b) Art plus a small time overlay drawn after the render — hybrid, and someone has to design that
   overlay. (c) The DSL renders the time itself, which it can already do via `$HOUR`/`$MINUTE`, and
   the answer is "whatever the author decided" — the most Micropatterns-ish answer, and it makes the
   time a creative element rather than a chrome element. Recommendation: **(c)**, with (b) available
   as a per-script flag. This choice changes nothing technically but everything about what the
   product is.

3. **Cadence, given the budget.** §7.4 says hourly-to-half-hourly is sustainable and per-minute is
   arithmetically impossible on a 200 mAh cell. Within that: is 30 min the right *feel*? Would you
   rather have 5 min and charge weekly-becomes-every-other-day? Note §7.4 rule 6 makes this partly
   automatic — a script that only reads `$HOUR` renders hourly no matter what you set, because
   rendering more often produces an identical image.
   *Related:* wrist-raise. The BMA423 accelerometer is sitting right there and could render on a
   raise instead of on a timer — arguably the correct interaction for a watch. It is explicitly out
   of scope for v1 (§5.2) but it is a genuinely attractive v2, and it would let the timer cadence
   drop a lot. Worth wanting?

4. **Fetch cadence.** Daily (0.13 mAh/day) is proposed. Hourly is ~3.2 mAh/day — about 1.6 % of the
   cell per day just to ask "anything new?". Does the project want "I published it, it's on my wrist
   within the hour"? If so, a cheaper design exists: a tiny `HEAD`/etag check instead of a full list
   fetch, which cuts the airtime but not the association cost.

5. **M5Paper refactor appetite.** §6.4 asks for 8–12 days of refactoring a *working* firmware before
   any Watchy value ships. Acceptable? Or fork the core temporarily (R0–R2 only), accept duplication,
   and pay it back after the Watchy port proves itself? The one step that must not be skipped either
   way is R2's golden images — they are what make the rest safe.

6. **Flashing the watch, and the restore path.** M2 replaces InkWatchy. The backup exists on disk
   (`InkWatchy/.pio/build/Watchy_2/{bootloader,partitions,firmware}.bin`, built today) but restoring
   it also means re-flashing InkWatchy's custom partition table at `0x19000` and re-uploading its
   2 MB LittleFS asset image — which is *not* in that build directory and would have to be rebuilt
   from `resources/tools/fs/createFs.sh`. Is that restore path acceptable, or should the LittleFS
   image be pulled off the device first (`getFs.sh` does exactly this) while it is still intact?
   **Recommendation: pull it before M2.** It is read-only, cheap, and it is the difference between a
   reversible experiment and a one-way door.

7. **Is a second Watchy worth £/€ 50–80?** Not a design question, but: every milestone from M2 on
   competes with the user's ability to wear a working watch. A second unit turns this from a
   sequential, permission-gated process into a parallel one. Worth raising once.

---

## 10. Risks and things that could sink this

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| 1 | **Nothing looks good at 200×200.** The corpus is composed for a tall 540×960 canvas and may simply not translate; a hand-authored watch corpus is real creative work. | Medium-High | Project-defining | **M0 answers this before any firmware work** — render the corpus at 200×200 on a laptop and look. |
| 2 | **Render time is loop-bound, not pixel-bound**, so the 13× pixel reduction buys nothing and a rich script takes ~15 s at 80 MHz — which §7.4 shows caps cadence at ~35 min *and* makes button response unusable. | Medium | High | **M1 measures it on the M5Paper today, before any Watchy code.** Fallbacks: 160/240 MHz for the render only; the rasterizer optimisations (10–20× estimated); §7.3's interned-key and bytecode ladder for the interpreter half; bounding script complexity server-side. |
| 3 | **Battery life is unacceptable in practice.** The §7.4 numbers are estimates built on estimates — deep-sleep baseline, active current and WiFi cost were none of them measured on this unit. | Medium | High | M4's 48 h wrist test is the real answer. An in-line USB power meter would tighten §7.4 substantially and is non-destructive — worth doing before M4 rather than after. |
| 4 | **Ghosting makes generative art unreadable** under partial refresh, forcing full refreshes every cycle — 2.6 s of flashing and 5× the refresh energy. | Medium | High | M3's photo series quantifies it and sets N. Mitigations: lower N; parse-time detection of ≤2 px patterns; accept full-refresh-only at a 30 min cadence, which the budget can actually afford (0.116 vs 0.023 mAh is only 0.09 mAh/cycle). |
| 5 | **Parsed-command-tree RAM exceeds the estimate** — `MicroPatternsCommand` is 256 B and a rich script may have far more than the 200 commands assumed. | Medium | High | M3 measures. §7.3's ladder: interned param keys (−24 KB, kills thousands of allocations), then flattened bytecode (~51 KB → ~8 KB). |
| 6 | **Heap fragmentation on a device that never reboots.** Thousands of small `std::map`/`String` allocations per render, forever, with no PSRAM. Free heap can look fine while the largest free block shrinks. | Medium | High | M3 must log `heap_caps_get_largest_free_block()`, **not just** `getFreeHeap()`. If it trends down, the interned-key change stops being optional. Deep sleep does *not* reset the heap — but a full power-cycle does, and the wake model means each cycle starts from a fresh boot, which is a quiet structural mitigation worth confirming. |
| 7 | **R3 introduces a subtle visual regression on M5Paper** that nobody notices for weeks. | Medium | Medium | Exactly why M0/R2 golden images come first. Do not do R3 without them. |
| 8 | **We rediscover a panel bug InkWatchy already solved but that we didn't identify** as one of the three borrowed fixes. | Medium | Medium | The three in §5.3 are the ones visible in source. There may be more buried in the `Szybet/GxEPD2-watchy` fork's diff against upstream — **read that diff** before M2; it is the cheapest possible insurance and it is sitting in `.pio/libdeps/Watchy_2/GxEPD2/`. |
| 9 | **Losing the user's working watch.** One device, no OTA safety net on the stock layout, and the LittleFS restore image is not currently backed up. | Medium | High | §9 question 6: pull the LittleFS image with `getFs.sh` *before* M2. Verify the `.bin` backups restore. Never flash unasked. |
| 10 | **A bad flash bricks it mid-development.** Especially once we ship our own OTA (M7). | Low | High | The reserved dual OTA slots (§7.6) exist precisely for this — but they only help once M7 lands, so until then serial recovery is the only path and the USB connection is the weak link. |
| 11 | **Virtual `drawPixel` costs too much** on the hot path. | Low | Low-Medium | `fillSpan` first; template specialisation second (§6.3). Both known, bounded. |
| 12 | **The `Szybet/GxEPD2-watchy` fork goes unmaintained** or its pinned commit disappears. | Low | Medium | Vendor it into the repo rather than fetching from GitHub at build time. It is a small library and the whole point is that we want *this exact* patched version, frozen. |
| 13 | **`$WIDTH`/`$HEIGHT` integer division bites at small sizes** — `200/3 = 66` accumulates visible error in tiled compositions in a way `540/3 = 180` does not. | Low | Low | Author-visible, not a firmware bug. Worth a note in the DSL docs and a warning in the editor's Watchy preview. |

---

## 11. Dead ends and rejected alternatives, with reasons

Recorded so nobody re-litigates them.

### 11.1 Micropatterns as an app inside InkWatchy — **rejected by owner decision, not on technical merit**

This document's first draft recommended it, and the reasoning was not wrong. Preserved in full,
because the day someone asks "why didn't we just add it to InkWatchy?" this is the answer:

**What it would have given us, free and field-tested on this exact hardware:** the panel bring-up and
all three refresh workarounds (§4.4); the deep-sleep + ext1 + RTC-alarm machinery with its measured
70 µA of GPIO hygiene; battery monitoring with the v2 divider and charge detection; SmallRTC
timekeeping, NTP and Olson timezones; multi-credential WiFi; LittleFS; debounced buttons; coredump
handling; a watchface-selector UI to slot into; and — the big one — **the user keeps a working
watch**. `conwayApp` was an exact structural precedent: a generative, grid-based, self-updating app
already living in that codebase. Integration surface would have been small: one `UiPlace`, one
watchface file, one IDF component, ~32 bytes added to the `rtcMem` struct. Flash had ~517 KB free in
the app slot, ample for a ~60–100 KB core.

**What it would have cost:** 34 K lines of someone else's code to navigate; a personal fork whose
upstream merges the user owns forever; a hybrid ESP-IDF/CMake build; a function-based architecture
the class-based core would have to sit behind a facade of; an hourly forced full recompile; and a
firmware whose scope (books, games, alarms, vault, accelerometer, six board revisions) is
overwhelmingly not this project.

**Owner's decision:** *"I want a standalone firmware"* / *"I don't care about the InkWatchy firmware
apart from it being a well functioning example."* The scope mismatch is the deciding factor, and it
is a legitimate one — Micropatterns is structurally far closer to the existing M5Paper client than to
an everything-watch application. The parallel hardware study reached the same conclusion
independently on technical grounds.

**What we take instead:** three cited fixes and nothing else (§5.3). **What we accept losing:** §5.4.
**What stays open:** because the language lives in `micropatterns_core/`, an InkWatchy integration
remains a possible future at the cost of only an adapter layer.

### 11.2 The official SQFMI Watchy Arduino library as the base

Rejected. Per the hardware doc §6C: less actively hardened than InkWatchy for exactly the failure
modes that matter here (ghosting, RTC library breakage, board-revision quirks), and it does not
document the measured refresh timings this design relies on. It offers nothing that plain Arduino +
GxEPD2 does not, while adding a `Watchy` base class whose sleep/loop model we would immediately fight
(it assumes a minute-ticking watchface; §7.4 says we cannot afford one).

### 11.3 "A shared core was designed at repo-root `src/` and abandoned"

*False.* Investigated via git: `6c1b5cd` ("m5paper task and rtos rearchitecture") put manager files
at repo-root `src/` during a refactor; `19af41e` ("clean up scattered files", 2025-05-21) moved them
into `M5Paper_MicroPatterns/src/` and left the `build_src_filter` lines behind. Those 17
`+<../../src/...>` lines have been dead no-ops ever since — harmless only because `+<*>` already
matches the real files. **No shared-core attempt was ever made, let alone failed.** Delete them
(§6.4 R0). §6.1.

### 11.4 Porting the display-list renderer to Watchy as-is

Rejected: ~856 KB of heap on a device with ~180 KB (§7.3a). Off by 7×; not tunable.

### 11.5 Shrinking `DisplayListItem` to a POD and keeping the display list

A POD item is ~44–68 B, so 2,000 items is 88–136 KB — technically survivable, but it consumes the
entire budget to buy back an optimisation (occlusion culling) whose value at 40,000 pixels is
unproven. Rejected in favour of deleting the display list outright (§7.3b). **Revisit only if M3
shows streaming is too slow** — this is the one rejected alternative with a live trigger condition.

### 11.6 Keeping the 1-byte-per-pixel occupancy map

At 200×200 it is 40 KB — 22 % of the budget for an overdraw optimisation on a screen with 13× fewer
pixels than the one it was designed for. A 1bpp version would be 5 KB, but the streaming design has
no use for it at all: it needs front-to-back order, which needs the display list. Rejected with it.

### 11.7 Scaling / cropping / letterboxing 960×540 scripts to 200×200

Rejected. `DEFINE PATTERN` data is a pixel-exact bitmap and the DSL's `SCALE FACTOR` is an integer
≥ 1, so shrinking is not even expressible. Cropping shows a corner of nothing (compositions centre on
`$WIDTH/2`); letterboxing wastes 44 % of a small screen *and* still needs pattern scaling. §7.2.

### 11.8 "InkWatchy is ESP-IDF/CMake, so porting Arduino code is a rewrite"

*Investigated and false.* `InkWatchy/platformio.ini` line 4: `framework = espidf, arduino`. The
Arduino API is fully available; `src/defines/defines.h:20` includes `<Arduino.h>` and the tree uses
`String`, `pinMode`, `SPI`, `WiFi.h` throughout. This was raised as an argument against the InkWatchy
path and it was a bad argument — recorded here so it is not resurrected. The real (much smaller)
issue was a toolchain gap: arduino-esp32 2.0.4 / IDF 4.4 on M5Paper vs 3.1.0 / IDF 5.3.2 on
InkWatchy. Standalone means we simply pick a modern arduino-esp32 and the gap disappears. §4.2.

### 11.9 Caching the framebuffer or the script in RTC slow memory

Rejected even though standalone means we own all 8 KB. The *panel* holds the image across sleep —
that is what e-ink is — so caching 5 KB of framebuffer buys nothing but complexity. And a 5 KB
LittleFS read on wake is microamp-seconds against WiFi's milliamp-seconds. Keep RTC slow memory for
a ~32 byte state POD (§7.4 rule 8). InkWatchy needs the framebuffer there
(`InkWatchy/src/hardware/rtcMem/rtcMem.h:23`, `.rtc.data = 6464` in its map file) because it does
cross-sleep partial-window updates of clock digits; we do whole-screen renders and do not.

### 11.10 Dirty-rectangle / partial-window tracking

Rejected: GxEPD2 holds a full-height page buffer for this panel, generative renders dirty most of the
screen anyway, and the partial-window path is where the panel's known artefacts live. A whole-screen
partial update at a **measured** 500 ms is affordable at a 30 min cadence. §7.5. The cheap version of
the same idea — a whole-framebuffer memcmp to skip identical refreshes — *is* adopted.

### 11.11 A `Canvas` interface with a large blitting API

Rejected by evidence: the rasterizer compiles against exactly four methods (§3). A wider interface
would be speculative surface area, and every extra virtual is another thing each adapter must get
right. `fillSpan` is the single justified addition and it ships with a working default.

### 11.12 InkWatchy's ULP / lpCore always-on path

Out of scope. It exists to redraw clock digits without waking the main core — it pays for itself for
a ticking second hand, not for periodic art renders, and it costs "+150uA" while armed
(`InkWatchy/src/hardware/sleep/sleep.cpp:107`). At a 30 min cadence there is nothing for it to do.

### 11.13 A single-slot (no-OTA) partition table

Rejected, reversing an earlier instinct. "Scripts are fetched data, so we don't need OTA" is true and
irrelevant: the *renderer* is not data, and this project will iterate on firmware for a long time
(every row of §7.4's sensitivity table is a firmware change). The slots cost 1.625 MB of flash we
have no other use for — the alternative is 2.5 MB of dead space. Reserve at M2, implement at M7.
§7.6.

### 11.14 A full standalone build attempt during this design pass

Not run, and no flash/RAM figures for a Watchy Micropatterns build are claimed anywhere in this
document, because none were measured. Building the skeleton requires PlatformIO to download a
platform and the pinned GxEPD2/Adafruit-GFX forks — a network-fetch side effect this pass declined to
take unprompted. The cheaper and more informative experiment was run instead: **host compilation of
the core** (§3), which tests the actual load-bearing assumption. The device build is M2, with the
user's go-ahead.

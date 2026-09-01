# Watchy client: hardware identification, constraints, and reference-firmware study

Scope: read-only hardware characterization of the connected device, a constraints comparison against the working M5Paper client, explicit power/render-budget math, and a study of the local InkWatchy reference tree at `/Users/julien/Documents/GitHub/InkWatchy` to ground a future porting decision. **No writes/erases were performed on the device** — all probes below were read-only (`chip_id`, `flash_id`, `read_flash`).

---

## 1. Device identification

### Verdict: Watchy 2.0, high confidence

**Read-only serial evidence (this session, `/dev/cu.usbserial-110`):**
- `esptool.py chip_id`: **ESP32-PICO-D4, revision v1.0**, dual core 240MHz, embedded flash, no PSRAM. MAC `4c:75:25:a7:65:c0`.
- `esptool.py flash_id`: manufacturer `c8` (GigaDevice), device `4016`, **4MB flash**.
- USB-UART bridge enumerates as `usbserial` via a CP2102N (per your prior probe) — consistent with SQFMI's later Watchy production runs, though web sources are inconsistent about which exact revision shipped with CP2104 vs CP2102N (see Open Questions). **Treat the USB chip as corroborating, not decisive.**
- Reading the bootloader header at flash offset `0x1000` (read-only): magic byte `0xE9` valid, IDF version string `5.3.2`, build timestamp string **`Aug 27 2026 13:30:26`**.
- Reading the (non-standard) partition-table region: the table is **not** at the default ESP32 offset `0x8000` (read back all `0xFF`, i.e. erased/unused) — it lives at `0x19000`, an InkWatchy-specific custom offset (see §3).

**Local-repo evidence (`/Users/julien/Documents/GitHub/InkWatchy`), which is far stronger than the web-only signals:**
- The only checked-in per-environment sdkconfig is `sdkconfig.Watchy_2` (`CONFIG_PARTITION_TABLE_OFFSET=0x19000`, matching the device read above exactly).
- The repo has a **local `.pio/build/Watchy_2/` build output** with `bootloader.bin` timestamped `Aug 27 13:30` and `firmware.bin` timestamped `Aug 27 13:36` — **the same day and the same minute** as the bootloader build string read directly off the device's flash (`Aug 27 2026 13:30:26`). This is about as close to a direct match as read-only forensics can get without dumping and diffing the full app image: **the firmware currently on the device is (almost certainly) this exact local `Watchy_2` build**, built today.
- `platformio.ini` defines six environments: `Unknown` (stub/disabled), `Watchy_1`, `Watchy_1_5`, `Watchy_2` (board `esp32dev`, i.e. classic ESP32, not S3/C6), `Watchy_3` (board `esp32-s3-devkitc-1`), `Yatchy` (board `esp32-c6-devkitm-1`, community RP2/C6 variant). Only `esp32dev`-class boards (`Watchy_1/1.5/2`) match the confirmed ESP32-PICO-D4 chip — this alone rules out Watchy 3.0 (ESP32-S3) and Yatchy (ESP32-C6), matching your original hardware-based exclusion of v3.0.
- `src/defines/condition.h` distinguishes `WATCHY_1` / `WATCHY_1_5` / `WATCHY_2` only by three GPIOs and the RTC chip:
  - `UP_PIN`: **35** for `WATCHY_2`, **32** for `WATCHY_1`/`WATCHY_1_5`.
  - `BATT_ADC_PIN`: **34** (`WATCHY_2`), **35** (`WATCHY_1_5`), **33** (`WATCHY_1`).
  - `RTC_TYPE`: `EXTERNAL_RTC` for all three (v1.0 = DS3231, v1.5/v2.0 = PCF8563 per SQFMI's own docs — InkWatchy's `SmallRTC` abstraction auto-detects which chip is present over I2C rather than hard-coding it per `ATCHY_VER`).
  - `EPD_CS/DC/RESET/BUSY` (5/10/9/19) and SPI pins (18/19/23/5) are identical across v1/v1.5/v2 — the panel wiring didn't change, only battery ADC and one button pin did.

**Conclusion:** the checked-out sdkconfig, the byte-for-byte-matching build timestamp between the local `.pio` output and the live device bootloader, and the ESP32-PICO-D4 (not S3/C6) chip identity together point at **Watchy 2.0**, built from this exact local InkWatchy checkout, today. Confidence: high on "Watchy 2.0, running this local InkWatchy build"; the CP2102N-vs-CP2104 signal is a secondary corroboration, not the load-bearing evidence.

---

## 2. M5Paper vs Watchy 2.0 constraints table

| Dimension | M5Paper (existing client) | Watchy 2.0 (target) | Ratio / delta |
|---|---|---|---|
| SoC | ESP32-D0WD, 2 core, 240MHz | ESP32-PICO-D4, 2 core, 240MHz | Same core, PICO-D4 is a single-package SiP (chip+flash+crystal) |
| SRAM | 520KB + **4MB PSRAM** | **520KB, no PSRAM** | M5Paper's PSRAM headroom is entirely gone |
| Flash | 16MB (`default_16MB.csv`) | **4MB** | 4x smaller; new partition table mandatory |
| Display panel | 960x540, 4bpp (16 grey levels), IT8951 controller | 200x200, **1bpp** monochrome, SSD1681 controller | |
| Pixel count | 518,400 px | 40,000 px | **~13.0x fewer pixels** (518400/40000 = 12.96) |
| Framebuffer size | M5EPD Canvas at 4bpp, 540x960 → 259,200 bytes (~253KB) | 200x200 @ 1bpp → 200*200/8 = **5,000 bytes** | **~51.8x smaller framebuffer** |
| Full refresh time | (IT8951, several hundred ms–seconds depending on mode; not re-measured here) | **2.6s measured** (`full_refresh_time = 2600` in the vendored `GxEPD2_154_D67.h`, comment "e.g. 2509602us" i.e. an actual logged measurement), with the characteristic full-panel flash | |
| Partial/fast refresh | supported on IT8951 | **500ms measured** (`partial_refresh_time = 500`, "e.g. 457282us") | |
| Panel power on/off | n/a here | 100ms / 150ms (measured constants in the same driver) | |
| Battery | mains/large battery, tolerates long renders | **~200mAh LiPo**, multi-day/week deep-sleep target | |
| Tolerable render time | ~15s acceptable today | **seconds, not tens of seconds** — see §4 | |

The headline number for feasibility: **13x fewer pixels to fill, but only if render cost is pixel-bound.** MicroPatterns scripts do per-command procedural work (loops, tiling/repeat counts, transform math) that does **not** automatically shrink with the framebuffer — a script with heavy loop counts could still take close to its M5Paper wall-clock time on Watchy even though it's writing 13x fewer final pixels, because the CPU is the same architecture at the same clock speed. Pixel-fill/blit-heavy operations should scale down roughly with pixel count; loop/branch-heavy interpretation will not. This must be verified empirically, not assumed, before committing to a UX design (see Open Questions).

---

## 3. Flash budget (Watchy 2.0, InkWatchy's own partition table)

`resources/tools/fs/in/partitions.csv` (this is the table actually written to `0x19000` on the current build, upload starts at `0x20000` per `board_upload.offset_address`):

```
factory,  app,  factory,  0x20000, 0x1cd000,     # 1,888,256 bytes  (~1.80 MB)
littlefs, data, littlefs, 0x1ed000, 0x200000,    # 2,097,152 bytes  (2.00 MB exactly)
nvs,      data, nvs,      0x3ed000, 0x3000,      # 12,288 bytes
coredump, data, coredump, 0x3f0000, 0x10000,     # 65,536 bytes
```

`0x20000 (bootloader+PT reserve) + 0x1cd000 + 0x200000 + 0x3000 + 0x10000 = 0x400000` — **exactly the full 4MB**, with zero slack. Two consequences for a port:

1. **There is no OTA partition** — this is a single "factory" app slot, not `ota_0`/`ota_1`. Any firmware update model has to be full-reflash-over-serial (or a from-scratch OTA scheme with its own partition table, which would have to steal space from `littlefs` or shrink the app slot).
2. **2MB of the 4MB flash is a LittleFS asset partition** — InkWatchy uses this heavily for resources (fonts, images, books, watchface assets — see `resources/` tree). A MicroPatterns port has a much narrower need (cached scripts + small assets), so this partition could likely shrink substantially, freeing flash for a future OTA slot or just leaving margin. Compare to M5Paper's 16MB, which the current client apparently doesn't need to ration at all.

---

## 4. Power / render-budget math — the central constraint

### Ingredients (labelled by source)

- Battery: **~200mAh** (confirmed by SQFMI docs and widely reported; not independently measured here).
- Deep sleep current: SQFMI's stock library targets "weeks" of battery life on this same 200mAh cell (`watchy.sqfmi.com/docs/battery-life/`, general claim, not a quoted µA figure from that page in this pass). InkWatchy's own source shows deliberate, aggressive deep-sleep current shaving: `src/hardware/sleep/sleep.cpp`, `ForceInputs()` (Watchy_1/1.5/2 branch) forces every otherwise-floating GPIO to `INPUT` before sleep with the inline comment **"Saves 70 uA"** — i.e. the InkWatchy authors measured a 70µA delta from this single optimization, implying baseline leakage in the same tens-of-µA order of magnitude. Treat "tens of µA in deep sleep" as a reasonable working estimate, not a hard spec — this is the strongest concrete "needs a read-only-safe empirical measurement" item in this report (see Open Questions; measuring current draw is non-destructive and doesn't require flashing).
- ESP32 active current: general ESP32 datasheet/community figures — CPU-active-no-radio ~40-80mA at 240MHz; WiFi associated/idle ~80-100mA; WiFi TX bursts ~120-260mA peak. Not independently measured on this specific board in this session.
- E-ink refresh timing: **measured, sourced directly from the vendored `GxEPD2_154_D67.h`/`.cpp` driver** InkWatchy actually ships (`.pio/libdeps/Watchy_2/GxEPD2/src/epd/GxEPD2_154_D67.{h,cpp}`, fork `Szybet/GxEPD2-watchy`):
  - `power_on_time = 100` ms
  - `power_off_time = 150` ms
  - `full_refresh_time = 2600` ms (comment: "e.g. 2509602us" — an actual logged run)
  - `partial_refresh_time = 500` ms (comment: "e.g. 457282us" — an actual logged run)
  - These are used as literal `_waitWhileBusy(..., X)` timeouts in `_PowerOn`/`_PowerOff`/`_Update_Full`/`_Update_Part` — i.e. this is the real, current, in-use timing for this exact panel/controller/library combination, not a generic SSD1681 datasheet number.

### Worked example

Take a single "fetch-and-render" cycle: WiFi connect + HTTP fetch of a script, CPU parse/render, then an e-ink update.

- WiFi connect + small HTTP fetch, ~4s at an average ~120mA (mix of association/TLS bursts and idle-connected draw): `0.120A × (4/3600)h ≈ 0.133 mAh`
- CPU-active parse + render: **this is the unknown that most needs empirical measurement** (see §2's pixel-vs-loop-bound caveat). Bounding it:
  - Optimistic (render time scales with the 13x pixel reduction from M5Paper's up-to-15s figure): ~1.2s × 80mA ≈ `0.027 mAh`
  - Pessimistic (render time dominated by script loop/interpretation cost, roughly M5Paper's worst case unchanged): 15s × 80mA ≈ `0.333 mAh`
- Display update: full refresh 2.6s at an estimated ~40-80mA combined CPU+panel draw during the update ≈ `0.058-0.116 mAh`; a partial refresh at 500ms is ~5x cheaper, ≈ `0.011-0.023 mAh`.

**Per-render energy cost: roughly 0.2 mAh (optimistic, partial refresh) to ~0.6 mAh (pessimistic, full refresh, loop-bound render).**

Against a 200mAh cell, and reserving the large majority of the budget for baseline deep-sleep + RTC + any per-minute time display (a stock watch face partial-refreshing every minute, at ~0.011-0.02 mAh/tick, already costs **16-29 mAh/day** just for 1,440 ticks — a full 8-15% of total capacity per day on its own, which is why real Watchy battery life is reported in single-digit-to-low-double-digit days, not weeks, once minute-ticking is active):

- If MicroPatterns renders are the **only** additional draw and the target is a 7-day charge cycle with, say, 100mAh of headroom after baseline: **100mAh / 0.2-0.6 mAh ≈ 165-500 renders over 7 days ≈ 24-70 renders/day**, i.e. **roughly hourly-to-half-hourly**, not per-minute and absolutely not "render continuously."
- **This is the key qualitative difference from the M5Paper client**: M5Paper tolerates a 15-second render because it's mains-adjacent and single-shot; on Watchy the same 15-second render, repeated even a few dozen times a day, is a plausible single-digit-percent-per-day battery drain on its own, before counting WiFi and refresh. A Watchy port needs either (a) render times pushed down toward the "optimistic" end above (likely by simplifying/bounding script complexity or optimizing the interpreter loop), (b) a much lower render cadence (e.g. once per hour or on-demand via button, not autonomous), or both.

---

## 5. InkWatchy architecture study (local source, `/Users/julien/Documents/GitHub/InkWatchy`)

### Build system

- **Hybrid framework**: `platformio.ini` line 4, `framework = espidf, arduino` — the Arduino API is available inside an ESP-IDF/CMake project (`CMakeLists.txt`, `sdkconfig.*`, `managed_components/`). This is **not** an Arduino-only PlatformIO project like the M5Paper client, but it is also not IDF-only — adopting it does not force a full IDF rewrite of MicroPatterns' Arduino-style code.
- Toolchain pinned to `pioarduino` `platform-espressif32` 53.03.13, ESP-IDF v5.3.2, `arduino-esp32` 3.1.0 — all fetched from GitHub release zips, not the stock PlatformIO registry.
- Board revision handled entirely by PlatformIO environments + one `-D ATCHY_VER=WATCHY_2`-style build flag, fanned out through `#if ATCHY_VER == ...` blocks in `src/defines/condition.h` (see §1). No runtime board detection — it's a compile-time choice per environment.
- Heavy reliance on vendored/forked libraries pinned to exact commits in `lib_deps` (`Szybet/GxEPD2-watchy`, `Szybet/StableBMA`, `Szybet/SmallRTC`, `Szybet/Olson2POSIX`, etc.) — the project forks and patches upstream libraries rather than using them stock.
- `custom_component_remove` strips a long list of unused managed ESP-IDF components (rainmaker, zigbee, esp32-camera, esp-sr, etc.) to control build size/time — a good pattern to copy regardless of foundation chosen.
- No confirmed heap boost from PSRAM: `sdkconfig.Watchy_2` line 1287, `# CONFIG_SPIRAM is not set` (expected, since PICO-D4 has none).

### Display layer (`src/hardware/display/display.{h,cpp}`)

- Thin wrapper directly over `GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> *dis`, no independent framebuffer — GxEPD2's own internal page buffer is the only buffer (confirmed by the driver's `WIDTH/HEIGHT = 200` and the fact InkWatchy never allocates a separate canvas).
- `disUp()` (display.cpp) implements dirty-tracking + periodic forced-full-refresh: a `dUChange` boolean flag gates whether anything is redrawn at all, and a counter `rM.updateCounter` forces a full refresh (`FULL_UPDATE`) every `FULL_DISPLAY_UPDATE_QUEUE = 60` partial updates (`src/defines/config.h:69`), otherwise uses `PARTIAL_UPDATE`. This is the ghosting-mitigation strategy: cheap partials most of the time, a "real" full refresh periodically to clear accumulated ghosting.
- Two panel-misbehavior workarounds, both load-bearing for a port targeting the same physical panel:
  - `SCREEN_PARTIAL_GREY_WORKAROUND` (on by default, "Experimental, at your own risk!" per the `platformio.ini` comment): on first boot only (`bootStatus.fromWakeup == false`), forces a full clear+white fill+`FULL_UPDATE` before any partial updates are attempted (`display.cpp` init, `#if SCREEN_PARTIAL_GREY_WORKAROUND`). This implies partial updates issued without a preceding full init/clear can leave the panel in a grey/ghosted state on this specific SSD1681/GDEH0154D67 pairing.
  - `SCREEN_FULL_WHITE_WORKAROUND` (off by default): if enabled, every full-update call is immediately followed by a forced `PARTIAL_UPDATE` 50ms later (`updateDisplay()`, display.cpp:222-233) — a fallback for panels that go fully white after a full refresh. Left off by default, but present because it was apparently needed on some physical units.
  - `SCREEN_BLACK_BORDER` (off by default) and a hand-documented patch location (comment block in `initDisplay()`) for changing the `BorderWavefrom` command (`0x3C`) in the vendored `GxEPD2_154_D67.cpp` to remove/change the panel's border color — confirms the specific SSD1681 border-waveform quirk is a real, previously-hit issue, not speculative.
- README also documents a multi-week ghosting fix effort ("Huge thanks for pointing out the direction for a potential fix for screen ghosting, I easily have spend 2 weeks on it" — README.md:136), corroborating that ghosting on this exact panel was a nontrivial, hard-won engineering problem, not a minor detail.

### Power management / sleep architecture

- `src/hardware/sleep/sleep.{h,cpp}`: `goSleep()` detaches all button interrupts, optionally engages the ULP "LP core" path (`LP_CORE` build flag) for continuing minimal clock-face updates without waking the main CPU (`src/hardware/lpCore/`), then calls `deInitScreen()` (hibernates the EPD) before `esp_sleep_enable_ext1_wakeup(pinToMask(UP_PIN) | DOWN_PIN | MENU_PIN | BACK_PIN, EXT1_WAKEUP_STATE)` and `esp_deep_sleep_start()` (sleep.cpp:169,182).
- Wake sources: EXT1 (any of the four buttons) plus, implicitly, RTC alarm interrupt wiring for periodic wake (external PCF8563/DS3231 RTC drives timekeeping across sleep, not the ESP32's own RTC timer alone).
- `ForceInputs()` (sleep.cpp, top) is a hand-tuned, per-GPIO leakage-current reduction pass specific to this exact board's unused/floating pins — a genuinely good, cheap idea to copy (it's essentially free correctness/power hygiene) but its exact pin list is Watchy-2-hardware-specific and would need re-deriving if pin usage differs in a MicroPatterns port.
- The ULP "LP core" path (`src/hardware/lpCore/`) is a much bigger undertaking: it runs a small program on the ESP32's ultra-low-power coprocessor to redraw just the clock digits without waking the main core/WiFi/display driver stack at all. This is InkWatchy's most sophisticated power optimization and almost certainly **out of scope** for a first MicroPatterns Watchy port — it only pays for itself for a ticking clock face, not for periodic art-pattern renders, and it adds meaningfully to build/maintenance complexity.

### WiFi (`src/network/wifi/`)

- `wifiTask.cpp`: WiFi runs in its own FreeRTOS task (`createWifiTask`), tries a list of stored credentials (`SIZE_WIFI_CRED_STAT`), retries per-SSID for `WIFI_SYNC_TIME` before falling through to the next, and calls a completion callback (`functionToRunAfterConnection`) — a straightforward async-connect-then-callback pattern, reasonable to copy directly.
- Also sets a WiFi country code (`setWifiCountryCode()`, gated by `WIFI_COUNTRY_FIX`) — a small regulatory-compliance detail worth keeping.
- `wifiQuick.{h,cpp}` (not read in full) appears to be a lighter-weight variant, presumably for quick reconnects without the full retry ladder.
- **No active OTA code path** was found in application source (`grep -rl esp_https_ota\|esp_ota_ src/` returns nothing) — the `esp_https_ota` component and embedded certs (`board_build.embed_txtfiles` in `platformio.ini`, e.g. `rmaker_ota_server.crt`) appear to be **vestigial**, pulled in transitively by an ESP-IDF managed component (possibly RainMaker-adjacent) rather than something InkWatchy's own code calls. Do not assume InkWatchy has a working OTA update mechanism to borrow — it would need to be built from scratch either way.

### Resources / assets (`resources/`)

- `resources/tools/` holds a battery of shell/Python conversion scripts (`convertImages.sh`, `convertFonts.sh`, `convertBooks.sh`, `convertImagesVault.sh`) that pre-process source assets (images, fonts, EPUB-like books) into C headers or a LittleFS image at build time, plus `resources/tools/fs/{createFs.sh,flashFs.sh,getFs.sh}` for building/flashing/pulling the LittleFS partition, and `resources/tools/buildTime/preBuild.py` (referenced from `platformio.ini extra_scripts = pre:resources/tools/buildTime/preBuild.py`) which injects `GIT_COMMIT_HASH`, `GIT_BRANCH`, and an hourly `BUILD_TIME` define.
- This is a much larger asset pipeline than MicroPatterns needs (books, videos, watchface image sets, a "vault"). The build-time embedding pattern (compile assets into flash/LittleFS rather than fetching everything at runtime) is worth partially copying; the breadth of asset types is over-engineering for this project.

### Buttons / input (`src/hardware/input/`)

- `buttons.cpp` (~14KB) handles debouncing, short/long-press detection (`BUTTON_LONG_PRESS_MS = 500`), and a `combinations.cpp` module for multi-button chord detection — more sophistication (chords, long-press variants) than a MicroPatterns client likely needs, but the debounce/long-press core is standard and reusable.
- `buttonsInterrupt.cpp` wires the four EXT1 GPIOs to ISR-based wake, consistent with the sleep wake-source config in §above.

### Good ideas worth copying

1. The `dUChange` dirty-flag + `FULL_DISPLAY_UPDATE_QUEUE`-counted forced-full-refresh pattern (display.cpp) — simple, cheap ghosting mitigation directly applicable to a MicroPatterns render loop.
2. `ForceInputs()`'s floating-GPIO cleanup before sleep — cheap, measurable (70µA) power win.
3. The three panel-specific workaround flags (`SCREEN_PARTIAL_GREY_WORKAROUND`, `SCREEN_FULL_WHITE_WORKAROUND`, `SCREEN_BLACK_BORDER`) and the border-waveform patch note — this panel/controller pairing has known quirks, and re-discovering them from scratch would cost real time.
4. `custom_component_remove` for build-size hygiene, and the WiFi async-task-with-callback pattern.
5. The measured GxEPD2 driver timing constants themselves (§4) — use them as the empirical baseline rather than re-measuring from zero.

### Over-engineering for this project's needs

1. The ULP "LP core" always-on clock path — big complexity for a use case (periodic art renders, not a ticking second hand) that doesn't need it.
2. The book/video/vault/watchface-selector asset ecosystem in `resources/` — MicroPatterns needs a much smaller asset story (cached scripts, maybe a handful of small bitmaps).
3. Button chord combinations — likely unnecessary for a simpler "next pattern / refresh / settings" input model.
4. The six-board-revision compile-time matrix (`Watchy_1/1.5/2/3`, `Yatchy`) — a MicroPatterns port only needs Watchy 2.0 support initially; carrying the full matrix from day one is speculative generality.

---

## 6. Foundation recommendation

Three candidate foundations, evaluated honestly rather than defaulting to InkWatchy:

**A. InkWatchy (as a starting skeleton, stripped down).**
- Pro: hybrid Arduino+ESP-IDF framework means the M5Paper client's largely-Arduino-style C++ is portable in spirit; the display-quirk workarounds (§5) and measured refresh timings are already known-good for this exact panel; the sleep/power hygiene is more mature than anything MicroPatterns would write from scratch; and it is the **incumbent firmware already on this device**, so building on it means the existing InkWatchy install is a stepping stone rather than something to be entirely thrown away.
- Con: it's a large, general-purpose "everything watch" codebase (books, games, multiple watchfaces, accelerometer, RGB LED support, chord combos) — a huge amount of it is dead weight for MicroPatterns' narrow "fetch a script, render a pattern, sleep" use case. It also carries CMake/ESP-IDF project scaffolding and a bespoke partition table/build-flag system that adds onboarding cost. Forking it means either aggressively deleting most of `src/ui/places/*` and `resources/*`, or carrying unused code indefinitely.

**B. Plain Arduino + GxEPD2 directly (mirroring the M5Paper client's own structure).**
- Pro: closest structural match to the existing, working M5Paper codebase (`display_manager`, `network_manager`, `render_controller`, etc. in `M5Paper_MicroPatterns/src/`) — a genuine "port," reusing the same module boundaries and much of the parser/runtime/renderer code unchanged. Minimal, auditable dependency footprint. GxEPD2 (upstream `ZinggJM/GxEPD2`, or the `Szybet/GxEPD2-watchy` fork InkWatchy already vendors and has debugged for this panel) supports the SSD1681/GDEH0154D67 panel directly.
- Con: loses InkWatchy's already-solved sleep/power/pin hygiene and panel-quirk workarounds — those would need to be re-derived or manually ported over (though they're small, well-isolated files: `sleep.cpp`, the three `SCREEN_*` workaround flags in `display.cpp`, `ForceInputs()`).

**C. Official SQFMI Watchy Arduino library.**
- Pro: the "canonical" simplest path, pure Arduino, smallest and most beginner-friendly codebase, best documentation (`watchy.sqfmi.com`).
- Con: per web research, it is comparatively less actively maintained/hardened than InkWatchy for exactly the failure modes that matter here (ghosting, RTC library breakage, board-revision quirks); it was not found to document the same measured refresh timings or power figures used in this report. It doesn't offer anything InkWatchy or plain GxEPD2 don't also offer, and it would still need equivalent bespoke sleep/power work.

**Recommendation: B, with targeted borrowing from InkWatchy — not a fork of InkWatchy wholesale.** The framework-mismatch concern (Arduino vs ESP-IDF) is not actually a blocker (InkWatchy is hybrid, and Arduino APIs work fine inside it) — but the *scope* mismatch is real: InkWatchy is a full "everything watch" application, and a MicroPatterns port is structurally closer to the already-working M5Paper client than to InkWatchy's UI/asset ecosystem. The pragmatic path is: build fresh on plain Arduino + GxEPD2 (or the vendored `GxEPD2-watchy` fork, to inherit its panel-specific fixes for free — this is close to a strict win since it's the same code InkWatchy already debugged), mirroring the M5Paper client's module structure, and **explicitly port over** (not inherit wholesale) InkWatchy's: sleep/`ForceInputs()` pin hygiene, the dirty-flag/full-refresh-counter display pattern, and the three panel workaround flags with their documented rationale. This keeps the codebase small and auditable while not re-discovering hard-won panel quirks from scratch. Since the device currently runs InkWatchy, note for the user: adopting foundation B means the device will be **fully reflashed away from InkWatchy** (not incrementally modified) — worth confirming that's the intended tradeoff versus, say, trying to add a MicroPatterns "place"/mode inside the existing InkWatchy application instead of replacing it outright.

---

## 7. What's currently on the device

- Bootloader header (read-only dump, offset `0x1000`): valid ESP-IDF bootloader, IDF version `5.3.2`, build string `Aug 27 2026 13:30:26`.
- Partition table lives at the InkWatchy-custom offset `0x19000` (not the ESP32 default `0x8000`), consistent with `sdkconfig.Watchy_2`'s `CONFIG_PARTITION_TABLE_OFFSET=0x19000`.
- The local `/Users/julien/Documents/GitHub/InkWatchy` checkout has a `.pio/build/Watchy_2/` output whose `bootloader.bin` (13:30) and `firmware.bin` (13:36) timestamps match the device's bootloader build string to the minute.
- **Conclusion: the device is running (in all likelihood) exactly this local InkWatchy `Watchy_2` build**, built earlier today. Reflashing to any new firmware will overwrite this InkWatchy install; there is no OTA/dual-app-slot safety net (§3) — a full 4MB reflash replaces everything, including the 2MB LittleFS asset partition InkWatchy uses for its resources.

---

## 8. Open questions / needs a flash (or at least a powered-on read) to answer

1. **CP2102N vs CP2104 exact revision boundary** — web sources conflict (general Watchy docs say "CP2104"; this device's bridge was probed as CP2102N). Not resolved from static source alone; low-priority since the sdkconfig/build-timestamp evidence in §1 is already strong.
2. **~~Definitive RTC-chip check (DS3231 vs PCF8563)~~ ANSWERED 2026-09-01: this unit has a PCF8563.** The firmware's own `watchy_rtc.cpp` probes both addresses at boot and reports the result (`MPCON|rtc PCF8563 ok 22:28:25`); it answers on 0x51, not 0x68. Also learned on the device: **the PCF8563's VL bit is clear-only in software** — writing `0x80` to VL_seconds (0x02) leaves bit 7 clear (register read back `0x03`), so only the chip's low-voltage detector can raise it. That is why `rtc-unset` overrides in NVS rather than setting the flag. The original note, for the record: would need an I2C bus scan at runtime (the address ranges differ) — this requires the device to be running firmware that performs a scan, i.e. effectively a flash-and-run of a small diagnostic sketch, or reading InkWatchy's own boot log via serial monitor (non-destructive, just needs the device running — worth trying via serial monitor before any reflash, since InkWatchy's `debugLog`/`initLog()` likely logs the detected RTC type on boot if `DEBUG` is enabled in the currently-flashed build).
3. **Actual deep-sleep current draw** — not measured in this session (would need a multimeter/USB power meter in-line, which is safe/non-destructive but wasn't done here). The `ForceInputs()` "saves 70 uA" comment is the only concrete data point found; a real measurement would tighten the §4 power math substantially.
4. **Whether MicroPatterns render time is pixel-bound or loop-bound** (§2, §4) — this is the single most important unknown for the whole port's feasibility story, and it's answerable today, on the existing M5Paper hardware, by instrumenting `RenderController` (which already logs `renderDuration` via `millis()`, per `M5Paper_MicroPatterns/src/render_controller.cpp:82-93`) across a range of real user scripts and checking whether render time correlates with the *number of drawn pixels* (would shrink ~13x on Watchy) or with *script command/loop count* (would not shrink at all). No Watchy hardware needed for this — it's an M5Paper-side experiment.
5. **Real measured full-battery-life figure for stock InkWatchy on this exact unit** — not found documented with a specific number in README.md or source comments (README only makes qualitative "battery life features" claims); SQFMI's own "weeks" claim for the stock library was not independently re-verified here.

---

## Sources

- Local repo: `/Users/julien/Documents/GitHub/InkWatchy/` (`platformio.ini`, `sdkconfig.defaults`, `sdkconfig.Watchy_2`, `src/defines/condition.h`, `src/defines/config.h`, `src/hardware/display/display.{h,cpp}`, `src/hardware/sleep/sleep.{h,cpp}`, `src/hardware/battery/battery.{h,cpp}`, `src/network/wifi/wifiTask.{h,cpp}`, `src/main.cpp`, `README.md`, `resources/tools/fs/in/partitions.csv`, `.pio/build/Watchy_2/{bootloader,firmware,partitions}.bin`, `.pio/libdeps/Watchy_2/GxEPD2/src/epd/GxEPD2_154_D67.{h,cpp}`)
- Local repo: `/Users/julien/Documents/GitHub/micropatterns/M5Paper_MicroPatterns/src/display_manager.cpp`, `render_controller.cpp`, `micropatterns_command.h`
- Read-only serial probes this session: `esptool.py chip_id`, `flash_id`, `read_flash 0x1000` (bootloader header), `read_flash 0x8000`/`0x9000` (confirmed non-default partition table location)
- [Watchy Hardware docs — watchy.sqfmi.com](https://watchy.sqfmi.com/docs/hardware/)
- [Watchy Battery Life docs — watchy.sqfmi.com](https://watchy.sqfmi.com/docs/battery-life/)
- [Watchy Legacy Getting Started (v1/v1.5/v2) — watchy.sqfmi.com](https://watchy.sqfmi.com/docs/legacy/)
- [espboards.dev Watchy pinout/specs](https://www.espboards.dev/esp32/watchy/)
- [sqfmi/watchy-hardware v1.5 schematic (GitHub)](https://github.com/sqfmi/watchy-hardware/blob/v1.5/WatchySchematic.pdf)
- [Arduino Forum: SSD1680/GDEH0154D67 partial-refresh discussion](https://forum.arduino.cc/t/gxepd2-detailed-analysis-of-fast-partial-update-speed-of-1-54-waveshare/1247087)
- [ZinggJM/GxEPD2 (upstream library)](https://github.com/ZinggJM/GxEPD2)

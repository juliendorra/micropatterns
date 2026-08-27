# `mpharness` — host-side test & benchmark harness for the MicroPatterns C++ renderer

A native (macOS/Linux) build of the **platform-agnostic renderer core** from
`M5Paper_MicroPatterns/src/`, wrapped in a CLI that renders scripts to images,
times each phase, and byte-compares output against stored goldens.

It exists because, until now, the only way to see what the C++ renderer did was
to flash an M5Paper and look at the e-ink panel. That made every optimisation
claim unfalsifiable and every optimisation risky.

---

## What this is NOT

Read this section before quoting any number from this tool.

- **Not a device simulator.** It runs x86-64/ARM code with a host `malloc`, a
  host FPU, no PSRAM, no FreeRTOS scheduler, and no watchdog. It cannot tell you
  how many seconds a script takes on an M5Paper.
- **Not a substitute for on-device timing.** The firmware already logs the real
  device split: `M5Paper_MicroPatterns/src/render_controller.cpp` lines 60–93
  bracket `generateDisplayList()` and `render()` with `millis()` and emit both
  durations at `log_i` on every render. That is ground truth for wall-clock.
  This harness **complements** those logs — it tells you *which code* is hot and
  *whether a change altered the image*; the device logs tell you *how long it
  actually takes*.
- **Not a pixel-accurate model of the e-ink panel.** No waveform, no ghosting,
  no greyscale rendering behaviour. Only the canvas buffer contents.

**Host timings compare ALGORITHMS, not devices.** A change that is 30% faster
here is *probably* faster on device, and the harness will tell you where the
time went — but the ratio will not transfer. Confirm on hardware.

---

## Why `verify` is the important mode

This project already has a working method for choosing between renderer
implementations: build competing paths, prove they produce **identical output**,
then benchmark to pick a winner. The JS emulator carries three selectable
execution paths for exactly this reason, and the git history records the
sequence:

- `d1ea9e7` "Compiler is default" — the compiler path led first.
- `82906d5` / `01ded93` / `e4d18d9` — the display-list path was built and matured.
- `d427b02` **"All path gives same result"** — output equivalence established.
- `68d843e` "Default to display list" — display list won the benchmark and
  became the default (`micropatterns_emulator/index.html:103`,
  `value="displayList" checked`).

The C++ display-list architecture is therefore the **benchmark-winning design**,
not leftover technical debt.

`mpharness verify` is the automated form of `d427b02`. It is the equivalence
gate that must pass **before any benchmark comparison is meaningful**:

> An optimisation that changes the output is not faster. It is different.

Run `verify` first. Only then trust a `compare` delta.

### Cross-path comparison

`mpharness compare-paths A B` renders the same corpus through two render paths
and byte-compares them **against each other**, not against stored goldens.

**Status: seam only, one path implemented.** The C++ side currently ships a
single path (`displaylist`), so `compare-paths displaylist displaylist` is a
self-check. The seam is real and small: `src/render_path.h` defines
`RenderPath`, `src/render_path.cpp` implements `DisplayListPath` and a
`makeRenderPath()` registry. Adding a second path means subclassing `RenderPath`
and adding one line to the registry — `compare-paths` and `bench --path` then
work on it with no further changes. A full second implementation was out of
scope for this harness and would have been costly; the seam was cheap.

---

## Build

Nothing but a C++17 compiler and `make`. No external libraries, no package
manager, no image library.

```sh
cd tools/host_harness
make
```

Produces `build/mpharness`. Tested with Apple clang on macOS (Darwin 23.4).

Logging from the shimmed `log_d/log_i/log_w/log_e/log_v` macros is **compiled
out by default** (`HOST_LOG_LEVEL=0`) so it cannot distort benchmark timings.
To see the core's own log lines:

```sh
make clean && make LOGLEVEL=3   # 1=error 2=warn 3=info 4=debug 5=verbose
```

---

## Modes

```sh
build/mpharness list                # corpus, seeds, available render paths
build/mpharness render <script.mp> [--counter N --hour H --minute M --second S]
                                    [--width W --height H] [--out FILE] [--path NAME]
build/mpharness verify              # golden-image equivalence gate
build/mpharness bake                # REGENERATE goldens — see warning below
build/mpharness bench [--reps N] [--json FILE]
build/mpharness compare a.json b.json
build/mpharness compare-paths A B
```

Make targets wrap the common ones: `make list`, `make verify`, `make bench`,
`make bake`.

### `render`

Parses, generates the display list, rasterizes, writes a PNG (or PGM if the
`--out` path ends in `.pgm`), and prints the **phase split** plus counters:

```
PHASE TIMINGS (host, single run)
  parse                0.252 ms     0.1%
  displaylist        479.999 ms    97.1%
  rasterize           14.019 ms     2.8%
  ------------------------------------
  total              494.370 ms

COUNTERS
  display list items            20737
  rendered items                   85
  culled off-screen             20652
  culled by occlusion               0
  pixels skipped (occ.map)     207648
  non-white pixels             207648   (coverage proxy, not draw calls)
```

Counter provenance:

| counter | source |
|---|---|
| display list items | `MicroPatternsRuntime::getDisplayList().size()` |
| rendered items | `DisplayListRenderer::getRenderedItems()` |
| culled off-screen | `DisplayListRenderer::getCulledOffScreen()` |
| culled by occlusion | `DisplayListRenderer::getCulledByOcclusion()` |
| pixels skipped (occ. map) | `MicroPatternsDrawing::getOverdrawSkippedPixelsCount()` |
| non-white pixels | **derived by the harness** — a canvas scan, *not* a draw-call count |

The last row is honestly labelled: the drawing layer does not track how many
`rawPixel()` calls it made, so "pixels touched" is not directly observable
without adding a counter to firmware. `non-white pixels` is a coverage proxy.

### Determinism

Renders are deterministic: same script + same `(counter, hour, minute, second)`
gives a byte-identical image. Verified by rendering the same case repeatedly and
comparing MD5s — identical across runs. There is no RNG in the DSL; all
variation comes from the seed.

### `verify` / `bake`

`verify` re-renders every corpus script at every fixed seed and byte-compares to
`golden/<script>__<seed>.pgm`. On failure it prints the number of differing
pixels, the coordinates of the first difference, and writes a diff image to
`out/diff_<case>.png` (black = differing pixel).

```
  FAIL  artdeco_default__c0_123456   500 differing px (0.0965%), first at (785,20), diff -> out/diff_artdeco_default__c0_123456.png
```

> ### When NOT to regenerate goldens
>
> **Regenerating goldens to make a failing test pass defeats the entire point of
> the harness.** If `verify` fails after your change, the default assumption is
> that your change altered the rendered image — i.e. it is not a pure
> optimisation. Look at the diff image first.
>
> Only run `bake` when the output change is **intended, understood, and
> reviewed** (a genuine bug fix in the renderer, a deliberate DSL semantic
> change, or a new corpus script). Commit the regenerated goldens in the same
> commit as the change that caused them, and say why in the message.

Goldens are stored as binary PGM (P5). PGM, not PNG, because `verify` needs to
read them back for pixel-level diffing and PGM needs no inflate implementation.
`render --out foo.png` writes a real PNG (self-contained writer: stored-deflate
zlib stream, hand-rolled CRC32/Adler32) for viewing.

### `bench` / `compare`

`bench` runs the whole corpus `--reps` times and prints per-case
min/median/mean, a per-case phase split, and an aggregate. `--json FILE` saves
the run; `compare a.json b.json` prints per-case, per-phase percentage deltas
plus an aggregate.

Run-to-run noise on an unquiesced laptop is real: repeated identical runs of the
small cases have shown up to ±18% swing in the median. Treat anything under
about 20% on a small case as noise; raise `--reps`, prefer `min` over `median`
for micro-comparisons, and close other applications.

---

## Corpus

`corpus/*.mp` are the real scripts found in this repo, extracted verbatim:

| file | origin |
|---|---|
| `city.mp` | `micropatterns_server/local-s3-storage/scripts/7qpkkx4dys/city.json` (`content` field) |
| `artdeco_default.mp` | `ScriptManager::DEFAULT_SCRIPT_CONTENT` in `M5Paper_MicroPatterns/src/script_manager.cpp` — the firmware's built-in fallback |
| `emulator_welcome.mp` | the shipped example in `micropatterns_emulator/index.html` (`#scriptInput` textarea) |

`cases/`, `patterns/`, `src/` and `tools/` at repo root are empty, and the
`micropatterns-api.deno.dev` scripts are not checked in, so this is the complete
set of scripts actually present in the tree. Add more by dropping `.mp` files in
`corpus/` and running `make bake`.

Seeds (fixed, in `src/main.cpp`):

| tag | counter | time |
|---|---|---|
| `c0_123456` | 0 | 12:34:56 |
| `c7_000000` | 7 | 00:00:00 |
| `c42_235959` | 42 | 23:59:59 |

Canvas defaults to 960×540 (M5Paper landscape).

---

## The shim, and its fidelity limits

`shim/` provides host stand-ins for the Arduino/ESP-IDF headers the core
includes. They are on the include path **ahead of nothing** — the firmware
sources are compiled verbatim from `M5Paper_MicroPatterns/src/`.

| shim | provides | fidelity caveat |
|---|---|---|
| `Arduino.h` | `String`, `yield()`, `byte`, `PI` | see below |
| `M5EPD.h` | `M5EPD_Canvas` (packed 4bpp buffer), `m5epd_update_mode_t` | no EPD hardware, no waveform, no partial-update semantics |
| `esp32-hal-log.h` | `log_v/d/i/w/e`, verbosity switchable, **off by default** | real ESP logging costs UART time; host logging does not |
| `esp_task_wdt.h` | no-op `esp_task_wdt_reset()` | on device this is a real (cheap) call; here it is free |
| `freertos/FreeRTOS.h`, `freertos/semphr.h` | `TickType_t`, `SemaphoreHandle_t`, no-op mutex ops | single-threaded; no contention, no priority inversion |

### `String`

The shim's `String` is backed by `std::string` and reproduces the semantics the
parser and runtime rely on: value copy, implicit construction from
`int`/`char`/`const char*`, `+` and `+=`, `==`/`!=`/`<` (so it works as a
`std::map` key), `length`, `c_str`, `substring` (with Arduino's index clamping
and swap-if-reversed behaviour), `indexOf`/`lastIndexOf` returning `-1`,
`startsWith`/`endsWith`, `trim`, `toUpperCase`/`toLowerCase`, `toInt`
(strtol-based, `0` on failure), `charAt`/`operator[]` returning `'\0'` out of
range, and `begin()`/`end()` (ESP32's `WString.h` exposes these, and
`micropatterns_parser.cpp:511` range-for's over a `String`).

**Differences that matter for benchmarking:**

- **Allocator.** `std::string` on macOS has a 22-byte small-string optimisation
  and uses the host `malloc`. ESP32's `String` always heap-allocates via
  `realloc` on a much slower allocator, and can land in PSRAM. Any measurement
  dominated by `String` churn (and the display-list phase is — see below) will
  have a *different shape* on device: probably worse, not better.
- **Growth policy.** ESP32 `String::changeBuffer` grows in 16-byte steps;
  `std::string` roughly doubles. Concatenation-heavy code is cheaper here.
- **No `StringSumHelper`.** Arduino chains `a + b + c` through a helper type;
  the shim uses plain `operator+` returning a temporary. Same result, different
  temporary count.
- **`operator[]` on out-of-range** returns `'\0'` in both, but the shim does not
  reproduce Arduino's invalid-object (`buffer == NULL`) state after a failed
  allocation.

### Float

Host doubles/floats are IEEE-754 with x86-64/ARM64 rounding; the ESP32-D0WD FPU
is single-precision IEEE-754 but `sinf`/`cosf`/`sqrtf` come from a different
libm. The transform math in `matrix_utils.cpp` and `micropatterns_drawing.cpp`
uses `float` throughout, so **the golden images are not guaranteed to be
bit-identical to what the device produces.** The goldens are a regression baseline
for *host-to-host* comparison, not a device reference. (In practice the DSL
quantises to integer pixel coordinates, so small ULP differences rarely change a
pixel — but "rarely" is not "never", and this has not been checked against
hardware.)

### Memory system

No PSRAM latency, no 320 KB DRAM ceiling, no instruction-cache pressure from a
flash-resident `.text`. On device, the display-list `std::vector<DisplayListItem>`
(each item carries two `std::map`s and several `String`s) is a far bigger deal
than the host suggests. Do not use host memory behaviour to argue that an
allocation is cheap.

---

## Firmware-source edits

The whole point of this harness is not to perturb what it measures. **Exactly
one** firmware file was modified, additively:

1. **`M5Paper_MicroPatterns/src/display_list_renderer.h`** — added an inline
   getter:

   ```cpp
   unsigned int getOverdrawSkippedPixels() const { return _drawing.getOverdrawSkippedPixelsCount(); }
   ```

   `MicroPatternsDrawing::getOverdrawSkippedPixelsCount()` was already public and
   already logged by `DisplayListRenderer::render()`, but `_drawing` is a private
   member so the count was unreachable from outside. The getter is never called
   during rendering and changes no behaviour. Without it the harness could not
   report "pixels skipped by the occupancy map", which is one of the required
   counters.

No `.cpp` in the firmware was touched. No refactoring, no cleanup, no
reformatting.

`display_manager.h` is used **verbatim**: it parses on the host once
`M5EPD.h` and the freertos headers are shimmed. The method *bodies* live in
`src/host_display_manager.cpp`, which implements only what
`DisplayListRenderer` actually calls (`getCanvas`, `getWidth`, `getHeight`,
`initializeEPD`, ctor/dtor, and the lock stubs). Everything else is deliberately
left undefined — if a future change makes the renderer depend on more of
`DisplayManager`, this will fail to **link**, making the new dependency visible
rather than silently emulated.

---

## Profiling (macOS)

No profiler is required to build or run the harness. If you want one:

`/usr/bin/sample` ships with macOS and needs no Xcode:

```sh
build/mpharness bench --corpus corpus --reps 40 >/dev/null &
sample $! 2 -mayDie -f /tmp/mp.sample
awk '/Sort by top of stack/,0' /tmp/mp.sample | head -20
```

For a call-tree view, Instruments' Time Profiler works on `build/mpharness`
directly (the Makefile keeps `-g`). `make profile` rebuilds with
`-fno-omit-frame-pointer` for cleaner stacks. `xcrun xctrace` needs full Xcode,
not just Command Line Tools — on a CLT-only machine `xcrun -f xctrace` fails
while `/usr/bin/xctrace` is a stub, so `sample` is the reliable option.

Caveat: host profiles attribute a lot of time to `libsystem_malloc` and
`_platform_memcmp` because of the `std::string`-backed `String` shim and
`std::map<String,…>` lookups. The *shape* (which renderer functions dominate) is
informative; the absolute allocator split is not transferable to ESP32.

---

## Layout

```
tools/host_harness/
  Makefile
  README.md              (this file)
  shim/                  host stand-ins for Arduino / M5EPD / ESP-IDF headers
    Arduino.h  M5EPD.h  esp32-hal-log.h  esp_task_wdt.h  freertos/{FreeRTOS,semphr}.h
  src/
    host_display_manager.{h,cpp}   host bodies for the firmware DisplayManager
    image_io.{h,cpp}               dependency-free PNG writer, PGM read/write
    render_path.{h,cpp}            RenderPath seam + the displaylist path
    main.cpp                       CLI
  corpus/*.mp            real scripts extracted from the repo
  golden/*.pgm           committed golden images
  build/                 (generated, gitignored)
  out/                   (generated, gitignored)
```

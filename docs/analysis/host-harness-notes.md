# Host harness — design notes, dead ends, and the measured phase split

Companion to `tools/host_harness/README.md` (which is the user-facing manual).
This file records *how* the harness was built, what did **not** work on the way,
and the first real numbers it produced.

Written 2026-08-27. Host: macOS (Darwin 23.4.0), Apple clang, `-O2`.

---

## 1. Headline result: the phase split

**It builds and runs, natively, with one additive one-line firmware edit.**

The single most useful thing the harness produced is the answer to "how does the
render time divide between interpretation and rasterization?" — and the answer
is **it depends entirely on the script, by roughly forty to one**.

Real console output, `build/mpharness bench --corpus corpus --reps 9`:

```
BENCH: 3 scripts x 3 seeds x 9 reps, path=displaylist, canvas 960x540

case                              parse   displist     raster      total    items
----------------------------------------------------------------------------------
artdeco_default|c0_123456         0.086      0.034      7.327      7.545        7
artdeco_default|c7_000000         0.062      0.031      7.318      7.421        7
artdeco_default|c42_235959        0.060      0.045     10.579     10.675       13
city|c0_123456                    0.265    502.185     13.028    515.724    20737
city|c7_000000                    0.280    539.316     13.925    554.324    20737
city|c42_235959                   0.299    562.472     15.166    578.339    20737
emulator_welcome|c0_123456        0.089      0.195      9.054      9.352      103
emulator_welcome|c7_000000        0.091      0.191      9.556      9.834      103
emulator_welcome|c42_235959       0.093      0.237      9.526      9.844      103
----------------------------------------------------------------------------------
SUM of medians                    1.327   1604.708     95.479   1703.058
PHASE SPLIT                        0.1%      94.2%       5.6%

PER-CASE PHASE SPLIT (% of that case's median total)
case                              parse   displist     raster
------------------------------------------------------------
artdeco_default|c0_123456          1.1%       0.5%      97.1%
artdeco_default|c7_000000          0.8%       0.4%      98.6%
artdeco_default|c42_235959         0.6%       0.4%      99.1%
city|c0_123456                     0.1%      97.4%       2.5%
city|c7_000000                     0.1%      97.3%       2.5%
city|c42_235959                    0.1%      97.3%       2.6%
emulator_welcome|c0_123456         1.0%       2.1%      96.8%
emulator_welcome|c7_000000         0.9%       1.9%      97.2%
emulator_welcome|c42_235959        0.9%       2.4%      96.8%
```

Three findings, in order of importance:

1. **Parsing is negligible. Always.** 0.06–0.30 ms, ≤1.1% of any case, ≤0.1% of
   the aggregate. Optimising the parser is not where the seconds are.

2. **There is no single phase split — it is bimodal, and it tracks display-list
   size.** For scripts that emit a handful of items (`artdeco_default`: 7 items,
   `emulator_welcome`: 103 items) rasterization is **97–99%** of the time.
   For a script that emits many (`city`: 20 737 items) display-list generation
   is **97%** and rasterization is 2.5%.

   Anyone claiming "the bottleneck is the rasterizer" or "the bottleneck is the
   interpreter" is right about *some* scripts and wrong about others. Any
   optimisation proposal should say which regime it targets.

3. **`city` culls 20 652 of its 20 737 items as off-screen — 99.6%** — yet still
   spends ~500 ms building them. The display list is being fully materialised
   (each `DisplayListItem` carries two `std::map`s and several `String`s) before
   anything gets to throw 99.6% of it away. That is the shape of the cost, from
   `render`:

   ```
     display list items            20737
     rendered items                   85
     culled off-screen             20652
     culled by occlusion               0
     pixels skipped (occ.map)     207648
   ```

   Diagnosing or fixing that is explicitly **not** this harness's job (other
   agents own performance work). It is recorded here because the harness is what
   made it visible, and because it is a candidate the phase split points straight at.

A supporting `/usr/bin/sample` profile of the same workload (top-of-stack, so
allocator-heavy — read the *shape*, not the absolute split):

```
Sort by top of stack, same collapsed (when >= 5):
        _platform_memcmp  (in libsystem_platform.dylib)        221
        free_tiny  (in libsystem_malloc.dylib)        120
        tiny_free_no_lock  (in libsystem_malloc.dylib)        115
        tiny_malloc_from_free_list  (in libsystem_malloc.dylib)        110
        MicroPatternsRuntime::resolveValue(ParamValue const&, int, int)         104
        tiny_free_list_add_ptr  (in libsystem_malloc.dylib)         85
        tiny_malloc_should_clear  (in libsystem_malloc.dylib)         75
        DYLD-STUB$$memcmp  (in mpharness)         64
        _malloc_zone_malloc  (in libsystem_malloc.dylib)         58
        _free  (in libsystem_malloc.dylib)         53
        std::vector<ParamValue>::__push_back_slow_path<ParamValue const&>         52
        MicroPatternsRuntime::evaluateExpression(...)         47
        MicroPatternsRuntime::processCommandForDisplayList(...)         43
        MicroPatternsRuntime::evaluateCondition(...)         41
        std::map<String, int>::at(String const&)         40
```

`memcmp` + `malloc/free` + `std::map<String,int>::at` dominating is consistent
with `String`-keyed map lookups inside the display-list generation loop. Note
the caveat in §5: the host `String` shim uses `std::string`, so the *allocator*
attribution here is not transferable to the ESP32 — but the fact that the hot
frames are map lookups and expression evaluation is.

### What these numbers are not

Host x86-64/ARM64 timings, `-O2`, host `malloc`, host libm, no PSRAM, no cache
pressure from flash-resident code. **These are not ESP32 milliseconds.** The
device already logs its own real generation-vs-rasterization split via `millis()`
in `M5Paper_MicroPatterns/src/render_controller.cpp:60-93`; the harness
complements those logs, it does not replace them. Use the harness for *relative*
comparison of algorithmic changes and to locate hotspots.

---

## 2. Design

```
tools/host_harness/
  Makefile                    dependency-free; system clang + make only
  shim/                       Arduino.h  M5EPD.h  esp32-hal-log.h
                              esp_task_wdt.h  freertos/{FreeRTOS,semphr}.h
  src/host_display_manager    host bodies for the firmware's DisplayManager class
  src/image_io                self-contained PNG writer + PGM read/write
  src/render_path             RenderPath seam; DisplayListPath implementation
  src/main                    CLI: render / verify / bake / bench / compare /
                                   compare-paths / list
  corpus/*.mp                 real scripts extracted from the repo
  golden/*.pgm                committed goldens (3 scripts x 3 seeds = 9)
```

The six platform-agnostic firmware sources
(`micropatterns_parser`, `micropatterns_runtime`, `micropatterns_drawing`,
`display_list_renderer`, `occlusion_buffer`, `matrix_utils`) are compiled
**verbatim** from `M5Paper_MicroPatterns/src/`. `DisplayListPath::run()` mirrors
`RenderController::renderScript()` step for step so the phase boundaries match
the ones the device times.

Three principles shaped it:

- **The instrument must not perturb what it measures.** Logging compiles out at
  `HOST_LOG_LEVEL=0` (the default); `esp_task_wdt_reset()` and `yield()` are
  genuine no-ops; the one firmware getter added is never called during a render.
- **Prefer include-path shims to firmware edits.** One additive line was
  unavoidable (§4); everything else was solved with headers.
- **Equivalence before speed.** `verify` is the automated form of commit
  `d427b02` "All path gives same result" — the project's own established
  practice of proving competing implementations produce identical output before
  benchmarking them (the JS emulator's interpreter / compiler / display-list
  paths; `68d843e` "Default to display list" is the benchmark that followed).
  A change that alters the image is not faster, it is different.

`compare-paths` exists as a **seam, not a second implementation**. `RenderPath`
+ `makeRenderPath()` mean a future alternative C++ path can be diffed against
the display-list path with a subclass and one registry line. Building a full
second C++ path was out of scope.

---

## 3. Dead ends and false paths

Recorded because the "what didn't work" is the part that is expensive to
rediscover.

### 3.1 Shimming `display_manager.h` by putting a copy in `shim/` — would not have worked

The obvious plan was to write a stripped-down `shim/display_manager.h` and let
the include path pick it up. **This cannot work.**
`display_list_renderer.h` uses a *quoted* include (`#include "display_manager.h"`),
and quoted includes search the including file's own directory first. The
firmware's `display_manager.h` would always win over anything in `shim/`,
regardless of `-I` order.

The route that *did* work is better anyway: shim the headers *underneath* it
(`M5EPD.h`, `freertos/FreeRTOS.h`, `freertos/semphr.h`), let the real
`display_manager.h` be included verbatim, and supply only the method **bodies**
in `src/host_display_manager.cpp`. Implementing only the methods the renderer
actually calls means an unimplemented method becomes a **link error**, so any
new dependency on `DisplayManager` shows up loudly instead of being silently
emulated.

Same trap applies to every quoted include in the firmware tree — worth knowing
before anyone tries to shim `micropatterns_command.h` or similar.

### 3.2 Assuming Arduino's `String` has no iterators

First compile failure, and the only one:

```
micropatterns_parser.cpp:511:17: error: invalid range expression of type 'String';
    no viable 'begin' function available
    for (char c : outVarName) {
```

The tempting fix was to edit `micropatterns_parser.cpp` to index the string
manually. That would have been **wrong** — checking the actual ESP32 core at
`~/.platformio/packages/framework-arduinoespressif32/cores/esp32/WString.h:257-266`
shows `String` really does expose `char *begin()` / `char *end()`. The firmware
code is legal Arduino; the shim was incomplete. Added `begin()`/`end()` to the
shim, no firmware edit.

General lesson that saved further edits: **when the shim fails to compile
firmware code, check the real `WString.h` before assuming the firmware is at
fault.** The shim is the thing that is wrong by default.

### 3.3 Reaching the overdraw counter — two hacks rejected

`MicroPatternsDrawing::getOverdrawSkippedPixelsCount()` is public, but
`DisplayListRenderer::_drawing` is private, so the count was unreachable.
Considered and rejected:

- **`#define private public` before the include.** Zero firmware edits, but it
  is UB, ABI-fragile, and would silently break on any layout change. Rejected.
- **Scraping it out of the `log_i` line in `render()`.** Also zero edits, but it
  requires building with `HOST_LOG_LEVEL=3`, which re-enables `log_w` inside the
  per-item render loop — i.e. it distorts the exact measurement it is there to
  support. Rejected.

Settled on a one-line additive inline getter (§4). Reporting this counter is a
stated requirement of the harness, and an accessor that is never called during a
render is the least-harm option.

### 3.4 "Pixels touched" is not observable, and was not faked

The brief asked for "pixels touched". `MicroPatternsDrawing` tracks pixels
*skipped* (`_overdrawSkippedPixels`) but does **not** count committed
`rawPixel()` calls, and adding such a counter would mean putting an increment
inside the innermost drawing loop — the one thing an instrument must not do to
a benchmark. So the harness reports:

- `pixels skipped (occ.map)` — the real, existing counter, and
- `non-white pixels` — a canvas scan, **explicitly labelled a coverage proxy,
  not a draw-call count**, in both the CLI output and the README.

No invented number is presented as if the renderer produced it.

### 3.5 PNG goldens would have required an inflate implementation

Goldens were initially going to be PNG. But `verify` has to *read* the golden
back to diff it pixel-by-pixel, and reading PNG means implementing inflate —
several hundred lines of decompressor to maintain in a test harness, or an
external dependency the brief forbids. Switched goldens to binary PGM (P5):
header plus raw bytes, trivially written, read, and byte-compared. `render`
still writes real PNG for viewing (writer uses *stored*, uncompressed deflate
blocks — larger files, ~50 lines, no dependency).

### 3.6 Canvas polarity

First `city.png` came out inverted. The DSL/canvas convention is `0 = white,
15 = black` (see `DRAWING_COLOR_WHITE = 0` / `DRAWING_COLOR_BLACK = 15` in
`micropatterns_drawing.h`), which is the opposite of an 8-bit grayscale image
where 0 is black. `DisplayListPath::run()` maps `v -> 255 - v*17` so ink reads
as black. This is a harness-side presentation choice; it does not touch the
renderer, and it applies identically to goldens and fresh renders so it cannot
mask a regression.

### 3.7 Minor: `argv[0]`-relative root

The binary lives in `build/`, but `corpus/`, `golden/` and `out/` are siblings
of `build/`. Deriving the root as `dirname(argv[0]) + "/.."` worked but printed
paths like `./build/../out/diff_x.png`. Collapsed the trailing `/..` for
readability. Explicit `--corpus`/`--golden`/`--out` always override.

### 3.8 Non-issues, checked rather than assumed

- **Determinism**: rendered `city.mp` three times, MD5-identical every time; the
  other two scripts byte-compare identical across runs. There is no RNG in the
  DSL — all variation comes from `$COUNTER`/`$HOUR`/`$MINUTE`/`$SECOND`.
  **No non-determinism was observed.**
- **`verify` failure path**: exercised by corrupting 500 bytes of a golden. It
  correctly reported `500 differing px (0.0965%), first at (785,20)` and wrote a
  diff image. Not just a green-path claim.
- **Compiler warnings**: the firmware sources produce three pre-existing
  warnings under `-Wall` (`isElse` and `isNegative` set-but-unused in the
  parser; a ctor member-init reorder in the runtime). **Left untouched** —
  cleaning them is another agent's territory and would perturb the baseline.

---

## 4. Firmware edits — the complete list

One file, one additive line.

**`M5Paper_MicroPatterns/src/display_list_renderer.h`**, in the public stats
block:

```cpp
unsigned int getOverdrawSkippedPixels() const { return _drawing.getOverdrawSkippedPixelsCount(); }
```

Rationale in §3.3. It changes no behaviour, is not called during rendering, and
exposes a value `render()` already computes and already logs.

Nothing else in `M5Paper_MicroPatterns/` was modified. No `.cpp` was touched. No
reformatting, no refactoring, no warning cleanup.

---

## 5. Shim fidelity — what the numbers cannot tell you

Full table in the README; the ones that matter for interpreting §1:

- **`String` is `std::string`-backed.** macOS `std::string` has a 22-byte
  small-string optimisation and roughly doubles on growth; ESP32's `String`
  always heap-allocates and grows in 16-byte steps on a slower allocator, with
  PSRAM in play. Display-list generation is `String`-churn-heavy, so its cost is
  likely **understated** here relative to device — the 97% figure for `city` is
  if anything conservative.
- **Float.** Host libm vs ESP32 libm; `matrix_utils.cpp` and the drawing code are
  `float` throughout. The goldens are a **host-to-host** regression baseline and
  are *not* verified bit-identical to device output. The DSL quantises to integer
  pixels so ULP differences rarely surface, but "rarely" is not "never" and this
  has not been checked against hardware.
- **No PSRAM latency, no DRAM ceiling, no i-cache pressure.** Do not use host
  memory behaviour to argue an allocation is cheap on device.
- **No FreeRTOS, no watchdog, single-threaded.** The `yield()` /
  `esp_task_wdt_reset()` calls sprinkled through the drawing and runtime loops
  cost nothing here and something (small) on device.

---

## 6. Known gaps

- **Corpus is three scripts.** That is every MicroPatterns script actually
  checked into the tree (`cases/`, `patterns/`, `src/`, `tools/` at repo root are
  empty directories; the `micropatterns-api.deno.dev` library is not vendored).
  Three scripts is enough to show the split is bimodal but not enough to
  characterise the distribution. Dropping more `.mp` files into `corpus/` and
  running `make bake` is the whole extension procedure.
- **One render path.** `compare-paths` is a working seam with a self-check, not
  a real cross-path comparison. It becomes useful the moment a second C++ path
  exists.
- **No device correlation.** Nobody has yet rendered the same corpus at the same
  seeds on hardware and compared either the images or the ratios. Until someone
  does, the claim "host relative timings track device relative timings" is an
  assumption. The device's existing `render_controller.cpp` `log_i` lines make
  the timing half of that check cheap to run.
- **"Pixels touched" remains unmeasured** (§3.4) — only "skipped" is real.
- **Bench noise.** On an unquiesced laptop, repeated identical runs of the small
  cases have swung ±18% in the median. Under ~20% on a small case is noise; use
  more reps and prefer `min`.
- **No CI wiring.** `make verify` returns a non-zero exit status on failure and
  is ready to be a CI gate; nothing has been added to `.github/workflows/` yet.

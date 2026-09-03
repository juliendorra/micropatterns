# WASM device-constraint simulator development journal

**Status:** complete

**Started:** 2026-09-02
**Objective:** make the browser's C++/WASM renderer reproduce the allocation
constraints and failure envelope of the Watchy and M5Paper, not only their
rendered pixels.

This file is the handoff point if work is interrupted. Keep the current state,
decisions, commands, measurements, and next actions accurate after every
meaningful milestone.

## Non-negotiable target

The final path is the high-fidelity implementation. Allocation accounting and
synthetic allocator tests are observability and verification around that path;
they are not separate approximate emulators to ship.

Each device build must model:

- its version-matched Arduino `String` layout and growth;
- its version-matched ESP-IDF TLSF/multi-heap allocation behavior;
- separate capability-aware internal DRAM and PSRAM regions where applicable;
- the actual lifetime of source text, parser, runtime, display list, renderer,
  occupancy map, and framebuffer;
- state-dependent radio reservations (radios off, BLE, Wi-Fi/TLS);
- fatal STL allocation behavior as a structured "device would reboot" result;
- allocation phase, request size, total free, largest block, heap capability,
  and source subsystem in every failure report.

Browser-only allocations, the 8-bit output image, and the JavaScript/WASM bridge
must not consume simulated device memory.

## Repository state at start

- Branch `main`, initially clean apart from user-owned untracked files:
  `examples/scripts/seascape.mp`, `seascape2.mp`, and `seascape3.mp`. Do not add,
  modify, or delete them.
- Existing WASM renderer compiles the six shared firmware core files and uses a
  host shim: `tools/host_harness/wasm/build.sh`.
- Existing build is unconstrained: `ALLOW_MEMORY_GROWTH=1`, `INITIAL_MEMORY=32MB`.
- `mp_wasm.cpp` retains an 8-bit `width * height` output vector in WASM memory.
  That buffer is browser output, not a device allocation.
- `device_renderer.js` loads one shared module and reports render counters and
  host timings. There is no device profile or memory report yet.

## Exact toolchain facts established

### Watchy

- `Watchy_MicroPatterns/platformio.ini` pins pioarduino 53.03.13.
- Built package reports Arduino ESP32 **3.1.3** and ESP-IDF **5.3.x**.
- Installed ESP-IDF source package is **5.3.2** at
  `~/.platformio/packages/framework-espidf`.
- Exact Arduino 3.1.3 source is installed at
  `~/.platformio/packages/framework-arduinoespressif32-src-702d0f93023d86e22d8ef62aa333f0b7`.
- No PSRAM. Before parsing on the connected firmware, measured internal heap is
  about **177,476 bytes free, 110,580-byte largest block**.
- Watchy loads source into an Arduino `String`, parses it, then explicitly clears
  the source before display-list generation (`main.cpp`, `renderScript`).

### M5Paper

- `M5Paper_MicroPatterns/platformio.ini` pins Arduino ESP32 **2.0.4**
  (`framework-arduinoespressif32@3.20004.0`).
- Its bundled `esp_idf_version.h` reports ESP-IDF **4.4.1**.
- It has internal DRAM plus PSRAM. The M5EPD framebuffer and transfer packing
  buffer explicitly prefer PSRAM.
- `RenderController` owns its parser, runtime, and renderer across calls. Source
  text remains resident through the render. This lifecycle must not be replaced
  with the Watchy's one-shot local-object lifecycle.

### Important correction to earlier host documentation

Both installed Arduino versions have small-string optimization. The Arduino
3.1.3 `changeBuffer()` rounds heap capacities in 16-byte units after the inline
buffer. A default empty string becomes inline storage; `c_str() == nullptr`
describes an invalid/failed-allocation state, not every normal empty string.
The current `std::string` shim therefore differs in allocation count, object
layout, capacity growth, failure state, and potentially inline capacity.

## Architectural decisions

1. Build a separate constrained WASM module for each device/toolchain. A single
   generic "ESP32" module would erase the version and lifecycle differences.
2. Keep the canonical unconstrained WASM path as the pixel-parity oracle.
3. Vendor the required versioned compatibility sources with their licenses;
   builds must not depend on paths under one developer's home directory.
4. Use the version-matched ESP-IDF TLSF implementation for allocation placement
   and coalescing. Add a small capability-routing layer matching the relevant
   `heap_caps` policy for internal DRAM and PSRAM.
5. Keep browser support memory separate. During firmware phases, global C++
   `new/delete` route STL nodes and vectors to the active device heap; pointer
   range determines the correct free path even after the phase ends. Arduino
   `String` routes its `malloc/realloc/free` through the same layer.
6. Simulate a fatal STL OOM at the exact failed request but catch it only at the
   outer WASM boundary to return diagnostics. Discard/recreate the constrained
   session after such a result, matching the device reboot boundary.
7. Initial heap state is a calibration input. Nominal RAM size or only
   `free/largest` is insufficient for exact fragmentation; collect real free
   block layouts or allocation traces at phase boundaries.

## Findings that affect implementation

- The current host render path constructs the canvas before parsing and copies
  it to an 8-bit result after rasterization. Those allocations must remain in
  support memory. The corresponding real framebuffer cost is already reflected
  if a device profile is seeded from a measured `before parse` heap snapshot.
- The current host call creates a temporary Arduino shim `String` only for
  `parser.parse()`. This happens to resemble Watchy's source lifetime but not
  M5Paper's.
- Watchy Seascape 2 fails after parsing when the display-list `std::vector`
  requests a larger contiguous block. The observed post-parse state is about
  **21,924 bytes free, 17,396-byte largest block**.
- City 2 produces only 2--37 display-list items in the measured seed sweep. Its
  instability is associated with parser/internal-DRAM pressure, historically
  amplified by BLE, not with a large display list.

## Work log

### 2026-09-02 — audit started

Inspected both PlatformIO pins, installed Arduino sources, ESP-IDF 5.3.2
`multi_heap`/TLSF sources, the current host `String` shim, WASM wrapper, host
render path, canvas shim, and firmware object lifetimes. No implementation files
changed yet beyond creating this journal.

### 2026-09-02 — exact allocator sources vendored and first wasm32 proof

Vendored the upstream heap sources from ESP-IDF tags/releases 4.4.1 and 5.3.2
under `tools/host_harness/device_compat/`. Both firmware sdkconfigs enable light
heap poisoning, so the compatibility builds must compile `multi_heap_poisoning.c`
as well as multi-heap/TLSF; omitting it would miss the canary/header overhead in
every allocation.

Added the first device allocator interface, fixed-memory telemetry structures,
capability routing, radio-state reservations, and an allocator smoke test under
`tools/host_harness/device_constraints/`. The exact IDF 5.3.2 sources compile and
run as wasm32 with Emscripten 4.0.10.

First Watchy smoke result before final region calibration:

```text
profile=watchy2 idf=5.3.2 initial_free=183576 largest=110580
fragmented_free=180472 fragmented_largest=106484 failure=183577
```

The first raw region already produces the hardware's exact 110,580-byte largest
block. Aggregate free was 6,100 bytes above the measured 177,476, so the second
raw region was reduced to 64,488 bytes. The reproducible smoke test now reports
the exact measured starting pair:

```text
profile=watchy2 idf=5.3.2 initial_free=177476 largest=110580
fragmented_free=174372 fragmented_largest=106484 failure=177477
profile=m5paper idf=4.4.1 initial_free=193388 largest=192500
fragmented_free=190292 fragmented_largest=184308 failure=193389
```

The full IDF checkout initially attempted for 4.4.1 exhausted temporary disk
space during examples extraction. It was removed and replaced by a 2.4 MB sparse
checkout of only `components/heap`; that temporary checkout was also removed
after the exact source files were copied. No repository data was affected.

### 2026-09-02 — exact Arduino String integration

Vendored the exact Arduino-ESP32 3.1.3 and 2.0.4 `WString.{h,cpp}` and
`stdlib_noniso.{h,c}` sources. The constrained build compiles the appropriate
version per profile. A narrowly force-included redirect sends only WString's
`malloc/realloc/free` calls to the simulated multi-heap; all other firmware
C++ allocations use the global `new/delete` redirect while a device phase is
active. The two newlib-only `itoa/utoa` symbols are bridged for Emscripten.

This is the first result that demonstrates why the exact String layer matters:

```text
prims.mp:      success, peak internal used=22,380
seascape2.mp:  device OOM, phase=display-list, request=10,240,
               failure free=13,200, failure largest=7,668,
               peak internal used=165,604
```

Before replacing the host's `std::string` shim, the same exact allocator and
heap profile rendered Seascape 2 successfully with a 148,912-byte peak. Exact
WString allocation sizes, growth, and lifetimes are therefore sufficient to
cross the fragmentation boundary; VM bytecode would not address this class of
failure. The test was run at 200x200, but the framebuffer is browser support
memory and does not affect the seeded device heap.

Useful commands:

```sh
pio run -d Watchy_MicroPatterns -e watchy2
pio run -d M5Paper_MicroPatterns -e m5stack-fire
tools/host_harness/wasm/build.sh
node tools/host_harness/wasm/verify.mjs
```

## Current next actions

The user requested an immediate stop and handoff. Continue from
`2026-09-02-wasm-device-constraints-handoff.md`; do not redo the completed
allocator/String work.

At stop time:

- Both constrained modules build and their String smoke tests pass.
- Watchy Seascape 2 deterministically reproduces a display-list OOM.
- M5Paper's persistent RenderController lifecycle and post-OOM reset boundary
  are implemented and compile.
- Complete failure/source/capability exports are implemented and compile.
- `micropatterns_emulator/device_renderer.js` is partially updated to load
  three profiles and collect telemetry, but HTML controls, artifact syncing,
  browser testing, corpus automation, final docs, and calibration remain.
- `git diff --check` passes.

Do not touch the user-owned untracked `examples/scripts/seascape*.mp` files.

### 2026-09-02 — resumed: browser integration and verification

Resumed from the handoff. The branch had meanwhile renamed Emscripten ES-module
glue from `.mjs` to `.js` because the production static server does not send
a JavaScript MIME type for `.mjs`. Updated the constrained builder to follow
that convention and added
`wasm/sync_constrained_to_emulator.sh`, which rebuilds before copying both
profile pairs.

Added the emulator device profile and memory-state controls plus a persistent
memory report. It shows toolchain/profile, calibration status, initial/current/
peak internal and PSRAM consumption, allocation call counts, and a decoded
"DEVICE WOULD REBOOT" failure snapshot. Browser verification covered:

- default M5Paper at 540x960 with a clearly marked provisional map;
- Watchy selection changing the canvas to 200x200;
- calibrated Watchy BLE initial free of 87,668 bytes;
- unconstrained reference fallback;
- Seascape 2 showing a display-list/C++-new OOM;
- the next simple Watchy render succeeding after the simulated reboot.

The first City 2 sweep exposed a simulator bug: constrained builds had not
defined `ARDUINO_ARCH_ESP32`, so the shared renderer skipped its real
largest-block guard and tried to allocate the 5,000-byte occupancy bitmap.
Added a minimal `esp_heap_caps.h` compatibility surface and routed
`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` to the simulated heaps.
After the fix, City 2 completes 180/180 samples (60 seeds in each of radios off,
BLE, and Wi-Fi/TLS) and never exceeds 37 display-list items.

Added `verify_constrained.mjs` and Make targets. Current verified output:

```text
SAME  Watchy prims
SAME  Watchy Art Deco
SAME  M5Paper prims
OOM   Watchy Seascape 2  request=10240 free=13200 largest=7668
PASS  post-OOM Watchy reboot boundary
PASS  Watchy states  off=177476 BLE=87668 WiFi/TLS=101460
PASS  M5Paper persistent RenderTask allocations
PASS  City 2 states 0/1/2  60/60 each; max items=37
```

The canonical WASM golden gate also passes **18/18 identical**.

Permanent documentation is now in
`docs/analysis/wasm-device-resource-fidelity.md`. It separates display-list,
streaming, and VM/compact-IR effects and records the remaining calibration and
timing limits.

## Current status after resumption

Implementation and functional/browser verification are complete. Remaining
closeout is repository QA: run the full Make targets once through their public
entry points, inspect diffs and generated artifact status, and decide whether
the user wants this uncommitted work committed/pushed.

### 2026-09-02 — final correctness pass

Made calibration reporting state-specific: Watchy radios-off and BLE are
measured, Watchy Wi-Fi/TLS remains provisional, and all M5Paper budgets remain
provisional. Added the pinned Arduino core version to the WASM metadata/UI so a
report identifies both allocator and WString provenance. Corrected the native
shim documentation: `MP_SHIM_NULL_CSTR` is an intentionally aggressive safety
test, not an exact empty-WString model. Also normalized C++ `operator new(0)` to
a one-byte device allocation, as required of a replaceable global new even
though the underlying ESP malloc may return null for a zero-byte request.

Made the verification gate independent of the user-owned, untracked Seascape
scripts by adding a small tracked display-list OOM fixture. The exact Seascape 2
assertions still run when that local script is present; on a clean checkout the
suite reports the optional case as skipped while retaining deterministic OOM,
recovery, pixel-parity, lifecycle, state, and City 2 coverage.

Final repository QA completed:

- `make verify-wasm-constrained`: passed all constrained checks through the
  public target, including both exact WString smoke programs;
- `make verify-wasm`: **18/18 identical** to canonical C++ goldens;
- `device_constraints/smoke.sh`: Watchy and M5 allocator fragmentation/failure
  smoke tests passed;
- `git diff --check`: clean;
- synced emulator artifacts are SHA-256 identical to the latest build outputs;
- browser reload confirmed M5Paper is labeled provisional, Watchy Wi-Fi/TLS is
  labeled provisional, and Watchy BLE is labeled calibrated at 87,668 bytes.

The local HTTP server and browser QA tab were closed. No commit or push was
made in this continuation; the three untracked `examples/scripts/seascape*.mp`
files remain untouched and user-owned.

### 2026-09-02 — CI and deployment drift gate

Added a single pre-deploy aggregate, `make ci`, covering native goldens, sweep
audits, fatal ASan/UBSan runs, strict String stress, renderer source manifests,
canonical WASM goldens, explicit positive/negative language contracts,
constrained behavior, and byte-for-byte freshness of all six committed editor
artifacts. The source-manifest check proves that M5Paper, Watchy, the native
harness, canonical WASM, and constrained WASM still compile the same six core
renderer sources. The language suite locks command case-insensitivity, nested
control flow, environment variables, and representative syntax/semantic
rejections without adding a second parser implementation.

The first fatal sanitizer run found pre-existing signed-overflow UB in DSL
integer arithmetic. The ESP32 intent is 32-bit wraparound, so add, subtract,
multiply, and the `INT_MIN / -1` edge were rewritten with explicit modulo-2^32
semantics. Existing native and WASM goldens remained identical; the fix removes
a genuine cross-compiler drift risk rather than changing the language.

GitHub Actions now runs the aggregate plus pinned PlatformIO builds for Watchy 2
and M5Paper on every push and pull request. FTP deployment depends on both, so a
failed engine, language, artifact, or firmware build gate prevents publication.
The generated lifetime soak is weekly/manual to keep routine feedback bounded.

Local validation after the change:

- `make ci`: passed, including 18/18 native and WASM golden cases;
- Watchy 2 and M5Paper PlatformIO builds: passed;
- `make soak`: 41 scripts × 24 seeds × two renders = 1,968 deterministic
  ASan/UBSan-clean renders;
- YAML, shell, JavaScript syntax, and `git diff --check`: clean.

The user-owned `examples/scripts/seascape*.mp` files are still excluded from
version control.

The first pushed Actions run then caught a Linux-only bug in the pre-existing
City probe drift checker. Its end marker used `\+` in a basic `sed` expression:
BSD sed interpreted that as a literal plus, while GNU sed interpreted it as a
repetition operator and extracted through end-of-file. Replaced arithmetic
operators in both boundary expressions with portable bracket literals (`[+]`
and `[*]`). The gate now compares the intended block on both platforms; this is
also a useful demonstration that executing the gates in CI adds coverage beyond
running them on the development Mac.

The next run passed both real firmware builds and every semantic WASM check,
then found that the committed `.wasm` byte comparison still encoded the local
absolute checkout path. ESP-IDF allocator assertions use `__FILE__`, so the
same Emscripten version embedded `/Users/...` on macOS and `/home/runner/...` on
Linux. Added `-ffile-prefix-map=<repository>=.` to every canonical and
constrained compilation stage. This keeps assertion provenance useful while
making the generated modules reproducible across checkout locations and hosts.

### 2026-09-02 — source-aware editor diagnostics

Put the constrained profiles to direct use in the online editor. Added a small
firmware-safe source-line hook at the parser, display-list generator, and
rasterizer boundaries. It compiles to a no-op in native firmware and feeds the
constrained allocator in WASM. Allocation failure and peak-usage telemetry now
carry the executing MicroPatterns line. Peak attribution is restarted at each
render while preserving M5Paper's long-lived heap/session, preventing a prior
script's line from leaking into the next report.

Added two workload signals from the shared C++ path: display-list bytes and
whether the occupancy map was actually available. The editor can now distinguish
a fatal OOM from a successful-but-slower painter-order fallback, memory pressure,
and risky list growth.

Kept the UX independent of this particular engine. `device_diagnostics.js`
defines schema version 1 and translates raw runtime results into stable codes,
severity, source line/excerpt, explanations, evidence, and suggestions. The
editor cards know only that schema. They highlight the relevant CodeMirror line
and provide a direct navigation button. A later engine replacement needs one
adapter update; the cards do not need to be reconnected. Unstructured error
strings still receive a useful generic diagnostic and best-effort line parsing.

Before the compiled-program change, browser verification on exact Seascape 2
and Watchy radios-off showed:

```text
MP_DEVICE_OOM at line 360
request 10,240; internal free 13,200; largest block 7,668
explanation: allocator fragmentation
suggestion: compact/stream the display list; VM bytecode alone does not help
```

The line button focused line 360 in CodeMirror. The later compiled-program work
removed that wake-time failure by compiling with radios off and loading a compact
program at render time. The alert remains applicable to the tracked synthetic
display-list OOM fixture and any future script that exhausts the list itself.

The full local `make ci` gate passed at this point: 18/18 native and WASM
goldens, language contract, sanitizers, source parity, diagnostic contract,
constrained Seascape/City checks, and all six committed editor artifacts
matching fresh builds. Both Watchy 2 and M5Paper PlatformIO builds also passed.

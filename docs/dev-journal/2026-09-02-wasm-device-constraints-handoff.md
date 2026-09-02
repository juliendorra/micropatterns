# Pickup prompt — high-fidelity WASM device constraints

> **Superseded after resumption.** The implementation and verification work
> listed below was completed later the same day. Continue from the final entries
> in `2026-09-02-wasm-device-constraints.md`; this file remains as the historical
> stop-point record.

Continue the in-progress high-fidelity device-resource simulation in:

`/Users/julien/Documents/GitHub/micropatterns`

Read this file and
`docs/dev-journal/2026-09-02-wasm-device-constraints.md` first. Continue
implementation; do not restart the research or replace the exact compatibility
path with an approximate accounting-only model.

## Goal

Ship device-selectable emulator modes that use the shared C++ rendering engine
while reproducing the relevant allocation limits of:

- Watchy 2: Arduino-ESP32 3.1.3, ESP-IDF 5.3.2, internal RAM only.
- M5Paper: Arduino-ESP32 2.0.4, ESP-IDF 4.4.1, internal RAM plus PSRAM.

The result must reproduce contiguous-block/fragmentation failures, report them
without crashing the browser, distinguish display-list pressure from parser and
String pressure, and explain why VM bytecode does not solve allocator
fragmentation.

## Preserve

The user owns these untracked files. Do not add, edit, delete, or overwrite:

- `examples/scripts/seascape.mp`
- `examples/scripts/seascape2.mp`
- `examples/scripts/seascape3.mp`

The branch is `main`. The prior Watchy firmware work (83-second wake interval,
BLE only on top-left, render crash guard) was already committed, pushed, and
flashed before this work. The current WASM work is uncommitted. The user did not
request a commit for this phase yet.

## Implemented and verified

1. Exact versioned ESP-IDF multi_heap/TLSF sources are vendored under
   `tools/host_harness/device_compat/esp-idf-{4.4.1,5.3.2}`, including light
   heap poisoning.
2. Exact Arduino WString sources are vendored under
   `device_compat/arduino-esp32-{2.0.4,3.1.3}`, with provenance and SHA-256
   values in `device_compat/README.md`.
3. `tools/host_harness/device_constraints/` contains:
   - capability-aware constrained allocator;
   - allocation phase/source/failure telemetry;
   - global C++ new/delete routing;
   - WString malloc/realloc/free routing;
   - allocator and String smoke tests.
4. Watchy is calibrated to the connected-device start pair:
   177,476 bytes free and 110,580-byte largest block.
5. M5Paper routes allocations <=4096 bytes internal-first and larger allocations
   PSRAM-first with fallback, matching `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`.
   Its region sizes and radio reservations are explicitly provisional.
6. The constrained render path keeps browser canvas/output allocations outside
   device heaps, frees Watchy source text after parsing, and models M5Paper's
   persistent parser/runtime/renderer lifecycle across render jobs.
7. Fatal device allocation failures are caught only at the outer WASM boundary.
   M5 persistent objects are destroyed and the next call starts at a reset
   boundary.
8. Both modules and tests last built successfully:

```text
profile=watchy2 arduino_string_size=16 sso_max=13 rounded_capacity=31 oom_request=60016 oom_phase=2
mp_render_watchy.mjs 21K
mp_render_watchy.wasm 133K
profile=m5paper arduino_string_size=16 sso_max=13 rounded_capacity=31 oom_request=3000016 oom_phase=2
mp_render_m5paper.mjs 21K
mp_render_m5paper.wasm 134K
```

9. The decisive Watchy reproduction is:

```text
seascape2.mp -> device OOM
phase=display-list (3)
source=C++ new (1)
request=10,240
failure internal free=13,200
failure largest block=7,668
peak internal used=165,604
```

With the exact allocator but the old std::string shim, Seascape 2 succeeded.
Exact Arduino String allocation/growth is what crosses the fragmentation
boundary. This is direct evidence that VM bytecode alone would not fix it.

10. M5 sequential test results after the persistent lifecycle change:

```text
prims:      success; retained internal free=181,596
seascape2:  success; retained internal free=52,928
city:       success; retained internal free=159,440
```

These M5 numbers are behaviorally useful but not hardware-calibrated.

## Partial edit at handoff

`micropatterns_emulator/device_renderer.js` has just been changed to:

- lazy-load `reference`, `watchy`, and `m5paper` modules;
- select a radio state;
- bind all memory/failure exports;
- return a structured memory snapshot for success and failure.

This edit has not yet been browser-tested. The constrained artifacts have not
yet been copied into `micropatterns_emulator/wasm/`, and the page has no
profile/state controls or memory report yet.

## Resume in this order

1. Review `device_renderer.js` for syntax/API correctness.
2. Add emulator controls:
   - device profile: M5Paper, Watchy, unconstrained reference;
   - device state: radios off, BLE, Wi-Fi/TLS;
   - a compact memory/failure report showing calibration status, IDF version,
     initial/current/peak internal and PSRAM, allocation counts, and decoded
     failure phase/source/capability.
   Couple native display defaults to M5Paper 540x960 and Watchy 200x200, while
   retaining an explicit reference option.
3. Rebuild constrained modules and copy generated
   `mp_render_{watchy,m5paper}.{mjs,wasm}` into
   `micropatterns_emulator/wasm/`. Add a reproducible sync/check command; do
   not manually create binary patches.
4. Add a Node verification script for constrained profiles:
   - pixel parity with canonical output for successful renders;
   - expected Watchy Seascape 2 OOM signature;
   - M5 persistent successive-render behavior;
   - radio-state reductions;
   - post-OOM next-call recovery.
5. Extract City 2 from
   `tools/server/backups/2026-08-27-s3/scripts/kksh2hjtkb/city-2-by-telohtrab.json`
   in-memory during tests. Sweep representative counter/time seeds on Watchy.
   Record whether failures are parse/String/internal fragmentation or display
   list. Do not confuse it with `tools/host_harness/corpus/city.mp`, which can
   request an 81,920-byte display-list growth on Watchy.
6. Run the canonical build and golden verification to prove unconstrained pixel
   behavior did not regress.
7. Browser-test the emulator, including a Seascape 2 failure and switching
   profiles/states.
8. Update the main journal and permanent documentation. Clearly label M5Paper
   as uncalibrated until a real heap-block capture is available.
9. Run `git diff --check` and inspect status. Do not commit unless the user
   asks.

## Commands

```sh
tools/host_harness/device_constraints/smoke.sh
tools/host_harness/wasm/build_constrained.sh
tools/host_harness/wasm/build.sh
node tools/host_harness/wasm/verify.mjs
git diff --check
git status --short
```

Emscripten is at:

`/Users/julien/Documents/GitHub/qemu-ipod_touch_1g/.wasm-toolchain/emsdk`

The linker may need permission to write its cache outside the repository.

## Important technical checks

- Do not count the host 4bpp canvas or browser 8-bit output buffer against a
  device heap; the profile starts from the measured firmware phase boundary.
- On allocation failure, report the captured failure snapshot, not
  `currentFree` after stack unwinding.
- Failure enum mappings:
  - phase: 0 idle, 1 source, 2 parse, 3 display-list, 4 rasterize, 5 output;
  - source: 0 explicit, 1 C++ new, 2 Arduino String, 3 radio reservation;
  - capability: 0 default, 1 internal, 2 PSRAM.
- Watchy starts a fresh allocator every render. M5Paper keeps the latest
  parser/runtime/renderer allocations between jobs and resets on state change
  or simulated fatal OOM.
- M5 profile calibration is the main remaining fidelity caveat. Exact
  allocator algorithms do not make an invented initial block map exact.

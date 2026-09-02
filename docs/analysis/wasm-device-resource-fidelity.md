# WASM device-resource fidelity

## Outcome

The browser has three builds of the same MicroPatterns C++ renderer:

| Profile | Rendering code | Allocation model | Calibration |
|---|---|---|---|
| Unconstrained reference | shared firmware C++ | Emscripten libc++/malloc | pixel oracle |
| Watchy 2 | shared firmware C++ | Arduino 3.1.3 WString + ESP-IDF 5.3.2 multi-heap/TLSF, light poisoning | radios-off start budget and BLE free-heap drop measured; Wi-Fi/TLS provisional |
| M5Paper | shared firmware C++ | Arduino 2.0.4 WString + ESP-IDF 4.4.1 multi-heap/TLSF, light poisoning, internal/PSRAM routing | initial region map provisional |

This is not allocation accounting layered over a host allocator. Allocations
are placed, split, and coalesced by the version-matched ESP allocator, so a
request fails when no suitable contiguous block exists even if aggregate free
memory is larger.

## What runs in simulated device memory

While a firmware phase is active, global C++ `new/delete` routes STL nodes,
vectors, maps, lists, deques, and function objects through the device heap.
Arduino WString's exact `malloc/realloc/free` calls use the same heap. Failure
telemetry records:

- source load, parse, display-list generation, or rasterization;
- C++/STL, Arduino String, explicit, or radio-reservation source;
- requested capability and bytes;
- internal and PSRAM free bytes and largest block at the failed request.

The browser's canvas backing store, grayscale transfer buffer, JS bridge, and
error-report strings stay in support memory. Counting them against the device
would double-count framebuffers already reflected in a hardware phase-boundary
heap capture.

Watchy starts a new heap each render, matching reboot/deep-sleep wakes, and
releases source text after parsing. M5Paper retains its parser and latest
heap-allocated runtime/renderer across jobs, matching the long-lived
`RenderTask`. A simulated fatal OOM discards that session before the next job.

## Seascape 2: reproduced cause

At the fixed verification seed (counter 0, 12:34:56), calibrated Watchy reports:

```text
phase:              display-list generation
allocation source:  C++ new / STL
request:            10,240 bytes
internal free:      13,200 bytes
largest block:       7,668 bytes
peak internal used: 165,604 bytes
```

The same allocator initially rendered Seascape 2 when the host
`std::string` shim was still in use. Replacing it with exact Arduino 3.1.3
WString changed allocation sizes, 16-byte capacity growth, and fragmentation,
and the failure appeared. Aggregate free memory is not the deciding value:
13,200 bytes are free, but no 10,240-byte contiguous block exists.

## City 2: not a large-display-list failure

A deterministic sweep of 60 counters and all 60 second positions was run for
each Watchy state. All 180 renders completed and the maximum display list was
37 items. City 2 therefore does not reproduce as a display-list-capacity
failure in the current firmware.

An earlier constrained build falsely failed five BLE samples on a 5,000-byte
raster allocation. The cause was a simulator fidelity bug: it had not enabled
the firmware's `ARDUINO_ARCH_ESP32` branch that checks the largest free block
before allocating the 1-bit occupancy map. The constrained build now routes
`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` to its simulated heaps.
Under pressure, the renderer correctly drops the map and uses painter's order.

If City 2 still reboots on current hardware, likely remaining causes are outside
the present memory reproduction: watchdog/runtime cost, a different deployed
script or firmware revision, or an unmeasured startup heap layout. Capture the
serial crash phase and heap snapshot before attributing it to tile count.

## Display list, streaming, and VM bytecode

These are separate axes:

- **Streaming instead of a display list** directly removes the growing
  `std::vector<DisplayListItem>`. It would prevent the reproduced Seascape 2
  allocation, but sacrifices front-to-back occupancy/occlusion culling and may
  redraw expensive overlapping primitives.
- **A compact display-list representation** keeps the buffered architecture but
  reduces its slope and allocation sizes. It addresses Seascape more directly
  than changing execution format elsewhere.
- **VM bytecode by itself does nothing to allocator fidelity or OOM.** If the
  existing String/map/list parse tree is retained and then compiled to
  bytecode, peak memory can increase because both representations coexist.
- **A parser that emits a compact linear bytecode/IR directly** could help
  parser fragmentation by eliminating many String and tree-node allocations.
  That benefit comes from the compact allocation/lifetime design, not from the
  fact that execution is called a VM. It also does not remove a separately
  materialized display list unless the VM streams primitives or uses the IR as
  the render list.

The measured failures should determine which change is justified: Seascape 2
currently points at display-list growth after a fragmentation-heavy parse;
City 2 does not.

## Verification and rebuild

```sh
cd tools/host_harness
make verify-wasm                  # canonical pixel goldens
make verify-wasm-constrained      # parity, OOM, recovery, states, persistence, City 2
make sync-wasm                    # rebuild and copy all six static browser artifacts
make ci                           # complete pre-deploy drift gate
```

The constrained verification checks successful pixels against the canonical
module, always tests a tracked display-list OOM fixture, reproduces the exact
Watchy Seascape 2 failure when that user script is present, renders successfully
after the simulated reboot, compares radio budgets, exercises M5Paper's
persistent lifecycle, and sweeps City 2. The tracked fixture keeps the clean-
checkout gate self-contained without taking ownership of local example scripts.

The CI aggregate additionally checks the positive and negative DSL parser
contract, confirms all five consumers compile the same six renderer sources,
runs native sanitizer gates, and rebuilds every committed editor artifact before
comparing it byte for byte. GitHub Actions compiles the Watchy and M5Paper
PlatformIO targets too; the online editor deploy only starts after both firmware
builds and the aggregate engine gate succeed. The longer generated lifetime soak
runs weekly and on manual dispatch.

## Remaining fidelity limits

- M5Paper needs a real capability-tagged free-block capture at the same
  lifecycle boundary used by the simulator. Until then its allocator algorithm
  is exact but its starting region sizes and fragmentation are not.
- Watchy's calibrated totals/largest block do not encode every initial free
  block. A hardware heap walk would improve failures near a boundary.
- The Wi-Fi/TLS reservation is provisional. Watchy BLE total free is calibrated
  to the measured 177,476 -> 87,668-byte change.
- CPU instruction timing, cache/PSRAM latency, FreeRTOS interleavings, watchdog
  expiry, and e-ink transfer/waveform behavior are not simulated.
- Browser timings are not device timings.

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

The parser, display-list generator, and rasterizer also publish the currently
executing MicroPatterns source line to the constrained allocator. An OOM can
therefore identify the command whose expansion or drawing triggered the failed
allocation. That line is an execution site, not always the whole root cause:
an enclosing `REPEAT`, earlier parser fragmentation, and the selected radio
state can all contribute. The editor links to the line and explains the wider
context rather than claiming that one command is intrinsically invalid.

The browser's canvas backing store, grayscale transfer buffer, JS bridge, and
error-report strings stay in support memory. Counting them against the device
would double-count framebuffers already reflected in a hardware phase-boundary
heap capture.

The emulator follows the device's two lifecycle stages. A sync compiles source
straight into a compact `MpProgram` with radios off and stores serialized bytes
outside simulated RAM, like flash. A wake loads that program and renders it in
the selected radio state without parsing source. Watchy starts a new render
heap at each wake; M5Paper retains its latest runtime/renderer across jobs,
matching its long-lived `RenderTask`. A simulated fatal OOM discards that
session before the next job.

## Seascape 2: reproduced cause and mitigation

The original source-at-wake path reproduced this failure at the fixed
verification seed (counter 0, 12:34:56):

```text
phase:              display-list generation
allocation source:  C++ new / STL
request:            10,240 bytes
internal free:      13,200 bytes
largest block:       7,668 bytes
peak internal used: 165,604 bytes
```

The display-list vector growth was the failed request, but the retained parsed
tree had already consumed and fragmented most of the heap. Aggregate free
memory was not the deciding value: 13,200 bytes were free, but no 10,240-byte
contiguous block existed.

The current firmware fixes that lifecycle. Seascape 2 compiles during sync with
radios off, then renders from its stored compact program at a roughly 68 KB
Watchy peak in both radios-off and BLE states. The display list remains; the
failed allocation disappeared because the 146 KB fragmented parse tree no
longer coexists with it. A tracked synthetic fixture still forces genuine
display-list growth past the heap boundary, so that failure class and its
diagnostics remain covered in clean-checkout CI.

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
  `std::vector<DisplayListItem>`. It prevents a genuine list-capacity failure,
  but sacrifices front-to-back occupancy/occlusion culling and may redraw
  expensive overlapping primitives.
- **A compact display-list representation** keeps the buffered architecture but
  reduces its slope and allocation sizes. It addresses Seascape more directly
  than changing execution format elsewhere.
- **VM bytecode added beside the old tree would not help.** Peak memory could
  increase because both representations coexist.
- **The implemented compiler emits a compact linear program directly and moves
  compilation to sync time.** That removes thousands of String/tree allocations
  from the wake-time heap. It fixes Seascape 2 even though the allocation that
  finally failed was display-list growth, because that list no longer competes
  with the fragmented parse tree. The benefit comes from representation and
  lifetime, not merely calling execution a VM.
- **The VM does not remove the display list.** A script whose generated list is
  itself too large can still fail; the tracked OOM fixture proves this. Streaming
  or a more compact list is the relevant remedy for that distinct case.

The measured failure layer should determine which change is justified:
Seascape 2 needed the parsed-program lifetime fixed; the synthetic list fixture
needs list compaction/streaming; City 2 needs neither in current sweeps.

## Stable editor diagnostic boundary

Raw WASM exports are deliberately not the UI contract. `DeviceRenderer`
collects engine errors and telemetry, then `device_diagnostics.js` translates
them into a versioned, engine-neutral object containing severity, stable code,
origin, source line/excerpt, explanation, evidence, and suggestions. The editor
cards and source navigation consume only that object.

This separates three responsibilities:

1. the current runtime reports whatever facts it can observe;
2. one adapter normalizes those facts and degrades gracefully when a future
   engine exposes only an error string;
3. the editor presents and navigates diagnostics without understanding ESP-IDF
   heap fields.

The contract currently reports parser errors, device OOM/reboot risks,
low-memory occupancy-map fallback, high heap pressure, and large display-list
growth. A pure Node contract test locks both structured current-runtime input
and unstructured future-runtime fallback.

## Verification and rebuild

```sh
cd tools/host_harness
make verify-wasm                  # canonical pixel goldens
make verify-wasm-constrained      # parity, OOM, recovery, states, persistence, City 2
make verify-device-diagnostics    # engine-neutral editor alert contract
make sync-wasm                    # rebuild and copy all six static browser artifacts
make ci                           # complete pre-deploy drift gate
```

The constrained verification checks successful pixels against the canonical
module, always tests a tracked display-list OOM fixture, and—when the user-owned
Seascape 2 script is present—checks that it compiles radios-off and renders from
the stored program in both radios-off and BLE states. It also checks recovery,
radio budgets, M5Paper's persistent lifecycle, and the City 2 sweep. The tracked
fixture keeps clean-checkout CI self-contained without taking ownership of local
example scripts.

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
- A successful editor run proves only one `(script, dimensions, counter, time,
  profile, radio state)` sample. Intermittent scripts require a sweep.

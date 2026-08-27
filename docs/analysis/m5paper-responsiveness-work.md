# M5Paper: rasterizer speedup and responsiveness work (2026-08-27)

Follows the three actions agreed after the render-interrupt fix
(`m5paper-render-interrupt-bug.md`): instant button indicators, faster interrupt
cut-in, and attacking render time itself. All three are done. This records the
results, the **dead ends**, and what is still open.

## 1. Measured results (on device, not estimates)

Like-for-like on `circuits` with an identical 351-item display list:

| phase | before | after | note |
|---|---|---|---|
| rasterize | **8037 ms** | **844 ms** | 9.5x |
| display-list generation | 2276 ms | 2288 ms | unchanged — this is the interpreter |
| interrupt abort latency | not measurable (mechanism inert) | **~100 ms** | request -> "Skipping canvas push" |

Host harness medians improved 1.88x. The device gain is much larger, as
predicted: the host under-represents soft-float divide and PSRAM latency, both
far more expensive on an LX6. **The host is a correctness gate and a relative
instrument; it is not a device predictor.**

**The bottleneck has moved.** Rasterization is no longer the dominant cost —
display-list *generation* (the interpreter) now is, at ~2.2 s for `circuits`.
Further work on the rasterizer has much less headroom than
`m5paper-rasterizer-perf.md` implies, because most of that headroom is spent.
See `m5paper-interpreter-perf.md` for the new target.

## 2. Why the indicators never appeared: a locking bug, not a drawing bug

This corrects an earlier explanation. The unresponsiveness was **not** solely a
consequence of removing the stale canvas push. There was a pre-existing defect:

`DisplayManager` had a **single mutex** (`_epdMutex`) guarding *both* the
540x960 script framebuffer *and* every EPD hardware transaction. `RenderTask`
holds it for the entire render — parse, generate, rasterize, push. Therefore:

- `drawActivityIndicator()` requested it with a **100 ms timeout**, timed out,
  logged `failed to take mutex`, and returned. The indicator code ran on every
  button press and drew nothing, exactly when the device was busy.
- `showMessage()` (the script-name title) did the same with a 500 ms timeout.
- Both *also* drew into the main `_canvas` "for consistency", stamping UI
  chrome into the script framebuffer that gets pushed later.

Task priorities were never the problem: `InputTask` is priority 3 on core 1,
`RenderTask` priority 1 on core 0.

**Fix:** split into `_canvasMutex` (long-held, framebuffer) and `_panelMutex`
(short-held, EPD transactions + indicator scratch canvas), lock order always
canvas -> panel. Indicators take only the panel lock and never touch `_canvas`.
Both properties matter: panel-only is what makes them instant during a render;
staying off `_canvas` is what stops them racing the live rasterizer now that the
big lock no longer covers them.

## 3. Dead ends and deliberate non-choices

Recorded so they are not re-attempted without new information.

### Rasterizer

- **Reciprocal-multiply for all scale factors** (the perf doc's P1 as written).
  Rejected: a 1-ulp change that can flip boundary pixels on scripts outside the
  corpus. Shipped instead only where `1/s` is *exactly* representable
  (power-of-two factors, including the default 1), which is bit-for-bit
  identical and captures most of the device win.
- **Incremental (DDA) stepping** of the transformed coordinate along a span.
  The obvious next speedup, and what P4 implies — but `u += du` diverges from
  `im0*fx + im2*fy + im4` after a few steps. Not bit-exact. Not attempted.
- **1-bit occupancy bitmap** (P3). NOT done. Likely a real device win (8x less
  PSRAM traffic, ~450 KB freed) but it measures **neutral-to-slower on the
  host**, and the host was the only instrument available. A device-side
  follow-up, and a good example of the host's limits.
- **Span/row write API on `MPCanvas`.** Probably the biggest remaining win
  (direct nibble packing, no per-pixel call) but it widens the deliberately
  narrow four-method platform surface that makes the Watchy port cheap.
  Rejected on architecture grounds, not performance grounds.
- **Analytic (sqrt) span for `fillCircle`.** Not bit-exact near the vertex.
  Shipped a conservative |dx|<=r, |dy|<=r bound (+1 px slack) with the exact
  disk test still running inside. Circles were only ~6% of profile samples.
- **`intParams.at("X")` map lookups.** Confirmed the perf doc's finding: all
  call sites are outside the pixel loops. Left alone.

### Responsiveness

- **Making `showMessage` always clear the canvas and draw the title.** That is a
  write to a framebuffer an active render owns. Under the old single mutex it
  merely failed; under the split lock it would be a genuine data race. Hence the
  banner fallback.
- **Keeping the main-canvas draw in the indicators as a "fallback".** Same race,
  plus it bakes UI chrome into the script image.
- **A recursive mutex** so `pushCanvasUpdate()` could be called from within a
  `lockEPD()` section. `pushMainCanvasLocked()` makes the two lock levels
  explicit instead of hiding re-entrancy.
- **Splitting large `PIXEL` items inside `DisplayListRenderer`** for finer
  polling. Not possible: each primitive computes its own screen bounds from the
  item; the renderer cannot clip it. Hence the poll inside the primitive.
- **Tightening polling back toward per-pixel** now that the check is cheap.
  Unnecessary — 25-65 ms worst case is far below perception — and it would give
  back the rasterizer win.

### A number that should not be quoted

An intermediate measurement showed the interrupt check costing **+95% to +154%**
of rasterize time. That was against the *pre-optimisation* per-pixel tree. After
the move to per-scanline polling the same check costs **1-3%**. The large figure
is historically real but no longer describes the code.

## 4. Open / not fixed

- **NEW, needs investigation:** `loadScriptContent: Failed to take mutex for
  fileId s0 after 1000ms` was observed on device during an interrupt sequence.
  This is `ScriptManager`'s SPIFFS mutex, unrelated to the DisplayManager split.
  The system recovered (the next script rendered normally), but it indicates
  contention between an aborted render's content load and the incoming one.
  Not diagnosed.
- **The clear-before-render race is still unfixed.** `RENDER_INTERRUPT_BIT` is
  still cleared before the render starts, so an interrupt requested between
  queuing and that clear is dropped. A render epoch/generation counter remains
  the better design and remains unimplemented.
- **`city`'s per-item cull loop.** ~4.2 ms of its "rasterize" time is actually
  `std::map<String,int>::at` in `calculateScreenBounds`, over 20,737 items —
  not rasterization. The next target for item-heavy scripts.
- **Not verified on device:** that concurrent panel access is safe on silicon
  under sustained load. The reasoning is that rasterization only calls
  `drawPixel`/`fillCanvas` (pure framebuffer, no driver), so an indicator push
  during a render contends only on `M5.EPD`, which `_panelMutex` serializes.
  Sound, but if garbled panel output ever appears, this is the first suspect.
- **Not observed directly:** an activity indicator drawn *during* an in-flight
  render. The startup indicator's region push was confirmed on device; the
  in-render case was not isolated in a capture.

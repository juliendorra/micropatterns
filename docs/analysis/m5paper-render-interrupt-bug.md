# M5Paper: stale render paints over the title on script switch

**Reported by Julien:** "when switching to another script, the previous script render over the
title, then the new script appears."

**Status:** diagnosed, not yet fixed. Diagnosis is from source reading only — not yet confirmed on
hardware (the M5Paper was off USB at the time of writing).

## Root cause: the mid-render interrupt is a no-op

The cooperative interrupt machinery looks complete but **is not wired up**. Nothing can set the flag
that the render loop polls.

The full set of call sites (`grep requestInterrupt|RENDER_INTERRUPT_BIT`):

| Site | What it does |
|---|---|
| `main.cpp:398` | MainControlTask signals a wanted interrupt via `xEventGroupSetBits(g_renderTaskEventFlags, RENDER_INTERRUPT_BIT)` |
| `render_controller.cpp:15` | `checkInterrupt()` returns **only** `_interrupt_requested_for_runtime_or_renderer` |
| `render_controller.cpp:21` | `renderScript()` sets that flag to **`false`** on entry |
| `main.cpp:787` | `renderCtrl.requestInterrupt()` — the **only** caller, and it runs **after `renderScript()` has already returned** |

So during a render:

- `MicroPatternsRuntime` and `DisplayListRenderer` poll `checkInterrupt()` via
  `_interrupt_check_cb`.
- That returns `_interrupt_requested_for_runtime_or_renderer`.
- **Nothing sets that flag to `true` while the render is running.** MainControlTask sets an event
  group bit instead, and **no code inside the render path ever reads the event group.**

The two signalling mechanisms were never connected to each other.

## How that produces the reported symptom

1. User presses a button to switch scripts.
2. MainControlTask sets `RENDER_INTERRUPT_BIT` (`main.cpp:398`) and preemptively goes IDLE
   (`main.cpp:402`), then draws the new script's title via `showMessage`.
3. The in-flight RenderTask **does not notice**. `DisplayListRenderer::render()` clears the canvas —
   **wiping the title** — and rasterizes the *old* script to completion (~2.3 s measured, see
   `../measurements/2026-08-27-m5paper-baseline.md`).
4. `renderScript()` returns. Only now does `main.cpp:778` read the event bit and mark the result
   `interrupted = true`.
5. **`main.cpp:803-812` pushes the canvas anyway, unconditionally:**

   ```cpp
   // After rendering is complete (or interrupted), push the canvas
   M5EPD_Canvas* canvas = g_displayManager->getCanvas();
   if (canvas) {
       canvas->pushCanvas(0, 0, UPDATE_MODE_GC16);   // <-- no check on resultData.interrupted
   }
   ```

   There is **no guard on `resultData.interrupted` or `resultData.success`**. The completed old
   render is pushed to the panel.
6. The new render then runs and pushes.

Net visible effect: title → old script → new script. Exactly as reported.

There are therefore **two independent defects**, and fixing either alone is insufficient:

- **A. The interrupt never fires mid-render** (signalling mechanisms unconnected). Fixing only B
  would still mean waiting a full render before switching.
- **B. The canvas is pushed even when the render is known to be interrupted.** Fixing only A would
  narrow the window but still push a *partial* canvas when an interrupt does land mid-rasterization.

## Bonus consequence: the per-pixel interrupt check is pure cost

`micropatterns_drawing.cpp` calls `_interrupt_check_cb` **once per pixel** — and in `fillRect`,
`fillCircle` and `drawAsset`, once per row *and* once per pixel. The rasterizer analysis
(`m5paper-rasterizer-perf.md`, proposal P2) flagged this `std::function` indirect call as a
significant share of the ~500-600 cycle per-pixel cost.

Given the above, **that callback can never return `true`.** It is currently a per-pixel indirect
call through `std::function` that performs a load and a compare, millions of times per render, to
answer a question whose answer is structurally always "no".

This is worth stating plainly because it changes how P2 should be framed: it is not a tradeoff
between responsiveness and speed. Right now the project pays the full cost and gets **none** of the
responsiveness. Any fix should restore the responsiveness *and* remove the per-pixel cost — these are
not in tension.

## Proposed fix (not yet applied)

Three parts, smallest first:

1. **Guard the push** (`main.cpp:803`). Do not `pushCanvas` when `resultData.interrupted` is true.
   One-line change, immediately removes the "old script appears over the title" symptom even before
   the interrupt is properly wired. Consider also whether an interrupted render should repaint the
   title it destroyed.

2. **Wire the interrupt for real.** Give `RenderController` a way to observe MainControlTask's
   signal. Options, in rough order of preference:
   - Pass a `std::function<bool()>` into `RenderController` that reads
     `xEventGroupGetBits(g_renderTaskEventFlags) & RENDER_INTERRUPT_BIT` — keeps the existing
     signalling, no new state. Note `xEventGroupGetBits` is not free; do **not** call it per pixel.
   - Or have MainControlTask set a `volatile bool` that `RenderController` already owns, alongside
     the event bit. `_interrupt_requested_for_runtime_or_renderer` is already declared `volatile`
     (`render_controller.h:27`), so it is suitable — it just needs a reachable setter and
     `renderScript()` must stop clearing it unconditionally on entry without regard to a
     newer pending request.

3. **Move the check off the per-pixel path** (this is P2 from the rasterizer analysis, now
   additionally justified by correctness). Check once per scanline, or once per N scanlines, reading
   a plain `volatile bool`. At 960 px wide, per-scanline checking cuts the check count by ~960x while
   bounding worst-case latency to a single row — far below human perception.

**Also worth fixing while here:** `main.cpp:764` clears `RENDER_INTERRUPT_BIT` *after* acquiring the
EPD lock and *before* starting the render. If MainControlTask sets the bit between the job being
queued and this clear, **the interrupt request is silently dropped**. A render epoch/generation
counter compared at push time would be more robust than bit clearing, and would also let a stale
result be discarded outright rather than reinterpreted.

## Open questions

- Not yet reproduced under instrumentation. The serial log at
  `../measurements/m5paper-baseline-serial.log` contains no script switch, so the sequence above is
  inferred from source, not observed. **Capture a switch on hardware to confirm** — expect to see
  `RENDER_INTERRUPT_BIT was set by MainControlTask` logged only *after* a full-duration
  "Display list rendering ... took N ms" line, which would confirm the render ran to completion.
- Whether `FetchTask` has an analogous unconnected-signal problem. `main.cpp:405-407` contains a
  comment admitting the fetch interrupt path is incomplete ("might need a flag for
  NetworkManager"). Not investigated.
- `DisplayManager::showMessage` and `DisplayListRenderer::render()` both write the shared canvas.
  Canvas ownership is guarded by the EPD mutex in some paths; whether `showMessage` from
  MainControlTask always takes it is not verified here.

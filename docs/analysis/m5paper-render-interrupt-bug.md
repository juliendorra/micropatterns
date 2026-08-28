# M5Paper: stale render paints over the title on script switch

**Reported by Julien:** "when switching to another script, the previous script render over the
title, then the new script appears."

**Status:** FIXED and confirmed on hardware, 2026-08-27. The diagnosis below was written from
source reading only; it has since been verified on the device and proved correct in both parts.
The original text is preserved unchanged as history — see "Resolution" at the end for what was
actually done, what the diagnosis got right, and what it missed.

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


---

# Resolution (2026-08-27)

## The diagnosis was correct, in both parts

Both defects were real and both needed fixing. Neither fix alone is sufficient, exactly as the
analysis above predicted.

### Fix 1 — connect the two signalling mechanisms

`RenderController::checkInterrupt()` now reads the event group that `MainControlTask` actually
sets, and latches it:

```cpp
if (_interrupt_requested_for_runtime_or_renderer) return true;   // fast path, already latched
if (g_renderTaskEventFlags != NULL &&
    (xEventGroupGetBits(g_renderTaskEventFlags) & RENDER_INTERRUPT_BIT) != 0) {
    _interrupt_requested_for_runtime_or_renderer = true;
    return true;
}
return false;
```

The runtime and the renderer already poll this callback, so this is all that was needed to make the
cooperative interrupt mechanism live. `render_controller.cpp` now includes `main.h` for
`g_renderTaskEventFlags` / `RENDER_INTERRUPT_BIT`.

### Fix 2 — do not push an interrupted render

The unconditional `pushCanvas` is now guarded. This is the fix that removes the *visible* symptom:

```cpp
} else if (resultData.interrupted) {
    log_i("RenderTask: Render interrupted for '%s'. Skipping canvas push ...");
} else {
    canvas->pushCanvas(0, 0, UPDATE_MODE_GC16);
}
```

## Hardware confirmation

The "Open questions" section above asked for a switch captured under instrumentation. Done — start
a slow script, interrupt it mid-render via the serial console, and the log shows the full chain
firing in order:

```
MainCtrl: Input received during render. Requesting interrupt.
RenderTask: RENDER_INTERRUPT_BIT was set by MainControlTask. Overriding result to interrupted.
RenderTask: Render interrupted for 'eyes'. Skipping canvas push to avoid painting stale content.
```

Note what this *disproves*: the prediction was that the interrupt bit would be logged only after a
full-duration render line. With Fix 1 in place the render is genuinely cut short instead (~0.2 s
vs the ~8 s a complete `circuits` render takes), so the mechanism aborts rather than merely being
detected after the fact.

## What the diagnosis missed: a UX regression

Removing the stale push is correct, but it also removed the only visual feedback during a long
render. Previously the abandoned partial frame was painted, so the panel visibly changed on a
button press. Now the panel holds its previous image for the full duration of the *new* render —
8-10 s measured — and the device reads as unresponsive even though it is working correctly.

This was reported by the user as "seem stuck, not responding to the buttons commands".

**Partially corrected later:** it is only *partly* a consequence of this fix. The indicators that
should have provided feedback were themselves broken by a pre-existing locking bug -- they were
starved of a mutex held for the whole render and drew nothing. See
`m5paper-responsiveness-work.md` §2. All three follow-ups below are now DONE (rasterize ~7x faster overall,
interrupt abort ~100ms); results and dead ends are in that document.

The agreed follow-ups were:

1. Button-press indicators must be as instant as possible (they already exist; they need to survive
   and appear immediately).
2. Tighten interrupt granularity. The check is currently polled per display-list item, which is
   coarse on a 351-item script.
3. Reduce render time itself. See `m5paper-rasterizer-perf.md` — estimated 10-20x headroom on the
   hot path. This is the substantive fix, and it also decides whether the Watchy port is usable.

## Still open (unchanged by this work)

- The `main.cpp` clear-before-render race noted above is **not** fixed. `RENDER_INTERRUPT_BIT` is
  still cleared before the render starts, so an interrupt requested in that window is still dropped.
  The render epoch/generation-counter idea remains the better design and remains unimplemented.
- `FetchTask`'s analogous incomplete interrupt path is still not investigated.
- Canvas ownership between `showMessage` and `render()` is still not verified.

---

## 2026-08-28 -- Title browsing, and the three visible symptoms

The remaining flakiness was reported from the device, not from the code:
paging through scripts showed a stale "Render stopped" over the new title, the
*previous* script flashed up briefly anyway, and the whole thing felt slow.

Three separate causes, none of them the interrupt mechanism itself.

### 1. A blocking half-second per press

`selectNextScript()` was followed by `vTaskDelay(pdMS_TO_TICKS(500))` "to
display the selection message", and only then triggered a render. So every
press cost 500ms before the *next* press was even read from the queue, and each
press started a render the following press then had to interrupt. Paging ran at
two titles per second at best, with a render begun and abandoned for each.

Replaced with a settle timer: a press shows the title and arms a render for
`TITLE_SETTLE_MS` (450) later, and every further press re-arms it. Presses are
read immediately. Only the title you stop on is rendered.

The Watchy has the same model now, and its serial `next`/`prev` route through
the same function so the behaviour can be exercised over a cable. Measured:
five steps 150ms apart draw five titles and produce exactly one render, where
before they produced five.

### 2. "Render stopped" arriving after the new title

A render is only ever interrupted because the user pressed a button, and the
press has already replaced the screen with the new script's name. The message
therefore landed a moment later, painting over the title the user was reading,
to announce something they had just done deliberately. It is now silent -- the
interruption is logged and not drawn.

### 3. The previous script appearing briefly

`RenderTask` already declines to push an interrupted canvas, so this was not a
partial frame -- it was a *completed* render whose result arrived after the
user had moved on, being handled as if it were current. `MainCtrlTask` now
ignores any render result for a script that is no longer the selected one:

    MainCtrl: Received render result for 'circuits'. Success: No, Interrupted: Yes
    MainCtrl: Ignoring stale render result for 'circuits' (now on 'city-2-by-telohtrab').

This is a narrower version of the render epoch/generation counter proposed
above -- comparing script ids rather than a monotonic epoch. It is enough for
the observed symptom and does not close the underlying race: two renders of the
*same* script id, queued back to back, are still indistinguishable. The epoch
remains the better design and remains unimplemented.

### Watchy: aborting mid-render

The display list renderer has had `setInterruptCheckCallback()` since the
beginning and the Watchy simply never supplied one. It now polls the four
buttons, and the raster loop breaks out **without calling `nextPage()`** --
which is what pushes the buffer to the panel, so an abandoned frame costs the
work but is never shown. The de-ghost counter is rolled back for a frame that
never appeared.

## Still open (unchanged by this work)

- The `main.cpp` clear-before-render race is **not** fixed. `RENDER_INTERRUPT_BIT`
  is still cleared before the render starts, so an interrupt requested in that
  window is still dropped.
- `FetchTask`'s analogous incomplete interrupt path is still not investigated.
- Canvas ownership between `showMessage` and `render()` is still not verified.

### Why the M5Paper then felt SLOWER than the Watchy

Removing the blocking delay made the M5Paper worse, not better, and the button
indicators lingered. Three causes, and the one everybody would have guessed --
the panel -- was the smallest.

Measured, title-to-title while paging:

| | before | after |
|---|---|---|
| whole cycle | ~1080 ms | **~230 ms** |
| of which: script selection | ~900 ms | ~60 ms |
| of which: panel update | ~250 ms (was ~600 as GC16) | ~50 ms (band) |

**1. Selection was doing ~900ms of SPIFFS work per step.** `selectNextScript()`
re-read `list.json` from flash and then re-validated every entry's fileId by
stat-ing its content file -- seven `SPIFFS.exists()` calls, ~500ms of the
total -- on every single press. The Watchy never did this: it holds its list in
RAM and steps an index. `ScriptManager` now caches the parsed list, dropping
the cache whenever the list is written. Dropped rather than updated, so a
failed or partial write can never leave RAM claiming something flash does not
say.

**2. The title was a full-panel GC16.** `showMessage(..., full_update=true)`
pushes all 540x960 with the slowest waveform, holding the panel mutex for the
whole of it. A title is transient black text on white and has no use for 16
greys. It is DU4 now, and after the first title of a browse burst it is a
*band* push -- 34 rows instead of 960, since the panel is already white.

**3. Indicators outlived their press.** An activity indicator is a region push
from `InputTask`, so it is only erased when something later covers that region.
While a render holds the canvas mutex `showMessage()` falls back to a banner
band that does not reach it, so the indicator sat there until the next full
push -- which, once title rendering was deferred by the settle timer, could be
seconds. `clearActivityIndicators()` now wipes it explicitly before each title,
and clears only the one position actually drawn (256 rows) rather than the
768-row strip all three share.

The general lesson, which cost the whole investigation: on this device
"it feels slow" points at the e-ink panel, and the panel was responsible for
less than a quarter of it. Filesystem work on the control path was four times
worse and completely invisible.

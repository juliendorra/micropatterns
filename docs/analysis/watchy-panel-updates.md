# Watchy: panel update policy (why it stopped flashing)

## The problem

Every render called `setFullWindow()`, which drives a **full refresh**: every
pixel is pushed through black and white. That is the flashing, and the user's
description was "very annoying".

It was also slow. `GxEPD2_154_D67` reports:

| | value |
|---|---|
| `full_refresh_time` | **2600 ms** |
| `partial_refresh_time` | **500 ms** |
| `hasFastPartialUpdate` | `true` |

So each render paid **5x the time** to produce an effect nobody wanted.

## Why the fast path is safe here

Fast partial updates degrade greyscale — that is the normal reason to avoid
them. **Our content is pure black and white.** `micropatterns_drawing.h` defines
`DRAWING_COLOR_WHITE = 0` and `DRAWING_COLOR_BLACK = 15`, and no script ever
emits anything else; the Watchy canvas maps those to `GxEPD_BLACK`/`GxEPD_WHITE`
with nothing in between. There is no grey to lose.

This is the identical argument to the M5Paper's move from `GC16` (16-grey,
450 ms) to `DU` (1-bit, 260 ms) — see `m5paper-panel-refresh.md`. Two different
panels, two different controllers, one shared reason: **the DSL is 1bpp, so
every greyscale-preserving cost is waste.**

## The policy

- Normal renders use `setPartialWindow(0, 0, W, H)` — full-screen area, fast
  waveform, no flash.
- Every `WATCHY_DEGHOST_INTERVAL`-th update is a full refresh to clear
  accumulated ghosting.
- **The first update after boot is forced full.** The panel's prior contents are
  unknown — e-ink retains its last image with no power, and this firmware does
  not clear at boot. InkWatchy carries the same first-boot workaround
  (`SCREEN_PARTIAL_GREY_WORKAROUND`) because partial updates issued onto an
  uninitialised panel can leave it grey.
- The explicit full-refresh gesture (top-left held 5 s) resets the budget.

### The interval is 24, and that number is MEASURED

It shipped at 8, was checked on the device across a full cycle, and the panel
stayed visibly clean — so the budget was needlessly conservative and was raised
to 24.

> **Do not copy this number to the M5Paper.** Its `SCRIPT_DEGHOST_INTERVAL` is
> also 8 but is still a *guess*: a different panel, a different controller and a
> different waveform, and its ghosting has never been eyeballed. The two
> constants look alike and mean different things.

**Caveat on the measurement:** it was taken over the six committed scripts. Large
solid black areas ghost harder than line art, so a future script could need a
lower value. Symptom is grey residue of previous frames accumulating; the fix is
to lower the constant.

## Title frames only on change

`showScript()` takes an `announce` flag. The script name gets its own cleared
frame **only when the script actually changes**. Re-running the current script
(bottom-left) and the full-refresh gesture skip it: you already know what you are
looking at, and the title was a whole extra panel update.

The title frame clears to white first, deliberately. Drawing the name over the
outgoing script's still-visible image is the bug reported on the M5Paper, where
the name appeared layered on the old art.

## What is not done

- **No dirty-rectangle tracking.** Every render rewrites the whole 200x200 area,
  because `DisplayListRenderer::render()` clears and redraws everything. Real
  regional updates would need a previous-frame copy and a diff. On a 200x200
  panel the win is small; on the M5Paper's 540x960 it would matter more, and it
  is listed there as rejected-for-now for the same reason.
- **No temperature compensation, no custom waveforms.** GxEPD2 selects the
  panel's built-in LUTs; we do not touch them. See
  `eink-fast-refresh-research.md` for what deeper control would require and why
  it is not reachable on a controller-driven panel.

# Why display-list generation got 82x faster on device

Measured, not estimated: generation across the six device scripts went from
**5024 ms to 61 ms** (medians, repeated runs, `tools/device/measure_render.py`).
The same change measured **21x** on the host harness. This explains the
mechanism, and why the two numbers differ by 4x.

> **Caveat, read this first.** The commit that produced these numbers
> (`7b9efaf`) also introduced a crash: `thunderstorms` hangs the firmware with a
> task-watchdog abort and a corrupted backtrace. Bisected on hardware — clean on
> the commit before, reliable failure on this one. The speedup is real and the
> measurements stand, but **this is not shippable until that is fixed.** Do not
> quote the win without the caveat.

## The old shape

`DisplayListItem` carried its parameters in a `std::map<String, int>` — a
red-black tree keyed by an Arduino `String` — plus an inline `matrix[6]`,
`inverseMatrix[6]` and `scaleFactor`, copied into **every** item.

Reading a parameter therefore meant: construct a `String` temporary from a
literal, hash nothing (it is a tree, not a hash map), walk O(log n) nodes
comparing strings, and destroy the temporary. Writing an item meant allocating
2-5 tree nodes, each with its own `String` key buffer.

For a 20,000-item script that is millions of tiny allocations and pointer
chases, purely to describe the drawing.

## The new shape

Three changes, none of them clever — all of them representational:

1. **`int32_t p[4]` slots** whose meaning is fixed by the item's `type`, with
   named accessors (`x()`, `y()`, `w()`, `radius()` …). No keys, no tree, no
   allocation.
2. **Interned integer slots** for runtime variables instead of
   `std::map<String,int>`. Name→slot interning happens once per token per
   render.
3. **A pooled `TransformSnapshot` pointer** instead of 52 bytes of matrix copied
   per item. A snapshot is appended only when the transform actually changed
   *and* differs bitwise from the previous one, so scripts that re-issue
   identical `RESET_TRANSFORMS`/`SCALE` inside a loop collapse to a couple of
   entries. Bitwise equality is what makes this safe: every item still sees
   exactly the floats it saw before.

Parameters now resolve by `const char*` and a linear `strcmp` over a map of at
most four entries — which beats a tree walk at that size, and allocates nothing.
Non-drawing commands (VAR/LET/IF/REPEAT/state) stopped constructing an item at
all.

## The numbers

| | before | after |
|---|---|---|
| bytes per item | 120 | **40** |
| heap allocations generating `city` | 3,566,180 | **58** |
| generation, `city` (host) | 242.5 ms | **11.3 ms** |
| generation, all six scripts (device) | 5024 ms | **61 ms** |

Per-item size was measured by compiling `sizeof` with the actual xtensa-esp32
`g++`, not assumed from the host.

## Why the device gain (82x) is 4x the host gain (21x)

This is the interesting part, and it is a property of the two `String` types:

- **Host `std::string` has small-string optimisation.** Short keys like `"X"`
  live inside the string object itself. They never touch the heap. So on the
  host the old code was doing tree walks but comparatively few allocations.
- **ESP32's `Arduino::String` always heap-allocates.** There is no SSO. Every
  one of those millions of key buffers was a real `malloc`/`free` on a slower
  allocator, potentially in PSRAM.

So the old code was doing work that *barely existed* on a laptop and
*dominated* on the device. The host harness under-measured the win by roughly
4x — which is the same directional bias it showed for the rasterizer work
(soft-float divide and PSRAM latency are also far cheaper on a laptop).

**The general lesson for this project:** the host harness is an excellent
*correctness* gate and a good *relative* instrument, but it systematically
under-reports wins that come from removing allocations or floating-point
division. When a change targets either, expect the device to do better than the
harness says — and measure it there before quoting a number.

## What this exposed

- **The golden corpus tested none of LINE/RECT/CIRCLE/PIXEL** — precisely the
  commands whose parameter slots were remapped. The gate was green throughout a
  change to the code it was least equipped to check. Corpus now covers them
  (15 goldens).
- **The harness had no header dependency tracking**, so a change to
  `micropatterns_command.h` left stale objects in `build/`. It surfaced as a bus
  error from a mixed binary; it could as easily have been a silently wrong
  measurement. Fixed with `-MMD -MP`.
- And the crash above, which the gate did not catch either.

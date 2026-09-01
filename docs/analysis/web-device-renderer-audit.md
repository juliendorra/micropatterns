# Auditing the claim that the web and device renderers are identical

The emulator and the firmware are supposed to be the same renderer in two
languages. Nothing checked it. The C++ side has had a golden-image gate since
the beginning (`make verify`); the JS side had none, so a divergence could only
be found by a human looking at two screens and noticing.

## How to check it (the answer to "what is the best way")

Run the **web** renderer headlessly over the **same corpus, same seeds, same
canvas size** as the C++ harness, and byte-compare against the **same golden
images**.

    cd tools/host_harness
    make audit-js              # web renderer vs C++ goldens
    make audit-js-occlusion    # does the web renderer's culling change its own output?
    ./build/mpharness compare-paths displaylist displaylist-noocc   # same, in C++

`js/render.mjs` does the web half. The emulator's modules are plain ES modules
that touch a very small slice of Canvas2D -- `fillStyle`, `fillRect`,
`beginPath`/`rect`/`fill`, `get`/`put`/`createImageData` -- so a ~60-line shim
runs them under Node with **no native canvas dependency**, which keeps this
runnable anywhere. Node has no `DOMMatrix` either; there is a small 2D affine
shim for it.

The shim deliberately **throws** on any canvas feature the emulator does not
currently use, so a future emulator change that reaches for a real canvas
feature fails loudly here instead of silently rendering something different
from the browser.

## What the audit found

### 1. Occlusion culling was corrupting the device's output

Culling must be **output-neutral**: it may only skip items it has proven are
completely covered. Rendering the corpus with and without it should be
byte-identical. In C++ it was not:

    9 of 15 golden images differed, up to 2488 pixels

and the culled render had *fewer* ink pixels (20,244 vs 22,732 on `prims`) --
i.e. culling was **deleting visible ink**. The goldens, baked with culling on,
encoded the bug.

The cause was how an item claimed screen area as opaque. The C++ marked a
rectangle derived from the shape's **area**, not from what it actually painted:

- `FILL_CIRCLE` marked a square of half-width `r*sqrt(pi/4) = 0.886r`. That
  square has the same area as the circle, but its **corners sit at 1.253r** --
  a quarter of a radius outside it. Those corner pixels are never painted.
- A rotated `FILL_RECT` marked its AABB scaled by `sqrt(fillFactor)` about the
  centre. Again area-matching; a rotated rectangle's inscribed axis-aligned box
  is not a centred scaling of its AABB.

Both marked unpainted pixels as opaque, so items behind those pixels were culled
and their ink disappeared.

**The web renderer never had this bug** -- it marks a block opaque only if every
pixel in it was actually painted (`OcclusionBuffer.updateFromPixelMap`, reading
the pixel occupation map). The C++ now does the same, using the 1-bit occupancy
map the drawing layer already maintains for overdraw skipping. After the change:

    C++  15 identical, 0 differing
    JS   15 identical, 0 differing

A first attempt at fixing the geometry instead -- inscribed radius `r/sqrt(2)`,
and skipping marking for rotated rects -- got `prims` from 2488 to 1852
differing pixels but not to zero, because the "ensure marking bounds are valid"
fallback silently reset a deliberately-empty box back to the full visual bounds.
Chasing the heuristic was the wrong approach; the pixel map is exact.

**Cost of the fix:** on this corpus, none and no benefit either. Culling now
fires on **0** items, where the unsound version "culled" 40 on `prims` -- all of
them wrongly. Steady-state timing for `city` is ~14.2ms either way (an early
33.7ms reading was first-run noise). A targeted probe confirms the machinery
still works when occlusion is genuinely provable: a full-screen solid
`FILL_RECT` drawn over a small one culls it.

That culling buys nothing here is worth understanding rather than filing away: a
**patterned** fill never fills a 16x16 block completely, so it can never occlude.
`city`, the heaviest script, is almost entirely patterned fills. Occlusion
culling is only ever going to pay on solid overdraw.

### 2. Two parser divergences, both of which made the device laxer

- **Digits in argument keys.** The web parser's key regex was
  `/^([a-zA-Z_]+)\s*=\s*/` -- no digits. `LINE` takes `X1`/`Y1`/`X2`/`Y2`, so
  **`LINE` was unparseable in the editor while it worked on the device**. The
  editor's own language reference does not document `LINE` at all, and no script
  on the server uses it, which is how it stayed hidden. Fixed by matching the
  firmware, which accepts `isalnum` throughout a key.
- **Quoted asset names.** The firmware accepts `FILL NAME=chk`; the web requires
  `FILL NAME="chk"`. Real scripts always quote, so nothing is broken in
  practice, but the corpus was written against the laxer parser. The corpus now
  uses the quoted form -- which changes no pixels -- so it is a *shared* corpus
  rather than a C++ one. **This divergence is still open**: the firmware should
  probably be made strict to match, but that risks rejecting scripts already
  stored on devices.

### 3. What actually still differs

After the occlusion fix, with both engines on their defaults:

    9/15 identical, 6 differing, 33,907 pixels

| script | diff px | nature |
|---|---|---|
| `city` (x3) | 0 | identical |
| `emulator_welcome` (x3) | 0 | identical |
| `nest` (x3) | 0 | identical |
| `artdeco_default` | 76-263 | edge pixels only, out of ~498,000 ink |
| `prims` (x3) | 11,164 | substantial |

**The common path is genuinely identical.** Patterns, fills, transforms,
`DRAW`, the display-list generator -- byte for byte, across three seeds.

The divergence is confined to the outline primitives. Isolating them
(`tools/host_harness/probe/corpus/p_*.mp`, occlusion off on both):

| primitive | differing px | C++ ink | JS ink |
|---|---|---|---|
| `LINE` | 5,097 | 4,651 | 4,650 |
| `CIRCLE` | 3,013 | 3,114 | 3,091 |
| `RECT` | 2,966 | 4,025 | 3,873 |
| `PIXEL`/`FILL_PIXEL` | 36 | 96 | 96 |

For `LINE` and `CIRCLE` the ink *totals* match almost exactly while nearly every
pixel differs -- the two engines draw the same amount of line in different
places. That is a rasterization-algorithm difference under rotation and scale
(which Bresenham variant, where the rounding happens), not a missing feature.
`RECT` differs in total ink too.

These are exactly the primitives that had **no golden coverage at all** until
`prims.mp` was added. They were never compared because nothing rendered them.

## The other answer: compile the firmware renderer to WebAssembly

Auditing the gap between two implementations is worth doing, but the gap only
exists because there *are* two. The device's renderer runs in the browser as-is:

    cd tools/host_harness
    make wasm            # build it
    make verify-wasm     # byte-compare it to the same goldens

**Result: 15/15 identical to the C++ goldens.** Not "close" -- the same bytes,
because it is the same code.

This was nearly free, and the reason is worth stating: the host harness had
already done the hard part. It compiles the six core files **verbatim** from the
firmware tree (2,903 lines) against a 326-line shim, with no Arduino, no
FreeRTOS and no hardware -- the single ESP-specific include in the core is
already `#if defined(ARDUINO_ARCH_ESP32)`-guarded. Emscripten consumes that
same file list. The output side needed no translation either: `render_path.cpp`
already hands back a flat `width*height` greyscale buffer, which is an
`ImageData` blit away from a canvas.

    mp_render.wasm   87 KB
    mp_render.mjs    13 KB   (Emscripten glue, ES module)

Rasterization in the browser is not a compromise: `city`, the heaviest corpus
script at 20,737 display-list items, rasterizes in **4.6ms**.

Exceptions and RTTI are off in the WASM build, matching Arduino. Leaving them on
would let code compile here that cannot run on the device -- exactly the class of
divergence this exercise exists to remove.

What it buys beyond pixel fidelity: the preview would use the **firmware's
parser**, so both parser divergences above disappear from the preview path
without touching the JS parser at all.

What it does not solve:

- It does not delete the JS renderer, and should not. The editor needs fast
  incremental feedback while typing, and the JS parser drives linting and
  autocomplete. WASM makes the JS renderer stop being the *source of truth*, not
  redundant.
- Error messages and editor diagnostics still come from the JS parser unless
  those are routed through WASM too. Two parsers is the deeper problem.
Two caveats I stated when first proposing this were **wrong**, and checking them
turned up a real bug:

- *"It is 960x540 4bpp; a Watchy preview needs the 200x200 1bpp canvas
  modelled."* No. `mp_render()` already takes width and height, and the
  4bpp/1bpp distinction does not exist for this DSL: scripts only ever produce
  `DRAWING_COLOR_WHITE` (0) and `DRAWING_COLOR_BLACK` (15), which is why
  `watchy_canvas.h` says the M5Paper's nominal 4bpp model maps onto a
  monochrome panel "with no loss whatsoever". Verified: WASM at 200x200 is
  byte-identical to native C++ at 200x200 across the corpus.
- *"It cannot simulate the Watchy dropping its occupancy map."* It can now --
  and that turned out to matter. See below.

Emscripten is not on `PATH` on this machine. `wasm/build.sh` falls back to the
one vendored at `qemu-ipod_touch_1g/.wasm-toolchain/emsdk` (4.0.10); set `EMSDK`
to override. Build output is gitignored -- it is reproducible, and shipping it
to the editor is a separate decision.

## The occupancy map is load-bearing for correctness, not speed

Chasing the caveat above found a real bug on the Watchy.

`DisplayListRenderer::render()` walked the display list **front-to-back**
(reverse of painter's order) and relied on `emitPixel()` skipping any pixel
already written this pass, so the first writer wins and the first writer is the
front-most item. Remove the occupancy map and the skip disappears:
later-iterated items -- the ones BEHIND -- overwrite the ones in front.

The Watchy really does remove it. The map is 5KB there, the heap fragments to a
~26KB largest free block mid-render, and `initPixelOccupationMap()` drops it and
carries on -- under a comment claiming that costs *"speed, not correctness"*.

Measured with a `displaylist-nomap` harness path that simulates exactly that:

    0 identical, 15 differing      <- every image in the corpus

It cost correctness. `render()` now checks whether the map is live and falls
back to plain painter's order, back-to-front, where overwriting is the intended
behaviour and no map is needed. Occlusion culling is disabled in that mode,
since "is this already covered by something nearer?" is always no in painter's
order. After the fix:

    15 identical, 0 differing

A memory-starved Watchy now draws the same picture, just slower. The misleading
comment is corrected in place.

## Still open

- Reconcile `LINE`/`RECT`/`CIRCLE`/`PIXEL` rasterization between the two
  engines. Deciding which is *correct* is a product question -- the device is
  the thing people look at, so the web should probably follow it.
- `artdeco_default`'s 76-263 edge pixels: unexplained, small, plausibly float
  rounding in the transform stack. Note the emulator copies matrices with
  `new DOMMatrix(otherMatrix)`, which in a browser goes through the
  **CSS-string** arm of the constructor's union -- a serialise/reparse in the
  middle of the transform stack. Worth ruling in or out.
- Make the firmware's parser strict about quoted asset names, or the web lax.
- The audit runs at 960x540 only. The Watchy renders 200x200 and is not covered.

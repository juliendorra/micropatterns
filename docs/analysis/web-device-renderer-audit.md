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

## Which renderer is right where they differ: the device

The divergence turned out not to be two different algorithms. It is one
rounding convention.

The firmware converts a transformed coordinate to a pixel with `round()`
(`micropatterns_drawing.cpp`: line endpoints, rect corners drawn as four lines,
circle centre and radius -- all `round()`). The web renderer used
`Math.trunc()`. `round(19.6) = 20`, `trunc(19.6) = 19`, which is exactly the
symptom: at the same y, the web drew rotated lines a consistent **one pixel to
the left**.

    cpp-only pixels: (20,46) (22,49) (24,52) (45,84) ...
    js-only  pixels: (19,46) (21,49) (23,52) (44,84) ...

Truncation is not a defensible tie: it biases every coordinate toward the
origin, and asymmetrically so across zero. Round-to-nearest is correct. **The
device is right here and the web should follow it.**

Isolating it confirmed the diagnosis -- an unrotated line and a scaled line were
already byte-identical between the two engines; only rotation differed, which is
the only case where the transformed coordinate lands off a pixel centre.

Changing `_rawLine` alone -- four `Math.trunc` to `Math.round`:

| | before | after |
|---|---|---|
| whole corpus | 33,907 px | **17,290 px** |
| `LINE` probe | 5,097 px | **0** |
| `RECT` probe | 2,966 px | 2,267 px |
| `CIRCLE` probe | 3,013 px | 3,013 px |
| `PIXEL` probe | 36 px | 36 px |

`drawing.js` has 33 `Math.trunc` sites, but only the ones that PLACE a
transformed coordinate should round. The ones that compute a clipping box stay
truncating -- `fillRect`'s canvas clamp, `fillCircle`'s and `drawAsset`'s
iteration bounds -- because rounding a lower bound upward clips content, and the
firmware uses `floor`/`ceil` for exactly those. Sites changed: `_rawLine`
(4), `drawCircle` centre and radius (3), `setPixel` (2).

Finishing the alignment closed almost all of it:

| | before | after |
|---|---|---|
| whole corpus | 33,907 px | **1,228 px** |
| `LINE` | 5,097 | **0** |
| `CIRCLE` | 3,013 | **0** |
| `RECT` | 2,966 | **0** |
| `PIXEL` | 36 | 6 |

### RECT was the device's bug, not the web's

`RECT` did not close by rounding. The two engines disagreed on the rectangle's
*extent*: the firmware transformed corners at `lx + lw`, the web at
`x + width - 1`. Measured with `RECT X=10 Y=10 WIDTH=20 HEIGHT=20`:

    RECT      cpp  x 10..30  (21 wide)      js  x 10..29  (20 wide)
    FILL_RECT cpp  x 10..29  (20 wide)      js  x 10..29  (20 wide)

`WIDTH` is a pixel count, so 20 is right -- and the firmware's own `FILL_RECT`,
in the same file, already agreed. Its outline was one pixel wider than its fill
for identical arguments. **The web was correct here and the device was not**,
which is worth stating plainly: the direction of the fix is not uniform, and
"the device is the reference" is a rule about which to converge on, not a claim
that it is right in every case.

### What is left

`PIXEL` keeps a 6-pixel residual and it is not a rounding difference. The two
engines use different algorithms: the firmware transforms the 1x1 logical
pixel's corners, takes the screen AABB, and inverse-maps every pixel in it to
test membership; the web transforms one point and stamps a `scale x scale`
block. They agree unrotated and diverge slightly under rotation. Closing it
means porting one algorithm to the other, which changes how PIXEL renders for
real scripts -- a decision, not a cleanup.

`artdeco_default` keeps 76-263 pixels, still unexplained, still plausibly float
rounding in the transform stack.

## Editor diagnostics can come from the firmware parser too

The editor's linting comes from the JS parser, which is a second implementation
of the language and has already been caught disagreeing with the firmware --
it rejected `LINE`'s `X1`/`Y1`/`X2`/`Y2` outright. The WASM build now exports
the firmware's own parser:

    mp_parse(src) -> 0/1
    mp_parse_error_count()
    mp_parse_error_at(i) -> "Line N: message"

Working, through WASM:

    good script        ok=1  errors=0
    unknown command    ok=0  Line 2: Unknown command: FLOOP
    bad argument       ok=0  Line 2: Missing value for parameter 'Y'.
    undeclared var     ok=0  Line 1: Cannot assign to undeclared variable: $nope

The `Line N:` prefix parses trivially into CodeMirror lint markers. That would
give the editor the same verdict the device reaches, and remove the second
parser from the correctness path entirely -- it would still be wanted for
autocomplete and syntax highlighting, which need a token stream rather than a
verdict.

## Integer width: the device is 32-bit, the web was not

Relayed from another session and confirmed here. The C++ runtime evaluates every
expression as `int` (`micropatterns_runtime.cpp`, `evaluateExpressionRange`);
JavaScript numbers are doubles. The two agree right up until a script overflows,
and then they disagree **silently** -- no error on either side.

Probe (`corpus/i32.mp`):

    VAR $a = 100000
    LET $a = $a * 100000      # 10,000,000,000 -- does not fit in int32
    LET $a = $a % 97
    FILL_RECT X=$a ...

    device (int32 wrap)   1410065408 % 97 = 76
    web    (double)      10000000000 % 97 = 49

The rect landed 27 pixels apart. This is not a corner case: scripts that seed a
pseudo-random sequence from `$SECOND`/`$COUNTER` multiply exactly like this, so
it is the mechanism by which a script that looks right in the editor renders
differently on the watch.

The device is the reference -- it is the target the language exists for -- so
the web now emulates its width. `int32.js` holds the operations and both JS
evaluators (`display_list_generator.js` and `runtime.js`) use them.
`Math.imul()` is required for multiplication: `a * b | 0` is wrong for large
operands, because the double product loses low bits *before* the truncation.

`corpus/i32.mp` is now a permanent corpus entry covering both positive and
negative wrap, so the harness gates this rather than trusting it.

Note for the C++ side: signed overflow is formally undefined behaviour. Every
compiler targeting these devices wraps in practice, and that wrap is now the
specified behaviour of the language, so it would be worth making it explicit
(compute through `uint32_t` and cast back) rather than relying on it.

## A bug class none of these gates can see

Raised by another session, verified here, and worth recording because it sits on
a different axis from everything above.

Every gate in this document compares **single frames for equality** -- against a
golden, against another render path, against the other language. A script can
pass all of them and still be wrong in a way nobody notices: render a
byte-perfect frame every time, and repeat itself every 32 draws.

The usual cause is a pseudo-random sequence read through its low bits. For a
linear congruential generator with a power-of-two modulus -- including the
implicit 2^32 of `int` arithmetic -- bit k repeats with period 2^(k+1).
Measured, for `seed = seed * 1103515245 + 12345` mod 32768:

    bit 0: period 2      bit 4: period 32
    bit 1: period 4      bit 5: period 64
    bit 2: period 8      bit 6: period 128
    bit 3: period 16     bit 7: period 256

So `($seed / 8) % 4` reads bits 3 and 4 and cannot beat period 32, no matter how
many draws are taken. Reducing modulo a **prime** (32749 rather than 32768)
mixes the high bits back in.

`mpharness cycle <script.mp> --draws N` renders N consecutive `$COUNTER` values
and reports distinct frames and the repeat period, exiting non-zero if the
script cycles within the window. Two scripts differing only in the modulus:

    modulus 32768   distinct frames 4 of 64   repeats every 32 draws   exit 1
    modulus 32749   distinct frames 4 of 64   no repeat within 64      exit 0

`cycle` catches a script that repeats. It cannot catch one that never repeats
and still only reaches six of its ten variations -- every frame genuinely
differs, there are just fewer of them than there should be. That needs a
different question, so `mpharness sweep <script.mp> --levels N` asks it: sweep
the clock rather than the counter, and tally what comes out. The script must
encode one integer as its **non-white pixel count** (draw N isolated pixels for
choice N), which makes the frame directly countable.

Proven to detect what it claims, with a script whose clock-derived seed only
ever sets high bits:

    2:16.7%  4:25.0%  6:29.2%  8:4.2%  10:25.0%
    distinct values 5     unreachable of 1..10:  1 3 5 7 9

Every odd level unreachable -- the signature of an affine low-bit map. That
probe is kept (`probe/corpus/sweep_bad.mp`) precisely so the tool has a case it
must keep failing.

`make audit-sweep` runs the whole set with **per-case expected exit codes**,
because two of them must fail and a run-everything-expect-zero loop would call
those broken. A negative test is only a test if something checks it still goes
negative.

It also runs `probe/check_city_block.sh`, which re-extracts the zoom block from
`examples/scripts/city.mp` and from each probe and diffs them. The DSL has no
include, so the copy cannot be removed -- but it can be made loud instead of
silent. Editing the source now fails the gate rather than quietly invalidating
the probes.

That check found real drift the moment it existed: both probes were missing two
clamp guards (`IF $max_scaling < $min_scaling` and `IF $rare_from < 1`) present
in the source. At 200x200 both are no-ops, which is exactly why the numbers
still matched and nobody noticed -- the probes were not verbatim and would have
diverged from the real script at any other sweep size. Restored; sweep results
unchanged.

Its own first run reported drift that was not there, too: `s/[[:space:]]\+/ /g`
is a BSD-sed trap, where `\+` is a literal plus rather than a repetition
operator, so the normaliser was eating every `+` in the arithmetic.

`cycle` deliberately does **not** fail on a low distinct-frame count. A script that
legitimately picks one of four positions has four distinct frames however often
it is drawn; flagging that would make the tool cry wolf on correct scripts. The
period is the signal, the count is context.

Two traps found while building it, both worth knowing before writing such a
script:

- **C's `%` keeps the sign of the dividend.** The multiply overflows int32 into
  negatives, so `$seed % 32768` is negative half the time and the sequence is
  not an LCG at all -- it is an LCG plus a sign flip, which destroys the
  periodicity. The probe scripts add the modulus and reduce again to force a
  non-negative state. A first attempt at simulating this in Python got the
  opposite answer because Python's `%` returns non-negative; the tool was right
  and the model was wrong.
- Two related claims from the same report -- unreachable choices and a heavy
  concentration on a few of them -- did not reproduce **in my measurement, which
  measured the wrong quantity**. I took 400 consecutive draws from one seed and
  found all ten levels present. That test cannot show the bug and never could:
  over a full period an LCG visits every state exactly once, so any function of
  the state comes out uniform. A long stream is guaranteed to look fine.

  The quantity that matters for a script like `city.mp` is different. It
  **re-seeds from the clock on every render** and consumes one or two draws,
  then stops. So what is being sampled is the seed-to-first-output map over the
  seed set the clock actually produces -- and that seed set is itself a linear
  function of the clock. With a power-of-two modulus the low bits of the first
  output are an affine function of the low bits of the seed, so structure in the
  seed set survives into the output; a prime modulus mixes across all bits and
  destroys it.

  Their measurement, taken on this harness rather than from a model: 384 renders
  sweeping hour/minute/second/counter, level encoded as a pixel count.

      M=32768   3, 4, 7 and 9 unreachable; 2/5/8 take 89% of renders
      M=32749   none unreachable

  Reproduced here on `city.mp`'s **real** zoom block, carried verbatim into
  `probe/corpus/sweep_city_*.mp`: mod 32768 gives 6 distinct values with 3, 4, 7
  and 9 unreachable; mod 32749 gives all ten. Percentages agree with their
  independent sweep to within 0.3 points.

  A sweep of my own, same shape but with a seed construction **I invented**,
  showed no unreachability at either modulus -- and that reconstruction is kept
  in the gate as a PASSING case, because it is the sharper lesson:

  > A probe that reconstructs the script under test is testing the
  > reconstruction. The reconstruction passes at both moduli while the real
  > block fails at one. Copy the block; do not paraphrase it.

  It also means a power-of-two modulus is not on its own enough to produce the
  fault. It needs a seed set with the right structure, and the clock supplies
  one. Nothing weaker than the real script would have shown that.

  Recorded at length because the original entry here asserted the opposite with
  more confidence than the evidence supported. The scepticism was fair about
  what had been *written* -- it read as a claim about a stream -- but the
  conclusion drawn from it was wrong.

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

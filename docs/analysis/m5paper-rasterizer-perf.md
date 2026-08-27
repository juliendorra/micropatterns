# M5Paper rasterizer — performance analysis

Scope: the per-pixel rasterization hot path only.
`micropatterns_drawing.{h,cpp}`, `display_list_renderer.{h,cpp}`, `occlusion_buffer.{h,cpp}`,
`matrix_utils.{h,cpp}`, `micropatterns_command.h` (DisplayListItem), `display_manager.{h,cpp}`.

Interpreter / display-list *generation* and toolchain flags are covered by other analyses. Where a
proposal crosses those boundaries it is flagged `[cross-boundary]`.

**All speedup figures in this document are relative to RASTERIZATION time, not to total script
time.** `render_controller.cpp:60-93` already brackets the two phases separately with `millis()`
and logs both (`"Display list generation for '%s' took %lu ms"` and `"Display list rendering for
'%s' took %lu ms"`). Until a serial log with that split arrives, the fraction of the observed
10–19 s that this document can address is unknown. Every estimate below is written so it can be
checked against that split.

---

## 0. Method — this is measured, not guessed

The repo contains a built object file at
`M5Paper_MicroPatterns/.pio/build/m5stack-fire/src/micropatterns_drawing.cpp.o`, and the M5EPD
library source is vendored at `M5Paper_MicroPatterns/.pio/libdeps/m5stack-fire/M5EPD/`. So the
inner loop could be disassembled with `xtensa-esp32-elf-objdump` rather than reasoned about from
source. Everything in §1 is read off the actual generated Xtensa code. Where I am inferring rather
than reading, I say so.

Build context (from `platformio.ini`): `-Os` (PlatformIO Arduino default, no explicit `-O` flag),
`-DBOARD_HAS_PSRAM`, `-mfix-esp32-psram-cache-issue`, `-DCORE_DEBUG_LEVEL=5`. No LTO. The object
file also references `__stack_chk_fail`, so `-fstack-protector` is on.

Canvas geometry: `display_manager.cpp:46` creates `540 x 960` → 518,400 px, 4 bpp,
`_bytewidth = 270`, framebuffer 259,200 bytes.

---

## 1. Cost model of the inner loop (read from disassembly)

### 1.1 What one pixel of `fillRect` actually costs

The inner loop is `fillRect+0x24c` … `fillRect+0x2e6`. Annotated:

| Step | Evidence (offset in `fillRect`) | Est. cycles @240 MHz |
|---|---|---|
| `std::function` interrupt check — real windowed `callx8`, once per pixel | `0x24e bnez` → `0x30f callx8 std::function<bool()>::operator()` | ~25 |
| `pixelCount % 2000` — GCC emitted a magic-multiply chain (`muluh`+shifts), *not* `remu` | `0x321`–`0x338`, 8 instrs | ~10 |
| `int→float` ×2 (`float.s`), `+0.5f` ×2 (`add.s`) | `0x254`–`0x263` | ~8 |
| `matrix_apply_to_point` — **out-of-line windowed call**, separate TU, no LTO | `0x283 callx8` | ~45 |
| 4× `mul.s` recomputing `lx*scaleFactor`, `(lx+lw)*scaleFactor`, … | `0x28c`, `0x2a1`, `0x2ad`, `0x2bc` | ~25 |
| 4× `ole.s`/`olt.s` + `bf`, operands **reloaded from stack** each iteration (`lsi f1, a1, 16`) | `0x292`–`0x2c2` | ~20 |
| `getFillColor` call (patterned fill only) — see §1.2 | `0x2d3 callx8` | ~250 |
| `rawPixel` call — see §1.3 | `0x2e1 callx8` | ~110 |
| `memw` barriers from `-mfix-esp32-psram-cache-issue` | `0x27d`, and in `rawPixel` | ~5 |
| **Total, drawn pixel, patterned fill** | | **~500–600** |
| **Total, pixel rejected by the shape test** (inside AABB, outside shape) | | **~140–170** |

~500–600 cycles ≈ **2.1–2.5 µs per drawn pixel**. One full-screen patterned fill ≈ **1.1–1.3 s**.
The observed 10–19 s is therefore consistent with roughly 8–15 full-screen-equivalents of drawn
pixels — entirely plausible for a dense script like art-deco-3. The model and the measurement
agree, which is the main reason I trust the model.

### 1.2 `getFillColor` — the single most expensive thing per pixel

Disassembly of `MicroPatternsDrawing::getFillColor`, with relocations:

```
0x36  callx8 -> MicroPatternsDrawing::screenToLogicalBase   (windowed call)
0x3e  callx8 -> floorf                                       (windowed call!)
0x49  rems   a4, a10, a2        <- asset.width  modulo, hardware but ~30cy latency
0x51  callx8 -> floorf                                       (windowed call!)
0x5c  rems   a10, a10, a8       <- asset.height modulo
0x69  mull   a2, a2, a10
```

and `screenToLogicalBase` in turn:

```
0x11  callx8 -> matrix_apply_to_point   (out-of-line, separate TU)
0x3d  callx8 -> __divsf3                (SOFT FLOAT DIVIDE)
0x4a  callx8 -> __divsf3                (SOFT FLOAT DIVIDE)
```

**Confirmed by `nm -u`: the object's only undefined math symbols are `__divsf3`, `floorf`,
`sqrtf`.** The Xtensa LX6 FPU has *no* single-precision divide instruction (only a reciprocal
approximation that GCC will not use without `-ffast-math`), so every `x / item.scaleFactor` is a
libgcc software routine — roughly 50–60 cycles each, plus windowed-call overhead.

So one `getFillColor` = 1 nested call chain + **2 `__divsf3`** + **2 `floorf` calls** + 2 `rems` +
a `mull`, ≈ 250 cycles. And `fillCircle` / `drawAsset` call `screenToLogicalBase` a *second* time
per pixel on top of that — those primitives pay **4 `__divsf3` per pixel**.

The dividing is pure waste. `scaleFactor` is constant per item. See proposal **P1**.

### 1.3 `rawPixel` — three nested calls per pixel, one of them virtual

```
0x29  callx8 -> isPixelOccupied     (out-of-line; re-does the bounds check and the
                                     _usePixelOccupationMap / .empty() tests)
0x42  callx8 -> markPixelOccupied   (out-of-line; re-does them AGAIN)
0x4d  l32i a2, a10, 0     <- load vtable pointer
0x4f  l32i a2, a2, 24     <- load slot 6
0x51  callx8 a2           <- VIRTUAL CALL to M5EPD_Canvas::drawPixel
```

The virtual dispatch is real: `TFT_eSprite::drawPixel` is declared `virtual` at
`utility/Sprite.h:36`, `M5EPD_Canvas::drawPixel` overrides it, `M5EPD_Canvas` is not `final`, so
GCC cannot devirtualize. Confirming suspicion #4 exactly.

And `M5EPD_Canvas::drawPixel` itself (`M5EPD_Canvas.cpp:324`) then re-does the bounds check a
*third* time, does `y * _bytewidth + (x>>1)`, and performs a **read-modify-write on a PSRAM
byte** to pack the nibble.

So one logical "set a pixel" = 4 bounds checks, 3 windowed calls, 1 vtable indirection,
1 PSRAM byte read + 1 PSRAM byte write for occupancy, 1 PSRAM byte RMW for colour.

### 1.4 Where the buffers live

- **Canvas framebuffer**: `M5EPD_Canvas::callocSprite` (`M5EPD_Canvas.cpp:138`) uses `ps_calloc`
  when `psramFound() && _usePsram`, and `_usePsram` defaults to `true` (`utility/Sprite.h:152`).
  259,200 bytes → **PSRAM**.
- **`_pixelOccupationMap`**: `std::vector<uint8_t>` of 540×960 = **518,400 bytes**. That cannot
  fit in ESP32 internal DRAM (320 KB total, much of it consumed by WiFi + the Arduino heap), so
  under arduino-esp32's default `CONFIG_SPIRAM_USE_MALLOC` it necessarily lands in **PSRAM** too.
  *Uncertainty: I could not verify this at runtime from reading alone. It should be confirmed with
  `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` before and after `initPixelOccupationMap()`, or by
  printing `esp_ptr_internal(_pixelOccupationMap.data())`. If the allocation is silently failing,
  the map is doing nothing at all and that is a separate bug.*

PSRAM on M5Paper is quad-SPI at 40 MHz ≈ 20 MB/s, cache line 32 B, miss latency in the low
hundreds of cycles. Scanline-coherent access amortizes it (1 byte/px occupancy = 32 px/line,
0.5 byte/px colour = 64 px/line), so raw memory is ~20–40 cycles/px — real but *not* currently the
dominant term. It becomes dominant only after the arithmetic is fixed, which is why §3 tier 3
matters.

Also: `resetPixelOccupationMap()` does `std::fill` over 518,400 PSRAM bytes at the start of every
render ≈ **26 ms**. Once per frame, so acceptable, but it drops to ~3 ms with a 1-bit map.

### 1.5 Suspicion #6 — `intParams.at("X")` — is NOT a hot-path problem

`std::map<String,int>::at` with `String::operator<` comparisons is genuinely slow (~1 µs), but
every call site is **outside** the pixel loops: once at the top of each primitive, plus ~4 more in
`calculateScreenBounds`, plus `determineItemOpacity`. Call it ~10 lookups per display-list item.
Even at 5,000 items that is ~50 ms — under 0.5% of the render. **Do not spend effort here for
rasterization reasons.** (The `std::map` + `String` members do make `DisplayListItem` fat and
expensive to *construct*, which matters for display-list generation — `[cross-boundary]`, the
interpreter analysis owns that.)

### 1.6 Suspicion #7 — fixed point — mostly the wrong lever

The ESP32 LX6 FPU handles add/mul/compare/convert in ~1–3 cycles, and after the span rewrite
(P4) there is almost no per-pixel float arithmetic left to convert. Q16.16 wins in exactly one
place: stepping asset/pattern texture coordinates, where fixed point gives you the integer index
by a shift instead of `floorf` + `trunc.s`. That is folded into P2/P4. A wholesale float→fixed
port would be large, risky for parity, and would win little on top of the span rewrite.

The one float operation that *is* catastrophic is division (`__divsf3`), and P1 deletes all of it
without changing number formats.

### 1.7 e-ink push — the hard floor `[cross-boundary]`

`M5EPD_Driver` runs SPI at 10 MHz (`M5EPD_Driver.cpp:22`). `pushCanvas` sends all 259,200 bytes:
~0.21 s of SPI, plus panel refresh time in `UpdateArea`. So there is a **floor of roughly
0.3–2 s** on total script time no matter how fast the rasterizer becomes. Getting rasterization to
~1 s means further rasterizer work has sharply diminishing returns on the number the user actually
feels. Worth knowing before over-investing.

---

## 2. On the occlusion buffer and reverse-order painting — keep them

The git history shows the display-list architecture *won a measured comparison* against the
compiler path (`d1ea9e7 "Compiler is default"` → `82906d5`/`01ded93`/`e4d18d9` build the display
list → `d427b02 "All path gives same result"` establishes output equivalence → `68d843e "Default
to display list"`). Reverse-order painting plus culling is very plausibly *why* it won. This
analysis treats both as load-bearing and **does not propose removing either**.

What it does propose is making them cheaper:

- **`OcclusionBuffer` (16×16 blocks, 34×60 = 2,040 bytes) is already cheap and effective.** It is
  per-*item*, not per-pixel; a full check scans at most 2,040 bytes. It is not a bottleneck and
  should be kept as-is. If anything its block size could go *down* to 8 for finer culling at
  4× the (still trivial) memory.
- **The per-pixel occupancy map is the thing that costs.** Its *semantics* (first writer in
  reverse order wins) must be preserved exactly. Its *representation* — 1 byte per pixel in PSRAM,
  accessed through two out-of-line function calls per pixel — is what should change. P3 keeps the
  semantics bit-for-bit and changes only the representation.

**Is the occupancy map earning its keep today?** Partially. `_overdrawSkippedPixels` is already
logged at the end of `DisplayListRenderer::render` — the answer is in the existing serial output.
The break-even is: it pays for itself when the fraction of skipped pixels exceeds roughly
`cost(check+mark) / cost(full draw)` ≈ 70/570 ≈ **12%**. Art-deco-style scripts with heavy
background overdraw are almost certainly well above that, so it is very likely a net win *today*.
But note the trap: after P1/P2 cut the per-pixel draw cost by ~4×, the break-even rises to ~40%,
and the map could flip to net-negative *in its current 1-byte-PSRAM form*. That is an argument for
P3 (make the check nearly free), not for deletion.

**A divergence worth noting:** the JS emulator moved past the C++ here. `display_list_renderer.js`
calls `occlusionBuffer.updateFromPixelMap(...)` — it updates the occlusion grid from the pixels
that were *actually painted*. The C++ (`display_list_renderer.cpp`, `calculateScreenBounds`) still
uses the older heuristic `markingBounds` shrink factors (`sqrtf(M_PI/4)` for circles, `fillFactor`
for rotated rects). The two are not equivalent, so device and editor can already cull differently
in edge cases. That is a pre-existing parity gap independent of any change proposed here.

---

## 3. Proposals

### Tier 1 — surgical, low risk, no semantic change (≈1 day)

---

#### P1. Fold `scaleFactor` into the inverse matrix — deletes every `__divsf3`

**Mechanism.** Forward: `screen = matrix · (scale · logical)`. Inverse:
`logical = (1/scale) · matrix⁻¹ · screen`. Since `(1/scale)` is a uniform scalar, it composes into
the inverse matrix by multiplying all six entries. So build `inverseMatrixBase` once, at
display-list build time, and the per-pixel divide vanishes.

Before (`micropatterns_drawing.cpp`, called 1–2× per pixel):

```cpp
void MicroPatternsDrawing::screenToLogicalBase(float sx, float sy,
        const DisplayListItem& item, float& blx, float& bly) {
    float slx, sly;
    matrix_apply_to_point(item.inverseMatrix, sx, sy, slx, sly);   // out-of-line call
    if (item.scaleFactor == 0.0f) { blx = slx; bly = sly; }
    else { blx = slx / item.scaleFactor;      // __divsf3  ~55 cy
           bly = sly / item.scaleFactor; }    // __divsf3  ~55 cy
}
```

After — in `micropatterns_command.h`, alongside the existing `inverseMatrix`:

```cpp
struct DisplayListItem {
    ...
    float inverseMatrixBase[6];   // == (1/scaleFactor) * inverseMatrix

    void finalizeMatrices() {                 // call once when the item is built
        const float inv = (scaleFactor == 0.0f) ? 1.0f : 1.0f / scaleFactor;
        for (int i = 0; i < 6; ++i) inverseMatrixBase[i] = inverseMatrix[i] * inv;
    }
};
```

and in `matrix_utils.h`, make the apply inlinable across TUs (it is currently a real call because
it lives in `matrix_utils.cpp` and there is no LTO):

```cpp
static inline __attribute__((always_inline))
void matrix_apply_to_point(const float M[6], float x, float y, float& ox, float& oy) {
    ox = M[0]*x + M[2]*y + M[4];
    oy = M[1]*x + M[3]*y + M[5];
}
```

Then `screenToLogicalBase` becomes one inlined 4-mul/2-add expression, and the shape tests compare
against plain `lx`/`lx+lw` instead of `lx*scaleFactor` — which also removes the four un-hoisted
`mul.s` at `fillRect+0x28c…0x2bc`.

**Speedup.** Removes ~130 cy per `screenToLogicalBase` call. Patterned `fillRect`: −130. `fillCircle`
and `drawAsset`: −260 each (they call it twice). On the ~570 cy baseline that is a **1.3× on
`fillRect`, ~1.8× on `fillCircle`/`drawAsset`** — call it **~1.4–1.6× of rasterization time**
overall, from about 30 lines of code.
**Effort: S. Visual risk: essentially nil** — one rounding step is removed rather than added, so
results are equal to within 1 ulp; a pixel can only differ where a sample lands exactly on a shape
boundary.
**Emulator parity:** JS does the same divide in `drawing.js:163 screenToLogicalBase`. Mirroring is
*optional* (the values agree to 1 ulp) but cheap and recommended for exactness. Note JS uses
doubles, so device/emulator already differ by more than this at the last bit.

---

#### P2. Get the interrupt check, the yield bookkeeping, and `floorf` out of the pixel loop

**Mechanism.** Three independent removals, all in the same loops.

```cpp
// --- interrupt: today, a windowed callx8 through std::function, PER PIXEL
for (...) {
    if (_interrupt_check_cb && _interrupt_check_cb()) return;      // per row
    for (...) {
        if (_interrupt_check_cb && _interrupt_check_cb()) return;  // PER PIXEL, ~25 cy
        if (pixelCount > 0 && pixelCount % 2000 == 0) {            // magic-mul chain, ~10 cy
            yield();
            if (pixelCount % 8000 == 0) esp_task_wdt_reset();
        }
        pixelCount++;
```

The callback body is only `return _interrupt_requested_for_runtime_or_renderer;`
(`render_controller.cpp:14`) — a single `volatile bool` load, wrapped in ~25 cycles of
`std::function` machinery. Replace the whole mechanism with a pointer to the flag:

```cpp
// micropatterns_drawing.h
const volatile bool* _interrupt_flag = nullptr;   // set once, replaces std::function
void setInterruptFlag(const volatile bool* f) { _interrupt_flag = f; }

// hot loop
int rowsUntilYield = 8;
for (int sy = min_sy; sy < max_sy; ++sy) {
    if (__builtin_expect(_interrupt_flag && *_interrupt_flag, 0)) return;   // per SCANLINE
    if (--rowsUntilYield == 0) { rowsUntilYield = 8; yield(); esp_task_wdt_reset(); }
    for (int sx = min_sx; sx < max_sx; ++sx) {
        /* no check, no counter, no modulo */
    }
}
```

A scanline is at most 540 px ≈ 90 µs after Tier 1 — far below any watchdog or responsiveness
threshold, and yielding every 8 rows is *more* generous than the current every-2000-pixels for
tall thin shapes. Keep `std::function` in the public API if you like; just cache
`_interrupt_check_cb.target<...>()`-equivalent state, or simply have `RenderController` hand over
`&_interrupt_requested_for_runtime_or_renderer` (it is already `volatile bool`, declared at
`render_controller.h:27`).

And in `getFillColor` / `drawAsset`, replace the `floorf` **calls**:

```cpp
// before: 2 windowed calls to floorf, then trunc.s
int assetX = (int)floorf(base_lx) % asset.width;
// after: branchless floor-to-int for the range we actually use
static inline int ifloor(float v) { int i = (int)v; return i - (v < (float)i); }
```

Better still, P4 removes the need to floor at all by stepping an integer texel coordinate.

**Speedup.** −25 (interrupt) −10 (modulo) −50 (2× `floorf`) ≈ **−85 cy/px**, i.e. **~1.2× of
rasterization time** on top of P1.
**Effort: S. Visual risk: nil** (`ifloor` is exact for |v| < 2³¹; the interrupt change only alters
*when* an abort is noticed, not what is drawn).
**Emulator parity: none required** — none of this is observable in output.

---

#### P3. 1-bit occupancy bitmap in internal SRAM, checked a word at a time

**Mechanism.** Keep the semantics exactly (first writer in reverse order wins). Change the
representation: 540×960 bits = **64,800 bytes** instead of 518,400, small enough to request
`MALLOC_CAP_INTERNAL` and get real SRAM instead of PSRAM. Inline the accessors — today they are
two out-of-line windowed calls per pixel that each re-validate bounds the caller already checked.

```cpp
// micropatterns_drawing.h
uint32_t* _occ = nullptr;          // heap_caps_malloc(..., MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)
int _occStrideWords;               // (width + 31) >> 5

// span-level: returns bits NOT yet occupied, and claims them, 32 px at a time
static inline uint32_t claimSpanBits(uint32_t* row, int wordIdx, uint32_t wanted) {
    const uint32_t occupied = row[wordIdx];
    const uint32_t fresh    = wanted & ~occupied;   // pixels we are allowed to paint
    row[wordIdx] = occupied | fresh;                // claim them
    return fresh;
}
```

The caller loops over 32-pixel words: `if (fresh == 0) continue;` skips 32 fully-occluded pixels
in ~3 cycles, versus 32 × ~70 cycles today. On a heavily overdrawn script that is the difference
between the occupancy map paying for itself and not.

Fall back to PSRAM (`ps_malloc`) if the internal allocation fails, and log which one you got.

**Speedup.** Occupancy cost drops from ~70 cy/px to ~1–2 cy/px amortized; the 26 ms per-frame
`std::fill` drops to ~3 ms. Roughly **−65 cy/px**, and it *increases* how often occlusion is worth
checking. **~1.15–1.4× of rasterization time** depending on overdraw. Also frees ~450 KB of PSRAM.
**Effort: M. Visual risk: nil** — identical semantics, different bit packing.
**Emulator parity: none required** — JS uses a `Uint8Array` (`display_list_renderer.js:375`) and
should keep doing so; representation is not observable.

---

#### P4. Span rasterization — compute the exact x-range per scanline, analytically

**This is the structural win.** It is also, importantly, **parity-exact for rects and circles** —
it produces precisely the same set of pixels as the current per-pixel test, because it solves the
*same inequality* rather than approximating it.

**Mechanism, `fillRect`.** With `B = inverseMatrixBase` (from P1), for a sample at
`(x+0.5, y+0.5)`:

```
u(x) = B0*(x+0.5) + B2*(y+0.5) + B4
v(x) = B1*(x+0.5) + B3*(y+0.5) + B5
```

Both are **affine in x** with constant slopes `B0`, `B1`. The current test is
`lx <= u < lx+lw && ly <= v < ly+lh` (after P1). Each half-plane is a linear inequality in x, so
each contributes a closed x-interval; the span is their intersection. One scanline setup, zero
per-pixel testing, and the pixel set is identical by construction.

```cpp
// Intersect [lo,hi) with the x-range where  a*x + b  lies in  [t0, t1)
static inline void clipAxis(float a, float b, float t0, float t1, int& lo, int& hi) {
    if (fabsf(a) < 1e-9f) {                       // constant along the row
        const float val = b + a * ((float)lo + 0.5f);
        if (!(val >= t0 && val < t1)) { hi = lo; } // whole row rejected
        return;
    }
    // x such that t0 <= a*(x+0.5) + b < t1
    float xa = (t0 - b) / a - 0.5f;
    float xb = (t1 - b) / a - 0.5f;
    if (a < 0.0f) { const float t = xa; xa = xb; xb = t; }
    lo = std::max(lo, (int)ceilf(xa));            // 2 divides + 2 ceils PER SCANLINE
    hi = std::min(hi, (int)ceilf(xb));
}

void MicroPatternsDrawing::fillRectSpan(const DisplayListItem& item) {
    const float* B = item.inverseMatrixBase;
    const int lx = ..., ly = ..., lw = ..., lh = ...;

    for (int sy = min_sy; sy < max_sy; ++sy) {
        if (__builtin_expect(_interrupt_flag && *_interrupt_flag, 0)) return;

        const float fy = (float)sy + 0.5f;
        const float bu = B[2]*fy + B[4];          // u = B0*(x+0.5) + bu
        const float bv = B[3]*fy + B[5];          // v = B1*(x+0.5) + bv

        int x0 = min_sx, x1 = max_sx;
        clipAxis(B[0], bu, (float)lx, (float)(lx+lw), x0, x1);
        if (x0 >= x1) continue;                   // whole scanline missed — FREE
        clipAxis(B[1], bv, (float)ly, (float)(ly+lh), x0, x1);
        if (x0 >= x1) continue;

        emitSpan(sy, x0, x1, item);               // see P5
    }
}
```

`fillCircle` is the same idea one degree up: substituting the affine `u(x), v(x)` into
`(u-cx)² + (v-cy)² <= r²` gives a **quadratic in x**, solved with **one `sqrtf` per scanline**
instead of per pixel — and again the same pixel set, because it is the same inequality. If the
discriminant is negative the whole scanline is skipped for free.

`drawAsset` cannot become a single span (the asset is a bitmap, not a convex shape), but it gets
two large wins: (a) x is clipped to the exact quad span first, so no AABB-reject pixels; (b) the
texel coordinate steps incrementally in **Q16.16**, so the per-pixel work is `u += du; idx = u>>16;`
— an add and a shift, replacing a full transform + 2 `floorf` + 2 `rems`.

**Speedup, two separate effects:**
1. *Removes AABB-reject waste entirely.* Today every pixel in the AABB pays ~140–170 cy even when
   it is outside the shape. A 45°-rotated rect wastes ~50% of its AABB; a circle wastes
   `1 - π/4` ≈ 21%. On rotation-heavy scripts (art-deco is exactly that) this alone is
   **1.3–2×**.
2. *Removes all per-pixel geometry.* The remaining per-pixel cost is just fill + write.

Combined with P1–P3, per-drawn-pixel drops from ~570 cy to roughly **25–40 cy**.
**~4–6× of rasterization time on top of P1–P3.**
**Effort: L** (this is the real work — a genuine rewrite of `fillRect`, `fillCircle`, `drawAsset`).
**Visual risk: LOW-but-verify.** Mathematically identical for rect and circle; the risk is
floating-point edge cases where a sample sits exactly on a boundary and `ceilf` rounds the other
way. Mitigate with a golden-image test: render a corpus of scripts (including `dca8a7f`'s
180°-rotated-rect regression case) with old and new paths and diff the framebuffers.
**Emulator parity: recommended but not required.** Because the pixel set is identical, the JS can
stay per-pixel and still match. If JS speed matters, mirror it — but the C++ can land first
without breaking parity, which makes this much safer to ship than it looks.

---

#### P5. Span writer — pack nibbles directly, no virtual call, pattern by DDA

**Mechanism.** Replace `rawPixel`-per-pixel with a span emitter that walks the 4 bpp framebuffer
directly. `_img8` is reachable via `M5EPD_Canvas::frameBuffer()`; `_bytewidth = width>>1` (see
`M5EPD_Canvas.cpp:135`). Two pixels per byte, high nibble = even x.

The pattern lookup also collapses: along a scanline the texel index advances by a constant, so a
wrapping counter replaces `%`:

```cpp
void MicroPatternsDrawing::emitSpan(int sy, int x0, int x1, const DisplayListItem& item) {
    uint8_t* row = _fb + (size_t)sy * _bytewidth;
    uint32_t* occRow = _occ + (size_t)sy * _occStrideWords;

    if (!item.fillAsset) {                       // SOLID — the common case
        emitSolidSpan(row, occRow, x0, x1, item.color);   // memset-like, see below
        return;
    }
    // PATTERNED, axis-aligned fast path: texel index steps by a constant, wraps by compare
    const MicroPatternsAsset& a = *item.fillAsset;
    int ax = startTexelX(item, sy, x0);          // computed once per scanline
    const uint8_t* prow = &a.data[startTexelY(item, sy) * a.width];
    const uint8_t on  = item.color;                       // 0 or 15
    const uint8_t off = (item.color == DRAWING_COLOR_WHITE) ? DRAWING_COLOR_BLACK
                                                            : DRAWING_COLOR_WHITE;
    for (int x = x0; x < x1; ++x) {
        if (!occTestAndSet(occRow, x)) { if (++ax == a.width) ax = 0; continue; }
        const uint8_t c = prow[ax] ? on : off;
        uint8_t* p = row + (x >> 1);
        if (x & 1) *p = (*p & 0xF0) | c;
        else       *p = (*p & 0x0F) | (uint8_t)(c << 4);
        if (++ax == a.width) ax = 0;             // replaces `% asset.width`
    }
}
```

Note `if (++ax == a.width) ax = 0;` — this is the fix for suspicion #3, and it needs no
power-of-two constraint on pattern dimensions, so no pattern authoring rules change.

For the solid case, and for fully-unoccupied runs found by P3's word scan, go further: handle the
odd first/last pixel individually and `memset` the aligned middle, exactly the way
`M5EPD_Canvas::fillRect` already does (`M5EPD_Canvas.cpp:50-87`). A solid horizontal run becomes a
`memset` at PSRAM bandwidth — roughly **1 cycle per 2 pixels**.

A further refinement, since only two colours exist in this system (`DRAWING_COLOR_WHITE = 0`,
`DRAWING_COLOR_BLACK = 15`): build the *whole* pattern row as a 32-bit mask once per scanline and
expand mask bits to nibbles through a 256-entry `uint32_t` LUT (byte of bits → 4 bytes of nibbles),
writing 8 pixels per store.

**Speedup.** Replaces ~110 cy (3 windowed calls + vtable + 4 bounds checks + PSRAM RMW) with
~4–8 cy/px, or ~0.5 cy/px on solid runs. **~1.5–2.5× of rasterization time** on top of P4,
strongly dependent on how much of the workload is solid fills.
**Effort: M. Visual risk: LOW** — same nibble packing as `M5EPD_Canvas::drawPixel`, just without
the redundant checks. The one real hazard is that writing `_img8` directly bypasses M5EPD's
bounds guards, so the span code must be trusted to clip; a `#ifdef DEBUG` assert on
`x0 >= 0 && x1 <= width` is worth keeping.
**Emulator parity: none required** — the JS canvas already writes via `putImageData` on an
`ImageData` block (`drawing.js:377, 638`), which is the same optimization by another name.

---

### Tier 3 — bigger structural bets, only worth it after Tier 1+2 land

#### P6. Tile-based rendering into internal SRAM

Only two colours are ever used, so the *working* framebuffer could be a 1-bit plane:
540×960/8 = 64,800 bytes, plus 64,800 for occupancy = ~130 KB. That is too much internal DRAM to
count on with WiFi up. **Tiles solve it**: process the screen in 540×64 strips (colour plane
4,320 B + occupancy plane 4,320 B — both comfortably resident), bucket display-list items into
tiles by their AABB, render each tile with zero PSRAM traffic, then expand the strip to 4 bpp into
the PSRAM canvas with a 256-entry LUT in one sequential streaming write.

Extra benefits: a tile whose occupancy plane is fully set can skip **every** remaining item for
that tile — a very cheap and very strong early-out under reverse-order painting, and a natural
extension of the design that already won the benchmark. Also gives cache-resident inner loops.

**Speedup: ~1.3–1.8×** on top of P1–P5 (by which point memory is the binding constraint).
**Effort: L. Visual risk: LOW** (bucketing is a scheduling change, not a rasterization change) —
but it interacts with occlusion ordering and needs care.
**Emulator parity: none** — invisible.

#### P7. Dual-core split

Core 1 runs the render task; core 0 runs WiFi. With P6's tiles the work is embarrassingly
parallel: hand alternating tile strips to two tasks. Occupancy and occlusion are per-tile, so
there is no shared mutable state and no locking.
**Speedup: 1.5–1.8×** if core 0 has headroom — which on this device, during a render, it largely
does. **Effort: L. Risk: MEDIUM** (FreeRTOS FPU context-switch cost is real but is paid per
switch, not per pixel; both tasks must have the FPU enabled). **Do this last** — it multiplies
whatever inefficiency remains, so it is worth strictly less the earlier you do it.

#### P8. Compiler and placement `[cross-boundary — platform lane]`

Briefly, because it interacts with everything above:
- The project builds at **`-Os`**. `-O2` on the four rasterizer TUs alone would help materially,
  and is nearly free to try (`build_flags` per-file, or `#pragma GCC optimize("O2")`).
- **No LTO** — which is exactly why `matrix_apply_to_point` is an out-of-line call. P1 sidesteps
  this by inlining in the header, but LTO would fix a whole class of these.
- `-DCORE_DEBUG_LEVEL=5` compiles in verbose logging; `log_d`/`log_v` in or near loops cost real
  time and flash-cache pressure.
- `-fstack-protector` is on (the object references `__stack_chk_fail`) — per-call, small, but the
  hot path makes a lot of calls today.
- `IRAM_ATTR` on the final span functions removes flash-cache misses, which matter more than usual
  here because flash and PSRAM contend for cache.
- `-mfix-esp32-psram-cache-issue` inserts the `memw` barriers visible throughout the disassembly.
  It is a correctness workaround for a silicon erratum and **must not be removed** on affected
  revisions; but P6 (tile rendering out of internal SRAM) sharply reduces how often it bites.

---

## 4. Ranked table

Speedups are multiplicative and **relative to rasterization time**, each assuming the ones above
it have landed.

| # | Proposal | Mechanism in one line | Est. speedup (of rasterization) | Effort | Visual risk | Emulator parity |
|---|---|---|---|---|---|---|
| P1 | Fold `scale` into inverse matrix; inline `matrix_apply_to_point` | Deletes 2–4 `__divsf3` soft-float divides per pixel | **1.4–1.6×** | S | ~nil | optional |
| P2 | Per-scanline interrupt flag; drop `%` yield counter; `floorf`→`ifloor` | Removes a windowed `std::function` call + a magic-mul chain + 2 library calls per pixel | **1.2×** | S | nil | none |
| P3 | 1-bit occupancy bitmap in internal SRAM, word-at-a-time | 518 KB PSRAM → 65 KB SRAM; 2 calls/px → ~1 cy/px; skips 32 occluded px at once | **1.15–1.4×** | M | nil | none |
| P4 | **Span rasterization** — analytic per-scanline x-range | Same inequality solved once per row, not per pixel; kills AABB-reject waste | **4–6×** | L | low (verify) | recommended |
| P5 | Span writer: direct nibble packing, `memset` runs, pattern DDA | No virtual call, no redundant bounds checks, no `%` | **1.5–2.5×** | M | low | none |
| P6 | Tile rendering into internal SRAM, 1-bit planes | Cache-resident inner loop; full-tile early-out | 1.3–1.8× | L | low | none |
| P7 | Dual-core tile split | Two render tasks over alternating strips | 1.5–1.8× | L | medium | none |
| P8 | `-O2` on rasterizer TUs, LTO, `IRAM_ATTR`, lower debug level | Better codegen, cross-TU inlining, no flash-cache misses | 1.2–1.5× | S | nil | none |
| — | ~~`intParams.at()` in hot path~~ | **Not a rasterization problem** — all call sites are per-item | 1.00× | — | — | — |
| — | ~~Wholesale float→Q16.16~~ | Little left to convert after P4; keep Q16.16 only for texel stepping | ~1.0× | L | medium | would require mirroring |
| — | ~~Remove occlusion buffer~~ | **Do not** — per-item and already cheap; it is load-bearing | — | — | — | — |

**Cumulative:** Tier 1 (P1+P2+P3) ≈ **2.0–2.7×** for about a day's work, all of it low risk and
independently shippable. Tier 1+2 (adding P4+P5) ≈ **12–40×** on rasterization. Adding Tier 3
≈ 25–100×, but by then the e-ink push (§1.7) dominates what the user perceives.

**A deliberately conservative headline: 10–20× on rasterization time is achievable with
P1–P5.** If rasterization is, say, 80% of the current 12 s, that is 9.6 s → ~0.6–1.0 s, giving
a total of roughly 2.5–3.5 s including generation and the panel push. **Confirming that "80%" is
the single most valuable next step** — it is already in the log.

---

## 5. Already tried / historical (from git)

| Commit | What it did | Where | Verdict |
|---|---|---|---|
| `80fd47f` | "Late stage overdraw optimization attempt" | JS `compiled_runtime.js` | Superseded by the display-list work |
| `82906d5` | First display list with **reverse-order painting** | JS | Foundation of the current design |
| `01ded93` | "Improved display list, more precise culling" | JS | Introduced `updateFromPixelMap` |
| `d427b02` | "All path gives same result" | JS | Established output equivalence — made a fair benchmark possible |
| `e4d18d9` | "Correct display list transforms and cache key" | JS | |
| `dca8a7f` | "Ensure 180° rotated rects draw with occlusion culling" | JS `drawing.js` only | **A real regression was found here once.** Keep this case in any golden-image test for P4 |
| `d1ea9e7` | "Compiler is default" | JS | The compiler path was default *first* |
| `68d843e` | "Default to display list" | JS | **The display list beat the compiler in measurement and replaced it** |
| `20cf696`, `d8d5e76` | "clean up optimization choices/setting" | JS | Consolidation around the winner; removed dead toggles |
| `27eda12` | "Render use display list with pixel occupancy map" | **C++** (the only device-side port) | Current architecture; 30–49 s → 10–19 s |

**Reading of the history.** The display-list architecture is the deliberate, benchmark-backed
winner, not a naive baseline — its culling and reverse-order painting are very plausibly *why* it
won. Nothing in this analysis proposes weakening either. What the history also shows is that
**every optimization to date has been at the display-list / culling level — deciding *which*
pixels to draw. Nobody has yet optimized the cost of drawing *one* pixel.** That inner loop is
still, essentially, the first thing that worked. That is why the headroom there is so large.

**Dead weight identified:**
- The 518 KB `_pixelOccupationMap` in PSRAM is 8× larger than it needs to be and reached through
  two out-of-line calls per pixel. Its *semantics* are sound and load-bearing; its
  *representation* is the problem (P3).
- The per-pixel `std::function` interrupt check looks like defensive responsiveness work that was
  never revisited once it was inside a 518,400-iteration loop (P2).
- The `pixelCount % 2000` / `% 8000` yield bookkeeping is a watchdog workaround that now costs
  ~10 cy on every pixel to decide, 1999 times out of 2000, to do nothing (P2).
- The C++ never received the `updateFromPixelMap` refinement the JS got in `01ded93`, and still
  uses heuristic `markingBounds` shrink factors. A pre-existing parity gap.

---

## 6. Uncertainties — things I could not determine by reading

1. **The generation/rasterization split of the 10–19 s.** Everything here is scaled by this and I
   do not have it. `render_controller.cpp:60-93` already logs both numbers; one serial capture
   settles it. If rasterization turns out to be only 30% of the total, the priority ordering of
   this document versus the interpreter analysis should flip.
2. **Where `_pixelOccupationMap` actually lands.** I reason it must be PSRAM (518 KB cannot fit in
   internal DRAM), but I have not confirmed the allocation succeeds at all. Check
   `esp_ptr_internal(_pixelOccupationMap.data())` and the `vector::resize` outcome — a silent
   `bad_alloc`/failure would mean the map is inert today.
3. **Exact cycle counts.** I counted instructions from real disassembly and applied published LX6
   latencies; I have not run a cycle counter on the device. The *ratios* between components are
   solid; the absolute µs figures could be off by ±30%. `esp_cpu_get_cycle_count()` around one
   `fillRect` would confirm the model in minutes and is worth doing before committing to P4.
4. **PSRAM clock.** I assumed 40 MHz. If this board is configured for 80 MHz, memory terms halve
   and the arithmetic proposals (P1, P2, P4) matter proportionally *more*, not less.
5. **`__divsf3` cost.** I read the *call* from the relocations, which is certain. The 50–60 cycle
   figure is from libgcc soft-float norms, not measured here.
6. **`UpdateArea` blocking behaviour.** I confirmed the 10 MHz SPI transfer (~0.21 s) but did not
   trace whether panel refresh blocks the caller. This sets the floor in §1.7 and deserves its own
   measurement.
7. **Available internal DRAM at render time.** P3 (65 KB) is very likely fine; P6's non-tiled
   variant (130 KB) probably is not, with WiFi up. Tiles avoid the question, which is part of why
   P6 is specified as tiled.

#include "mp_wdt.h"
#if defined(ARDUINO_ARCH_ESP32)
#include "esp_heap_caps.h"
#endif
#include "micropatterns_drawing.h"
#include "micropatterns_drawing.h"
#include <cmath> // For floor, ceil, fabs -- no trigonometry: ROTATE uses the table in matrix_utils.cpp
#include <algorithm> // For std::min, std::max
#include <cstring>   // For memcpy
#include <cstdint>

namespace {

// If `v` is a power of two, 1/v is representable exactly and `x * (1/v)` gives
// bit-for-bit the same result as `x / v` for every x. SCALE in the DSL resolves
// to std::max(1, <int>) so scaleFactor is always a positive integer -- 1, 2, 4,
// 8 ... hit this path, and 1 (the default, by far the most common) always does.
// This matters much more on the ESP32 than on the host: the LX6 has no
// single-precision divide instruction, so every `x / scaleFactor` in a pixel
// loop was a libgcc __divsf3 software call. When v is not a power of two we
// keep dividing rather than accept a 1-ulp change in the rendered image.
inline bool exactReciprocal(float v, float& r) {
    uint32_t b;
    memcpy(&b, &v, sizeof(b));
    const uint32_t exp = (b >> 23) & 0xFFu;
    if (exp == 0u || exp == 0xFFu) return false;   // zero, subnormal, inf, NaN
    if (b & 0x007FFFFFu) return false;             // mantissa set -> not a power of two
    r = 1.0f / v;
    uint32_t rb;
    memcpy(&rb, &r, sizeof(rb));
    const uint32_t rexp = (rb >> 23) & 0xFFu;
    if (rexp == 0u || rexp == 0xFFu) return false; // reciprocal over/underflowed
    return true;
}

// Exact floor-to-int, no libm call. On the ESP32 `floor()` was a real (windowed)
// call to floorf per pixel; this is two instructions and gives the identical
// result for every value representable as an int.
inline int ifloor_i(float v) {
    int i = static_cast<int>(v);
    return i - (v < static_cast<float>(i));
}


// --- Q16.16 fixed point ----------------------------------------------------
//
// A pattern coordinate along a scanline is affine in x:  b(x+1) = b(x) + d,
// with d constant for the whole row. In float that recurrence is not usable --
// repeated addition drifts -- so the float loops below recompute im*x + c from
// scratch at every pixel: a multiply, two adds, a reciprocal multiply and a
// float->int conversion, per axis. In Q16.16 the recurrence IS usable, because
// integer addition is exact, and `>> 16` is an exact floor for negatives too,
// so the conversion disappears with it. Two adds and two shifts replace all of
// that.
//
// This is the one place where fixed point is not merely "integer instead of
// float" but a different and cheaper algorithm. It is also why the argument
// "the ESP32 has an FPU so fixed point cannot win" does not settle the
// question: the win on offer is not a faster multiply, it is not multiplying.
//
// RANGE. Q16.16 in an int32 spans +/-32768. Pattern coordinates are screen
// coordinates pushed through the inverse transform and divided by the integer
// SCALE, so in practice they sit within a few thousand -- but a script may
// TRANSLATE by any int32, which puts the visible span arbitrarily far from the
// logical origin. Every scanline therefore range-checks BOTH ends of its span
// before committing and falls back to the float loop when they do not fit.
// Silent wraparound would show up as a wrong pattern phase rather than a
// crash, which is precisely the kind of bug that survives a golden gate on a
// corpus that never translates that far.
static const int   MP_FX_SHIFT = 16;
static const float MP_FX_ONE   = 65536.0f;
static const float MP_FX_LIMIT = 32000.0f;   // margin under 32768

inline bool fxFits(float v) { return v > -MP_FX_LIMIT && v < MP_FX_LIMIT; }

// lrintf, not a cast: the start value is a pixel centre, and truncation toward
// zero would bias it by up to one ulp on the negative side of the origin only
// -- an asymmetry that would show as a one-pixel pattern seam at x = 0.
inline int32_t fxFrom(float v) { return (int32_t)lrintf(v * MP_FX_ONE); }

// --- Span narrowing -------------------------------------------------------
//
// Every fill primitive used to walk the whole screen-space AABB and run a
// rejection test on each pixel. For a rotated rect that wastes up to half the
// AABB; for a circle about 21%; and the "test" is the expensive part (a full
// inverse transform, sometimes two divides).
//
// All the per-axis tests in this file have the same shape: a quantity g(x) that
// is *monotone* in the pixel index x along a scanline must lie in [t0, t1).
// g(x) is an affine function of (float)x evaluated in float, optionally divided
// by a constant -- and float multiply, add and divide are all monotone, so the
// set of accepted x is a contiguous run. narrowSpan finds its two ends by
// bisecting on the *same expression* the per-pixel test would have evaluated,
// so the accepted pixel set is bit-identical; only the rejected pixels stop
// being visited, in O(log n) evaluations instead of O(n).
//
// NaN safety: if the transform is degenerate every comparison is false, both
// searches return `hi`, and the span collapses to empty -- which is what the
// old per-pixel test did too (all its comparisons were false as well).
template <typename G>
inline int firstTrueGE(G g, float t, int lo, int hi) {
    while (lo < hi) { int m = lo + ((hi - lo) >> 1); if (g(m) >= t) hi = m; else lo = m + 1; }
    return lo;
}
template <typename G>
inline int firstTrueLT(G g, float t, int lo, int hi) {
    while (lo < hi) { int m = lo + ((hi - lo) >> 1); if (g(m) < t) hi = m; else lo = m + 1; }
    return lo;
}

template <typename G>
inline void narrowSpan(G g, float t0, float t1, int& lo, int& hi) {
    if (lo >= hi) return;
    const float gLo = g(lo);
    const float gHi = g(hi - 1);
    if (gLo == gHi) {                       // constant along this scanline
        if (!(gLo >= t0 && gLo < t1)) hi = lo;
        return;
    }
    int newLo, newHi;
    if (gHi > gLo) {                        // non-decreasing
        newLo = firstTrueGE(g, t0, lo, hi); // first x with g >= t0
        newHi = firstTrueGE(g, t1, lo, hi); // first x with g >= t1
    } else {                                // non-increasing
        newLo = firstTrueLT(g, t1, lo, hi); // first x with g <  t1
        newHi = firstTrueLT(g, t0, lo, hi); // first x with g <  t0
    }
    if (newLo > lo) lo = newLo;
    if (newHi < hi) hi = newHi;
    if (hi < lo) hi = lo;
}

} // namespace

MicroPatternsDrawing::MicroPatternsDrawing(MPCanvas* canvas)
    : _canvas(canvas), _interrupt_check_cb(nullptr), _usePixelOccupationMap(false), _overdrawSkippedPixels(0) {
    if (_canvas) {
        _canvasWidth = _canvas->width();
        _canvasHeight = _canvas->height();
    } else {
        _canvasWidth = 0;
        _canvasHeight = 0;
    }
}

void MicroPatternsDrawing::setCanvas(MPCanvas* canvas) {
    _canvas = canvas;
     if (_canvas) {
        _canvasWidth = _canvas->width();
        _canvasHeight = _canvas->height();
    } else {
        _canvasWidth = 0;
        _canvasHeight = 0;
    }
}

void MicroPatternsDrawing::setInterruptCheckCallback(std::function<bool()> cb) {
    _interrupt_check_cb = cb;
}

void MicroPatternsDrawing::enablePixelOccupationMap(bool enable) {
    _usePixelOccupationMap = enable;
    if (_usePixelOccupationMap) {
        initPixelOccupationMap(); // Ensure it's sized
    }
}

void MicroPatternsDrawing::initPixelOccupationMap() {
    if (_canvasWidth > 0 && _canvasHeight > 0) {
        _occStride = (_canvasWidth + 7) / 8;
        const size_t need = (size_t)_occStride * _canvasHeight;
        // Resize only if necessary or if size changed
        if (_pixelOccupationMap.size() != need) {
#if defined(ARDUINO_ARCH_ESP32)
            // Check BEFORE allocating. Exceptions are disabled on Arduino, so a
            // vector resize that cannot be satisfied calls abort() outright --
            // the device reboots mid-render with no usable message.
            //
            // That is not hypothetical: on the Watchy (200x200, no PSRAM) this
            // map is 40,000 bytes, and parsing the largest script fragments the
            // heap down to a ~26KB largest block. Switching to that script
            // aborted and rebooted every time.
            //
            // Dropping the map is safe ONLY because DisplayListRenderer checks
            // for it and falls back to painter's order. It is not merely an
            // overdraw optimisation: the fast path walks the display list
            // front-to-back and depends on this map to make the first writer
            // win. An earlier version of this comment claimed dropping it cost
            // "speed, not correctness" -- measured, it changed all 15 corpus
            // images. See the ordering comment in display_list_renderer.cpp.
            const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            if (largest < need + 4096) {   // margin for allocator overhead
                _pixelOccupationMap.clear();
                _usePixelOccupationMap = false;
                return;
            }
#endif
            _pixelOccupationMap.resize(need, 0);
        }
        // No need to fill with 0 here, resetPixelOccupationMap will do it.
    } else {
        _pixelOccupationMap.clear();
    }
}

void MicroPatternsDrawing::resetPixelOccupationMap() {
    if (_usePixelOccupationMap && !_pixelOccupationMap.empty()) {
        std::fill(_pixelOccupationMap.begin(), _pixelOccupationMap.end(), 0);
    }
    _overdrawSkippedPixels = 0;
    _fixedPointPixels = 0;
    _integerXformCalls = 0;
    _spanRows = 0;
    _intDdaRows = 0;
    _tiledRows = 0;
    _clippedRows = 0;
}

void MicroPatternsDrawing::clearCanvas() {
    if (_canvas) {
        _canvas->fillCanvas(DRAWING_COLOR_WHITE);
    }
    if (_usePixelOccupationMap) {
        resetPixelOccupationMap(); // Also reset occupation map when canvas is cleared
    }
}

// --- Transformation ---
// Uses DisplayListItem's snapshotted state
void MicroPatternsDrawing::transformPoint(float logical_x, float logical_y, const DisplayListItem& item, float& screen_x, float& screen_y) {
    float scaled_lx = logical_x * item.xf->scale;
    float scaled_ly = logical_y * item.xf->scale;
    matrix_apply_to_point(item.xf->matrix, scaled_lx, scaled_ly, screen_x, screen_y);
}

// Exact integer forward transform: a logical point to screen space, returned as
// Q15 numerators rather than pixels.
//
// This is the float transformPoint() below, done without floats. Every input is
// already whole -- the logical coordinate, the scale, and both table entries --
// and the table entries share the denominator MP_Q15_ONE, so the result is a
// whole number over that same denominator. Nothing rounds until a caller asks
// for a pixel index.
//
// int64 throughout: a script may TRANSLATE by any int32, and the products here
// reach 2^15 * 2^31 before the offset is even added.
static inline void xformPointQ15(const DisplayListItem& item, int32_t lx, int32_t ly,
                                 int64_t& sxNum, int64_t& syNum) {
    const TransformSnapshot& xf = *item.xf;
    const int32_t C = xf.cosQ15;   // resolved once per snapshot, not per point
    const int32_t S = xf.sinQ15;
    const int64_t X = (int64_t)lx * xf.scaleInt;
    const int64_t Y = (int64_t)ly * xf.scaleInt;

    // 32-BIT FAST PATH.
    //
    // The int64 form below is correct for every input a script can express --
    // lx and SCALE are both int32, so X can be enormous -- but that generality
    // was being paid on every item, and 64-bit arithmetic is expensive here:
    // Xtensa has no add-with-carry, so a 64-bit add becomes a compare and a
    // branch. Measured on a Watchy, the int64-only version cost 3.6% on
    // op_rect_outline (four corners per item) against 0.6% on op_line (two).
    // The cost tracked the NUMBER OF MULTIPLIES, not the arithmetic being
    // integer -- which is also why replacing the integer sqrt changed nothing.
    //
    // Every coordinate a real script produces fits in 32 bits with room spare.
    // With |X|,|Y| <= 2^14 and |t| <= 2^26 the worst case is
    //     32768 * 2^14 * 2 + 2^26  =  1.14e9  <  2^31
    // so the expression cannot overflow. Anything outside that -- a translate
    // of millions, a scale that makes X astronomical -- falls through to the
    // exact int64 form rather than wrapping silently.
    const int64_t kLim = 1 << 14;
    const int64_t kOff = 1 << 26;
    if (X <= kLim && X >= -kLim && Y <= kLim && Y >= -kLim &&
        xf.txNum <= kOff && xf.txNum >= -kOff &&
        xf.tyNum <= kOff && xf.tyNum >= -kOff) {
        const int32_t x32 = (int32_t)X, y32 = (int32_t)Y;
        sxNum = (int64_t)(C * x32 - S * y32 + (int32_t)xf.txNum);
        syNum = (int64_t)(S * x32 + C * y32 + (int32_t)xf.tyNum);
        return;
    }

    sxNum = (int64_t)C * X - (int64_t)S * Y + xf.txNum;
    syNum = (int64_t)S * X + (int64_t)C * Y + xf.tyNum;
}

// Screen AABB of a logical rectangle's four corners, exactly, with no float.
// The corners are transformed as Q15 numerators and the box taken there, so the
// floor/ceil at the end are a shift and a negated shift rather than libm calls.
static inline void aabbQ15(const DisplayListItem& item,
                           int32_t lx0, int32_t ly0, int32_t lx1, int32_t ly1,
                           int& minx, int& maxx, int& miny, int& maxy) {
    int64_t px[4], py[4];
    xformPointQ15(item, lx0, ly0, px[0], py[0]);
    xformPointQ15(item, lx1, ly0, px[1], py[1]);
    xformPointQ15(item, lx0, ly1, px[2], py[2]);
    xformPointQ15(item, lx1, ly1, px[3], py[3]);
    int64_t mnx = px[0], mxx = px[0], mny = py[0], mxy = py[0];
    for (int i = 1; i < 4; ++i) {
        if (px[i] < mnx) mnx = px[i];
        if (px[i] > mxx) mxx = px[i];
        if (py[i] < mny) mny = py[i];
        if (py[i] > mxy) mxy = py[i];
    }
    minx = (int)mp_q15_floor(mnx);
    miny = (int)mp_q15_floor(mny);
    maxx = (int)(-((-mxx) >> MP_Q15_SHIFT));   // ceil, divisor is a power of two
    maxy = (int)(-((-mxy) >> MP_Q15_SHIFT));
}

// Screen-space radius of a logical radius under the item's transform.
//
// The transform matrix is RIGID -- built only from TRANSLATE and ROTATE, since
// SCALE lives in xf->scale and never enters the matrix -- so its columns are
// unit vectors and a transformed radius keeps its length. The screen radius is
// therefore exactly lr * scale: no sqrt, no hypot, and no sampling. A circle
// under a rigid transform plus a uniform scale is still a circle, which is what
// makes the exact answer this short.
static inline float screenRadiusOf(int lr, const DisplayListItem& item) {
    return static_cast<float>(lr) * item.xf->scale;
}

void MicroPatternsDrawing::screenToLogicalBase(float screen_x, float screen_y, const DisplayListItem& item, float& base_logical_x, float& base_logical_y) {
    float scaled_logical_x, scaled_logical_y;
    matrix_apply_to_point(item.xf->inverseMatrix, screen_x, screen_y, scaled_logical_x, scaled_logical_y);

    if (item.xf->scale == 0.0f) {
        base_logical_x = scaled_logical_x;
        base_logical_y = scaled_logical_y;
    } else {
        base_logical_x = scaled_logical_x / item.xf->scale;
        base_logical_y = scaled_logical_y / item.xf->scale;
    }
}


// --- Raw Drawing ---
void MicroPatternsDrawing::rawPixel(int sx, int sy, uint8_t color) {
    if (!_canvas) return;
    if (sx >= 0 && sx < _canvasWidth && sy >= 0 && sy < _canvasHeight) {
        if (_usePixelOccupationMap) {
            if (isPixelOccupied(sx, sy)) {
                _overdrawSkippedPixels++;
                return; // Pixel already occupied
            }
            markPixelOccupied(sx, sy);
        }
        _canvas->drawPixel(sx, sy, color);
    }
}

void MicroPatternsDrawing::rawLine(int sx1, int sy1, int sx2, int sy2, uint8_t color) {
    if (!_canvas) return;

    int dx_abs = abs(sx2 - sx1);
    int dy_abs = -abs(sy2 - sy1); // dy is negative for typical algorithm
    int current_sx = sx1;
    int current_sy = sy1;
    int stepX = (sx1 < sx2) ? 1 : -1;
    int stepY = (sy1 < sy2) ? 1 : -1;
    int err = dx_abs + dy_abs; // Error term

    while (true) {
        rawPixel(current_sx, current_sy, color);
        if (current_sx == sx2 && current_sy == sy2) break;
        int e2 = 2 * err;
        if (e2 >= dy_abs) { // Favor X step
            if (current_sx == sx2) break; // Reached end in X
            err += dy_abs;
            current_sx += stepX;
        }
        if (e2 <= dx_abs) { // Favor Y step
            if (current_sy == sy2) break; // Reached end in Y
            err += dx_abs;
            current_sy += stepY;
        }
    }
}

// --- Fill Pattern Helper ---
uint8_t MicroPatternsDrawing::getFillColor(float screen_pixel_center_x, float screen_pixel_center_y, const DisplayListItem& item) {
    if (!item.fillAsset) {
        return item.color; // Solid fill
    }
    float scaled_logical_x, scaled_logical_y;
    matrix_apply_to_point(item.xf->inverseMatrix, screen_pixel_center_x, screen_pixel_center_y,
                          scaled_logical_x, scaled_logical_y);
    return fillColorFromScaled(scaled_logical_x, scaled_logical_y, item);
}

uint8_t MicroPatternsDrawing::fillColorFromScaled(float scaled_logical_x, float scaled_logical_y, const DisplayListItem& item) const {
    float base_lx, base_ly;
    if (item.xf->scale == 0.0f) {
        base_lx = scaled_logical_x;
        base_ly = scaled_logical_y;
    } else {
        base_lx = scaled_logical_x / item.xf->scale;
        base_ly = scaled_logical_y / item.xf->scale;
    }
    return fillColorFromBase(base_lx, base_ly, item);
}

uint8_t MicroPatternsDrawing::fillColorFromBase(float base_lx, float base_ly, const DisplayListItem& item) const {
    const MicroPatternsAsset& asset = *item.fillAsset;
    if (asset.width <= 0 || asset.height <= 0 || asset.data.empty()) return DRAWING_COLOR_WHITE;

    int assetX = ifloor_i(base_lx) % asset.width;
    int assetY = ifloor_i(base_ly) % asset.height;
    if (assetX < 0) assetX += asset.width;
    if (assetY < 0) assetY += asset.height;

    int index = assetY * asset.width + assetX;
    if (index >= 0 && index < (int)asset.data.size()) {
        uint8_t patternBit = asset.data[index]; // 0 or 1
        if (item.color == DRAWING_COLOR_WHITE) { // Inverted mode for FILL
            return patternBit == 1 ? DRAWING_COLOR_WHITE : DRAWING_COLOR_BLACK;
        } else { // Normal mode (item.color is DRAWING_COLOR_BLACK) for FILL
            return patternBit == 1 ? DRAWING_COLOR_BLACK : DRAWING_COLOR_WHITE;
        }
    }
    return item.color == DRAWING_COLOR_WHITE ? DRAWING_COLOR_BLACK : DRAWING_COLOR_WHITE; // Default on error
}

// --- Drawing Primitives ---

void MicroPatternsDrawing::drawPixel(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.x();
    int ly = item.y();

    int min_sx, max_sx, min_sy, max_sy;
    if (_integerTransform) {
        ++_integerXformCalls;
        aabbQ15(item, lx, ly, lx + 1, ly + 1, min_sx, max_sx, min_sy, max_sy);
    } else {
    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + 1), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly + 1), item, s_br_x, s_br_y);

    // Determine screen-space bounding box (rounded to int for iteration)
    min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    }

    // Clip to canvas
    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);


    // Hoisted: these four products were recomputed on every pixel of the AABB.
    const float* IM = item.xf->inverseMatrix;
    const float im0 = IM[0], im1 = IM[1], im2 = IM[2], im3 = IM[3], im4 = IM[4], im5 = IM[5];
    const float sf = item.xf->scale;
    const float px0 = static_cast<float>(lx) * sf;
    const float px1 = static_cast<float>(lx + 1) * sf;
    const float py0 = static_cast<float>(ly) * sf;
    const float py1 = static_cast<float>(ly + 1) * sf;

    uint8_t* occ = occupancyBase();
    const int cw = _canvasWidth;
    unsigned int skipped = 0;

    const uint8_t color = item.color;
    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        // Per-scanline interrupt poll, matching fillRect/fillCircle/drawAsset.
        // PIXEL's screen bbox grows with SCALE^2 and can cover the whole panel, so a
        // single item could otherwise run ~0.5-1s with no opportunity to abort.
        if (_interrupt_check_cb && _interrupt_check_cb()) { _overdrawSkippedPixels += skipped; return; }
        const float fy = static_cast<float>(sy_iter) + 0.5f;
        const float m2y = im2 * fy, m3y = im3 * fy;
        uint8_t* occRow = occ ? occ + (size_t)sy_iter * _occStride : nullptr;
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            const float fx = static_cast<float>(sx_iter) + 0.5f;
            const float slx = im0 * fx + m2y + im4;
            const float sly = im1 * fx + m3y + im5;
            if (slx >= px0 && slx < px1 && sly >= py0 && sly < py1) {
                emitPixel(sx_iter, sy_iter, color, occRow, skipped);
            }
        }
    }
    _overdrawSkippedPixels += skipped;
}

void MicroPatternsDrawing::drawFilledPixel(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.x();
    int ly = item.y();

    int min_sx, max_sx, min_sy, max_sy;
    if (_integerTransform) {
        ++_integerXformCalls;
        aabbQ15(item, lx, ly, lx + 1, ly + 1, min_sx, max_sx, min_sy, max_sy);
    } else {
    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + 1), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly + 1), item, s_br_x, s_br_y);

    min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    }

    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);


    // Hoisted: these four products were recomputed on every pixel of the AABB.
    const float* IM = item.xf->inverseMatrix;
    const float im0 = IM[0], im1 = IM[1], im2 = IM[2], im3 = IM[3], im4 = IM[4], im5 = IM[5];
    const float sf = item.xf->scale;
    const float px0 = static_cast<float>(lx) * sf;
    const float px1 = static_cast<float>(lx + 1) * sf;
    const float py0 = static_cast<float>(ly) * sf;
    const float py1 = static_cast<float>(ly + 1) * sf;

    uint8_t* occ = occupancyBase();
    const int cw = _canvasWidth;
    unsigned int skipped = 0;

    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        // Per-scanline interrupt poll, matching fillRect/fillCircle/drawAsset.
        // PIXEL's screen bbox grows with SCALE^2 and can cover the whole panel, so a
        // single item could otherwise run ~0.5-1s with no opportunity to abort.
        if (_interrupt_check_cb && _interrupt_check_cb()) { _overdrawSkippedPixels += skipped; return; }
        const float fy = static_cast<float>(sy_iter) + 0.5f;
        const float m2y = im2 * fy, m3y = im3 * fy;
        uint8_t* occRow = occ ? occ + (size_t)sy_iter * _occStride : nullptr;
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            const float fx = static_cast<float>(sx_iter) + 0.5f;
            const float slx = im0 * fx + m2y + im4;
            const float sly = im1 * fx + m3y + im5;
            if (slx >= px0 && slx < px1 && sly >= py0 && sly < py1) {
                const uint8_t fillColor = item.fillAsset ? fillColorFromScaled(slx, sly, item) : item.color;
                emitPixel(sx_iter, sy_iter, fillColor, occRow, skipped);
            }
        }
    }
    _overdrawSkippedPixels += skipped;
}


void MicroPatternsDrawing::drawLine(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx1 = item.x1();
    int ly1 = item.y1();
    int lx2 = item.x2();
    int ly2 = item.y2();

    if (_integerTransform) {
        ++_integerXformCalls;
        // Two endpoints, exactly, then integer Bresenham. No float touched.
        int64_t ax, ay, bx, by;
        xformPointQ15(item, lx1, ly1, ax, ay);
        xformPointQ15(item, lx2, ly2, bx, by);
        rawLine(mp_q15_round(ax), mp_q15_round(ay),
                mp_q15_round(bx), mp_q15_round(by), item.color);
        return;
    }
    float sx1_f, sy1_f, sx2_f, sy2_f;
    transformPoint(static_cast<float>(lx1), static_cast<float>(ly1), item, sx1_f, sy1_f);
    transformPoint(static_cast<float>(lx2), static_cast<float>(ly2), item, sx2_f, sy2_f);
    rawLine(static_cast<int>(round(sx1_f)), static_cast<int>(round(sy1_f)),
            static_cast<int>(round(sx2_f)), static_cast<int>(round(sy2_f)), item.color);
}

void MicroPatternsDrawing::drawRect(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.x();
    int ly = item.y();
    int lw = item.w();
    int lh = item.h();
    if (lw <= 0 || lh <= 0) return;

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    // lw-1 / lh-1, not lw / lh: WIDTH is a pixel COUNT, so the last pixel of a
    // 20-wide rect is at x+19. Using lx+lw drew a 21-pixel outline for
    // WIDTH=20 -- one pixel wider than FILL_RECT with identical arguments, in
    // this same file. Measured: RECT X=10 WIDTH=20 covered x 10..30 while
    // FILL_RECT covered 10..29. The web emulator always used width-1 and was
    // right; see docs/analysis/web-device-renderer-audit.md.
    if (_integerTransform) {
        ++_integerXformCalls;
        int64_t tlx, tly, trx, trry, blx2, bly2, brx, bry;
        xformPointQ15(item, lx,          ly,          tlx,  tly);
        xformPointQ15(item, lx + lw - 1, ly,          trx,  trry);
        xformPointQ15(item, lx,          ly + lh - 1, blx2, bly2);
        xformPointQ15(item, lx + lw - 1, ly + lh - 1, brx,  bry);
        const int TLX = mp_q15_round(tlx), TLY = mp_q15_round(tly);
        const int TRX = mp_q15_round(trx), TRY = mp_q15_round(trry);
        const int BLX = mp_q15_round(blx2), BLY = mp_q15_round(bly2);
        const int BRX = mp_q15_round(brx), BRY = mp_q15_round(bry);
        rawLine(TLX, TLY, TRX, TRY, item.color);
        rawLine(TRX, TRY, BRX, BRY, item.color);
        rawLine(BRX, BRY, BLX, BLY, item.color);
        rawLine(BLX, BLY, TLX, TLY, item.color);
        return;
    }

    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + lw - 1), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + lh - 1), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + lw - 1), static_cast<float>(ly + lh - 1), item, s_br_x, s_br_y);

    rawLine(round(s_tl_x), round(s_tl_y), round(s_tr_x), round(s_tr_y), item.color); // Top
    rawLine(round(s_tr_x), round(s_tr_y), round(s_br_x), round(s_br_y), item.color); // Right
    rawLine(round(s_br_x), round(s_br_y), round(s_bl_x), round(s_bl_y), item.color); // Bottom
    rawLine(round(s_bl_x), round(s_bl_y), round(s_tl_x), round(s_tl_y), item.color); // Left
}

void MicroPatternsDrawing::fillRect(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.x();
    int ly = item.y();
    int lw = item.w();
    int lh = item.h();
    if (lw <= 0 || lh <= 0) return;

    int min_sx, max_sx, min_sy, max_sy;
    if (_integerTransform) {
        ++_integerXformCalls;
        aabbQ15(item, lx, ly, lx + lw, ly + lh, min_sx, max_sx, min_sy, max_sy);
    } else {
        float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
        transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
        transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly), item, s_tr_x, s_tr_y);
        transformPoint(static_cast<float>(lx), static_cast<float>(ly + lh), item, s_bl_x, s_bl_y);
        transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly + lh), item, s_br_x, s_br_y);
        min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
        max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
        min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
        max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    }

    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);
    if (min_sx >= max_sx || min_sy >= max_sy) return;

    // Loop invariants, hoisted. These four products used to be recomputed on
    // every single pixel; the compiler could not hoist them itself because the
    // canvas write in the loop body may alias `item`.
    const float* IM = item.xf->inverseMatrix;
    const float im0 = IM[0], im1 = IM[1], im2 = IM[2], im3 = IM[3], im4 = IM[4], im5 = IM[5];
    const float sf = item.xf->scale;
    const float rect_x0 = static_cast<float>(lx) * sf;
    const float rect_x1 = static_cast<float>(lx + lw) * sf;
    const float rect_y0 = static_cast<float>(ly) * sf;
    const float rect_y1 = static_cast<float>(ly + lh) * sf;

    const MicroPatternsAsset* fa = item.fillAsset;
    bool patterned = false;
    uint8_t flatColor = item.color;
    if (fa) {
        if (fa->width <= 0 || fa->height <= 0 || fa->data.empty()) flatColor = DRAWING_COLOR_WHITE;
        else patterned = true;
    }

    float rcp = 0.0f;
    const bool useRcp = exactReciprocal(sf, rcp);

    const uint8_t* patData = patterned ? fa->data.data() : nullptr;
    const int patW    = patterned ? fa->width : 0;
    const int patH    = patterned ? fa->height : 0;
    const int patSize = patterned ? (int)fa->data.size() : 0;
    const uint8_t patOn  = (item.color == DRAWING_COLOR_WHITE) ? DRAWING_COLOR_WHITE : DRAWING_COLOR_BLACK;
    const uint8_t patOff = (item.color == DRAWING_COLOR_WHITE) ? DRAWING_COLOR_BLACK : DRAWING_COLOR_WHITE;

    uint8_t* occ = occupancyBase();
    const int cw = _canvasWidth;
    unsigned int skipped = 0;

    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        // Interrupt is now checked once per scanline rather than once per pixel:
        // the callback was a std::function dispatch in the innermost loop. A
        // scanline is bounded by the canvas width, so responsiveness is unchanged
        // in any way a user can perceive.
        if (_interrupt_check_cb && _interrupt_check_cb()) { _overdrawSkippedPixels += skipped; return; }
        if ((sy_iter & 7) == 0) { yield(); mp_wdt_reset(); }

        const float fy = static_cast<float>(sy_iter) + 0.5f;
        // m2y / m3y are the only parts of the inverse transform that depend on y.
        // Keeping the +im4 / +im5 separate preserves the exact association
        // ((a*x) + (b*y)) + c that matrix_apply_to_point evaluates.
        const float m2y = im2 * fy;
        const float m3y = im3 * fy;
        auto gx = [&](int x) { return im0 * (static_cast<float>(x) + 0.5f) + m2y + im4; };
        auto gy = [&](int x) { return im1 * (static_cast<float>(x) + 0.5f) + m3y + im5; };

        int x0 = min_sx, x1 = max_sx;
        narrowSpan(gx, rect_x0, rect_x1, x0, x1);
        if (x0 >= x1) continue;
        narrowSpan(gy, rect_y0, rect_y1, x0, x1);
        if (x0 >= x1) continue;

        uint8_t* occRow = occ ? occ + (size_t)sy_iter * _occStride : nullptr;
        if (!patterned) {
            emitSolidSpan(sy_iter, x0, x1, flatColor, occRow, skipped);
            continue;
        }

        // Same hoist as drawAsset: with no x->y coupling the pattern row is
        // constant across the scanline, so its transform / unscale / floor /
        // modulo happen once per row instead of once per pixel.
        const uint8_t* patRow = nullptr;
        if (im1 == 0.0f && patW > 0) {
            float v = im1 * (static_cast<float>(x0) + 0.5f) + m3y + im5;
            if (sf != 0.0f) v = useRcp ? v * rcp : v / sf;
            int py = ifloor_i(v) % patH;
            if (py < 0) py += patH;
            if ((long)py * patW + patW <= (long)patSize) patRow = patData + (size_t)py * patW;
        }

        // The x-only recurrence, when the pattern ROW is already fixed.
        const float invSf = (sf != 0.0f) ? (useRcp ? rcp : 1.0f / sf) : 1.0f;
        const float bx0 = (im0 * (static_cast<float>(x0) + 0.5f) + m2y + im4) * invSf;
        const float dbx = im0 * invSf;
        const int   span = x1 - x0;
        IntDda idda; idda.ok = false;
        if (_integerDda) idda = intDdaRow(*item.xf, x0, sy_iter, 0, 0);

        if (patRow) {
            if (_fixedPointEnabled &&
                (idda.ok || (fxFits(bx0) && fxFits(bx0 + dbx * static_cast<float>(span - 1))))) {
                _fixedPointPixels += (unsigned long)span;
                if (idda.ok) ++_intDdaRows;
                int32_t bx = idda.ok ? idda.x0 : fxFrom(bx0);
                const int32_t dx = idda.ok ? idda.dx : fxFrom(dbx);
                if (_spanWriter && ((((x1) - 1) >> 3) - ((x0) >> 3) + 1) <= kMaxSpanBytes) {
                    // Span form of the loop below: same walk, but each pixel sets a
                    // bit instead of calling emitPixel, and the row is written once.
                    uint8_t ink[kMaxSpanBytes];
                    const int b0 = (x0) >> 3, nb = (((x1) - 1) >> 3) - b0 + 1;
                    memset(ink, 0, (size_t)nb);
                    // TILE, don't walk. The Q16.16 walk is exact integer arithmetic, so the
                    // ink sequence along this row repeats EXACTLY every P pixels, where P is
                    // the smallest count whose total advance P*dx is a whole multiple of the
                    // pattern width -- for any scale, not just powers of two; only the size of
                    // P depends on it. The mask bytes therefore repeat every B = P/gcd(P,8)
                    // bytes. Build B bytes by walking (from the byte boundary, so every bit of
                    // those bytes is right; the cover mask trims [x0,x1) later), then copy.
                    // For a 20-wide pattern at SCALE 1 that is 5 bytes built and the rest
                    // memcpy'd, in place of a shift, a modulo, a table read and a bit-set per
                    // pixel. Falls back to the walk when the period is too long to pay.
                    {
                        const int32_t mW = patW << MP_FX_SHIFT;
                        const int32_t g  = dx ? gcd32(mW, dx) : mW;
                        const int64_t P  = mW / g;                       // pixels
                        const int64_t Bb = (P % 8 == 0) ? P / 8 : (P % 4 == 0) ? P / 4 : (P % 2 == 0) ? P / 2 : P;   // = P / gcd(P, 8), in bytes
                        if (Bb <= kMaxSpanBytes && Bb * 2 <= nb) {
                            const int xs  = b0 << 3;
                            int32_t v = bx - (int32_t)(x0 - xs) * dx;
                            const int lim = xs + (int)(Bb << 3);
                            for (int x = xs; x < lim; ++x) {
                                int px = (int)(v >> MP_FX_SHIFT) % patW;
                                if (px < 0) px += patW;
                                if (patRow[px] == 1) ink[(x >> 3) - b0] |= (uint8_t)(0x80u >> (x & 7));
                                v += dx;
                            }
                            for (int j = (int)Bb; j < nb; ++j) ink[j] = ink[j - (int)Bb];
                            ++_tiledRows;
                        } else {
                        for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
                            int px = (int)(bx >> MP_FX_SHIFT) % patW;
                            if (px < 0) px += patW;
                            if (patRow[px] == 1) ink[(sx_iter >> 3) - b0] |= (uint8_t)(0x80u >> (sx_iter & 7));
                            bx += dx;
                        }
                        }
                    }
                    emitPatternSpan(sy_iter, x0, x1, ink, patOn, patOff, occRow, skipped);
                } else {
                    for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
                        int px = (int)(bx >> MP_FX_SHIFT) % patW;
                        if (px < 0) px += patW;
                        emitPixel(sx_iter, sy_iter, patRow[px] == 1 ? patOn : patOff, occRow, skipped);
                        bx += dx;
                    }
                }
                continue;
            }
            for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
                float blx = im0 * (static_cast<float>(sx_iter) + 0.5f) + m2y + im4;
                if (sf != 0.0f) blx = useRcp ? blx * rcp : blx / sf;
                int px = ifloor_i(blx) % patW;
                if (px < 0) px += patW;
                emitPixel(sx_iter, sy_iter, patRow[px] == 1 ? patOn : patOff, occRow, skipped);
            }
            continue;
        }

        // Both axes vary: the rotated case, where the row hoist is impossible
        // and the float path pays its full per-pixel cost twice over.
        if (_fixedPointEnabled && patW > 0 && patH > 0 && patSize >= patW * patH) {
            const float by0 = (im1 * (static_cast<float>(x0) + 0.5f) + m3y + im5) * invSf;
            const float dby = im1 * invSf;
            if (idda.ok || (fxFits(bx0) && fxFits(by0) &&
                fxFits(bx0 + dbx * static_cast<float>(span - 1)) &&
                fxFits(by0 + dby * static_cast<float>(span - 1)))) {
                _fixedPointPixels += (unsigned long)span;
                if (idda.ok) ++_intDdaRows;
                int32_t bx = idda.ok ? idda.x0 : fxFrom(bx0), by = idda.ok ? idda.y0 : fxFrom(by0);
                const int32_t dx = idda.ok ? idda.dx : fxFrom(dbx), dy = idda.ok ? idda.dy : fxFrom(dby);
                if (_spanWriter && ((((x1) - 1) >> 3) - ((x0) >> 3) + 1) <= kMaxSpanBytes) {
                    // Span form of the loop below: same walk, but each pixel sets a
                    // bit instead of calling emitPixel, and the row is written once.
                    uint8_t ink[kMaxSpanBytes];
                    const int b0 = (x0) >> 3, nb = (((x1) - 1) >> 3) - b0 + 1;
                    memset(ink, 0, (size_t)nb);
                    // Skip bytes the occupancy map has already painted in full: every ink bit
                    // computed for one would be masked off in the emit, so not computing it
                    // is exact. Doing that per byte costs a compare and a loop setup per byte,
                    // which measured +10..24% on rows with NOTHING to skip -- so first a
                    // one-pass scan decides whether this row has any such byte at all, and
                    // rows without one run the plain walk untouched. art_deco_4 paints 3.75x
                    // its pixels and gains 20% from the skip; grid paints each pixel once and
                    // must not pay for it.
                    bool rowHasFull = false;
                    if (occRow) { for (int b = b0; b < b0 + nb; ++b) if (occRow[b] == 0xFFu) { rowHasFull = true; break; } }
                    if (rowHasFull) {
                        for (int b = b0; b < b0 + nb; ++b) {
                            const int cs = (b == b0) ? (x0) : (b << 3);
                            const int ce = (b == b0 + nb - 1) ? (x1) : ((b + 1) << 3);
                            if (occRow[b] == 0xFFu) { bx += dx * (ce - cs); by += dy * (ce - cs); continue; }
                            for (int sx_iter = cs; sx_iter < ce; ++sx_iter) {
                                int px = (int)(bx >> MP_FX_SHIFT) % patW;
                                if (px < 0) px += patW;
                                int py = (int)(by >> MP_FX_SHIFT) % patH;
                                if (py < 0) py += patH;
                                if (patData[(size_t)py * patW + px] == 1) ink[(sx_iter >> 3) - b0] |= (uint8_t)(0x80u >> (sx_iter & 7));
                                bx += dx; by += dy;
                            }
                        }
                    } else {
                        for (int sx_iter = (x0); sx_iter < (x1); ++sx_iter) {
                        int px = (int)(bx >> MP_FX_SHIFT) % patW;
                        if (px < 0) px += patW;
                        int py = (int)(by >> MP_FX_SHIFT) % patH;
                        if (py < 0) py += patH;
                        if (patData[(size_t)py * patW + px] == 1) ink[(sx_iter >> 3) - b0] |= (uint8_t)(0x80u >> (sx_iter & 7));
                        bx += dx; by += dy;
                        }
                    }
                    emitPatternSpan(sy_iter, x0, x1, ink, patOn, patOff, occRow, skipped);
                } else {
                    for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
                        int px = (int)(bx >> MP_FX_SHIFT) % patW;
                        if (px < 0) px += patW;
                        int py = (int)(by >> MP_FX_SHIFT) % patH;
                        if (py < 0) py += patH;
                        emitPixel(sx_iter, sy_iter,
                                  patData[(size_t)py * patW + px] == 1 ? patOn : patOff,
                                  occRow, skipped);
                        bx += dx; by += dy;
                    }
                }
                continue;
            }
        }

        for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
            const float fx = static_cast<float>(sx_iter) + 0.5f;
            float blx = im0 * fx + m2y + im4;
            float bly = im1 * fx + m3y + im5;
            if (sf != 0.0f) {
                if (useRcp) { blx *= rcp; bly *= rcp; }
                else        { blx /= sf;  bly /= sf;  }
            }
            emitPixel(sx_iter, sy_iter, fillColorFromBase(blx, bly, item), occRow, skipped);
        }
    }
    _overdrawSkippedPixels += skipped;
    mp_wdt_reset(); // Ensure WDT is reset after the loop
}

void MicroPatternsDrawing::drawCircle(const DisplayListItem& item) {
    if (!_canvas) return;
    int lcx = item.x();
    int lcy = item.y();
    int lr = item.radius();
    if (lr <= 0) return;
     
    float scx_f = 0.0f, scy_f = 0.0f, screen_radius_approx = 0.0f;
    if (!_integerTransform) {
        transformPoint(static_cast<float>(lcx), static_cast<float>(lcy), item, scx_f, scy_f);
        // Was two sqrtf of the matrix column norms. Those columns are unit
        // vectors -- the matrix is rigid -- so both roots computed 1.0 to
        // within 1.5e-5.
        screen_radius_approx = screenRadiusOf(lr, item);
    }
     
    int scx, scy, scaledRadius;
    if (_integerTransform) {
        ++_integerXformCalls;
        // The centre exactly, and the radius is already whole: lr * scaleInt.
        int64_t cxn, cyn;
        xformPointQ15(item, lcx, lcy, cxn, cyn);
        scx = mp_q15_round(cxn);
        scy = mp_q15_round(cyn);
        scaledRadius = lr * item.xf->scaleInt;
    } else {
        scx = static_cast<int>(round(scx_f));
        scy = static_cast<int>(round(scy_f));
        scaledRadius = static_cast<int>(round(screen_radius_approx));
    }
    if (scaledRadius < 1) scaledRadius = 1;

    int x_coord = scaledRadius;
    int y_coord = 0;
    int err = 1 - scaledRadius;

    while (x_coord >= y_coord) {
        rawPixel(scx + x_coord, scy + y_coord, item.color); rawPixel(scx + y_coord, scy + x_coord, item.color);
        rawPixel(scx - y_coord, scy + x_coord, item.color); rawPixel(scx - x_coord, scy + y_coord, item.color);
        rawPixel(scx - x_coord, scy - y_coord, item.color); rawPixel(scx - y_coord, scy - x_coord, item.color);
        rawPixel(scx + y_coord, scy - x_coord, item.color); rawPixel(scx + x_coord, scy - y_coord, item.color);
        y_coord++;
        if (err <= 0) {
            err += 2 * y_coord + 1;
        } else {
            x_coord--;
            err += 2 * (y_coord - x_coord) + 1;
        }
    }
}

void MicroPatternsDrawing::fillCircle(const DisplayListItem& item) {
    if (!_canvas) return;
    int lcx = item.x();
    int lcy = item.y();
    int lr = item.radius();
    if (lr <= 0) return;

    float logical_radius = static_cast<float>(lr);

    // The bounding box is the centre plus the screen radius, on all four sides.
    //
    // This used to transform EIGHT points on the circle -- four cardinal, four
    // diagonal via a 0.7071f offset -- and take their min/max. That is the AABB
    // of an inscribed OCTAGON, not of the circle, and under rotation it
    // undershoots by r*(1 - cos 22.5deg) = 7.6% of the radius. Measured on a
    // radius-200 disk: correct at ROTATE 0 (400 px wide, 125,676 ink px) and
    // clipped to 372 px / 120,132 px at ROTATE 22, losing 4.4% of its area and
    // rendering as a disk with eight flat sides. drawCircle and the display-list
    // bounds both already computed the extent the exact way; only this one
    // sampled. The inside test below was always the exact disk test, so the
    // whole bug lived in these bounds.
    float scx_f = 0.0f, scy_f = 0.0f, screen_radius = 0.0f;
    if (!_integerTransform) {
        transformPoint(static_cast<float>(lcx), static_cast<float>(lcy), item, scx_f, scy_f);
        screen_radius = screenRadiusOf(lr, item);
    }

    // The same centre and radius held exactly. The radius needs no conversion
    // at all: it is lr * scaleInt, both whole.
    int64_t scxNum = 0, scyNum = 0;
    const int32_t screen_radius_i = lr * item.xf->scaleInt;
    // (R * ONE)^2 is constant for the whole item. It was being recomputed once
    // per scanline -- a 64-bit multiply per row for a value that never changes,
    // exactly the waste the float path had already been taught to avoid with
    // the hoisted im0..im5 in fillRect.
    int64_t radiusNumSq = 0;
    if (_integerTransform) {
        ++_integerXformCalls;
        xformPointQ15(item, lcx, lcy, scxNum, scyNum);
        const int64_t RN = (int64_t)screen_radius_i * MP_Q15_ONE;
        radiusNumSq = RN * RN;
    }

    int min_sx, max_sx, min_sy, max_sy;
    if (_integerTransform) {
        // The same box, from the exact centre and radius. ceil of a value over a
        // power of two is -((-v) >> k), floor is v >> k.
        const int64_t RN = (int64_t)screen_radius_i * MP_Q15_ONE;
        min_sx = (int)mp_q15_floor(scxNum - RN);
        max_sx = (int)(-((-(scxNum + RN)) >> MP_Q15_SHIFT));
        min_sy = (int)mp_q15_floor(scyNum - RN);
        max_sy = (int)(-((-(scyNum + RN)) >> MP_Q15_SHIFT));
    } else {
        min_sx = static_cast<int>(floor(scx_f - screen_radius));
        max_sx = static_cast<int>(ceil(scx_f + screen_radius));
        min_sy = static_cast<int>(floor(scy_f - screen_radius));
        max_sy = static_cast<int>(ceil(scy_f + screen_radius));
    }
    
    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    if (min_sx >= max_sx || min_sy >= max_sy) return;

    const float logical_radius_sq = logical_radius * logical_radius;
    const float* IM = item.xf->inverseMatrix;
    const float im0 = IM[0], im1 = IM[1], im2 = IM[2], im3 = IM[3], im4 = IM[4], im5 = IM[5];
    const float sf = item.xf->scale;
    const float flcx = static_cast<float>(lcx);
    const float flcy = static_cast<float>(lcy);

    const MicroPatternsAsset* fa = item.fillAsset;
    bool patterned = false;
    uint8_t flatColor = item.color;
    if (fa) {
        if (fa->width <= 0 || fa->height <= 0 || fa->data.empty()) flatColor = DRAWING_COLOR_WHITE;
        else patterned = true;
    }

    float rcp = 0.0f;
    const bool useRcp = exactReciprocal(sf, rcp);

    uint8_t* occ = occupancyBase();
    const int cw = _canvasWidth;
    unsigned int skipped = 0;

    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        if (_interrupt_check_cb && _interrupt_check_cb()) { _overdrawSkippedPixels += skipped; return; }
        if ((sy_iter & 7) == 0) { yield(); mp_wdt_reset(); }

        const float fy = static_cast<float>(sy_iter) + 0.5f;
        const float m2y = im2 * fy;
        const float m3y = im3 * fy;

        // The disk test dx*dx + dy*dy <= r*r is not monotone in x, so it cannot
        // be inverted directly. But |dx| <= r and |dy| <= r are *necessary*
        // conditions and both are monotone, so they give a conservative span --
        // one pixel of slack is added on each side, and the exact disk test still
        // runs inside the span. Nothing that used to be drawn can be dropped;
        // what disappears is the ~21% of the AABB (much more when rotated) that
        // the disk never covers.
        auto blx = [&](int x) {
            const float v = im0 * (static_cast<float>(x) + 0.5f) + m2y + im4;
            return (sf == 0.0f) ? v : (useRcp ? v * rcp : v / sf);
        };
        auto bly = [&](int x) {
            const float v = im1 * (static_cast<float>(x) + 0.5f) + m3y + im5;
            return (sf == 0.0f) ? v : (useRcp ? v * rcp : v / sf);
        };

        // --- The disk test, done in SCREEN space and hoisted out of the loop --
        //
        // The float path below inverse-transforms every pixel in the span for
        // the sole purpose of asking "is this inside the circle?", then throws
        // the coordinates away unless the fill is patterned. It does not have
        // to. The transform is rigid and the scale uniform, so a circle maps to
        // a CIRCLE: |base - centre| <= r is exactly |screen - Centre| <= r*sf,
        // and that can be answered in screen space with no transform at all.
        //
        // Better still, it can be answered per SCANLINE instead of per pixel. A
        // horizontal line across a circle enters and leaves exactly once, so a
        // row's inside-pixels are one contiguous run whose half-width is
        // sqrt(R^2 - dy^2) -- one square root per row, against a transform,
        // two multiplies and a compare per pixel. The old span narrowing goes
        // too: it bisected to a CONSERVATIVE span (|dx| <= r and |dy| <= r,
        // necessary but not sufficient) and still tested every pixel inside it.
        // This span is exact, so nothing inside it needs testing.
        if (_fixedPointEnabled) {
            int fx0, fx1;
            if (_integerTransform) {
                // No float and no sqrtf. A pixel centre sx+0.5 is the whole
                // number (2*sx+1) over 2, so in Q15 numerators it is
                // sx*MP_Q15_ONE + MP_Q15_ONE/2. The radius and the centre are
                // both exact, so R^2 - dy^2 is an exactly representable whole
                // number and its root is an integer square root.
                const int64_t dyN = (int64_t)sy_iter * MP_Q15_ONE + (MP_Q15_ONE / 2) - scyNum;
                const int64_t rr  = radiusNumSq - dyN * dyN;
                if (rr <= 0) continue;
                const int64_t hwN = mp_isqrt64(rr);
                // sx*ONE + ONE/2 must lie in [cx - hw, cx + hw]. The divisor is
                // a power of two, so ceil is -((-a) >> k) and floor is a >> k.
                const int64_t lo = scxNum - hwN - (MP_Q15_ONE / 2);
                const int64_t hi = scxNum + hwN - (MP_Q15_ONE / 2);
                fx0 = (int)(-((-lo) >> MP_Q15_SHIFT));
                fx1 = (int)(hi >> MP_Q15_SHIFT) + 1;
            } else {
                const float dyc = (static_cast<float>(sy_iter) + 0.5f) - scy_f;
                const float rr = screen_radius * screen_radius - dyc * dyc;
                if (rr <= 0.0f) continue;
                const float hw = sqrtf(rr);
                // Pixel centres, so the bounds are on sx + 0.5.
                fx0 = (int)ceilf(scx_f - hw - 0.5f);
                fx1 = (int)floorf(scx_f + hw - 0.5f) + 1;
            }
            if (fx0 < min_sx) fx0 = min_sx;
            if (fx1 > max_sx) fx1 = max_sx;
            if (fx0 >= fx1) continue;

            uint8_t* occRowF = occ ? occ + (size_t)sy_iter * _occStride : nullptr;

            if (!patterned) {
                _fixedPointPixels += (unsigned long)(fx1 - fx0);
                emitSolidSpan(sy_iter, fx0, fx1, flatColor, occRowF, skipped);
                continue;
            }

            const int patW = fa->width, patH = fa->height;
            const int patSize = (int)fa->data.size();
            const uint8_t* patData = fa->data.data();
            const uint8_t patOn  = (item.color == DRAWING_COLOR_WHITE) ? DRAWING_COLOR_WHITE : DRAWING_COLOR_BLACK;
            const uint8_t patOff = (item.color == DRAWING_COLOR_WHITE) ? DRAWING_COLOR_BLACK : DRAWING_COLOR_WHITE;
            if (patW > 0 && patH > 0 && patSize >= patW * patH) {
                const float invSf = (sf != 0.0f) ? (useRcp ? rcp : 1.0f / sf) : 1.0f;
                const float cbx0 = (im0 * (static_cast<float>(fx0) + 0.5f) + m2y + im4) * invSf;
                const float cby0 = (im1 * (static_cast<float>(fx0) + 0.5f) + m3y + im5) * invSf;
                const float cdbx = im0 * invSf, cdby = im1 * invSf;
                const int cspan = fx1 - fx0;
                IntDda cidda; cidda.ok = false;
                if (_integerDda) cidda = intDdaRow(*item.xf, fx0, sy_iter, 0, 0);
                if (cidda.ok || (fxFits(cbx0) && fxFits(cby0) &&
                    fxFits(cbx0 + cdbx * static_cast<float>(cspan - 1)) &&
                    fxFits(cby0 + cdby * static_cast<float>(cspan - 1)))) {
                    _fixedPointPixels += (unsigned long)cspan;
                    if (cidda.ok) ++_intDdaRows;
                    int32_t bxq = cidda.ok ? cidda.x0 : fxFrom(cbx0), byq = cidda.ok ? cidda.y0 : fxFrom(cby0);
                    const int32_t dbxq = cidda.ok ? cidda.dx : fxFrom(cdbx), dbyq = cidda.ok ? cidda.dy : fxFrom(cdby);
                    if (_spanWriter && ((((fx1) - 1) >> 3) - ((fx0) >> 3) + 1) <= kMaxSpanBytes) {
                        // Span form of the loop below: same walk, but each pixel sets a
                        // bit instead of calling emitPixel, and the row is written once.
                        uint8_t ink[kMaxSpanBytes];
                        const int b0 = (fx0) >> 3, nb = (((fx1) - 1) >> 3) - b0 + 1;
                        memset(ink, 0, (size_t)nb);
                        // Skip bytes the occupancy map has already painted in full: every ink bit
                        // computed for one would be masked off in the emit, so not computing it
                        // is exact. Doing that per byte costs a compare and a loop setup per byte,
                        // which measured +10..24% on rows with NOTHING to skip -- so first a
                        // one-pass scan decides whether this row has any such byte at all, and
                        // rows without one run the plain walk untouched. art_deco_4 paints 3.75x
                        // its pixels and gains 20% from the skip; grid paints each pixel once and
                        // must not pay for it.
                        bool rowHasFull = false;
                        if (occRowF) { for (int b = b0; b < b0 + nb; ++b) if (occRowF[b] == 0xFFu) { rowHasFull = true; break; } }
                        if (rowHasFull) {
                            for (int b = b0; b < b0 + nb; ++b) {
                                const int cs = (b == b0) ? (fx0) : (b << 3);
                                const int ce = (b == b0 + nb - 1) ? (fx1) : ((b + 1) << 3);
                                if (occRowF[b] == 0xFFu) { bxq += dbxq * (ce - cs); byq += dbyq * (ce - cs); continue; }
                                for (int sx_iter = cs; sx_iter < ce; ++sx_iter) {
                                    int px = (int)(bxq >> MP_FX_SHIFT) % patW; if (px < 0) px += patW;
                                    int py = (int)(byq >> MP_FX_SHIFT) % patH; if (py < 0) py += patH;
                                    if (patData[(size_t)py * patW + px] == 1) ink[(sx_iter >> 3) - b0] |= (uint8_t)(0x80u >> (sx_iter & 7));
                                    bxq += dbxq; byq += dbyq;
                                }
                            }
                        } else {
                            for (int sx_iter = (fx0); sx_iter < (fx1); ++sx_iter) {
                            int px = (int)(bxq >> MP_FX_SHIFT) % patW; if (px < 0) px += patW;
                            int py = (int)(byq >> MP_FX_SHIFT) % patH; if (py < 0) py += patH;
                            if (patData[(size_t)py * patW + px] == 1) ink[(sx_iter >> 3) - b0] |= (uint8_t)(0x80u >> (sx_iter & 7));
                            bxq += dbxq; byq += dbyq;
                            }
                        }
                        emitPatternSpan(sy_iter, fx0, fx1, ink, patOn, patOff, occRowF, skipped);
                    } else {
                        for (int sx_iter = fx0; sx_iter < fx1; ++sx_iter) {
                            int px = (int)(bxq >> MP_FX_SHIFT) % patW; if (px < 0) px += patW;
                            int py = (int)(byq >> MP_FX_SHIFT) % patH; if (py < 0) py += patH;
                            emitPixel(sx_iter, sy_iter,
                                      patData[(size_t)py * patW + px] == 1 ? patOn : patOff,
                                      occRowF, skipped);
                            bxq += dbxq; byq += dbyq;
                        }
                    }
                    continue;
                }
            }
            // Pattern unusable or out of Q16.16 range: keep the exact span, fall
            // back to the float lookup inside it.
            for (int sx_iter = fx0; sx_iter < fx1; ++sx_iter) {
                const float fx = static_cast<float>(sx_iter) + 0.5f;
                float bx = im0 * fx + m2y + im4;
                float by = im1 * fx + m3y + im5;
                if (sf != 0.0f) {
                    if (useRcp) { bx *= rcp; by *= rcp; }
                    else        { bx /= sf;  by /= sf;  }
                }
                emitPixel(sx_iter, sy_iter, fillColorFromBase(bx, by, item), occRowF, skipped);
            }
            continue;
        }

        int x0 = min_sx, x1 = max_sx;
        narrowSpan(blx, flcx - logical_radius - 1.0f, flcx + logical_radius + 1.0f, x0, x1);
        if (x0 >= x1) continue;
        narrowSpan(bly, flcy - logical_radius - 1.0f, flcy + logical_radius + 1.0f, x0, x1);
        if (x0 >= x1) continue;

        uint8_t* occRow = occ ? occ + (size_t)sy_iter * _occStride : nullptr;
        for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
            const float fx = static_cast<float>(sx_iter) + 0.5f;
            float base_logical_x = im0 * fx + m2y + im4;
            float base_logical_y = im1 * fx + m3y + im5;
            if (sf != 0.0f) {
                if (useRcp) { base_logical_x *= rcp; base_logical_y *= rcp; }
                else        { base_logical_x /= sf;  base_logical_y /= sf;  }
            }

            const float dx = base_logical_x - flcx;
            const float dy = base_logical_y - flcy;
            if (dx * dx + dy * dy <= logical_radius_sq) {
                const uint8_t c = patterned ? fillColorFromBase(base_logical_x, base_logical_y, item) : flatColor;
                emitPixel(sx_iter, sy_iter, c, occRow, skipped);
            }
        }
    }
    _overdrawSkippedPixels += skipped;
    mp_wdt_reset();
}

void MicroPatternsDrawing::drawAsset(const DisplayListItem& item, const MicroPatternsAsset& asset) {
    if (!_canvas || asset.width <= 0 || asset.height <= 0 || asset.data.empty()) return;
    int lx_asset_origin = item.x();
    int ly_asset_origin = item.y();

    int min_sx, max_sx, min_sy, max_sy;
    if (_integerTransform) {
        ++_integerXformCalls;
        aabbQ15(item, lx_asset_origin, ly_asset_origin,
                lx_asset_origin + asset.width, ly_asset_origin + asset.height,
                min_sx, max_sx, min_sy, max_sy);
    } else {
        float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
        transformPoint(static_cast<float>(lx_asset_origin), static_cast<float>(ly_asset_origin), item, s_tl_x, s_tl_y);
        transformPoint(static_cast<float>(lx_asset_origin + asset.width), static_cast<float>(ly_asset_origin), item, s_tr_x, s_tr_y);
        transformPoint(static_cast<float>(lx_asset_origin), static_cast<float>(ly_asset_origin + asset.height), item, s_bl_x, s_bl_y);
        transformPoint(static_cast<float>(lx_asset_origin + asset.width), static_cast<float>(ly_asset_origin + asset.height), item, s_br_x, s_br_y);
        min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
        max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
        min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
        max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    }

    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    if (min_sx >= max_sx || min_sy >= max_sy) return;

    const float* IM = item.xf->inverseMatrix;
    const float im0 = IM[0], im1 = IM[1], im2 = IM[2], im3 = IM[3], im4 = IM[4], im5 = IM[5];
    const float sf = item.xf->scale;
    const float forigin_x = static_cast<float>(lx_asset_origin);
    const float forigin_y = static_cast<float>(ly_asset_origin);
    const float fasset_w = static_cast<float>(asset.width);
    const float fasset_h = static_cast<float>(asset.height);
    const uint8_t* adata = asset.data.data();
    const int adata_size = (int)asset.data.size();
    const int aw = asset.width;
    const uint8_t color = item.color;

    float rcp = 0.0f;
    const bool useRcp = exactReciprocal(sf, rcp);

    uint8_t* occ = occupancyBase();
    const int cw = _canvasWidth;
    unsigned int skipped = 0;

    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        if (_interrupt_check_cb && _interrupt_check_cb()) { _overdrawSkippedPixels += skipped; return; }
        if ((sy_iter & 7) == 0) { yield(); mp_wdt_reset(); }

        const float fy = static_cast<float>(sy_iter) + 0.5f;
        const float m2y = im2 * fy;
        const float m3y = im3 * fy;

        // Both bounds tests are monotone in x, so the exact span is found by
        // bisection on the same expressions the per-pixel test evaluated.
        auto alx = [&](int x) {
            const float v = im0 * (static_cast<float>(x) + 0.5f) + m2y + im4;
            return ((sf == 0.0f) ? v : (useRcp ? v * rcp : v / sf)) - forigin_x;
        };
        auto aly = [&](int x) {
            const float v = im1 * (static_cast<float>(x) + 0.5f) + m3y + im5;
            return ((sf == 0.0f) ? v : (useRcp ? v * rcp : v / sf)) - forigin_y;
        };

        int x0 = min_sx, x1 = max_sx;
        narrowSpan(alx, 0.0f, fasset_w, x0, x1);
        if (x0 >= x1) continue;
        narrowSpan(aly, 0.0f, fasset_h, x0, x1);
        if (x0 >= x1) continue;

        uint8_t* occRow = occ ? occ + (size_t)sy_iter * _occStride : nullptr;

        // When the inverse transform has no x->y coupling (im1 == 0, i.e. no
        // rotation or shear) the asset row is the same for the whole scanline,
        // so the y half of the work -- transform, unscale, floor, row offset --
        // is hoisted out of the pixel loop. im1 * fx is exactly +0 for every
        // finite fx, so this is the identical value the general path computes.
        const uint8_t* assetRow = nullptr;
        if (im1 == 0.0f) {
            float v = im1 * (static_cast<float>(x0) + 0.5f) + m3y + im5;
            if (sf != 0.0f) v = useRcp ? v * rcp : v / sf;
            const float aly_v = v - forigin_y;
            if (!(aly_v >= 0 && aly_v < fasset_h)) continue;   // whole scanline misses the asset
            const int iy = ifloor_i(aly_v);
            if (iy >= 0 && (long)iy * aw + aw <= (long)adata_size) assetRow = adata + (size_t)iy * aw;
        }

        // Same Q16.16 recurrence as fillRect. Asset-local coordinates, so the
        // origin is folded into the start value and never subtracted per pixel.
        const float invSf = (sf != 0.0f) ? (useRcp ? rcp : 1.0f / sf) : 1.0f;
        const float ax0 = (im0 * (static_cast<float>(x0) + 0.5f) + m2y + im4) * invSf - forigin_x;
        const float dax = im0 * invSf;
        const int   span = x1 - x0;
        IntDda aidda; aidda.ok = false;
        if (_integerDda) aidda = intDdaRow(*item.xf, x0, sy_iter, lx_asset_origin, ly_asset_origin);

        if (assetRow) {
            if (_fixedPointEnabled &&
                (aidda.ok || (fxFits(ax0) && fxFits(ax0 + dax * static_cast<float>(span - 1))))) {
                _fixedPointPixels += (unsigned long)span;
                if (aidda.ok) ++_intDdaRows;
                int32_t axq = aidda.ok ? aidda.x0 : fxFrom(ax0);
                const int32_t daxq = aidda.ok ? aidda.dx : fxFrom(dax);
                if (_spanWriter && ((((x1) - 1) >> 3) - ((x0) >> 3) + 1) <= kMaxSpanBytes) {
                    // Span form of the loop below: same walk, but each pixel sets a
                    // bit instead of calling emitPixel, and the row is written once.
                    uint8_t cover[kMaxSpanBytes];
                    const int b0 = (x0) >> 3, nb = (((x1) - 1) >> 3) - b0 + 1;
                    memset(cover, 0, (size_t)nb);
                    // CLIP, don't test. ix advances by a constant, so "0 <= ix < aw" is
                    // true on ONE contiguous run of pixels. Its ends solve in closed form
                    // from the same integer arithmetic the walk uses -- the walk is exact,
                    // so this is exact -- and the pixels outside it are never visited.
                    // A rotated asset's box is up to twice its area; every pixel of the
                    // excess used to cost a shift, two compares and a branch to reject.
                    {
                        const int span = x1 - x0;
                        const int64_t a0 = axq, d = daxq, top = ((int64_t)aw << MP_FX_SHIFT);
                        int64_t k0 = 0, k1 = (int64_t)span - 1;      // inclusive run in k
                        if (d > 0) {
                            if (a0 < 0)     k0 = (-a0 + d - 1) / d;                 // first k with a >= 0
                            k1 = (top - 1 - a0) >= 0 ? (top - 1 - a0) / d : -1;    // last k with a < top
                        } else if (d < 0) {
                            const int64_t nd = -d;
                            if (a0 >= top)  k0 = (a0 - top + nd) / nd;              // first k with a < top
                            k1 = a0 >= 0 ? a0 / nd : -1;                            // last k with a >= 0
                        } else if (a0 < 0 || a0 >= top) {
                            k1 = -1;
                        }
                        if (k0 < 0) k0 = 0;
                        if (k1 > span - 1) k1 = span - 1;
                        if (k0 <= k1) {
                            ++_clippedRows;
                            int32_t a = (int32_t)(a0 + k0 * d);
                            const int xs = x0 + (int)k0, xe = x0 + (int)k1 + 1;
                            for (int sx_iter = xs; sx_iter < xe; ++sx_iter) {
                                if (assetRow[(int)(a >> MP_FX_SHIFT)] == 1) {
                                    cover[(sx_iter >> 3) - b0] |= (uint8_t)(0x80u >> (sx_iter & 7));
                                }
                                a += daxq;
                            }
                        }
                    }
                    emitMaskSpan(sy_iter, x0, x1, cover, color, occRow, skipped);
                } else {
                    for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
                        const int ix = (int)(axq >> MP_FX_SHIFT);
                        if (ix >= 0 && ix < aw && assetRow[ix] == 1) {
                            emitPixel(sx_iter, sy_iter, color, occRow, skipped);
                        }
                        axq += daxq;
                    }
                }
                continue;
            }
            for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
                float blx = im0 * (static_cast<float>(sx_iter) + 0.5f) + m2y + im4;
                if (sf != 0.0f) blx = useRcp ? blx * rcp : blx / sf;
                const int ix = ifloor_i(blx - forigin_x);
                if (ix >= 0 && ix < aw && assetRow[ix] == 1) {
                    emitPixel(sx_iter, sy_iter, color, occRow, skipped);
                }
            }
            continue;
        }

        if (_fixedPointEnabled) {
            const float ay0 = (im1 * (static_cast<float>(x0) + 0.5f) + m3y + im5) * invSf - forigin_y;
            const float day = im1 * invSf;
            if (aidda.ok || (fxFits(ax0) && fxFits(ay0) &&
                fxFits(ax0 + dax * static_cast<float>(span - 1)) &&
                fxFits(ay0 + day * static_cast<float>(span - 1)))) {
                _fixedPointPixels += (unsigned long)span;
                if (aidda.ok) ++_intDdaRows;
                int32_t axq = aidda.ok ? aidda.x0 : fxFrom(ax0), ayq = aidda.ok ? aidda.y0 : fxFrom(ay0);
                const int32_t daxq = aidda.ok ? aidda.dx : fxFrom(dax), dayq = aidda.ok ? aidda.dy : fxFrom(day);
                if (_spanWriter && ((((x1) - 1) >> 3) - ((x0) >> 3) + 1) <= kMaxSpanBytes) {
                    // Span form of the loop below: same walk, but each pixel sets a
                    // bit instead of calling emitPixel, and the row is written once.
                    uint8_t cover[kMaxSpanBytes];
                    const int b0 = (x0) >> 3, nb = (((x1) - 1) >> 3) - b0 + 1;
                    memset(cover, 0, (size_t)nb);
                    // Skip bytes the occupancy map has already painted in full: every ink bit
                    // computed for one would be masked off in the emit, so not computing it
                    // is exact. Doing that per byte costs a compare and a loop setup per byte,
                    // which measured +10..24% on rows with NOTHING to skip -- so first a
                    // one-pass scan decides whether this row has any such byte at all, and
                    // rows without one run the plain walk untouched. art_deco_4 paints 3.75x
                    // its pixels and gains 20% from the skip; grid paints each pixel once and
                    // must not pay for it.
                    bool rowHasFull = false;
                    if (occRow) { for (int b = b0; b < b0 + nb; ++b) if (occRow[b] == 0xFFu) { rowHasFull = true; break; } }
                    if (rowHasFull) {
                        for (int b = b0; b < b0 + nb; ++b) {
                            const int cs = (b == b0) ? (x0) : (b << 3);
                            const int ce = (b == b0 + nb - 1) ? (x1) : ((b + 1) << 3);
                            if (occRow[b] == 0xFFu) { axq += daxq * (ce - cs); ayq += dayq * (ce - cs); continue; }
                            for (int sx_iter = cs; sx_iter < ce; ++sx_iter) {
                                const int idx = (int)(ayq >> MP_FX_SHIFT) * aw + (int)(axq >> MP_FX_SHIFT);
                                if (idx >= 0 && idx < adata_size && adata[idx] == 1) {
                                    cover[(sx_iter >> 3) - b0] |= (uint8_t)(0x80u >> (sx_iter & 7));
                                }
                                axq += daxq; ayq += dayq;
                            }
                        }
                    } else {
                        for (int sx_iter = (x0); sx_iter < (x1); ++sx_iter) {
                        const int idx = (int)(ayq >> MP_FX_SHIFT) * aw + (int)(axq >> MP_FX_SHIFT);
                        if (idx >= 0 && idx < adata_size && adata[idx] == 1) {
                            cover[(sx_iter >> 3) - b0] |= (uint8_t)(0x80u >> (sx_iter & 7));
                        }
                        axq += daxq; ayq += dayq;
                        }
                    }
                    emitMaskSpan(sy_iter, x0, x1, cover, color, occRow, skipped);
                } else {
                    for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
                        const int idx = (int)(ayq >> MP_FX_SHIFT) * aw + (int)(axq >> MP_FX_SHIFT);
                        if (idx >= 0 && idx < adata_size && adata[idx] == 1) {
                            emitPixel(sx_iter, sy_iter, color, occRow, skipped);
                        }
                        axq += daxq; ayq += dayq;
                    }
                }
                continue;
            }
        }

        for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
            const float fx = static_cast<float>(sx_iter) + 0.5f;
            float base_logical_x = im0 * fx + m2y + im4;
            float base_logical_y = im1 * fx + m3y + im5;
            if (sf != 0.0f) {
                if (useRcp) { base_logical_x *= rcp; base_logical_y *= rcp; }
                else        { base_logical_x /= sf;  base_logical_y /= sf;  }
            }

            const float asset_local_x = base_logical_x - forigin_x;
            const float asset_local_y = base_logical_y - forigin_y;

            // No range test here: narrowSpan already restricted [x0,x1) to exactly
            // the pixels that pass it, using the same expressions. The index
            // bounds check below is kept as the memory-safety backstop.
            const int asset_data_index = ifloor_i(asset_local_y) * aw + ifloor_i(asset_local_x);
            if (asset_data_index >= 0 && asset_data_index < adata_size && adata[asset_data_index] == 1) {
                emitPixel(sx_iter, sy_iter, color, occRow, skipped);
            }
        }
    }
    _overdrawSkippedPixels += skipped;
    mp_wdt_reset();
}

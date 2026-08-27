#include "micropatterns_drawing.h"
#include "micropatterns_drawing.h"
#include <cmath> // For round, floor, ceil, sinf, cosf, fabs, sqrtf
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
        // Resize only if necessary or if size changed
        if (_pixelOccupationMap.size() != (size_t)_canvasWidth * _canvasHeight) {
            _pixelOccupationMap.resize((size_t)_canvasWidth * _canvasHeight, 0);
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
    float scaled_lx = logical_x * item.scaleFactor;
    float scaled_ly = logical_y * item.scaleFactor;
    matrix_apply_to_point(item.matrix, scaled_lx, scaled_ly, screen_x, screen_y);
}

void MicroPatternsDrawing::screenToLogicalBase(float screen_x, float screen_y, const DisplayListItem& item, float& base_logical_x, float& base_logical_y) {
    float scaled_logical_x, scaled_logical_y;
    matrix_apply_to_point(item.inverseMatrix, screen_x, screen_y, scaled_logical_x, scaled_logical_y);

    if (item.scaleFactor == 0.0f) {
        base_logical_x = scaled_logical_x;
        base_logical_y = scaled_logical_y;
    } else {
        base_logical_x = scaled_logical_x / item.scaleFactor;
        base_logical_y = scaled_logical_y / item.scaleFactor;
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
    matrix_apply_to_point(item.inverseMatrix, screen_pixel_center_x, screen_pixel_center_y,
                          scaled_logical_x, scaled_logical_y);
    return fillColorFromScaled(scaled_logical_x, scaled_logical_y, item);
}

uint8_t MicroPatternsDrawing::fillColorFromScaled(float scaled_logical_x, float scaled_logical_y, const DisplayListItem& item) const {
    float base_lx, base_ly;
    if (item.scaleFactor == 0.0f) {
        base_lx = scaled_logical_x;
        base_ly = scaled_logical_y;
    } else {
        base_lx = scaled_logical_x / item.scaleFactor;
        base_ly = scaled_logical_y / item.scaleFactor;
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
    int lx = item.intParams.at("X");
    int ly = item.intParams.at("Y");

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + 1), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly + 1), item, s_br_x, s_br_y);

    // Determine screen-space bounding box (rounded to int for iteration)
    int min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    int max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));

    // Clip to canvas
    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            float screen_center_x = static_cast<float>(sx_iter) + 0.5f;
            float screen_center_y = static_cast<float>(sy_iter) + 0.5f;

            float scaled_logical_x, scaled_logical_y;
            matrix_apply_to_point(item.inverseMatrix, screen_center_x, screen_center_y, scaled_logical_x, scaled_logical_y);

            float logical_pixel_start_x_scaled = static_cast<float>(lx) * item.scaleFactor;
            float logical_pixel_end_x_scaled = static_cast<float>(lx + 1) * item.scaleFactor;
            float logical_pixel_start_y_scaled = static_cast<float>(ly) * item.scaleFactor;
            float logical_pixel_end_y_scaled = static_cast<float>(ly + 1) * item.scaleFactor;
            
            if (scaled_logical_x >= logical_pixel_start_x_scaled && scaled_logical_x < logical_pixel_end_x_scaled &&
                scaled_logical_y >= logical_pixel_start_y_scaled && scaled_logical_y < logical_pixel_end_y_scaled) {
                rawPixel(sx_iter, sy_iter, item.color);
            }
        }
    }
}

void MicroPatternsDrawing::drawFilledPixel(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.intParams.at("X");
    int ly = item.intParams.at("Y");

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + 1), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + 1), static_cast<float>(ly + 1), item, s_br_x, s_br_y);

    int min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    int max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));

    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    for (int sy_iter = min_sy; sy_iter < max_sy; ++sy_iter) {
        for (int sx_iter = min_sx; sx_iter < max_sx; ++sx_iter) {
            float screen_center_x = static_cast<float>(sx_iter) + 0.5f;
            float screen_center_y = static_cast<float>(sy_iter) + 0.5f;

            float scaled_logical_x, scaled_logical_y;
            matrix_apply_to_point(item.inverseMatrix, screen_center_x, screen_center_y, scaled_logical_x, scaled_logical_y);
            
            float logical_pixel_start_x_scaled = static_cast<float>(lx) * item.scaleFactor;
            float logical_pixel_end_x_scaled = static_cast<float>(lx + 1) * item.scaleFactor;
            float logical_pixel_start_y_scaled = static_cast<float>(ly) * item.scaleFactor;
            float logical_pixel_end_y_scaled = static_cast<float>(ly + 1) * item.scaleFactor;

            if (scaled_logical_x >= logical_pixel_start_x_scaled && scaled_logical_x < logical_pixel_end_x_scaled &&
                scaled_logical_y >= logical_pixel_start_y_scaled && scaled_logical_y < logical_pixel_end_y_scaled) {
                uint8_t fillColor = getFillColor(screen_center_x, screen_center_y, item);
                rawPixel(sx_iter, sy_iter, fillColor);
            }
        }
    }
}


void MicroPatternsDrawing::drawLine(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx1 = item.intParams.at("X1");
    int ly1 = item.intParams.at("Y1");
    int lx2 = item.intParams.at("X2");
    int ly2 = item.intParams.at("Y2");

    float sx1_f, sy1_f, sx2_f, sy2_f;
    transformPoint(static_cast<float>(lx1), static_cast<float>(ly1), item, sx1_f, sy1_f);
    transformPoint(static_cast<float>(lx2), static_cast<float>(ly2), item, sx2_f, sy2_f);
    rawLine(static_cast<int>(round(sx1_f)), static_cast<int>(round(sy1_f)),
            static_cast<int>(round(sx2_f)), static_cast<int>(round(sy2_f)), item.color);
}

void MicroPatternsDrawing::drawRect(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.intParams.at("X");
    int ly = item.intParams.at("Y");
    int lw = item.intParams.at("WIDTH");
    int lh = item.intParams.at("HEIGHT");
    if (lw <= 0 || lh <= 0) return;

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + lh), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly + lh), item, s_br_x, s_br_y);

    rawLine(round(s_tl_x), round(s_tl_y), round(s_tr_x), round(s_tr_y), item.color); // Top
    rawLine(round(s_tr_x), round(s_tr_y), round(s_br_x), round(s_br_y), item.color); // Right
    rawLine(round(s_br_x), round(s_br_y), round(s_bl_x), round(s_bl_y), item.color); // Bottom
    rawLine(round(s_bl_x), round(s_bl_y), round(s_tl_x), round(s_tl_y), item.color); // Left
}

void MicroPatternsDrawing::fillRect(const DisplayListItem& item) {
    if (!_canvas) return;
    int lx = item.intParams.at("X");
    int ly = item.intParams.at("Y");
    int lw = item.intParams.at("WIDTH");
    int lh = item.intParams.at("HEIGHT");
    if (lw <= 0 || lh <= 0) return;

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx), static_cast<float>(ly), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx), static_cast<float>(ly + lh), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx + lw), static_cast<float>(ly + lh), item, s_br_x, s_br_y);

    int min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    int max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));

    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);
    if (min_sx >= max_sx || min_sy >= max_sy) return;

    // Loop invariants, hoisted. These four products used to be recomputed on
    // every single pixel; the compiler could not hoist them itself because the
    // canvas write in the loop body may alias `item`.
    const float* IM = item.inverseMatrix;
    const float im0 = IM[0], im1 = IM[1], im2 = IM[2], im3 = IM[3], im4 = IM[4], im5 = IM[5];
    const float sf = item.scaleFactor;
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
        if ((sy_iter & 7) == 0) { yield(); esp_task_wdt_reset(); }

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

        uint8_t* occRow = occ ? occ + (size_t)sy_iter * cw : nullptr;
        if (!patterned) {
            for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
                emitPixel(sx_iter, sy_iter, flatColor, occRow, skipped);
            }
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

        if (patRow) {
            for (int sx_iter = x0; sx_iter < x1; ++sx_iter) {
                float blx = im0 * (static_cast<float>(sx_iter) + 0.5f) + m2y + im4;
                if (sf != 0.0f) blx = useRcp ? blx * rcp : blx / sf;
                int px = ifloor_i(blx) % patW;
                if (px < 0) px += patW;
                emitPixel(sx_iter, sy_iter, patRow[px] == 1 ? patOn : patOff, occRow, skipped);
            }
            continue;
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
    esp_task_wdt_reset(); // Ensure WDT is reset after the loop
}

void MicroPatternsDrawing::drawCircle(const DisplayListItem& item) {
    if (!_canvas) return;
    int lcx = item.intParams.at("X");
    int lcy = item.intParams.at("Y");
    int lr = item.intParams.at("RADIUS");
    if (lr <= 0) return;
     
    float scx_f, scy_f;
    transformPoint(static_cast<float>(lcx), static_cast<float>(lcy), item, scx_f, scy_f);
     
    float mat_scale_x = sqrtf(item.matrix[0]*item.matrix[0] + item.matrix[1]*item.matrix[1]);
    float mat_scale_y = sqrtf(item.matrix[2]*item.matrix[2] + item.matrix[3]*item.matrix[3]);
    float screen_radius_approx = static_cast<float>(lr) * item.scaleFactor * std::max(mat_scale_x, mat_scale_y);
     
    int scx = static_cast<int>(round(scx_f));
    int scy = static_cast<int>(round(scy_f));
    int scaledRadius = static_cast<int>(round(screen_radius_approx));
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
    int lcx = item.intParams.at("X");
    int lcy = item.intParams.at("Y");
    int lr = item.intParams.at("RADIUS");
    if (lr <= 0) return;

    float logical_radius = static_cast<float>(lr);
    float s_pts_x[8], s_pts_y[8];
    transformPoint(static_cast<float>(lcx), static_cast<float>(lcy - logical_radius), item, s_pts_x[0], s_pts_y[0]);
    transformPoint(static_cast<float>(lcx + logical_radius), static_cast<float>(lcy), item, s_pts_x[1], s_pts_y[1]);
    transformPoint(static_cast<float>(lcx), static_cast<float>(lcy + logical_radius), item, s_pts_x[2], s_pts_y[2]);
    transformPoint(static_cast<float>(lcx - logical_radius), static_cast<float>(lcy), item, s_pts_x[3], s_pts_y[3]);
    float diag_offset = logical_radius * 0.7071f;
    transformPoint(static_cast<float>(lcx + diag_offset), static_cast<float>(lcy - diag_offset), item, s_pts_x[4], s_pts_y[4]);
    transformPoint(static_cast<float>(lcx + diag_offset), static_cast<float>(lcy + diag_offset), item, s_pts_x[5], s_pts_y[5]);
    transformPoint(static_cast<float>(lcx - diag_offset), static_cast<float>(lcy + diag_offset), item, s_pts_x[6], s_pts_y[6]);
    transformPoint(static_cast<float>(lcx - diag_offset), static_cast<float>(lcy - diag_offset), item, s_pts_x[7], s_pts_y[7]);

    float min_sx_f = s_pts_x[0], max_sx_f = s_pts_x[0];
    float min_sy_f = s_pts_y[0], max_sy_f = s_pts_y[0];
    for(int i=1; i<8; ++i) {
        min_sx_f = std::min(min_sx_f, s_pts_x[i]); max_sx_f = std::max(max_sx_f, s_pts_x[i]);
        min_sy_f = std::min(min_sy_f, s_pts_y[i]); max_sy_f = std::max(max_sy_f, s_pts_y[i]);
    }

    int min_sx = static_cast<int>(floor(min_sx_f));
    int max_sx = static_cast<int>(ceil(max_sx_f));
    int min_sy = static_cast<int>(floor(min_sy_f));
    int max_sy = static_cast<int>(ceil(max_sy_f));
    
    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    if (min_sx >= max_sx || min_sy >= max_sy) return;

    const float logical_radius_sq = logical_radius * logical_radius;
    const float* IM = item.inverseMatrix;
    const float im0 = IM[0], im1 = IM[1], im2 = IM[2], im3 = IM[3], im4 = IM[4], im5 = IM[5];
    const float sf = item.scaleFactor;
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
        if ((sy_iter & 7) == 0) { yield(); esp_task_wdt_reset(); }

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

        int x0 = min_sx, x1 = max_sx;
        narrowSpan(blx, flcx - logical_radius - 1.0f, flcx + logical_radius + 1.0f, x0, x1);
        if (x0 >= x1) continue;
        narrowSpan(bly, flcy - logical_radius - 1.0f, flcy + logical_radius + 1.0f, x0, x1);
        if (x0 >= x1) continue;

        uint8_t* occRow = occ ? occ + (size_t)sy_iter * cw : nullptr;
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
    esp_task_wdt_reset();
}

void MicroPatternsDrawing::drawAsset(const DisplayListItem& item, const MicroPatternsAsset& asset) {
    if (!_canvas || asset.width <= 0 || asset.height <= 0 || asset.data.empty()) return;
    int lx_asset_origin = item.intParams.at("X");
    int ly_asset_origin = item.intParams.at("Y");

    float s_tl_x, s_tl_y, s_tr_x, s_tr_y, s_bl_x, s_bl_y, s_br_x, s_br_y;
    transformPoint(static_cast<float>(lx_asset_origin), static_cast<float>(ly_asset_origin), item, s_tl_x, s_tl_y);
    transformPoint(static_cast<float>(lx_asset_origin + asset.width), static_cast<float>(ly_asset_origin), item, s_tr_x, s_tr_y);
    transformPoint(static_cast<float>(lx_asset_origin), static_cast<float>(ly_asset_origin + asset.height), item, s_bl_x, s_bl_y);
    transformPoint(static_cast<float>(lx_asset_origin + asset.width), static_cast<float>(ly_asset_origin + asset.height), item, s_br_x, s_br_y);

    int min_sx = static_cast<int>(floor(std::min({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int max_sx = static_cast<int>(ceil(std::max({s_tl_x, s_tr_x, s_bl_x, s_br_x})));
    int min_sy = static_cast<int>(floor(std::min({s_tl_y, s_tr_y, s_bl_y, s_br_y})));
    int max_sy = static_cast<int>(ceil(std::max({s_tl_y, s_tr_y, s_bl_y, s_br_y})));

    min_sx = std::max(0, min_sx);
    min_sy = std::max(0, min_sy);
    max_sx = std::min(_canvasWidth, max_sx);
    max_sy = std::min(_canvasHeight, max_sy);

    if (min_sx >= max_sx || min_sy >= max_sy) return;

    const float* IM = item.inverseMatrix;
    const float im0 = IM[0], im1 = IM[1], im2 = IM[2], im3 = IM[3], im4 = IM[4], im5 = IM[5];
    const float sf = item.scaleFactor;
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
        if ((sy_iter & 7) == 0) { yield(); esp_task_wdt_reset(); }

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

        uint8_t* occRow = occ ? occ + (size_t)sy_iter * cw : nullptr;

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

        if (assetRow) {
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
    esp_task_wdt_reset();
}

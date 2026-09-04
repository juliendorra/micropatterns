#ifndef MICROPATTERNS_DRAWING_H
#define MICROPATTERNS_DRAWING_H

#include <cstring>
#include "mp_canvas.h" // MPCanvas: the 4-method platform canvas (M5EPD or Watchy)
#include <esp_task_wdt.h> // For watchdog reset functions
#include <functional> // For std::function
#include "micropatterns_command.h" // For DisplayListItem, MicroPatternsAsset, MicroPatternsState
#include "matrix_utils.h" // For matrix operations

// Define colors (consistent with runtime)
const uint8_t DRAWING_COLOR_WHITE = 0;
const uint8_t DRAWING_COLOR_BLACK = 15;

class MicroPatternsDrawing {
public:
    MicroPatternsDrawing(MPCanvas* canvas);

    void setCanvas(MPCanvas* canvas);
    void setInterruptCheckCallback(std::function<bool()> cb);
    void clearCanvas();

    // Drawing primitives now take DisplayListItem to get resolved params and snapshotted state
    void drawPixel(const DisplayListItem& item);
    void drawLine(const DisplayListItem& item);
    void drawRect(const DisplayListItem& item);
    void fillRect(const DisplayListItem& item);
    void drawCircle(const DisplayListItem& item);
    void fillCircle(const DisplayListItem& item);
    void drawAsset(const DisplayListItem& item, const MicroPatternsAsset& asset); // Asset passed in
    void drawFilledPixel(const DisplayListItem& item);

private:
    MPCanvas* _canvas;
    int _canvasWidth;
    int _canvasHeight;
    std::function<bool()> _interrupt_check_cb;
    // Overdraw map, ONE BIT PER PIXEL.
    //
    // This was one byte per pixel: 40,000 bytes on the Watchy's 200x200 panel,
    // 518,400 on the M5Paper. That is what broke City 2 -- after parsing the
    // largest script the biggest free block was ~26KB, the 40,000-byte resize
    // could not be satisfied, and with exceptions disabled std::vector::resize()
    // calls abort(). One bit per pixel makes it 5,000 bytes on the Watchy and
    // 64,800 on the M5Paper, which fits with room to spare.
    //
    // Rows are byte-aligned so a row pointer can still be hoisted out of the
    // pixel loop: stride = (width + 7) / 8.
    std::vector<uint8_t> _pixelOccupationMap;
    int _occStride = 0;
public:
    int occStride() const { return _occStride; }
private:
    bool _usePixelOccupationMap;
    bool _fixedPointEnabled = true;
    bool _integerTransform = false;
    unsigned int _overdrawSkippedPixels; // For stats

    // Pixels emitted through a fixed-point inner loop this render.
    //
    // This exists to make the equivalence gate falsifiable. When the Q16.16
    // path was first compared against the float path it reported 21/21
    // identical -- which is also exactly what a fixed path that silently never
    // executed would report. Proving otherwise took deliberately corrupting the
    // fixed loops to watch the comparison fail. A gate that cannot fail is not
    // a gate, and "go and sabotage it" is not a procedure anyone will repeat.
    //
    // So the renderer now says how much work the fast path actually did, and
    // `compare-paths` refuses to pass a fixed path that did none. Accumulated
    // per SPAN, never per pixel, so it costs nothing in the loop it measures.
    unsigned long _fixedPointPixels = 0;

    // Same reasoning as _fixedPointPixels, for the exact-integer transform:
    // "identical output" is also what a path that never ran would report, and
    // this one reported 21/21 identical on its first try too. Counted per ITEM,
    // so it costs nothing.
    unsigned long _integerXformCalls = 0;

    // Scanline spans written eight pixels at a time instead of one. Same
    // "prove it ran" gate as the two counters above.
    unsigned long _spanRows = 0;
    bool _spanWriter = false;

    void initPixelOccupationMap(); // Initialize map if needed

public: // Made public for DisplayListRenderer
    // Selects the Q16.16 fixed-point inner loops and the screen-space circle
    // span. ON by default since 2026-09-03: measured 15-42% faster per
    // operation on a Watchy, with its own golden set. Setting it false selects
    // the original float rasteriser, which is kept selectable and gated rather
    // than deleted -- see docs/measurements/2026-09-03-fixed-point-rasteriser.md.
    void setFixedPointEnabled(bool enable) { _fixedPointEnabled = enable; }

    // Exact-integer forward transform for endpoints, centres and spans. OFF by
    // default while it is being measured; see docs/measurements/.
    void setIntegerTransformEnabled(bool enable) { _integerTransform = enable; }
    bool fixedPointEnabled() const { return _fixedPointEnabled; }

    void enablePixelOccupationMap(bool enable);
    void resetPixelOccupationMap(); // Clears the map
    // Defined inline: rawPixel() calls both per pixel, and out-of-line these were
    // two calls that each re-validated bounds the caller had already checked.
    bool isPixelOccupied(int sx, int sy) const {
        if (!_usePixelOccupationMap || sx < 0 || sx >= _canvasWidth || sy < 0 || sy >= _canvasHeight) {
            return false;
        }
        if (_pixelOccupationMap.empty()) return false;
        return (_pixelOccupationMap[(size_t)sy * _occStride + (sx >> 3)] & (0x80u >> (sx & 7))) != 0;
    }
    void markPixelOccupied(int sx, int sy) {
        if (!_usePixelOccupationMap || sx < 0 || sx >= _canvasWidth || sy < 0 || sy >= _canvasHeight) {
            return;
        }
        if (_pixelOccupationMap.empty()) return;
        _pixelOccupationMap[(size_t)sy * _occStride + (sx >> 3)] |= (uint8_t)(0x80u >> (sx & 7));
    }
    unsigned int getOverdrawSkippedPixelsCount() const { return _overdrawSkippedPixels; }
    unsigned long getFixedPointPixels() const { return _fixedPointPixels; }
    unsigned long getIntegerXformCalls() const { return _integerXformCalls; }
    unsigned long getSpanRows() const { return _spanRows; }
    unsigned long getTiledRows() const { return _tiledRows; }
    void setSpanWriterEnabled(bool on) { _spanWriter = on; }

    // Paint [x0, x1) on row sy in ONE colour, a byte of pixels at a time.
    //
    // This is where the per-pixel cost actually was. Measured on a Watchy,
    // op_fill_rect_solid -- no coordinates, no pattern, nothing but emitPixel
    // across a span -- took 33 ms for 40,000 pixels: ~200 cycles to set one
    // bit. Almost all of it was the library's drawPixel re-deriving rotation,
    // window, page and stride for every pixel, plus this class's own
    // occupancy test, when both answers are constant along the run.
    //
    // So: build the span as a bit mask, fold the occupancy map in byte-wise
    // (paint = mask & ~occupied; occupied |= mask), and hand the canvas one
    // row of bytes. The occupancy map is MSB-first for exactly this reason.
    inline void emitSolidSpan(int sy, int x0, int x1, uint8_t color,
                              uint8_t* occRow, unsigned int& skipped) {
        if (x0 >= x1) return;
        const int b0 = x0 >> 3, b1 = (x1 - 1) >> 3, nb = b1 - b0 + 1;
        if (!_spanWriter || nb > kMaxSpanBytes) {
            for (int sx = x0; sx < x1; ++sx) emitPixel(sx, sy, color, occRow, skipped);
            return;
        }
        uint8_t cover[kMaxSpanBytes];
        for (int b = b0; b <= b1; ++b) {
            uint8_t m = 0xFFu;
            if (b == b0) m &= (uint8_t)(0xFFu >> (x0 & 7));               // drop bits left of x0
            if (b == b1) m &= (uint8_t)(0xFFu << (7 - ((x1 - 1) & 7)));   // drop bits right of x1-1
            if (occRow) {
                const uint8_t o = occRow[b];
                skipped += (unsigned int)__builtin_popcount((unsigned)(m & o));
                occRow[b] = (uint8_t)(o | m);
                m = (uint8_t)(m & ~o);
            }
            cover[b - b0] = m;
        }
        ++_spanRows;
        mp_canvas_fill_mask_row(_canvas, sy, b0, nb, cover, color);
    }
    static const int kMaxSpanBytes = 128;   // 1024 px; wider spans fall back

    static inline int32_t gcd32(int32_t a, int32_t b) {
        if (a < 0) a = -a; if (b < 0) b = -b;
        while (b) { const int32_t t = a % b; a = b; b = t; }
        return a;
    }
    unsigned long _tiledRows = 0;   // spans whose ink mask was tiled, not walked

    // Same, with a caller-built cover mask (bit set = paint this pixel `color`,
    // clear = leave it). `cover` is indexed from byte x0>>3 and MSB-first. This
    // is DRAW: an asset's set bits are painted, its clear bits are transparent.
    inline void emitMaskSpan(int sy, int x0, int x1, uint8_t* cover, uint8_t color,
                             uint8_t* occRow, unsigned int& skipped) {
        const int b0 = x0 >> 3, nb = ((x1 - 1) >> 3) - b0 + 1;
        if (occRow) {
            for (int b = 0; b < nb; ++b) {
                const uint8_t m = cover[b], o = occRow[b0 + b];
                skipped += (unsigned int)__builtin_popcount((unsigned)(m & o));
                occRow[b0 + b] = (uint8_t)(o | m);
                cover[b] = (uint8_t)(m & ~o);
            }
        }
        ++_spanRows;
        mp_canvas_fill_mask_row(_canvas, sy, b0, nb, cover, color);
    }

    // Two colours: every pixel of [x0,x1) is painted, `on` where the ink bit
    // is set and `off` where it is clear. This is a pattern fill. Two row
    // blits rather than a new canvas primitive; the rows are at most 120 bytes.
    inline void emitPatternSpan(int sy, int x0, int x1, const uint8_t* ink,
                                uint8_t on, uint8_t off, uint8_t* occRow, unsigned int& skipped) {
        const int b0 = x0 >> 3, b1 = (x1 - 1) >> 3, nb = b1 - b0 + 1;
        uint8_t onM[kMaxSpanBytes], offM[kMaxSpanBytes];
        for (int b = b0; b <= b1; ++b) {
            uint8_t m = 0xFFu;
            if (b == b0) m &= (uint8_t)(0xFFu >> (x0 & 7));
            if (b == b1) m &= (uint8_t)(0xFFu << (7 - ((x1 - 1) & 7)));
            if (occRow) {
                const uint8_t o = occRow[b];
                skipped += (unsigned int)__builtin_popcount((unsigned)(m & o));
                occRow[b] = (uint8_t)(o | m);
                m = (uint8_t)(m & ~o);
            }
            const uint8_t k = (uint8_t)(ink[b - b0] & m);
            onM[b - b0]  = k;
            offM[b - b0] = (uint8_t)(m & ~k);
        }
        ++_spanRows;
        mp_canvas_fill_mask_row(_canvas, sy, b0, nb, onM,  on);
        mp_canvas_fill_mask_row(_canvas, sy, b0, nb, offM, off);
    }


    // Transformation helpers using float math and matrices, now use DisplayListItem's state
    void transformPoint(float logical_x, float logical_y, const DisplayListItem& item, float& screen_x, float& screen_y);
    void screenToLogicalBase(float screen_x, float screen_y, const DisplayListItem& item, float& base_logical_x, float& base_logical_y);
private:
    // Raw drawing on canvas using screen coordinates (sx, sy)
    void rawPixel(int sx, int sy, uint8_t color);
    void rawLine(int sx1, int sy1, int sx2, int sy2, uint8_t color);

    // Helper for fill patterns. Takes screen pixel center coordinates and DisplayListItem's state.
    uint8_t getFillColor(float screen_pixel_center_x, float screen_pixel_center_y, const DisplayListItem& item);

    // Same as getFillColor(), but takes the *already computed* inverse-transformed
    // ("scaled logical") coordinates. Every fill loop has just computed those in
    // order to run its shape test; getFillColor() used to redo the identical
    // transform. Same arithmetic, same result, half the transforms.
    uint8_t fillColorFromScaled(float scaled_logical_x, float scaled_logical_y, const DisplayListItem& item) const;

    // As above but the caller has also already undone scaleFactor -- the fill
    // loops need the base-logical coordinates for their own shape test anyway.
    uint8_t fillColorFromBase(float base_lx, float base_ly, const DisplayListItem& item) const;

public: // occupancy map accessors -- DisplayListRenderer reads these to mark
        // the occlusion buffer from actually-painted pixels.
    // Returns the base of the occupancy map if it is live for this pass, else nullptr.
    // Hoisted out of the pixel loop by the fill primitives.
    const uint8_t* occupancyBase() const {
        if (!_usePixelOccupationMap || _pixelOccupationMap.empty()) return nullptr;
        return _pixelOccupationMap.data();
    }
    uint8_t* occupancyBase() {
        if (!_usePixelOccupationMap || _pixelOccupationMap.empty()) return nullptr;
        return _pixelOccupationMap.data();
    }

    // Write one pixel that is already known to be inside the canvas, with the
    // occupancy row pointer already resolved. `skipped` is a loop-local counter
    // folded back into _overdrawSkippedPixels when the primitive finishes -- the
    // member itself cannot be kept in a register across the canvas write.
    inline void emitPixel(int sx, int sy, uint8_t color, uint8_t* occRow, unsigned int& skipped) {
        if (occRow) {
            uint8_t* slot = occRow + (sx >> 3);
            // MSB-first: pixel x of a byte is bit (7 - x%8). This is the SAME
            // order a 1-bpp framebuffer uses, on purpose, so a span's
            // occupancy bytes and its framebuffer bytes can be combined with
            // one AND -- see emitSolidSpan.
            const uint8_t mask = (uint8_t)(0x80u >> (sx & 7));
            if (*slot & mask) { ++skipped; return; }
            *slot |= mask;
        }
        _canvas->drawPixel(sx, sy, color);
    }
};

#endif // MICROPATTERNS_DRAWING_H
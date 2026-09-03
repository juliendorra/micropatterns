#ifndef MICROPATTERNS_DRAWING_H
#define MICROPATTERNS_DRAWING_H

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

    void initPixelOccupationMap(); // Initialize map if needed

public: // Made public for DisplayListRenderer
    // Selects the Q16.16 fixed-point inner loops and the screen-space circle
    // span. ON by default since 2026-09-03: measured 15-42% faster per
    // operation on a Watchy, with its own golden set. Setting it false selects
    // the original float rasteriser, which is kept selectable and gated rather
    // than deleted -- see docs/measurements/2026-09-03-fixed-point-rasteriser.md.
    void setFixedPointEnabled(bool enable) { _fixedPointEnabled = enable; }
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
        return (_pixelOccupationMap[(size_t)sy * _occStride + (sx >> 3)] & (1u << (sx & 7))) != 0;
    }
    void markPixelOccupied(int sx, int sy) {
        if (!_usePixelOccupationMap || sx < 0 || sx >= _canvasWidth || sy < 0 || sy >= _canvasHeight) {
            return;
        }
        if (_pixelOccupationMap.empty()) return;
        _pixelOccupationMap[(size_t)sy * _occStride + (sx >> 3)] |= (uint8_t)(1u << (sx & 7));
    }
    unsigned int getOverdrawSkippedPixelsCount() const { return _overdrawSkippedPixels; }
    unsigned long getFixedPointPixels() const { return _fixedPointPixels; }


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
            const uint8_t mask = (uint8_t)(1u << (sx & 7));
            if (*slot & mask) { ++skipped; return; }
            *slot |= mask;
        }
        _canvas->drawPixel(sx, sy, color);
    }
};

#endif // MICROPATTERNS_DRAWING_H
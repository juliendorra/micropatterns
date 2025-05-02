#ifndef MICROPATTERNS_DRAWING_H
#define MICROPATTERNS_DRAWING_H

#include <M5EPD.h>
#include "micropatterns_command.h" // For state and asset structures

// Define fixed-point scaling factor (e.g., 2^8 = 256)
#define FIXED_POINT_SCALE 256
#define INT_TO_FIXED(x) ((x) * FIXED_POINT_SCALE)
#define FIXED_TO_INT(x) ((x) / FIXED_POINT_SCALE)
#define FIXED_MULT(a, b) ((int32_t)(a) * (b) / FIXED_POINT_SCALE)
#define FIXED_DIV(a, b) ((int32_t)(a) * FIXED_POINT_SCALE / (b))


class MicroPatternsDrawing {
public:
    MicroPatternsDrawing(M5EPD_Canvas* canvas);

    void setCanvas(M5EPD_Canvas* canvas);
    void clearCanvas(); // Helper to fill with white

    // Drawing primitives - operate on logical coordinates, apply state transform
    void drawPixel(int lx, int ly, const MicroPatternsState& state);
    void drawLine(int lx1, int ly1, int lx2, int ly2, const MicroPatternsState& state);
    void drawRect(int lx, int ly, int lw, int lh, const MicroPatternsState& state);
    void fillRect(int lx, int ly, int lw, int lh, const MicroPatternsState& state);
    void drawCircle(int lcx, int lcy, int lr, const MicroPatternsState& state);
    void fillCircle(int lcx, int lcy, int lr, const MicroPatternsState& state);
    void drawAsset(int lx, int ly, const MicroPatternsAsset& asset, const MicroPatternsState& state);
    void drawFilledPixel(int lx, int ly, const MicroPatternsState& state); // For FILL_PIXEL command

private:
    M5EPD_Canvas* _canvas;
    int _canvasWidth;
    int _canvasHeight;

    // Precomputed sin/cos tables (scaled by FIXED_POINT_SCALE)
    std::vector<int> _sinTable;
    std::vector<int> _cosTable;
    void precomputeTrigTables();

    // Transformation helper using integer math
    // Converts logical (lx, ly) to screen (sx, sy) using the state's transform sequence.
    void transformPoint(int lx, int ly, const MicroPatternsState& state, int& sx, int& sy);

    // Raw drawing on canvas using screen coordinates (sx, sy)
    void rawPixel(int sx, int sy, uint8_t color); // Draws a single canvas pixel
    void rawLine(int sx1, int sy1, int sx2, int sy2, uint8_t color); // Bresenham

    // Helper to draw a scaled block (used by drawPixel, fillRect etc.)
    void drawScaledBlock(int screenX, int screenY, int scale, uint8_t color);

    // Helper for fill patterns
    uint8_t getFillColor(int screenX, int screenY, const MicroPatternsState& state);
};

#endif // MICROPATTERNS_DRAWING_H
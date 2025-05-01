#ifndef MICROPATTERNS_DRAWING_H
#define MICROPATTERNS_DRAWING_H

#include <M5EPD.h>
#include "micropatterns_command.h" // For state and asset structures

class MicroPatternsDrawing {
public:
    MicroPatternsDrawing(M5EPD_Canvas* canvas);

    void setCanvas(M5EPD_Canvas* canvas);
    void clearCanvas(); // Helper to fill with white

    // Drawing primitives - operate on logical coordinates, apply state transform
    void drawPixel(float x, float y, const MicroPatternsState& state);
    void drawLine(float x1, float y1, float x2, float y2, const MicroPatternsState& state);
    void drawRect(float x, float y, float w, float h, const MicroPatternsState& state);
    void fillRect(float x, float y, float w, float h, const MicroPatternsState& state);
    void drawCircle(float cx, float cy, float r, const MicroPatternsState& state);
    void fillCircle(float cx, float cy, float r, const MicroPatternsState& state);
    void drawAsset(float x, float y, const MicroPatternsAsset& asset, const MicroPatternsState& state);
    void drawFilledPixel(float x, float y, const MicroPatternsState& state); // For FILL_PIXEL command

private:
    M5EPD_Canvas* _canvas;

    // Transformation helper
    void transformPoint(float lx, float ly, const MicroPatternsState& state, int& sx, int& sy);

    // Raw drawing on canvas using screen coordinates (sx, sy)
    void rawPixel(int sx, int sy, uint8_t color); // Draws a single canvas pixel
    void rawLine(int sx1, int sy1, int sx2, int sy2, uint8_t color); // Bresenham

    // Helper for fill patterns
    uint8_t getFillColor(int screenX, int screenY, const MicroPatternsState& state);
};

#endif // MICROPATTERNS_DRAWING_H
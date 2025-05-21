#ifndef MICROPATTERNS_DRAWING_H
#define MICROPATTERNS_DRAWING_H

#include <M5EPD.h>
#include <esp_task_wdt.h> // For watchdog reset functions
#include <functional> // For std::function
#include "micropatterns_command.h" // For state and asset structures
#include "matrix_utils.h" // For matrix operations

// Define colors (consistent with runtime)
const uint8_t DRAWING_COLOR_WHITE = 0;
const uint8_t DRAWING_COLOR_BLACK = 15;

class MicroPatternsDrawing {
public:
    MicroPatternsDrawing(M5EPD_Canvas* canvas);

    void setCanvas(M5EPD_Canvas* canvas);
    void setInterruptCheckCallback(std::function<bool()> cb); // Method to set the callback
    void clearCanvas(); // Helper to fill with white

    // Drawing primitives - operate on logical coordinates, apply state transform
    void drawPixel(int lx, int ly, const MicroPatternsState& state);
    void drawLine(int lx1, int ly1, int lx2, int ly2, const MicroPatternsState& state);
    void drawRect(int lx, int ly, int lw, int lh, const MicroPatternsState& state);
    void fillRect(int lx, int ly, int lw, int lh, const MicroPatternsState& state);
    void drawCircle(int lcx, int lcy, int lr, const MicroPatternsState& state);
    void fillCircle(int lcx, int lcy, int lr, const MicroPatternsState& state);
    void drawAsset(int lx_asset_origin, int ly_asset_origin, const MicroPatternsAsset& asset, const MicroPatternsState& state);
    void drawFilledPixel(int lx, int ly, const MicroPatternsState& state); // For FILL_PIXEL command

private:
    M5EPD_Canvas* _canvas;
    int _canvasWidth;
    int _canvasHeight;
    std::function<bool()> _interrupt_check_cb; // Callback to check for interrupt

    // Transformation helpers using float math and matrices
    // Converts logical (logical_x, logical_y) to screen (screen_x, screen_y)
    void transformPoint(float logical_x, float logical_y, const MicroPatternsState& state, float& screen_x, float& screen_y);
    
    // Converts screen (screen_x, screen_y) to base logical (base_logical_x, base_logical_y)
    // "Base logical" is the coordinate system of patterns/assets before any script scale/transforms.
    void screenToLogicalBase(float screen_x, float screen_y, const MicroPatternsState& state, float& base_logical_x, float& base_logical_y);

    // Raw drawing on canvas using screen coordinates (sx, sy)
    void rawPixel(int sx, int sy, uint8_t color); // Draws a single canvas pixel
    void rawLine(int sx1, int sy1, int sx2, int sy2, uint8_t color); // Bresenham

    // Helper for fill patterns. Takes screen pixel center coordinates.
    uint8_t getFillColor(float screen_pixel_center_x, float screen_pixel_center_y, const MicroPatternsState& state);
};

#endif // MICROPATTERNS_DRAWING_H
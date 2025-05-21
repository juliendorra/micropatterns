#ifndef MICROPATTERNS_DRAWING_H
#define MICROPATTERNS_DRAWING_H

#include <M5EPD.h>
#include <esp_task_wdt.h> // For watchdog reset functions
#include <functional> // For std::function
#include "micropatterns_command.h" // For DisplayListItem, MicroPatternsAsset, MicroPatternsState
#include "matrix_utils.h" // For matrix operations

// Define colors (consistent with runtime)
const uint8_t DRAWING_COLOR_WHITE = 0;
const uint8_t DRAWING_COLOR_BLACK = 15;

class MicroPatternsDrawing {
public:
    MicroPatternsDrawing(M5EPD_Canvas* canvas);

    void setCanvas(M5EPD_Canvas* canvas);
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
    M5EPD_Canvas* _canvas;
    int _canvasWidth;
    int _canvasHeight;
    std::function<bool()> _interrupt_check_cb;
    std::vector<uint8_t> _pixelOccupationMap;
    bool _usePixelOccupationMap;
    unsigned int _overdrawSkippedPixels; // For stats

    void initPixelOccupationMap(); // Initialize map if needed

public: // Made public for DisplayListRenderer
    void enablePixelOccupationMap(bool enable);
    void resetPixelOccupationMap(); // Clears the map
    bool isPixelOccupied(int sx, int sy) const;
    void markPixelOccupied(int sx, int sy);
    unsigned int getOverdrawSkippedPixelsCount() const { return _overdrawSkippedPixels; }


    // Transformation helpers using float math and matrices, now use DisplayListItem's state
    void transformPoint(float logical_x, float logical_y, const DisplayListItem& item, float& screen_x, float& screen_y);
    void screenToLogicalBase(float screen_x, float screen_y, const DisplayListItem& item, float& base_logical_x, float& base_logical_y);
private:
    // Raw drawing on canvas using screen coordinates (sx, sy)
    void rawPixel(int sx, int sy, uint8_t color);
    void rawLine(int sx1, int sy1, int sx2, int sy2, uint8_t color);

    // Helper for fill patterns. Takes screen pixel center coordinates and DisplayListItem's state.
    uint8_t getFillColor(float screen_pixel_center_x, float screen_pixel_center_y, const DisplayListItem& item);
};

#endif // MICROPATTERNS_DRAWING_H
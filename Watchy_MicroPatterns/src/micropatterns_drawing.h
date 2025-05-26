#pragma once

#include "micropatterns_command.h" // For DisplayListItem, MicroPatternsAsset
#include "IDrawingHAL.h"
#include "matrix_utils.h" // For matrix operations
#include <functional>     // For std::function (interrupt callback)
#include <vector>         // For pixelOccupationMap (if re-enabled)

class MicroPatternsDrawing {
public:
    MicroPatternsDrawing(IDrawingHAL* hal);

    void setInterruptCheckCallback(std::function<bool()> cb);
    void clearCanvas(); // Clears HAL, and potentially pixel map

    // Drawing primitives using DisplayListItem (which contains resolved params and state)
    void drawPixel(const DisplayListItem& item);
    void fillRect(const DisplayListItem& item);
    void drawLine(const DisplayListItem& item);
    void drawCircle(const DisplayListItem& item);
    void fillCircle(const DisplayListItem& item);
    void drawText(const DisplayListItem& item); // Assuming text is in item.stringParams["TEXT"] or similar
    void drawBitmap(const DisplayListItem& item, const MicroPatternsAsset* asset); // Asset explicitly passed

    // Optional: If pixel-perfect culling or overdraw prevention is desired at this level
    // void enablePixelOccupationMap(bool enable);
    // void resetPixelOccupationMap();
    // unsigned int getOverdrawSkippedPixelsCount() const;

private:
    IDrawingHAL* _hal;
    std::function<bool()> _interrupt_check_cb;

public: // Made public for DisplayListRenderer and internal use
    // Transformation helpers (can be static or member, using matrix from DisplayListItem)
    // These transform logical coordinates from DisplayListItem's frame to screen coordinates
    void transformPoint(float logical_x, float logical_y, const float matrix[6], float scaleFactor, float& screen_x, float& screen_y);
    void screenToLogicalBase(float screen_x, float screen_y, const float inverseMatrix[6], float scaleFactor, float& base_logical_x, float& base_logical_y);

private:    
    // Raw drawing methods that directly call HAL (can be private if only used by public methods)
    void _rawPixel(int16_t sx, int16_t sy, uint16_t color);
    void _rawLine(int16_t sx1, int16_t sy1, int16_t sx2, int16_t sy2, uint16_t color);

    // Fill pattern helper, using DisplayListItem's fillAsset and color
    uint16_t _getFillColor(float logical_x, float logical_y, const DisplayListItem& item); // Remains private or protected if only for internal drawing methods

    // Pixel occupation map (optional, for overdraw reduction / advanced culling)
    // std::vector<uint8_t> _pixelOccupationMap;
    // bool _usePixelOccupationMap;
    // unsigned int _overdrawSkippedPixels;
    // int _canvasWidthForMap; // If map is used
    // int _canvasHeightForMap;
    // void _initPixelOccupationMap();
    // bool _isPixelOccupied(int sx, int sy) const;
    // void _markPixelOccupied(int sx, int sy);
};

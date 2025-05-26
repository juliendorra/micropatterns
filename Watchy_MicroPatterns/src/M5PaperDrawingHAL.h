#pragma once
#include "IDrawingHAL.h"
#include "M5EPD.h" // Original M5Paper EPD library

class M5PaperDrawingHAL : public IDrawingHAL {
public:
    M5PaperDrawingHAL(); // Constructor might initialize or get M5EPD_Canvas instance
    ~M5PaperDrawingHAL() override;

    // Basic Display Info
    int16_t getScreenWidth() const override;
    int16_t getScreenHeight() const override;

    // Initialization & Update
    void initDisplay(bool partial_update_mode = false) override; // M5Paper has modes like UPDATE_MODE_GC16
    void clearScreen() override;
    void updateDisplay(bool partial = false) override;

    // Drawing Primitives
    void drawPixel(int16_t x, int16_t y, uint16_t color) override;
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override;

    // Bitmap Drawing
    void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint16_t color) override;

    // Text Handling
    void setTextColor(uint16_t color) override;
    void setTextSize(uint8_t size) override; // M5EPD uses pixels, not multiplier
    void setTextWrap(bool wrap) override;    // May need custom implementation if not direct
    void setCursor(int16_t x, int16_t y) override;
    // void setFont(const GFXfont *font = nullptr) override; // M5EPD uses its own font system
    void print(const char* text) override;
    void print(int val) override;
    void println(const char* text) override;
    void println(int val) override;
    void getTextBounds(const char *str, int16_t x, int16_t y, int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h) override;

private:
    M5EPD_Canvas _canvas; // The main drawing canvas for M5Paper
    // M5EPD_Canvas _sprite_canvas; // For text, if needed, like in original DisplayManager
    uint16_t _currentTextColor;
    uint8_t _textSizeInPixels; // M5EPD text size is in pixels

    // Helper to map HAL_COLOR to M5EPD colors (grayscale 0-15, or specific M5_BLACK/M5_WHITE)
    uint8_t mapHalColorToM5Epd(uint16_t hal_color) const;
    void recreateSpriteCanvas(); // If using sprite for text
};

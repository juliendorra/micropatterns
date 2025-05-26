#pragma once
#include <stdint.h> // For uint16_t, uint8_t

// Forward declaration if needed for complex types, e.g. Font
// struct GFXfont; // Example if using GFXfont type

class IDrawingHAL {
public:
    virtual ~IDrawingHAL() = default;

    // Basic Display Info
    virtual int16_t getScreenWidth() const = 0;
    virtual int16_t getScreenHeight() const = 0;

    // Initialization & Update
    virtual void initDisplay(bool partial_update_mode = false) = 0;
    virtual void clearScreen() = 0;
    virtual void updateDisplay(bool partial = false) = 0; // Full or partial update

    // Drawing Primitives
    virtual void drawPixel(int16_t x, int16_t y, uint16_t color) = 0;
    virtual void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) = 0;
    virtual void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) = 0;
    virtual void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) = 0; // Optional -> Now mandatory
    virtual void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) = 0; // Optional -> Now mandatory

    // Bitmap Drawing
    // virtual void drawBitmap(int16_t x, int16_t y, const uint8_t bitmap[], int16_t w, int16_t h, uint16_t color, uint16_t bg) = 0; // GFX style
    virtual void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint16_t color) = 0; // Simpler version, assumes background is transparent or pre-cleared

    // Text Handling
    virtual void setTextColor(uint16_t color) = 0;
    // virtual void setTextColor(uint16_t color, uint16_t backgroundcolor) = 0; // With background
    virtual void setTextSize(uint8_t size) = 0;
    virtual void setTextWrap(bool wrap) = 0;
    virtual void setCursor(int16_t x, int16_t y) = 0;
    // virtual void setFont(const GFXfont *font = nullptr) = 0; // Example for GFX fonts
    virtual void print(const char* text) = 0;
    virtual void print(int val) = 0;
    virtual void println(const char* text) = 0;
    virtual void println(int val) = 0;
    virtual void getTextBounds(const char *str, int16_t x, int16_t y, int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h) = 0;

    // Color definitions (platform-specific HAL implementation will map these to actual GxEPD colors or M5EPD colors)
    // These might be better as static const members or enums in a shared header if they are truly universal.
    // For now, expect implementations to handle their specific color values (e.g. GxEPD_BLACK).
    // static const uint16_t COLOR_BLACK = 0;
    // static const uint16_t COLOR_WHITE = 1; // Or actual color values
};

// Define common color constants that HAL implementations should use/map to.
// These are abstract colors; the HAL implementation will translate them.
// For monochrome, it's simple. For grayscale M5Paper, these would need expansion or different handling.
// Given the goal of monochrome Watchy and keeping runtime identical,
// the runtime should primarily deal with these abstract BLACK/WHITE.
constexpr uint16_t HAL_COLOR_BLACK = 0x0000; // A common representation for black
constexpr uint16_t HAL_COLOR_WHITE = 0xFFFF; // A common representation for white

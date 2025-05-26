#pragma once
#include "IDrawingHAL.h"
#include <GxEPD2_BW.h> // From Szybet/GxEPD2-watchy
#include <Fonts/FreeMonoBold9pt7b.h> // Default GFX font

// Define the GxEPD2 display class specifics for Watchy
// This typically involves:
// GxEPD2_DISPLAY_CLASS<GxEPD2_DRIVER_CLASS, MAX_HEIGHT(GxEPD2_DRIVER_CLASS)>
// For Watchy, it's usually GxEPD2_154_D67 (for the 1.54" display)
// Example: typedef GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> DisplayType;
// However, InkWatchy might have its own way of defining/accessing the display object.
// For now, let's assume we get a GxEPD2_GFX reference.
// We will use the `display` object provided by InkWatchy's base or system setup.

// Forward declare GxEPD2_GFX
class GxEPD2_GFX;

class WatchyDrawingHAL : public IDrawingHAL {
public:
    WatchyDrawingHAL(GxEPD2_GFX& display); // Constructor takes a reference to the GxEPD2 GFX object

    // Basic Display Info
    int16_t getScreenWidth() const override;
    int16_t getScreenHeight() const override;

    // Initialization & Update
    void initDisplay(bool partial_update_mode = false) override;
    void clearScreen() override; // Clears the buffer
    void updateDisplay(bool partial = false) override; // Pushes buffer to screen

    // Drawing Primitives
    void drawPixel(int16_t x, int16_t y, uint16_t color) override;
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override;

    // Bitmap Drawing
    void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint16_t color) override;

    // Text Handling
    void setTextColor(uint16_t color) override;
    void setTextSize(uint8_t size) override;
    void setTextWrap(bool wrap) override;
    void setCursor(int16_t x, int16_t y) override;
    // void setFont(const GFXfont *font = nullptr) override; // Implement if custom fonts are needed beyond default
    void print(const char* text) override;
    void print(int val) override;
    void println(const char* text) override;
    void println(int val) override;
    void getTextBounds(const char *str, int16_t x, int16_t y, int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h) override;

private:
    GxEPD2_GFX& _display;
    uint16_t _currentTextColor; // GxEPD2 text color needs to be set before each print
    // Helper to map HAL_COLOR to GxEPD_Color
    uint16_t mapHalColorToGx(uint16_t hal_color) const;
};

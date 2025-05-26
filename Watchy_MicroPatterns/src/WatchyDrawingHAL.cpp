#include "WatchyDrawingHAL.h"
#include <GxEPD2_GFX.h> // Ensure this is the correct include for GxEPD2 GFX operations

// Watchy Screen dimensions
const int16_t WATCHY_SCREEN_WIDTH = 200;
const int16_t WATCHY_SCREEN_HEIGHT = 200;

WatchyDrawingHAL::WatchyDrawingHAL(GxEPD2_GFX& display) : _display(display), _currentTextColor(GxEPD_BLACK) {}

int16_t WatchyDrawingHAL::getScreenWidth() const {
    return WATCHY_SCREEN_WIDTH;
}

int16_t WatchyDrawingHAL::getScreenHeight() const {
    return WATCHY_SCREEN_HEIGHT;
}

uint16_t WatchyDrawingHAL::mapHalColorToGx(uint16_t hal_color) const {
    if (hal_color == HAL_COLOR_BLACK) {
        return GxEPD_BLACK;
    }
    return GxEPD_WHITE; // Default to white for anything not black
}

void WatchyDrawingHAL::initDisplay(bool partial_update_mode) {
    // Display initialization is assumed to be handled by InkWatchy's setup.
    // This HAL's init might be used for setting HAL-specific defaults if any.
    // For GxEPD2, often involves _display.init(serial_baud_rate, reset_忙, cs_pin, dc_pin, rst_pin, busy_pin);
    // but Szybet/GxEPD2-watchy and InkWatchy's structure might abstract this.
    // We assume the _display object is already initialized.
    _display.setFullWindow(); // Ensure we're operating on the full window
    // Partial update mode for GxEPD2 can be complex; for now, we'll mostly use full updates.
    // If partial is true, InkWatchy's display object might need specific setup.
}

void WatchyDrawingHAL::clearScreen() {
    _display.fillScreen(GxEPD_WHITE); // Clear with white
}

void WatchyDrawingHAL::updateDisplay(bool partial) {
    _display.display(partial); // Use GxEPD2's display method. 'partial' might need careful handling.
}

void WatchyDrawingHAL::drawPixel(int16_t x, int16_t y, uint16_t color) {
    _display.drawPixel(x, y, mapHalColorToGx(color));
}

void WatchyDrawingHAL::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    _display.fillRect(x, y, w, h, mapHalColorToGx(color));
}

void WatchyDrawingHAL::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    _display.drawLine(x0, y0, x1, y1, mapHalColorToGx(color));
}

void WatchyDrawingHAL::drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    _display.drawCircle(x0, y0, r, mapHalColorToGx(color));
}

void WatchyDrawingHAL::fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    _display.fillCircle(x0, y0, r, mapHalColorToGx(color));
}

void WatchyDrawingHAL::drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint16_t color) {
    // GxEPD2 drawBitmap expects color to be GxEPD_BLACK or GxEPD_WHITE.
    // This HAL version assumes the bitmap is monochrome and 'color' is the foreground color.
    // Background is assumed transparent or already white.
    _display.drawBitmap(x, y, bitmap, w, h, mapHalColorToGx(color));
}

void WatchyDrawingHAL::setTextColor(uint16_t color) {
    _currentTextColor = mapHalColorToGx(color);
    // _display.setTextColor(mapHalColorToGx(color)); // GFX usually sets it globally
}

void WatchyDrawingHAL::setTextSize(uint8_t size) {
    _display.setTextSize(size);
}

void WatchyDrawingHAL::setTextWrap(bool wrap) {
    _display.setTextWrap(wrap);
}

void WatchyDrawingHAL::setCursor(int16_t x, int16_t y) {
    _display.setCursor(x, y);
}

// void WatchyDrawingHAL::setFont(const GFXfont *font) {
//    _display.setFont(font);
// }

void WatchyDrawingHAL::print(const char* text) {
    _display.setTextColor(_currentTextColor); // Set color before each print for safety with GxEPD
    _display.print(text);
}

void WatchyDrawingHAL::print(int val) {
    _display.setTextColor(_currentTextColor);
    _display.print(val);
}

void WatchyDrawingHAL::println(const char* text) {
    _display.setTextColor(_currentTextColor);
    _display.println(text);
}

void WatchyDrawingHAL::println(int val) {
    _display.setTextColor(_currentTextColor);
    _display.println(val);
}

void WatchyDrawingHAL::getTextBounds(const char *str, int16_t x, int16_t y, int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h) {
    _display.getTextBounds(str, x, y, x1, y1, w, h);
}

#include "M5PaperDrawingHAL.h"
#include "M5EPD.h" // Ensure M5EPD global object M5 is available

// Define M5EPD color constants if not directly available via M5EPD.h for clarity
const uint8_t M5EPD_COLOR_BLACK = 15;
const uint8_t M5EPD_COLOR_WHITE = 0;

M5PaperDrawingHAL::M5PaperDrawingHAL() 
    : _canvas(&M5.EPD), _halCurrentTextColor(HAL_COLOR_BLACK), _m5TextSize(16) { // Default to 16px font height
    // _canvas is now initialized using M5.EPD. Actual canvas creation (buffer allocation) happens in initDisplay.
}

M5PaperDrawingHAL::~M5PaperDrawingHAL() {
    _canvas.deleteCanvas(); // Free canvas buffer
}

uint8_t M5PaperDrawingHAL::mapHalColorToM5Epd(uint16_t hal_color) const {
    if (hal_color == HAL_COLOR_BLACK) {
        return M5EPD_COLOR_BLACK;
    }
    return M5EPD_COLOR_WHITE; // Default to white
}

int16_t M5PaperDrawingHAL::getScreenWidth() const {
    return _canvas.width(); // Assumes canvas is initialized and rotated
}

int16_t M5PaperDrawingHAL::getScreenHeight() const {
    return _canvas.height(); // Assumes canvas is initialized and rotated
}

void M5PaperDrawingHAL::initDisplay(bool partial_update_mode) {
    // Assumption: M5.begin() and M5.EPD.SetRotation(M5EPD_Driver::ROTATE_90) are called externally
    // before this HAL is used.
    if (!_canvas.isRenderEngineCreated()) { // Check if canvas buffer needs creation
         // For M5Paper, screen is 960x540. ROTATE_90 makes it 540 width, 960 height.
        _canvas.createCanvas(540, 960);
    }
    _canvas.setTextSize(_m5TextSize); // M5EPD text size is in pixels
    _canvas.setTextColor(mapHalColorToM5Epd(_halCurrentTextColor));
    _canvas.setTextDatum(TC_DATUM); // A common default
    // Initial clear is often good.
    // clearScreen(); // Call HAL's clearScreen to fill with white
    // updateDisplay(); // And push it.
    // The caller of initDisplay can decide to clear and update.
}

void M5PaperDrawingHAL::clearScreen() {
    _canvas.fillCanvas(mapHalColorToM5Epd(HAL_COLOR_WHITE));
}

void M5PaperDrawingHAL::updateDisplay(bool partial) {
    // M5Paper uses specific update modes.
    // UPDATE_MODE_DU4 is good for partial, UPDATE_MODE_GC16 for full.
    _canvas.pushCanvas(0, 0, partial ? UPDATE_MODE_DU4 : UPDATE_MODE_GC16);
}

void M5PaperDrawingHAL::drawPixel(int16_t x, int16_t y, uint16_t color) {
    _canvas.drawPixel(x, y, mapHalColorToM5Epd(color));
}

void M5PaperDrawingHAL::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    _canvas.fillRect(x, y, w, h, mapHalColorToM5Epd(color));
}

void M5PaperDrawingHAL::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    _canvas.drawLine(x0, y0, x1, y1, mapHalColorToM5Epd(color));
}

void M5PaperDrawingHAL::drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    _canvas.drawCircle(x0, y0, r, mapHalColorToM5Epd(color));
}

void M5PaperDrawingHAL::fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    _canvas.fillCircle(x0, y0, r, mapHalColorToM5Epd(color));
}

void M5PaperDrawingHAL::drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint16_t color) {
    // M5EPD_Canvas drawBitmap draws 1-bit bitmaps where 'color' is the foreground.
    // Background is transparent. This matches the HAL interface's intent.
    _canvas.drawBitmap(x, y, bitmap, w, h, mapHalColorToM5Epd(color), mapHalColorToM5Epd(HAL_COLOR_WHITE) /* bg, not used if transparent */);
    // Simpler: _canvas.drawBitmap(x, y, bitmap, w, h, mapHalColorToM5Epd(color)); if it handles transparency correctly.
    // The M5EPD canvas.drawBitmap has an overload:
    // void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint16_t color);
    // void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint16_t color, uint16_t bg);
    // We should use the one without bg to match MicroPatternsDrawing expectation (transparent background for asset pixels=0)
    // However, MicroPatternsDrawing draws asset pixels=1 with item.color. If item.color is white, it means draw white pixels.
    // This is tricky. The current MicroPatternsDrawing::drawAsset checks `asset.data[asset_data_index] == 1` then calls `rawPixel(sx_iter, sy_iter, item.color);`
    // The `rawPixel` in MicroPatternsDrawing will eventually call HAL's drawPixel.
    // So, for M5PaperDrawingHAL::drawBitmap, it should probably only draw if `color` is black.
    // This HAL method might not be directly used by MicroPatternsDrawing if it uses its own asset rendering loop that calls hal.drawPixel.
    // Let's provide a standard GFX-like bitmap draw:
    _canvas.drawBitmap(x, y, bitmap, w, h, mapHalColorToM5Epd(color));
}

void M5PaperDrawingHAL::setTextColor(uint16_t color) {
    _halCurrentTextColor = color;
    _canvas.setTextColor(mapHalColorToM5Epd(_halCurrentTextColor));
}

void M5PaperDrawingHAL::setTextSize(uint8_t size) {
    // size for this HAL is intended to be a GFX-like multiplier.
    // M5EPD _canvas.setTextSize expects pixel height.
    // We need a way to map GFX size to pixel height or decide HAL API.
    // For now, assume 'size' is directly pixel height for M5.
    // If MicroPatternsDrawing uses this, it must be aware.
    // Or, M5PaperDrawingHAL can try to map (e.g. size 1 -> 8px, 2->16px etc.)
    // Let's assume `size` passed to HAL is intended as pixel height for M5.
    _m5TextSize = size;
    _canvas.setTextSize(_m5TextSize);
}

void M5PaperDrawingHAL::setTextWrap(bool wrap) {
    _canvas.setTextWrap(wrap, wrap); // M5EPD uses (wordWrap, letterWrap)
}

void M5PaperDrawingHAL::setCursor(int16_t x, int16_t y) {
    _canvas.setCursor(x, y);
}

void M5PaperDrawingHAL::print(const char* text) {
    _canvas.print(text);
}

void M5PaperDrawingHAL::print(int val) {
    _canvas.print(val);
}

void M5PaperDrawingHAL::println(const char* text) {
    _canvas.println(text);
}

void M5PaperDrawingHAL::println(int val) {
    _canvas.println(val);
}

void M5PaperDrawingHAL::getTextBounds(const char *str, int16_t x, int16_t y, int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h) {
    _canvas.getTextBounds(str, x, y, x1, y1, w, h);
}

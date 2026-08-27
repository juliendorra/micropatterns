#ifndef WATCHY_CANVAS_H
#define WATCHY_CANVAS_H

// The Watchy side of the four-method canvas contract in mp_canvas.h.
//
// Bridges the Micropatterns rasterizer to GxEPD2's 1bpp framebuffer. The DSL
// only ever produces two colour values -- DRAWING_COLOR_WHITE (0) and
// DRAWING_COLOR_BLACK (15) -- so the M5Paper's nominal "4bpp" model maps onto a
// genuinely monochrome panel with no loss whatsoever. The 14 intermediate grey
// levels the M5Paper hardware can show are never requested by any script.
//
// No virtual methods: mp_canvas.h binds this by typedef, so drawPixel inlines
// into the rasterizer's innermost loop.

#include <Arduino.h>
#include <GxEPD2_BW.h>

extern GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> g_display;

class WatchyCanvas {
public:
    // Signatures mirror M5EPD_Canvas so the shared rasterizer compiles unchanged.
    int16_t width()  { return g_display.width(); }
    int16_t height() { return g_display.height(); }

    void drawPixel(int32_t x, int32_t y, uint32_t color) {
        // 0 = white, 15 = black (see micropatterns_drawing.h). Anything at or
        // above the midpoint is treated as ink, so a future grey-capable script
        // degrades sensibly rather than vanishing.
        g_display.drawPixel((int16_t)x, (int16_t)y,
                            (color >= 8) ? GxEPD_BLACK : GxEPD_WHITE);
    }

    void fillCanvas(uint32_t color) {
        g_display.fillScreen((color >= 8) ? GxEPD_BLACK : GxEPD_WHITE);
    }
};

#endif // WATCHY_CANVAS_H

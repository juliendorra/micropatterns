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
        probeLayout();
    }

    // --- row blit -----------------------------------------------------------
    //
    // GxEPD2_BW keeps a 1-bpp buffer, MSB-first, bit SET = white, indexed
    //     i = x/8 + y * (window_width/8)
    // -- but only after it has applied rotation, mirror, the partial-window
    // origin and the current page, all of which live in private fields. Rather
    // than assume them, fillCanvas() PROBES them: it draws two pixels through
    // the library's own drawPixel and checks that the exact bytes it expects
    // changed. If they did, rotation is 0, mirror and reverse are off, the
    // window starts at the origin with the stride we think, and there is one
    // page -- and every row write below is safe. If not, _fast stays false
    // and the fallback goes through drawPixel one bit at a time.
    bool _fast = false;
    int  _stride = 0;

    void probeLayout() {
        _fast = false;
        const int W = g_display.width(), H = g_display.height();
        if (g_display.pages() != 1 || g_display.getRotation() != 0 || W < 16 || H < 2) return;
        const int stride = W / 8;
        uint8_t* buf = g_display._buffer;
        const int ia = (W - 1) / 8, ib = stride;                 // (W-1, 0) and (0, 1)
        const uint8_t ma = (uint8_t)(0x80u >> ((W - 1) & 7)), mb = 0x80u;
        const uint8_t sa = buf[ia], sb = buf[ib];
        g_display.drawPixel(W - 1, 0, GxEPD_BLACK);
        g_display.drawPixel(0, 1, GxEPD_BLACK);
        const bool ok = ((buf[ia] & ma) == 0) && ((buf[ib] & mb) == 0) &&
                        ((buf[ia] | ma) == (sa | ma)) && ((buf[ib] | mb) == (sb | mb));
        buf[ia] = sa; buf[ib] = sb;                              // restore, whatever happened
        _fast = ok;
        _stride = stride;
    }

    void fillMaskRow(int y, int byteX0, int nBytes, const uint8_t* cover, uint32_t color) {
        if (_fast) {
            uint8_t* row = g_display._buffer + (size_t)y * _stride + byteX0;
            if (color >= 8) { for (int i = 0; i < nBytes; ++i) row[i] &= (uint8_t)~cover[i]; }   // black: clear
            else            { for (int i = 0; i < nBytes; ++i) row[i] |= cover[i]; }             // white: set
            return;
        }
        for (int i = 0; i < nBytes; ++i) {
            uint8_t m = cover[i];
            if (!m) continue;
            const int xb = (byteX0 + i) << 3;
            for (int k = 0; k < 8; ++k) if (m & (0x80u >> k)) drawPixel(xb + k, y, color);
        }
    }
};

inline void mp_canvas_fill_mask_row(WatchyCanvas* c, int y, int byteX0, int nBytes,
                                    const uint8_t* cover, uint32_t color) {
    c->fillMaskRow(y, byteX0, nBytes, cover, color);
}

#endif // WATCHY_CANVAS_H

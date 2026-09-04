#ifndef MP_CANVAS_H
#define MP_CANVAS_H

// Platform canvas selection for the Micropatterns rasterizer.
//
// micropatterns_drawing.cpp -- the whole rasterizer, every transform, all the
// pattern-fill logic -- touches the canvas through exactly four methods, at six
// call sites:
//
//     int  width();
//     int  height();
//     void drawPixel(x, y, color);
//     void fillCanvas(color);
//
// That is the entire platform surface of the renderer, which is why the Watchy
// port is cheap. The binding is done with a compile-time typedef rather than an
// abstract base class ON PURPOSE: drawPixel is the innermost loop of every fill,
// and a virtual call per pixel would cost real milliseconds per frame on a
// 240MHz ESP32 with no cache to spare. Each platform supplies a concrete class
// with these four methods; there is no vtable and no indirection.
//
// Select with -DMP_PLATFORM_WATCHY (see Watchy_MicroPatterns/platformio.ini).
// The M5Paper is the default so its build is unchanged by this file's arrival.

// A fifth entry point, added 2026-09-04: paint one row of a span from a bit
// mask, so the rasteriser can stop calling drawPixel 40,000 times a frame.
//
//     void mp_canvas_fill_mask_row(MPCanvas*, int y, int byteX0, int nBytes,
//                                  const uint8_t* cover, uint32_t color);
//
// `cover` is MSB-first: bit (7 - x%8) of cover[i] is pixel x = (byteX0+i)*8 +
// x%8. Set bits are painted `color`; clear bits are left alone. It is a FREE
// FUNCTION overloaded per canvas type rather than a method, because the
// M5Paper binds MPCanvas straight to the library's M5EPD_Canvas and cannot
// add methods to it. A platform that has no fast way to do this must still
// provide it, as a loop over set bits calling drawPixel -- correct by
// construction, and still a win, because the occupancy test above it has
// already gone byte-wise.

#if defined(MP_PLATFORM_WATCHY)

#include "watchy_canvas.h"
typedef WatchyCanvas MPCanvas;

#else // MP_PLATFORM_M5PAPER (default)

#include <M5EPD.h>
typedef M5EPD_Canvas MPCanvas;

#if !defined(MP_HOST_SHIM_HAS_FILL_MASK_ROW)
// The real M5EPD_Canvas: 4 bits per pixel, so a mask byte does not map onto a
// framebuffer byte. First version is the per-bit fallback; a nibble-wise blit
// through frameBuffer() is its own measurement, on its own device.
inline void mp_canvas_fill_mask_row(M5EPD_Canvas* c, int y, int byteX0, int nBytes,
                                    const uint8_t* cover, uint32_t color) {
    for (int i = 0; i < nBytes; ++i) {
        uint8_t m = cover[i];
        if (!m) continue;
        const int xb = (byteX0 + i) << 3;
        for (int k = 0; k < 8; ++k) {
            if (m & (0x80u >> k)) c->drawPixel(xb + k, y, color);
        }
    }
}
#endif

#endif

#endif // MP_CANVAS_H

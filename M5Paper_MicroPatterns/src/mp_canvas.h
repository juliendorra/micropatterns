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

#if defined(MP_PLATFORM_WATCHY)

#include "watchy_canvas.h"
typedef WatchyCanvas MPCanvas;

#else // MP_PLATFORM_M5PAPER (default)

#include <M5EPD.h>
typedef M5EPD_Canvas MPCanvas;

#endif

#endif // MP_CANVAS_H

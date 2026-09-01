// WASM entry point for the FIRMWARE renderer.
//
// This is the same six core .cpp files the device runs, compiled verbatim --
// not a reimplementation. The host harness already proved they are portable:
// they need no Arduino, no FreeRTOS and no hardware, only the 326-line shim in
// ../shim. Emscripten consumes exactly that same file list, so the browser can
// show what the device will ACTUALLY draw rather than a second implementation's
// idea of it.
//
// See docs/analysis/web-device-renderer-audit.md for what the second
// implementation currently gets differently.

#include <emscripten/emscripten.h>

#include <string>

#include "render_path.h"

namespace {
// One result kept alive between calls so JS can read the pixels out of the
// heap without a copy on this side.
RenderResult g_result;
std::string g_error;
}

extern "C" {

// Renders `src` and returns 1 on success, 0 on failure (see mp_error()).
EMSCRIPTEN_KEEPALIVE
int mp_render(const char* src, int width, int height,
              int counter, int hour, int minute, int second)
{
    g_result = RenderResult();
    g_error.clear();

    auto path = makeRenderPath("displaylist");
    if (!path) { g_error = "no such render path"; return 0; }

    RenderSeed seed;
    seed.counter = counter;
    seed.hour = hour;
    seed.minute = minute;
    seed.second = second;

    path->run(std::string(src ? src : ""), width, height, seed, g_result);
    if (!g_result.ok) g_error = g_result.error;
    return g_result.ok ? 1 : 0;
}

// 8-bit greyscale, width*height bytes, 255 = white. Same convention as the
// harness's PGM goldens, so the two are directly comparable.
EMSCRIPTEN_KEEPALIVE const unsigned char* mp_pixels() { return g_result.image.pixels.data(); }
EMSCRIPTEN_KEEPALIVE int mp_width()  { return g_result.image.width; }
EMSCRIPTEN_KEEPALIVE int mp_height() { return g_result.image.height; }
EMSCRIPTEN_KEEPALIVE const char* mp_error() { return g_error.c_str(); }

// Counters, so the browser can show the same numbers the device logs.
EMSCRIPTEN_KEEPALIVE int mp_display_list_items() { return g_result.counters.displayListItems; }
EMSCRIPTEN_KEEPALIVE int mp_rendered_items()     { return g_result.counters.renderedItems; }
EMSCRIPTEN_KEEPALIVE int mp_culled_offscreen()   { return g_result.counters.culledOffScreen; }
EMSCRIPTEN_KEEPALIVE int mp_culled_occlusion()   { return g_result.counters.culledByOcclusion; }
EMSCRIPTEN_KEEPALIVE double mp_ms_parse()        { return g_result.timings.parseMs; }
EMSCRIPTEN_KEEPALIVE double mp_ms_displaylist()  { return g_result.timings.displayListMs; }
EMSCRIPTEN_KEEPALIVE double mp_ms_rasterize()    { return g_result.timings.rasterizeMs; }

} // extern "C"

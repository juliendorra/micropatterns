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

#include <cstdio>
#include <new>
#include <string>

#include "micropatterns_parser.h"
#include "render_path.h"

#if MP_DEVICE_CONSTRAINTS
#include "device_allocator.h"
#endif

namespace {
// One result kept alive between calls so JS can read the pixels out of the
// heap without a copy on this side.
RenderResult g_result;
std::string g_error;

// Kept alive between calls so JS can read the strings out of the heap.
MicroPatternsParser g_parser;
std::vector<std::string> g_parseErrors;

// Snapshot of the assets the last mp_parse() found, in a stable order, so the
// editor's pattern previews and pattern editor can be fed from the firmware's
// parser rather than from a second one. Copied out of the parser's map because
// the map is keyed by uppercase name and the editor wants an index.
struct AssetView {
    std::string name;          // uppercase, the parser's key
    std::string originalName;  // as written in the script
    int width = 0, height = 0;
    std::vector<unsigned char> data;   // width*height of 0/1
};
std::vector<AssetView> g_assets;
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

#if MP_DEVICE_CONSTRAINTS
    try {
        path->run(std::string(src ? src : ""), width, height, seed, g_result);
    } catch (const std::bad_alloc&) {
        mpDeviceSetAllocationActive(false);
        MpAllocationTelemetry t = mpDeviceTelemetry();
        char message[256];
        if (t.failure.valid) {
            snprintf(message, sizeof(message),
                     "device OOM: phase=%u source=%u capability=%u request=%zu "
                     "internal_free=%zu largest=%zu psram_free=%zu psram_largest=%zu",
                     (unsigned)t.failure.phase, (unsigned)t.failure.source,
                     (unsigned)t.failure.capability, t.failure.requested,
                     t.failure.internal.totalFree, t.failure.internal.largestFree,
                     t.failure.psram.totalFree, t.failure.psram.largestFree);
        } else {
            snprintf(message, sizeof(message), "host WebAssembly allocation failed");
        }
        g_error = message;
        resetDeviceRenderSessionAfterFailure();
        return 0;
    }
#else
    path->run(std::string(src ? src : ""), width, height, seed, g_result);
#endif
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

#if MP_DEVICE_CONSTRAINTS
EMSCRIPTEN_KEEPALIVE void mp_set_device_state(int state)
{
    if (state < 0 || state > 2) state = 0;
    mpDeviceSetRequestedState(static_cast<MpDeviceState>(state));
}
EMSCRIPTEN_KEEPALIVE const char* mp_device_profile() { return mpDeviceProfileName(); }
EMSCRIPTEN_KEEPALIVE const char* mp_device_idf() { return mpDeviceIdfVersion(); }
EMSCRIPTEN_KEEPALIVE int mp_device_profile_calibrated()
    { return mpDeviceProfileCalibrated() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_allocation_calls()
    { return mpDeviceTelemetry().allocationCalls; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_realloc_calls()
    { return mpDeviceTelemetry().reallocCalls; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_free_calls()
    { return mpDeviceTelemetry().freeCalls; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_initial_internal_free()
    { return (unsigned int)mpDeviceTelemetry().initialInternal.totalFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_initial_internal_largest()
    { return (unsigned int)mpDeviceTelemetry().initialInternal.largestFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_current_internal_free()
    { return (unsigned int)mpDeviceTelemetry().currentInternal.totalFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_current_internal_largest()
    { return (unsigned int)mpDeviceTelemetry().currentInternal.largestFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_peak_internal_used()
    { return (unsigned int)mpDeviceTelemetry().peakInternalUsed; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_initial_psram_free()
    { return (unsigned int)mpDeviceTelemetry().initialPsram.totalFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_current_psram_free()
    { return (unsigned int)mpDeviceTelemetry().currentPsram.totalFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_peak_psram_used()
    { return (unsigned int)mpDeviceTelemetry().peakPsramUsed; }
EMSCRIPTEN_KEEPALIVE int mp_mem_failure_valid()
    { return mpDeviceTelemetry().failure.valid ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_failure_request()
    { return (unsigned int)mpDeviceTelemetry().failure.requested; }
EMSCRIPTEN_KEEPALIVE int mp_mem_failure_phase()
    { return (int)mpDeviceTelemetry().failure.phase; }
EMSCRIPTEN_KEEPALIVE int mp_mem_failure_source()
    { return (int)mpDeviceTelemetry().failure.source; }
EMSCRIPTEN_KEEPALIVE int mp_mem_failure_capability()
    { return (int)mpDeviceTelemetry().failure.capability; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_failure_internal_free()
    { return (unsigned int)mpDeviceTelemetry().failure.internal.totalFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_failure_internal_largest()
    { return (unsigned int)mpDeviceTelemetry().failure.internal.largestFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_failure_psram_free()
    { return (unsigned int)mpDeviceTelemetry().failure.psram.totalFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_failure_psram_largest()
    { return (unsigned int)mpDeviceTelemetry().failure.psram.largestFree; }
#endif

// --- parse-only, for editor diagnostics ------------------------------------
//
// The editor's linting currently comes from the JS parser, which is a SECOND
// implementation of the language and has already been caught disagreeing with
// this one (it rejected LINE's X1/Y1/X2/Y2 outright). Running the firmware's
// own parser gives the editor the same verdict the device will reach.
//
// Errors come back as the parser writes them, which is "Line N: ...".
EMSCRIPTEN_KEEPALIVE
int mp_parse(const char* src)
{
    g_parseErrors.clear();
    g_parser.reset();
    const bool ok = g_parser.parse(String(src ? src : ""));
    for (const String& e : g_parser.getErrors()) {
        g_parseErrors.push_back(std::string(e.c_str()));
    }
    g_assets.clear();
    for (const auto& kv : g_parser.getAssets()) {
        const MicroPatternsAsset& a = kv.second;
        AssetView v;
        v.name = a.name.c_str();
        v.originalName = a.originalName.c_str();
        v.width = a.width;
        v.height = a.height;
        v.data = a.data;
        g_assets.push_back(std::move(v));
    }
    return ok ? 1 : 0;
}

// --- assets from the last mp_parse() ----------------------------------------
EMSCRIPTEN_KEEPALIVE int mp_asset_count() { return (int)g_assets.size(); }
EMSCRIPTEN_KEEPALIVE const char* mp_asset_name(int i)
{ return (i >= 0 && i < (int)g_assets.size()) ? g_assets[(size_t)i].name.c_str() : ""; }
EMSCRIPTEN_KEEPALIVE const char* mp_asset_original_name(int i)
{ return (i >= 0 && i < (int)g_assets.size()) ? g_assets[(size_t)i].originalName.c_str() : ""; }
EMSCRIPTEN_KEEPALIVE int mp_asset_width(int i)
{ return (i >= 0 && i < (int)g_assets.size()) ? g_assets[(size_t)i].width : 0; }
EMSCRIPTEN_KEEPALIVE int mp_asset_height(int i)
{ return (i >= 0 && i < (int)g_assets.size()) ? g_assets[(size_t)i].height : 0; }
EMSCRIPTEN_KEEPALIVE const unsigned char* mp_asset_data(int i)
{ return (i >= 0 && i < (int)g_assets.size()) ? g_assets[(size_t)i].data.data() : nullptr; }

EMSCRIPTEN_KEEPALIVE int mp_parse_error_count() { return (int)g_parseErrors.size(); }
EMSCRIPTEN_KEEPALIVE const char* mp_parse_error_at(int i)
{
    if (i < 0 || i >= (int)g_parseErrors.size()) return "";
    return g_parseErrors[(size_t)i].c_str();
}

} // extern "C"

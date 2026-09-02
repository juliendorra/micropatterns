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

// The "file on flash": the program mp_compile() produced, and the source it
// was compiled from. mp_render() loads from it when the source matches --
// which is what a device does after a sync -- and compiles lazily otherwise,
// which is what a device does when no stored program exists yet.
std::vector<uint8_t> g_stored;
std::string g_storedSource;
RenderResult g_compileResult;
std::string g_compileError;

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

// Stage 1 of the device's process: compile `src` the way a sync does -- with
// the radios off -- and keep the result as the "file on flash". Returns 1 on
// success. Memory telemetry (mp_mem_*) describes this compile until the next
// mp_render() call.
EMSCRIPTEN_KEEPALIVE
int mp_compile(const char* src)
{
    g_compileResult = RenderResult();
    g_compileError.clear();
    g_stored.clear();
    g_storedSource.clear();

    auto path = makeRenderPath("compiled");
    if (!path) { g_compileError = "no such render path"; return 0; }
    const std::string text(src ? src : "");
#if MP_DEVICE_CONSTRAINTS
    // Sync stops BLE and tears WiFi down before compiling (script_sync.cpp),
    // so the compile always sees the radios-off heap whatever the render
    // state the editor is exploring.
    const MpDeviceState renderState = mpDeviceRequestedState();
    mpDeviceSetRequestedState(MpDeviceState::RadiosOff);
    bool ok = false;
    try {
        ok = path->compile(text, g_stored, g_compileResult);
    } catch (const std::bad_alloc&) {
        mpDeviceSetAllocationActive(false);
        MpAllocationTelemetry t = mpDeviceTelemetry();
        char message[320];
        if (t.failure.valid) {
            snprintf(message, sizeof(message),
                     "device OOM at sync (compile): line=%d phase=%u source=%u capability=%u request=%zu "
                     "internal_free=%zu largest=%zu psram_free=%zu psram_largest=%zu",
                     t.failure.sourceLine, (unsigned)t.failure.phase, (unsigned)t.failure.source,
                     (unsigned)t.failure.capability, t.failure.requested,
                     t.failure.internal.totalFree, t.failure.internal.largestFree,
                     t.failure.psram.totalFree, t.failure.psram.largestFree);
        } else {
            snprintf(message, sizeof(message), "host WebAssembly allocation failed");
        }
        g_compileError = message;
        resetDeviceRenderSessionAfterFailure();
        mpDeviceSetRequestedState(renderState);
        return 0;
    }
    mpDeviceSetRequestedState(renderState);
#else
    const bool ok = path->compile(text, g_stored, g_compileResult);
#endif
    if (!ok) { g_compileError = g_compileResult.error; g_stored.clear(); return 0; }
    g_storedSource = text;
    return 1;
}

EMSCRIPTEN_KEEPALIVE const char* mp_compile_error() { return g_compileError.c_str(); }
EMSCRIPTEN_KEEPALIVE double mp_compile_ms() { return g_compileResult.timings.parseMs; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_compile_program_bytes()
    { return (unsigned int)g_compileResult.counters.programBytes; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_compile_file_bytes()
    { return (unsigned int)g_stored.size(); }
// 1 when mp_render(src) will load the stored program rather than compile.
EMSCRIPTEN_KEEPALIVE int mp_has_stored(const char* src)
    { return (!g_stored.empty() && g_storedSource == (src ? src : "")) ? 1 : 0; }

// Stage 2: render. Loads the stored program when it was compiled from exactly
// this source (the device after a sync); otherwise compiles and loads in the
// current device state (the device's lazy path). Returns 1 on success, 0 on
// failure (see mp_error()).
EMSCRIPTEN_KEEPALIVE
int mp_render(const char* src, int width, int height,
              int counter, int hour, int minute, int second)
{
    g_result = RenderResult();
    g_error.clear();

    // "compiled" = parse, serialize, deserialize, run: the sequence a device
    // follows when it renders the program it stored at sync time. Byte-identical
    // to the direct path (mpharness compare-paths), and the one the browser
    // should exercise so the emulator cannot drift from the on-device process.
    auto path = makeRenderPath("compiled");
    if (!path) { g_error = "no such render path"; return 0; }

    RenderSeed seed;
    seed.counter = counter;
    seed.hour = hour;
    seed.minute = minute;
    seed.second = second;

    const std::string text(src ? src : "");
    const bool stored = !g_stored.empty() && g_storedSource == text;

#if MP_DEVICE_CONSTRAINTS
    try {
        if (stored) path->runStored(g_stored, width, height, seed, g_result);
        else        path->run(text, width, height, seed, g_result);
    } catch (const std::bad_alloc&) {
        mpDeviceSetAllocationActive(false);
        MpAllocationTelemetry t = mpDeviceTelemetry();
        char message[320];
        if (t.failure.valid) {
            snprintf(message, sizeof(message),
                     "device OOM: line=%d phase=%u source=%u capability=%u request=%zu "
                     "internal_free=%zu largest=%zu psram_free=%zu psram_largest=%zu",
                     t.failure.sourceLine, (unsigned)t.failure.phase, (unsigned)t.failure.source,
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
    if (stored) path->runStored(g_stored, width, height, seed, g_result);
    else        path->run(text, width, height, seed, g_result);
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
EMSCRIPTEN_KEEPALIVE unsigned int mp_display_list_bytes()
    { return (unsigned int)g_result.counters.displayListBytes; }
EMSCRIPTEN_KEEPALIVE int mp_rendered_items()     { return g_result.counters.renderedItems; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_program_bytes()
    { return (unsigned int)g_result.counters.programBytes; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_program_file_bytes()
    { return (unsigned int)g_result.counters.programFileBytes; }
EMSCRIPTEN_KEEPALIVE int mp_culled_offscreen()   { return g_result.counters.culledOffScreen; }
EMSCRIPTEN_KEEPALIVE int mp_culled_occlusion()   { return g_result.counters.culledByOcclusion; }
EMSCRIPTEN_KEEPALIVE double mp_ms_parse()        { return g_result.timings.parseMs; }
EMSCRIPTEN_KEEPALIVE double mp_ms_load()         { return g_result.timings.loadMs; }
EMSCRIPTEN_KEEPALIVE double mp_ms_displaylist()  { return g_result.timings.displayListMs; }
EMSCRIPTEN_KEEPALIVE double mp_ms_rasterize()    { return g_result.timings.rasterizeMs; }
EMSCRIPTEN_KEEPALIVE int mp_occupancy_map_used()
    { return g_result.counters.occupancyMapUsed ? 1 : 0; }

#if MP_DEVICE_CONSTRAINTS
EMSCRIPTEN_KEEPALIVE void mp_set_device_state(int state)
{
    if (state < 0 || state > 2) state = 0;
    mpDeviceSetRequestedState(static_cast<MpDeviceState>(state));
}
EMSCRIPTEN_KEEPALIVE const char* mp_device_profile() { return mpDeviceProfileName(); }
EMSCRIPTEN_KEEPALIVE const char* mp_device_arduino() { return mpDeviceArduinoVersion(); }
EMSCRIPTEN_KEEPALIVE const char* mp_device_idf() { return mpDeviceIdfVersion(); }
EMSCRIPTEN_KEEPALIVE int mp_device_profile_calibrated()
    { return mpDeviceProfileCalibrated() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int mp_device_state_calibrated()
    { return mpDeviceStateCalibrated() ? 1 : 0; }
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
EMSCRIPTEN_KEEPALIVE int mp_mem_peak_internal_line()
    { return mpDeviceTelemetry().peakInternalSourceLine; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_initial_psram_free()
    { return (unsigned int)mpDeviceTelemetry().initialPsram.totalFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_current_psram_free()
    { return (unsigned int)mpDeviceTelemetry().currentPsram.totalFree; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_peak_psram_used()
    { return (unsigned int)mpDeviceTelemetry().peakPsramUsed; }
EMSCRIPTEN_KEEPALIVE int mp_mem_peak_psram_line()
    { return mpDeviceTelemetry().peakPsramSourceLine; }
EMSCRIPTEN_KEEPALIVE int mp_mem_failure_valid()
    { return mpDeviceTelemetry().failure.valid ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE unsigned int mp_mem_failure_request()
    { return (unsigned int)mpDeviceTelemetry().failure.requested; }
EMSCRIPTEN_KEEPALIVE int mp_mem_failure_line()
    { return mpDeviceTelemetry().failure.sourceLine; }
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
    for (const MicroPatternsAsset& a : g_parser.getAssets()) {
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

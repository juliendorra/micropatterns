// mp_bench.cpp -- on-device benchmark harness. Compiled ONLY when MP_BENCH=1
// (env:m5paper-bench). See mp_bench.h and
// docs/analysis/device-benchmark-harness.md.
//
// What it does, per (script, seed, repetition):
//   parse -> display list -> rasterize -> canvas checksum -> pushCanvas
// timing each phase with esp_timer_get_time() (microseconds), then prints one
// `MPBENCH|RUN|{json}` line per repetition and one `MPBENCH|STAT|{json}` line
// per (script, seed) with min/median/mean per phase.
//
// It deliberately mirrors RenderController::renderScript() phase-for-phase
// rather than calling it, for three reasons:
//   1. Zero edits to the production render path (no #if soup in
//      render_controller.cpp).
//   2. RenderController swallows the phase boundaries into log_i() strings; the
//      bench needs the raw numbers.
//   3. It is the same shape as tools/host_harness/src/render_path.cpp, so tier
//      1 and tier 2 are measuring the same brackets by construction.
// The cost of that choice is real and must be stated: if RenderController ever
// gains a phase, this file must be updated to match. That drift risk is called
// out in the docs.
#if MP_BENCH

#include "mp_bench.h"
#include "mp_bench_corpus.h"

#include "../display_manager.h"
#include "../display_list_renderer.h"
#include "../micropatterns_parser.h"
#include "../micropatterns_runtime.h"

#include <algorithm>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <vector>

// --- Build-time knobs -------------------------------------------------------
#ifndef MP_BENCH_REPS
#define MP_BENCH_REPS 5 // repetitions per (script, seed)
#endif
#ifndef MP_BENCH_PUSH
#define MP_BENCH_PUSH 1 // 1 = actually push to the panel and time the waveform
#endif
#ifndef MP_BENCH_PUSH_MODE
#define MP_BENCH_PUSH_MODE UPDATE_MODE_GC16 // what RenderTask uses in production
#endif
#ifndef MP_BENCH_PUSH_1BPP
// 1 = use the vendored M5EPD 1bpp fast path (packed 8-px-per-byte transfer +
// IT8951 1bpp display mode) instead of the 4bpp path. Legal because the DSL
// only emits grey 0 and grey 15. Set to 0 to time the 4bpp path for an A/B.
// NOTE: with 1bpp, push_xfer_us INCLUDES the host-side transpose+pack, which
// the 4bpp path does not have to do.
#define MP_BENCH_PUSH_1BPP 1
#endif
#ifndef MP_BENCH_AUTOSTART_MS
#define MP_BENCH_AUTOSTART_MS 6000 // wait this long for a 'g' before self-starting
#endif
#define MP_BENCH_TASK_STACK (16384) // production RenderTask uses 8192; bench holds more locals
#define MP_BENCH_FORMAT_VERSION 1

// --- Fixed seeds ------------------------------------------------------------
// MUST stay identical to kSeeds in tools/host_harness/src/main.cpp, otherwise
// the two tiers are not comparable and the checksums cannot be cross-checked.
struct MPBenchSeed {
    const char* tag;
    int counter, hour, minute, second;
};
static const MPBenchSeed kSeeds[] = {
    {"c0_123456", 0, 12, 34, 56},
    {"c7_000000", 7, 0, 0, 0},
    {"c42_235959", 42, 23, 59, 59},
};
static const int kNumSeeds = (int)(sizeof(kSeeds) / sizeof(kSeeds[0]));

// ---------------------------------------------------------------------------
// CANONICAL CANVAS CHECKSUM
//
// Byte layout (this is the spec the host harness must reproduce byte for byte):
//   for y in 0 .. height-1:            // top to bottom
//     for x in 0 .. width-1 step 2:    // left to right, two pixels per byte
//       byte = (pixel(x, y) & 0x0F) << 4 | (pixel(x+1, y) & 0x0F)
// Pixel values are the raw 4bpp canvas values 0..15 as returned by
// M5EPD_Canvas::readPixel() -- 0 = white, 15 = black on this device. No
// inversion, no scaling to 8 bits, no row padding beyond the pairing above
// (width is even on this panel: 540). If width were odd the final pixel of a
// row would occupy the high nibble and the low nibble would be 0.
//
// Hash: FNV-1a 32-bit over that byte stream, in that exact order.
//   h = 2166136261; for each byte b: h ^= b; h *= 16777619;   (mod 2^32)
// Reported as 8 lowercase hex digits.
//
// Chosen over CRC32 only because it needs no table and no library; the property
// that matters here is "any single changed pixel changes the value", which both
// give. It is an equivalence gate, not a security hash.
//
// Note: on M5EPD this stream is bit-identical to the canvas's internal _img8
// buffer (row-major, _bytewidth = width>>1, even x in the high nibble), so the
// readPixel() loop is a definition, not a transformation. It is written via
// readPixel() so the host harness -- which reaches the same buffer through its
// own M5EPD shim -- can implement the identical loop.
// ---------------------------------------------------------------------------
uint32_t MPBench_CanvasFnv1a(M5EPD_Canvas* canvas, int width, int height,
                             uint32_t* outNonWhitePixels) {
    uint32_t h = 2166136261u;
    uint32_t nonWhite = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; x += 2) {
            uint8_t hi = (uint8_t)(canvas->readPixel(x, y) & 0x0F);
            uint8_t lo = (x + 1 < width) ? (uint8_t)(canvas->readPixel(x + 1, y) & 0x0F) : 0;
            if (hi) nonWhite++;
            if (lo && (x + 1 < width)) nonWhite++;
            uint8_t b = (uint8_t)((hi << 4) | lo);
            h ^= b;
            h *= 16777619u;
        }
    }
    if (outNonWhitePixels) *outNonWhitePixels = nonWhite;
    return h;
}

// ---------------------------------------------------------------------------
namespace {

struct RepResult {
    bool ok = false;
    int64_t parse_us = 0;
    int64_t displaylist_us = 0;
    int64_t rasterize_us = 0;
    int64_t push_xfer_us = 0; // SPI transfer + issuing the update
    int64_t push_wait_us = 0; // waiting for the e-ink waveform to finish
    int64_t checksum_us = 0;  // measured, and EXCLUDED from total_us
    int64_t total_us = 0;     // whole render, minus the checksum
    uint32_t fnv1a = 0;
    uint32_t non_white_px = 0;
    int display_list_items = 0;
    int rendered_items = 0;
    int culled_offscreen = 0;
    int culled_occlusion = 0;
    unsigned int overdraw_skipped_px = 0;
    uint32_t heap_free = 0;
    uint32_t psram_free = 0;
};

struct Stat {
    int64_t min = 0, median = 0;
    double mean = 0.0;
};

Stat statOf(std::vector<int64_t> v) {
    Stat s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    size_t n = v.size();
    s.median = (n % 2) ? v[n / 2] : (int64_t)((v[n / 2 - 1] + v[n / 2]) / 2);
    double sum = 0;
    for (int64_t x : v) sum += (double)x;
    s.mean = sum / (double)n;
    return s;
}

void emitStat(const char* key, const Stat& s, bool trailingComma) {
    Serial.printf("\"%s\":{\"min_us\":%lld,\"median_us\":%lld,\"mean_us\":%.1f}%s",
                  key, (long long)s.min, (long long)s.median, s.mean,
                  trailingComma ? "," : "");
}

DisplayManager* g_benchDisplay = nullptr;

// One repetition. Mirrors RenderController::renderScript phase for phase.
bool runOnce(const MPBenchScript& script, const MPBenchSeed& seed, RepResult& r) {
    DisplayManager& dm = *g_benchDisplay;
    const int W = dm.getWidth();
    const int H = dm.getHeight();

    MPBenchStopwatch total;
    MPBenchStopwatch sw;

    // --- Phase 1: parse -----------------------------------------------------
    MicroPatternsParser parser;
    sw.restart();
    bool parsed = parser.parse(String(script.text));
    r.parse_us = sw.us();
    if (!parsed) {
        Serial.printf(MPBENCH_MARKER "ERROR|{\"script\":\"%s\",\"seed\":\"%s\",\"stage\":\"parse\"}\n",
                      script.name, seed.tag);
        for (const String& e : parser.getErrors()) {
            Serial.printf(MPBENCH_MARKER "ERRTEXT|%s\n", e.c_str());
        }
        return false;
    }

    // --- Phase 2: display list generation -----------------------------------
    MicroPatternsRuntime runtime(W, H, parser.getAssets());
    runtime.setCommands(&parser.getCommands());
    runtime.setDeclaredVariables(&parser.getDeclaredVariables());
    runtime.setCounter(seed.counter);
    runtime.setTime(seed.hour, seed.minute, seed.second);

    sw.restart();
    runtime.generateDisplayList();
    r.displaylist_us = sw.us();

    // --- Phase 3: rasterization ---------------------------------------------
    // DisplayListRenderer takes the canvas directly (it used to take a
    // DisplayManager& only to call getCanvas() once). Matches
    // RenderController::RenderController().
    DisplayListRenderer renderer(dm.getCanvas(), parser.getAssets(), W, H);
    sw.restart();
    renderer.render(runtime.getDisplayList()); // clears the canvas, then draws
    r.rasterize_us = sw.us();

    r.display_list_items = (int)runtime.getDisplayList().size();
    r.rendered_items = renderer.getRenderedItems();
    r.culled_offscreen = renderer.getCulledOffScreen();
    r.culled_occlusion = renderer.getCulledByOcclusion();
    r.overdraw_skipped_px = renderer.getOverdrawSkippedPixels();

    // --- Checksum (BEFORE the push; excluded from total_us) ------------------
    M5EPD_Canvas* canvas = dm.getCanvas();
    sw.restart();
    r.fnv1a = MPBench_CanvasFnv1a(canvas, W, H, &r.non_white_px);
    r.checksum_us = sw.us();

    // --- Phase 4: canvas push / e-paper waveform -----------------------------
#if MP_BENCH_PUSH
    sw.restart();
    bool pushed_1bpp = false;
#if MP_BENCH_PUSH_1BPP
    pushed_1bpp = canvas->pushCanvas1bpp(0, 0, MP_BENCH_PUSH_MODE);
#endif
    if (!pushed_1bpp) {
        canvas->pushCanvas(0, 0, MP_BENCH_PUSH_MODE);
    }
    r.push_xfer_us = sw.us();
    // pushCanvas() returns as soon as the update is ISSUED: M5EPD_Driver::
    // UpdateArea() waits for the PREVIOUS waveform (CheckAFSR) and then fires
    // and forgets. So the waveform time of THIS update is only observable by
    // waiting for it explicitly. Without this the panel time would silently
    // land on the next repetition's push.
    sw.restart();
    M5.EPD.CheckAFSR();
    r.push_wait_us = sw.us();
#endif

    r.total_us = total.us() - r.checksum_us;
    r.heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    r.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    r.ok = true;
    return true;
}

void emitRun(const MPBenchScript& s, const MPBenchSeed& seed, int rep, const RepResult& r) {
    Serial.printf(MPBENCH_MARKER
                  "RUN|{\"script\":\"%s\",\"seed\":\"%s\",\"rep\":%d,"
                  "\"parse_us\":%lld,\"displaylist_us\":%lld,\"rasterize_us\":%lld,"
                  "\"push_xfer_us\":%lld,\"push_wait_us\":%lld,\"push_us\":%lld,"
                  "\"checksum_us\":%lld,\"total_us\":%lld,"
                  "\"canvas_fnv1a\":\"%08x\",\"non_white_px\":%u,"
                  "\"display_list_items\":%d,\"rendered_items\":%d,"
                  "\"culled_offscreen\":%d,\"culled_occlusion\":%d,"
                  "\"overdraw_skipped_px\":%u,"
                  "\"heap_free\":%u,\"psram_free\":%u}\n",
                  s.name, seed.tag, rep,
                  (long long)r.parse_us, (long long)r.displaylist_us, (long long)r.rasterize_us,
                  (long long)r.push_xfer_us, (long long)r.push_wait_us,
                  (long long)(r.push_xfer_us + r.push_wait_us),
                  (long long)r.checksum_us, (long long)r.total_us,
                  r.fnv1a, r.non_white_px,
                  r.display_list_items, r.rendered_items,
                  r.culled_offscreen, r.culled_occlusion,
                  r.overdraw_skipped_px,
                  r.heap_free, r.psram_free);
}

void emitCaseStats(const MPBenchScript& s, const MPBenchSeed& seed,
                   const std::vector<RepResult>& reps) {
    std::vector<int64_t> parse, dl, ras, push, total;
    bool checksumStable = true;
    uint32_t fnv = reps.empty() ? 0 : reps[0].fnv1a;
    for (const RepResult& r : reps) {
        parse.push_back(r.parse_us);
        dl.push_back(r.displaylist_us);
        ras.push_back(r.rasterize_us);
        push.push_back(r.push_xfer_us + r.push_wait_us);
        total.push_back(r.total_us);
        if (r.fnv1a != fnv) checksumStable = false;
    }
    const RepResult& c = reps.front();

    Serial.printf(MPBENCH_MARKER "STAT|{\"script\":\"%s\",\"sha256_16\":\"%s\",\"seed\":\"%s\","
                                 "\"reps\":%d,",
                  s.name, s.sha256_16, seed.tag, (int)reps.size());
    emitStat("parse", statOf(parse), true);
    emitStat("displaylist", statOf(dl), true);
    emitStat("rasterize", statOf(ras), true);
    emitStat("push", statOf(push), true);
    emitStat("total", statOf(total), true);
    Serial.printf("\"canvas_fnv1a\":\"%08x\",\"checksum_stable\":%s,"
                  "\"counters\":{\"display_list_items\":%d,\"rendered_items\":%d,"
                  "\"culled_offscreen\":%d,\"culled_occlusion\":%d,"
                  "\"overdraw_skipped_px\":%u,\"non_white_px\":%u}}\n",
                  fnv, checksumStable ? "true" : "false",
                  c.display_list_items, c.rendered_items, c.culled_offscreen,
                  c.culled_occlusion, c.overdraw_skipped_px, c.non_white_px);
}

void emitMeta() {
    Serial.printf(MPBENCH_MARKER "META|{\"format\":%d,\"harness\":\"mp_device_bench\","
                                 "\"device\":\"m5paper\",\"built\":\"%s %s\","
                                 "\"canvas_w\":%d,\"canvas_h\":%d,\"reps\":%d,"
                                 "\"push_enabled\":%d,\"cpu_mhz\":%u,"
                                 "\"scripts\":%d,\"seeds\":%d,"
                                 "\"checksum\":\"fnv1a32/4bpp-rowmajor-hi-nibble-even-x\"}\n",
                  MP_BENCH_FORMAT_VERSION, __DATE__, __TIME__,
                  g_benchDisplay->getWidth(), g_benchDisplay->getHeight(),
                  (int)MP_BENCH_REPS, (int)MP_BENCH_PUSH,
                  (unsigned)getCpuFrequencyMhz(), kMPBenchCorpusCount, kNumSeeds);
}

void runSuite() {
    MPBenchStopwatch suite;
    emitMeta();
    for (int si = 0; si < kMPBenchCorpusCount; ++si) {
        const MPBenchScript& s = kMPBenchCorpus[si];
        for (int qi = 0; qi < kNumSeeds; ++qi) {
            const MPBenchSeed& seed = kSeeds[qi];
            std::vector<RepResult> reps;
            for (int rep = 0; rep < MP_BENCH_REPS; ++rep) {
                esp_task_wdt_reset();
                RepResult r;
                if (!runOnce(s, seed, r)) break;
                emitRun(s, seed, rep, r);
                reps.push_back(r);
                // Let the idle task and the panel breathe between repetitions.
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (!reps.empty()) emitCaseStats(s, seed, reps);
        }
    }
    Serial.printf(MPBENCH_MARKER "DONE|{\"elapsed_ms\":%lld}\n", (long long)(suite.us() / 1000));
}

void benchTask(void*) {
    // Same posture as the production RenderTask, which also reconfigures the
    // task watchdog because a render legitimately blocks for many seconds.
    // panic=false here: a bench that trips the WDT should print, not reboot.
    esp_task_wdt_init(300, false);
    esp_task_wdt_add(NULL);

    g_benchDisplay = new DisplayManager();
    if (!g_benchDisplay || !g_benchDisplay->initializeEPD()) {
        Serial.println(MPBENCH_MARKER "FATAL|{\"error\":\"DisplayManager init failed\"}");
        for (;;) vTaskDelay(portMAX_DELAY);
    }

    for (;;) {
        Serial.printf(MPBENCH_MARKER "READY|{\"send\":\"g\",\"autostart_ms\":%d}\n",
                      (int)MP_BENCH_AUTOSTART_MS);
        // Wait for an explicit 'g' from the host collector, but self-start so a
        // device on a dumb USB charger still produces numbers.
        int64_t waitStart = esp_timer_get_time();
        bool triggered = false;
        while ((esp_timer_get_time() - waitStart) < (int64_t)MP_BENCH_AUTOSTART_MS * 1000) {
            esp_task_wdt_reset();
            while (Serial.available()) {
                if (Serial.read() == 'g') { triggered = true; break; }
            }
            if (triggered) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        Serial.printf(MPBENCH_MARKER "START|{\"trigger\":\"%s\"}\n",
                      triggered ? "serial" : "autostart");
        runSuite();
        // Loop back to READY so the operator can re-run without a power cycle.
    }
}

} // namespace

void MPBench_Start() {
    // Core 0, same core the production RenderTask is pinned to, so cache and
    // scheduler behaviour resemble the real thing.
    xTaskCreatePinnedToCore(benchTask, "MPBench", MP_BENCH_TASK_STACK, NULL,
                            tskIDLE_PRIORITY + 1, NULL, 0);
}

#endif // MP_BENCH

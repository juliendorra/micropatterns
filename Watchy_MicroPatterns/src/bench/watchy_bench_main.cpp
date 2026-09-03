// Watchy per-operation / per-phase renderer benchmark firmware.
//
// Build:   pio run -e watchy2-bench
// Flash:   pio run -e watchy2-bench -t upload
// Collect: python3 tools/device_bench/collect_watchy.py --port /dev/cu.usbserial-XXXX
//
// WHY THIS HAS ITS OWN main() instead of a hook in src/main.cpp, the way the
// M5Paper's bench does: the normal firmware pulls in SPIFFS, ScriptManager, the
// network stack, BLE provisioning and the RTC. None of that is under
// measurement, all of it competes for a PICO-D4's ~180 KB of heap, and the
// display list is the headline RAM risk on this device. `build_src_filter`
// excludes src/* entirely for this env and compiles the seven shared renderer
// sources plus this file. Nothing else is linked in.
//
// It also means this env cannot be broken by, or break, work in progress on the
// normal firmware -- and vice versa. (The M5Paper's env:m5paper-bench had bit
// rotted for exactly the opposite reason: mp_bench.cpp still calls a four-arg
// DisplayListRenderer constructor that became three-arg, so it no longer
// compiles.)
//
// WHAT IS AND IS NOT TIMED
//
// Timed, per script, with esp_timer (microseconds -- millis() cannot resolve a
// 300 us display-list pass):
//   parse       source text  -> MpProgram
//   displaylist MpProgram    -> std::vector<DisplayListItem>
//   raster      display list -> the GxEPD2 framebuffer
//
// NOT timed, and deliberately never even performed: the panel update.
// nextPage() is what transfers the buffer to the e-paper, and on this panel
// that is a fixed ~600-2000 ms of waveform that swamps everything above and
// wears the display. Rendering into the buffer and returning without calling
// nextPage() leaves the frame unshown -- the same trick src/main.cpp uses to
// abandon a render on a button press. So the panel is never driven here, and
// the numbers are pure compute.

// Guarded exactly the way M5Paper_MicroPatterns/src/bench/mp_bench.h is: the
// whole translation unit is compiled out unless MP_BENCH is defined. env:watchy2
// selects its sources with `+<*>`, so without this guard the mere existence of
// this file breaks the normal firmware's link with a duplicate setup(), loop()
// and g_display. Guarding here rather than adding `-<bench/>` to that env keeps
// the bench entirely self-contained.
#if MP_BENCH

#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <esp_timer.h>

#include "../watchy_canvas.h"
#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "display_list_renderer.h"
#include "watchy_bench_corpus.h"

// Watchy 2.0 panel wiring, same values as src/main.cpp.
#define EPD_CS   5
#define EPD_DC   10
#define EPD_RST  9
#define EPD_BUSY 19

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> g_display(
    GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

static WatchyCanvas g_canvas;

#ifndef MP_BENCH_REPS
#define MP_BENCH_REPS 7
#endif

#ifndef MP_BENCH_BUILD_TAG
#define MP_BENCH_BUILD_TAG "untagged"
#endif

// The fixed seed. Same values as the host harness's c0_123456 golden seed, so a
// device number and a host number describe the same frame.
static const int kCounter = 0;
static const int kHour = 12, kMinute = 34, kSecond = 56;

// Both render paths are measured in the SAME firmware run, alternating per rep,
// rather than by flashing twice. Two flashes cannot rule out a difference in
// clock, temperature, heap layout or flash cache state; alternating reps inside
// one run puts both paths under identical conditions and interleaves whatever
// drift remains across both of them equally.
static void benchOne(const MPBenchScript& s, bool fixedPoint)
{
    const int W = g_display.width();
    const int H = g_display.height();

    for (int rep = 0; rep < MP_BENCH_REPS; ++rep) {
        // Everything is constructed and destroyed inside the rep, so each one
        // starts from the same heap state. A parser kept across reps would let
        // the first rep pay all the allocation and flatter the rest.
        int64_t t0 = esp_timer_get_time();
        MicroPatternsParser parser;
        const bool ok = parser.parse(String(s.src));
        const int64_t parse_us = esp_timer_get_time() - t0;

        if (!ok) {
            Serial.printf("MPBENCH|kind=%s name=%s path=%s rep=%d error=parse\n",
                          s.kind, s.name, fixedPoint ? "fixed" : "float", rep);
            const std::vector<String>& errs = parser.getErrors();
            for (size_t i = 0; i < errs.size() && i < 3; ++i) {
                Serial.printf("MPBENCH|kind=%s name=%s parse_error=%s\n",
                              s.kind, s.name, errs[i].c_str());
            }
            Serial.flush();
            return;
        }

        t0 = esp_timer_get_time();
        MicroPatternsRuntime runtime(W, H, parser.getProgram());
        runtime.setCounter(kCounter);
        runtime.setTime(kHour, kMinute, kSecond);
        runtime.generateDisplayList();
        const int64_t dl_us = esp_timer_get_time() - t0;

        const std::vector<DisplayListItem>& dl = runtime.getDisplayList();

        // setFullWindow + firstPage prepare the framebuffer. Page height is the
        // full panel height on this device, so the render loop body would run
        // exactly once; we run it once by hand and never call nextPage().
        g_display.setFullWindow();
        g_display.firstPage();

        t0 = esp_timer_get_time();
        DisplayListRenderer renderer(&g_canvas, W, H);
        renderer.setFixedPointEnabled(fixedPoint);
        renderer.render(dl);
        const int64_t raster_us = esp_timer_get_time() - t0;

        Serial.printf("MPBENCH|kind=%s name=%s path=%s rep=%d items=%u rendered=%d "
                      "parse_us=%lld dl_us=%lld raster_us=%lld "
                      "prog_bytes=%u heap=%u largest=%u\n",
                      s.kind, s.name, fixedPoint ? "fixed" : "float", rep,
                      (unsigned)dl.size(), renderer.getRenderedItems(),
                      (long long)parse_us, (long long)dl_us, (long long)raster_us,
                      (unsigned)parser.getProgram().byteSize(),
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        Serial.flush();

        // The renderer yields and pets the watchdog per scanline, but the gap
        // BETWEEN scripts is ours to yield in.
        delay(5);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(2500);   // let the host attach before the first line goes out

    pinMode(EPD_CS,   OUTPUT);
    pinMode(EPD_RST,  OUTPUT);
    pinMode(EPD_DC,   OUTPUT);
    pinMode(EPD_BUSY, INPUT);
    // Same bring-up as src/main.cpp: the Szybet fork's parameters for this
    // SSD1681/GDEH0154D67 pairing, pulldown_rst_mode true, 10 ms reset.
    // No SPI.begin(): the default VSPI pins already match the wiring.
    g_display.epd2.selectSPI(SPI, SPISettings(20000000, MSBFIRST, SPI_MODE0));
    g_display.init(0, true, 10, true);

    Serial.printf("MPBENCH|begin build=%s canvas=%dx%d reps=%d scripts=%d "
                  "seed=c%d_%02d%02d%02d heap=%u\n",
                  MP_BENCH_BUILD_TAG, g_display.width(), g_display.height(),
                  MP_BENCH_REPS, MPBENCH_SCRIPT_COUNT,
                  kCounter, kHour, kMinute, kSecond, (unsigned)ESP.getFreeHeap());
    Serial.flush();

    for (int i = 0; i < MPBENCH_SCRIPT_COUNT; ++i) {
        benchOne(kMPBenchScripts[i], false);   // float
        benchOne(kMPBenchScripts[i], true);    // Q16.16
    }

    Serial.println("MPBENCH|end");
    Serial.flush();
}

void loop()
{
    // The bench runs once in setup(). Staying awake and quiet is deliberate:
    // this firmware must never deep-sleep, because a sleeping Watchy stops
    // answering the serial port and that is how "serial is worthless on this
    // device" got written down in the first place.
    delay(1000);
}

#endif // MP_BENCH

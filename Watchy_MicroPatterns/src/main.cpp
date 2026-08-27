// Micropatterns on Watchy 2.0 -- first standalone firmware.
//
// Scope of this version, deliberately: boot, render a real script to the panel,
// and let you move between a few of them (button or serial). No WiFi, no
// server, no filesystem -- the scripts are compiled into flash. Those come
// later; this exists to prove the core renders correctly on this hardware.
//
// The parser, runtime, rasterizer, occlusion buffer and display-list renderer
// are the SAME SOURCES the M5Paper firmware builds (see platformio.ini). The
// only Watchy-specific code is the canvas in watchy_canvas.h.

#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <SPI.h>

#include "embedded_scripts.h"
#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "display_list_renderer.h"
#include "watchy_canvas.h"

// --- Watchy 2.0 pinout (docs/analysis/watchy-hardware-and-references.md) ---
// EPD wiring is identical across Watchy v1/v1.5/v2; only the battery ADC and
// one button pin differ between revisions.
#define EPD_CS   5
#define EPD_DC   10
#define EPD_RST  9
#define EPD_BUSY 19

// Vibration motor (InkWatchy condition.h: VIB_MOTOR_PIN 13 on Watchy 1/1.5/2).
// This is our TELEMETRY CHANNEL. Serial output is proven worthless on this
// device -- the official InkWatchy image runs visibly while emitting zero bytes
// over 60s -- and the panel cannot report progress that happens BEFORE the
// panel works. The motor can: it needs one GPIO and nothing else, so it reports
// from the very first instruction of setup().
#define VIB_MOTOR_PIN 13

#define BTN_MENU 26
#define BTN_BACK 25
#define BTN_UP   32
#define BTN_DOWN 4

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> g_display(
    GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

static WatchyCanvas g_canvas;
static int  g_currentScript = 0;
static int  g_counter = 0;

// STAGE REPORTER -- the only working instrument on this device.
//
// Serial is proven worthless here (the official InkWatchy image runs visibly
// while emitting zero bytes over 60s), and the panel cannot report progress
// that happens before the panel works. A minimal motor-only firmware was
// confirmed buzzing on this watch, so GPIO 13 is a channel that definitely
// works.
//
// A one-shot buzz per stage is not enough: if setup() HANGS, you hear a couple
// of buzzes and then silence, and picking the watch up late tells you nothing.
// So a separate FreeRTOS task repeats the furthest stage reached, forever. It
// runs independently of setup(), so it keeps reporting even while setup() is
// blocked -- which is exactly the case we are trying to diagnose.
//
//   1 = setup() entered            4 = probe pattern pushed to panel
//   2 = about to bring up panel    5 = first script rendered; running loop()
//   3 = display init() RETURNED
// Default OFF. Set -DMP_STAGE_BUZZ=1 in platformio.ini to re-enable.
// Kept rather than deleted because this was the ONLY instrument that worked on
// this device: serial is silent even for firmware that is demonstrably running,
// and the panel cannot report progress that happens before the panel works.
#ifndef MP_STAGE_BUZZ
#define MP_STAGE_BUZZ 0
#endif

volatile int g_stage = 0;

static void buzzN(int n)
{
#if !MP_STAGE_BUZZ
    (void)n; return;                 // motor silent in normal builds
#else
    for (int i = 0; i < n; ++i) {
        digitalWrite(VIB_MOTOR_PIN, HIGH);
        delay(140);
        digitalWrite(VIB_MOTOR_PIN, LOW);
        delay(220);
    }
#endif
}

static void StageReporterTask(void* pv)
{
    (void)pv;
    for (;;) {
        int st = g_stage;
        if (st > 0) buzzN(st);
        vTaskDelay(pdMS_TO_TICKS(3500)); // long gap so counts stay countable
    }
}

static void logHeap(const char* stage)
{
    // RAM is the headline risk on a PICO-D4: no PSRAM, and the display list
    // costs roughly 370 bytes per item. Log it at every stage so a failure to
    // fit is obvious rather than a silent crash.
    log_i("[heap] %-22s free=%u largest-block=%u", stage,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// Renders EMBEDDED_SCRIPTS[index] and pushes it to the panel.
static bool renderScript(int index)
{
    if (index < 0 || index >= EMBEDDED_SCRIPT_COUNT) return false;
    const EmbeddedScript& scr = EMBEDDED_SCRIPTS[index];
    log_i("=== Rendering '%s' (%s) ===", scr.name, scr.id);
    logHeap("before parse");

    MicroPatternsParser parser;
    parser.reset();
    if (!parser.parse(String(scr.content))) {
        log_e("Parse failed for '%s':", scr.id);
        for (const String& e : parser.getErrors()) log_e("  %s", e.c_str());
        return false;
    }
    logHeap("after parse");

    const int W = g_display.width();
    const int H = g_display.height();

    // Display list generation. Scripts read $WIDTH/$HEIGHT, so ones written to
    // adapt (Circuits says so in its own header) lay themselves out for 200x200
    // rather than being cropped from 960x540. Ones with hardcoded coordinates
    // will not, and that is a product decision still open -- see
    // docs/analysis/watchy-port-design.md sections 7.2 and 9.
    unsigned long t0 = millis();
    MicroPatternsRuntime runtime(W, H, parser.getAssets());
    runtime.setCommands(&parser.getCommands());
    runtime.setDeclaredVariables(&parser.getDeclaredVariables());
    runtime.setCounter(g_counter);
    runtime.setTime(0, 0, 0);
    runtime.generateDisplayList();
    unsigned long tGen = millis() - t0;

    const std::vector<DisplayListItem>& dl = runtime.getDisplayList();
    log_i("Display list: %u items in %lu ms", (unsigned)dl.size(), tGen);
    logHeap("after display list");

    // Rasterize. Page height is the full panel height, so this loop body runs
    // exactly once; the display list is built outside it so the expensive work
    // is not repeated per page.
    t0 = millis();
    g_display.setFullWindow();
    g_display.firstPage();
    do {
        DisplayListRenderer renderer(&g_canvas, parser.getAssets(), W, H);
        renderer.render(dl);
    } while (g_display.nextPage());
    unsigned long tRender = millis() - t0;

    log_i("Rendered '%s' in %lu ms (gen %lu + raster %lu)",
          scr.name, tGen + tRender, tGen, tRender);
    logHeap("after render");
    return true;
}

static void showScript(int index)
{
    g_currentScript = (index % EMBEDDED_SCRIPT_COUNT + EMBEDDED_SCRIPT_COUNT) % EMBEDDED_SCRIPT_COUNT;
    g_counter++;
    if (!renderScript(g_currentScript)) {
        log_e("Render failed for index %d", g_currentScript);
    }
}

// Minimal serial channel, mirroring the M5Paper console: list / run N / next.
static void pollSerial()
{
    static char line[64];
    static size_t len = 0;
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            line[len] = '\0';
            String cmd = String(line); cmd.trim(); cmd.toLowerCase();
            len = 0;
            if (cmd == "list") {
                for (int i = 0; i < EMBEDDED_SCRIPT_COUNT; i++)
                    Serial.printf("MPCON|%d\t%s\t%s\n", i,
                                  EMBEDDED_SCRIPTS[i].id, EMBEDDED_SCRIPTS[i].name);
                Serial.printf("MPCON|%d script(s)\n", EMBEDDED_SCRIPT_COUNT);
            } else if (cmd == "next") {
                Serial.println("MPCON|ok next");
                showScript(g_currentScript + 1);
            } else if (cmd.startsWith("run ")) {
                int idx = cmd.substring(4).toInt();
                Serial.printf("MPCON|ok run %d\n", idx);
                showScript(idx);
            } else {
                Serial.println("MPCON|commands: list | run <index> | next");
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}

void setup()
{
    // Motor FIRST, before anything that could hang, so stage 1 proves the
    // application is executing at all.
    pinMode(VIB_MOTOR_PIN, OUTPUT);
    digitalWrite(VIB_MOTOR_PIN, LOW);
    g_stage = 1;                                  // setup() reached
#if MP_STAGE_BUZZ
    xTaskCreatePinnedToCore(StageReporterTask, "StageRep", 2048, NULL,
                            tskIDLE_PRIORITY + 1, NULL, 1);
#endif

    Serial.begin(115200);
    delay(200);
    Serial.println("MPCON|boot: setup() entered"); Serial.flush();
    log_i("Micropatterns Watchy build starting");
    logHeap("boot");

    pinMode(BTN_MENU, INPUT);
    pinMode(BTN_BACK, INPUT);
    pinMode(BTN_UP,   INPUT);
    pinMode(BTN_DOWN, INPUT);

    // Bracketed deliberately: GxEPD2's init drives RESET and then waits on the
    // BUSY line. If the panel never answers, this is where the firmware goes
    // quiet, and without a marker on each side that is indistinguishable from
    // "never booted at all". Print before, print after.
    Serial.println("MPCON|display init: starting"); Serial.flush();
    // Panel bring-up copied from InkWatchy's WATCHY_2 path
    // (src/hardware/display/display.cpp initDisplay), which is known to drive
    // this exact SSD1681 / GDEH0154D67 on this exact watch. The previous
    // version used GxEPD2's defaults and the panel never updated. Differences
    // that matter, in rough order of suspicion:
    //
    //   pulldown_rst_mode = TRUE  (was false) -- wrong RST drive mode can leave
    //       the panel unreset, so init() waits on BUSY forever.
    //   reset_duration    = 10ms  (was 2ms)   -- 2ms may be too short to reset.
    //   explicit pinMode()s before init, including BUSY as INPUT.
    //   selectSPI() with explicit 20MHz MODE0 settings.
    //   serial_diag_bitrate = 0 (was 115200) so GxEPD2 does not re-open Serial.
    //
    // Note EPD_BUSY is GPIO19, which is also VSPI's default MISO. InkWatchy
    // does NOT call SPI.begin() on Watchy 2 -- the default VSPI pins already
    // match (SCK 18, MOSI 23, SS 5) -- so neither do we.
    g_stage = 2;                                  // about to bring up the panel
    pinMode(EPD_CS,   OUTPUT);
    pinMode(EPD_RST,  OUTPUT);
    pinMode(EPD_DC,   OUTPUT);
    pinMode(EPD_BUSY, INPUT);
    g_display.epd2.selectSPI(SPI, SPISettings(20000000, MSBFIRST, SPI_MODE0));
    g_display.init(0, true, 10, true);
    g_stage = 3;                                  // init() RETURNED: no BUSY hang
    Serial.println("MPCON|display init: returned"); Serial.flush();
    g_display.setRotation(0);
    Serial.println("MPCON|display init: returned"); Serial.flush();
    log_i("Display initialised: %dx%d", g_display.width(), g_display.height());

    // PANEL-AS-INSTRUMENT probe.
    //
    // Serial output is proven WORTHLESS on this device: the official InkWatchy
    // image runs visibly while emitting zero bytes over 60s (see
    // docs/analysis/watchy-port-attempt-log.md 4.2d). So the panel is the only
    // trustworthy output channel, and this draws to it before ANY script,
    // parser or renderer code can fail or hang.
    //
    // Reading the result:
    //   diagonal-stripe pattern visible -> init() returned AND the panel is
    //       being driven correctly. Any later blank screen is then a fault in
    //       the script/render path, not in the display or the boot.
    //   screen unchanged -> we never got this far: init() blocked (most likely
    //       waiting on BUSY) or the app is not executing at all.
    g_display.setFullWindow();
    g_display.firstPage();
    do {
        g_display.fillScreen(GxEPD_WHITE);
        for (int y = 0; y < g_display.height(); ++y) {
            for (int x = 0; x < g_display.width(); ++x) {
                if (((x + y) / 8) % 2 == 0) g_display.drawPixel(x, y, GxEPD_BLACK);
            }
        }
    } while (g_display.nextPage());
    g_stage = 4;                                  // probe pattern pushed
    Serial.println("MPCON|probe pattern pushed"); Serial.flush();
    delay(2500); // hold it long enough to be seen before the first script draws

    showScript(0);
    g_stage = 5;                                  // first script rendered; loop() runs
    Serial.println("MPCON|ready -- commands: list | run <index> | next");
}

void loop()
{
    pollSerial();

    // Heartbeat. Without it, a device whose boot output was simply missed looks
    // exactly like a hung one -- which cost real time diagnosing this port. Any
    // capture started at any moment now shows within 5s whether the firmware is
    // alive, and how far it got.
    static unsigned long lastBeat = 0;
    if (millis() - lastBeat > 5000) {
        lastBeat = millis();
        Serial.printf("MPCON|alive t=%lus script=%d/%d heap=%u\n",
                      millis() / 1000, g_currentScript, EMBEDDED_SCRIPT_COUNT,
                      ESP.getFreeHeap());
        Serial.flush();
    }

    // Any button steps to the next script. Watchy buttons read HIGH when pressed.
    if (digitalRead(BTN_MENU) == HIGH || digitalRead(BTN_UP) == HIGH ||
        digitalRead(BTN_DOWN) == HIGH || digitalRead(BTN_BACK) == HIGH) {
        showScript(g_currentScript + 1);
        delay(300); // crude debounce; this build is not power-optimised
    }
    delay(20);
}

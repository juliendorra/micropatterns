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

// Watchy 2.0 button pins, from InkWatchy src/defines/condition.h.
// NOTE UP is 35 on Watchy 2.0 -- 32 is Watchy 1/1.5 only. This firmware had 32,
// so that button never registered at all.
// Buttons read HIGH when pressed (InkWatchy's BUT_CLICK_STATE for this board).
//
// Physical layout: the four buttons sit at the four corners. Mapping below is
// the working assumption; the corner press indicator is what verifies it -- if
// a press lights the wrong corner, swap the entries here.
//
// Verified on the device by pressing each button and watching which corner
// lit up. The first attempt had MENU/BACK swapped -- the names do not match
// the physical layout, so trust the corners, not the pin names.
//
//        top-left  BACK (25)        UP (35)  top-right
//     bottom-left  MENU (26)      DOWN (4)   bottom-right
#define BTN_BACK 25   // top-left     -- hold 5s: full refresh
#define BTN_MENU 26   // bottom-left  -- re-run current script
#define BTN_UP   35   // top-right    -- previous script
#define BTN_DOWN 4    // bottom-right -- next script

#define BTN_LONG_PRESS_MS 5000  // hold time for the full-refresh gesture

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

enum Corner { CORNER_TL, CORNER_TR, CORNER_BL, CORNER_BR };
static void drawCornerIndicator(Corner c, bool filled);
static void showScriptName(const char* name);
static void fullRefresh();

// --- Panel update policy --------------------------------------------------
//
// GxEPD2_154_D67 reports full_refresh_time = 2600ms and
// partial_refresh_time = 500ms, and hasFastPartialUpdate = true. A full refresh
// drives every pixel through black and white -- that is the flashing -- and is
// 5x slower. A fast partial update rewrites only what changed, with no flash.
//
// We can use the fast path almost always because the content is PURE BLACK AND
// WHITE: there are no grey levels to degrade, which is exactly the condition
// fast waveforms are safe under. Same reasoning as the M5Paper's move from
// GC16 to DU -- see docs/analysis/m5paper-panel-refresh.md.
//
// Fast updates still accumulate ghosting, so every WATCHY_DEGHOST_INTERVAL-th
// update is a full refresh that cleans the panel. The first update after boot
// is ALSO forced full: the panel's prior contents are unknown, and InkWatchy
// carries the same first-boot workaround (SCREEN_PARTIAL_GREY_WORKAROUND)
// because partial updates issued onto an uninitialised panel can leave it grey.
// Raised from 8 to 24 after checking on the device: the panel stayed visibly
// clean through a full cycle of 8, so the budget was needlessly conservative.
// This is a MEASURED value, unlike the M5Paper's equivalent (still 8, and still
// a guess -- that is a different panel driven by a different waveform, so do not
// copy this number across without looking at it).
//
// Symptom if it is too high: grey residue of previous frames building up. Lower
// it. Cost of it being too low: an unnecessary 2.6s flashing refresh.
static const int WATCHY_DEGHOST_INTERVAL = 24;

// Periodic automatic re-render, mirroring the M5Paper, which wakes from light
// sleep every SystemManager::DEFAULT_SLEEP_DURATION_S (77s) and re-renders the
// current script. Scripts are time- and counter-dependent -- $COUNTER advances
// and $HOUR/$MINUTE/$SECOND move -- so a static panel is a script frozen in
// time rather than a finished picture.
//
// Matched to 77s deliberately: the same script should evolve at the same rate
// on both devices.
//
// NOT power-optimised. The M5Paper spends this interval in light sleep; this
// firmware stays awake in loop(). Battery life is a known open item for the
// Watchy port -- the design doc puts the sustainable autonomous cadence at
// roughly hourly, which this does not respect.
static const unsigned long AUTO_RERUN_INTERVAL_MS = 77UL * 1000UL;
static unsigned long g_lastRenderMs = 0;
static int g_updatesSinceFull = WATCHY_DEGHOST_INTERVAL;  // force full on first use

// Selects the window mode for the next page loop. Returns true if this update
// will be a full (flashing) refresh.
static bool beginPanelUpdate(bool forceFull)
{
    const bool full = forceFull || (g_updatesSinceFull >= WATCHY_DEGHOST_INTERVAL);
    if (full) {
        g_display.setFullWindow();
        g_updatesSinceFull = 0;
    } else {
        g_display.setPartialWindow(0, 0, g_display.width(), g_display.height());
        g_updatesSinceFull++;
    }
    return full;
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
    const bool fullRefreshThisTime = beginPanelUpdate(false);
    g_display.firstPage();
    do {
        DisplayListRenderer renderer(&g_canvas, parser.getAssets(), W, H);
        renderer.render(dl);
    } while (g_display.nextPage());
    unsigned long tRender = millis() - t0;
    log_i("Panel update: %s (%d/%d until de-ghost)",
          fullRefreshThisTime ? "FULL (de-ghost)" : "fast partial",
          g_updatesSinceFull, WATCHY_DEGHOST_INTERVAL);

    log_i("Rendered '%s' in %lu ms (gen %lu + raster %lu)",
          scr.name, tGen + tRender, tGen, tRender);
    logHeap("after render");
    return true;
}

// `announce` shows the script name on its own frame first. Only worth doing
// when the script actually CHANGES -- on a re-run you already know what you are
// looking at, and the title frame is a whole extra panel update.
static void showScript(int index, bool announce)
{
    g_currentScript = (index % EMBEDDED_SCRIPT_COUNT + EMBEDDED_SCRIPT_COUNT) % EMBEDDED_SCRIPT_COUNT;
    g_counter++;
    if (announce) showScriptName(EMBEDDED_SCRIPTS[g_currentScript].name);
    g_lastRenderMs = millis();
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
                showScript(g_currentScript + 1, true);
            } else if (cmd.startsWith("run ")) {
                int idx = cmd.substring(4).toInt();
                Serial.printf("MPCON|ok run %d\n", idx);
                showScript(idx, true);
            } else {
                Serial.println("MPCON|commands: list | run <index> | next");
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}


// --- Feedback -------------------------------------------------------------

// Corner press indicator. The four buttons are at the four physical corners, so
// the acknowledgement appears at the corner you actually pressed. Drawn as a
// partial window so it lands in well under a second and does not disturb the
// rest of the panel.
static void drawCornerIndicator(Corner c, bool filled)
{
    const int S = 26;                     // indicator box side, px
    const int W = g_display.width();
    const int H = g_display.height();
    int x = (c == CORNER_TL || c == CORNER_BL) ? 0 : W - S;
    int y = (c == CORNER_TL || c == CORNER_TR) ? 0 : H - S;

    g_display.setPartialWindow(x, y, S, S);
    g_display.firstPage();
    do {
        g_display.fillScreen(GxEPD_WHITE);
        if (filled) g_display.fillRect(x, y, S, S, GxEPD_BLACK);
    } while (g_display.nextPage());
}

// Shows the script name on a CLEARED panel before rendering.
//
// Deliberately clears to white first. On the M5Paper the name was being drawn
// over the outgoing script's still-visible image, so it read as garbage layered
// on the old art. The name is a transition, so it gets its own clean frame.
static void showScriptName(const char* name)
{
    const int W = g_display.width();
    beginPanelUpdate(false);   // fast partial: announcing a switch must not flash
    g_display.firstPage();
    do {
        g_display.fillScreen(GxEPD_WHITE);
        g_display.setTextColor(GxEPD_BLACK);
        g_display.setTextSize(2);
        // Default GFX glyphs are 6x8 before scaling, so 12px per char at size 2.
        int textW = (int)strlen(name) * 12;
        int x = (textW < W) ? (W - textW) / 2 : 2;
        g_display.setCursor(x, g_display.height() / 2 - 8);
        g_display.print(name);
        g_display.setTextSize(1);
    } while (g_display.nextPage());
}

// Full refresh: drive the panel through black and white to clear accumulated
// ghosting, then re-render. On the M5Paper the equivalent gesture also re-syncs
// scripts from the server; this firmware has NO networking and the API endpoint
// is dead (Deno Deploy Classic was sunset 2026-07-20), so the sync half is not
// implemented. See docs/analysis/watchy-port-attempt-log.md.
static void fullRefresh()
{
    g_updatesSinceFull = WATCHY_DEGHOST_INTERVAL;  // next render is a full refresh
    g_display.setFullWindow();
    for (int pass = 0; pass < 2; ++pass) {
        g_display.firstPage();
        do { g_display.fillScreen(pass == 0 ? GxEPD_BLACK : GxEPD_WHITE); }
        while (g_display.nextPage());
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
    pinMode(BTN_UP,   INPUT);   // GPIO35 is input-only on ESP32; INPUT is correct
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

    showScript(0, true);
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

    // Periodic re-render, so time-dependent scripts keep advancing on their own.
    // Deliberately checked BEFORE the buttons: if a press arrives during the
    // render the button handler simply sees it on the next pass.
    if (millis() - g_lastRenderMs >= AUTO_RERUN_INTERVAL_MS) {
        log_i("Auto re-render after %lus idle", AUTO_RERUN_INTERVAL_MS / 1000);
        showScript(g_currentScript, false);   // no title: same script
    }

    // --- Buttons ----------------------------------------------------------
    //
    // Mirrors the M5Paper's controls onto the four corner buttons:
    //   top-right    previous script   (M5Paper UP)
    //   bottom-right next script       (M5Paper DOWN)
    //   bottom-left  re-run current    (M5Paper PUSH / confirm)
    //   top-left     hold 5s: full refresh
    //
    // Actions are dispatched on CORNER, not on pin name, so the physical layout
    // lives in exactly one place (the btns[] table) and cannot drift out of sync
    // with the indicator again.
    //
    // Edge-triggered on release so a long press does not also fire the short
    // action, and so holding a button does not repeat.
    struct Btn { uint8_t pin; Corner corner; bool wasDown; unsigned long downAt; };
    static Btn btns[4] = {
        { BTN_BACK, CORNER_TL, false, 0 },   // top-left
        { BTN_UP,   CORNER_TR, false, 0 },   // top-right
        { BTN_MENU, CORNER_BL, false, 0 },   // bottom-left
        { BTN_DOWN, CORNER_BR, false, 0 },   // bottom-right
    };

    for (Btn& b : btns) {
        bool down = (digitalRead(b.pin) == HIGH);

        if (down && !b.wasDown) {
            // Acknowledge immediately, at the corner actually pressed. This is
            // the whole point of corner feedback: it must land before the
            // multi-second render starts, not after.
            b.wasDown = true;
            b.downAt  = millis();
            drawCornerIndicator(b.corner, true);
            continue;
        }

        if (down && b.wasDown && b.corner == CORNER_TL &&
            (millis() - b.downAt) >= BTN_LONG_PRESS_MS) {
            // Long press fires while still held, then swallows the release.
            log_i("Button: top-left held %dms -> full refresh", BTN_LONG_PRESS_MS);
            fullRefresh();
            showScript(g_currentScript, false);   // re-run: no title
            b.wasDown = false;
            continue;
        }

        if (!down && b.wasDown) {
            b.wasDown = false;
            drawCornerIndicator(b.corner, false);   // clear the acknowledgement
            unsigned long held = millis() - b.downAt;
            if (held < 40) continue;                 // debounce bounce/noise

            switch (b.corner) {
                case CORNER_TR:
                    log_i("Button: top-right -> previous script");
                    showScript(g_currentScript - 1, true);   // changed: announce
                    break;
                case CORNER_BR:
                    log_i("Button: bottom-right -> next script");
                    showScript(g_currentScript + 1, true);   // changed: announce
                    break;
                case CORNER_BL:
                    log_i("Button: bottom-left -> re-run current script");
                    showScript(g_currentScript, false);      // same script: no title
                    break;
                case CORNER_TL:
                    // Short press: no action -- this is the full-refresh button.
                    break;
            }
        }
    }
    delay(20);
}

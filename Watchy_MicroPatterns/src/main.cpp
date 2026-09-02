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

#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "display_list_renderer.h"
#include "watchy_canvas.h"
#include "watchy_rtc.h"
#include "mp_provisioning.h"

// These four are pulled in by the shared managers below, not used directly
// here. They are named explicitly because PlatformIO's library dependency
// finder only scans sources under src/, and the managers arrive from the
// M5Paper tree via build_src_filter -- without these lines HTTPClient, SPIFFS
// and WiFiClientSecure never enter the dependency graph and the build fails
// with "No such file or directory". lib_ldf_mode = deep+ would also fix it and
// breaks the framework's own WiFi library instead; see platformio.ini.
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>

#include "script_manager.h"
#include "network_manager.h"
#include "script_sync.h"
#include "mp_messages.h"
#include <vector>
#include <Preferences.h>
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "nvs.h"
#include "esp_partition.h"
#include "esp_attr.h"

#ifndef MP_NVS_SCREEN
#define MP_NVS_SCREEN 0
#endif

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

// Scripts come from the server and live in SPIFFS, exactly as on the M5Paper.
// They used to be a compiled-in EMBEDDED_SCRIPTS[] table, which was a
// scaffolding measure from before this firmware could reach the network: it
// meant a script change needed a reflash, and the watch could never see
// anything the user had written since the last build.
static ScriptManager*    g_scriptManager = nullptr;
static MPNetworkManager* g_networkManager = nullptr;

struct ScriptEntry { String humanId; String name; String fileId; };
static std::vector<ScriptEntry> g_scripts;

static int  g_currentScript = 0;

// A render can exhaust the fragmented heap and abort() before setup reaches
// loop(). RTC slow memory survives that reboot without adding an NVS write on
// every 83-second render. If this marker is still armed on the next boot, the
// saved script did not finish and must not be attempted again immediately.
static const uint32_t RENDER_GUARD_MAGIC = 0x4d505247; // "MPRG"
RTC_NOINIT_ATTR static volatile uint32_t g_renderGuardMagic;
RTC_NOINIT_ATTR static volatile uint32_t g_renderGuardScriptHash;
RTC_NOINIT_ATTR static volatile uint32_t g_renderCrashCount;
static bool g_renderRecoverySafeMode = false;
static bool g_continueRenderCrashStreak = false;

// Title browsing. A press shows the next script's name at once and arms a
// timer; the render only starts once the presses stop. Same value and same
// behaviour as the M5Paper.
#define TITLE_SETTLE_MS 450
static int           g_pendingScript = -1;   // -1: nothing waiting to render
static unsigned long g_renderDueAt   = 0;


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
#ifndef MP_PANEL_PROBE
#define MP_PANEL_PROBE 0
#endif

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
static void showRenderError(const char* scriptName, const char* reason);
static void showNotice(const char* title, const char* line1, const char* line2,
                       const char* hint);
static void fullRefresh();
static bool loadScriptIndex();
static void browseScript(int delta);
static void renderIfSettled();
static bool anyButtonDown();
static void syncScripts(bool announce);
static bool syncTimeFromNTP();

static uint32_t scriptIdHash(const String& id)
{
    uint32_t hash = 2166136261u; // FNV-1a
    for (size_t i = 0; i < id.length(); ++i) {
        hash ^= (uint8_t)id[i];
        hash *= 16777619u;
    }
    return hash;
}

static void armRenderGuard(const String& id)
{
    // Magic is written last, so a reset between these stores cannot make a
    // partially-written marker look valid.
    if (!g_continueRenderCrashStreak) g_renderCrashCount = 0;
    g_continueRenderCrashStreak = false;
    g_renderGuardScriptHash = scriptIdHash(id);
    g_renderGuardMagic = RENDER_GUARD_MAGIC;
}

static void disarmRenderGuard(bool completed)
{
    g_renderGuardMagic = 0;
    if (completed) g_renderCrashCount = 0;
}

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

// Periodic automatic re-render. Scripts are time- and counter-dependent --
// $COUNTER advances and $HOUR/$MINUTE/$SECOND move -- so a static panel is a
// script frozen in time rather than a finished picture.
//
// 83 is coprime with 60. Successive renders therefore advance by 23 seconds
// modulo a minute and visit every second exactly once before repeating, rather
// than sampling a small fixed subset of seconds. The old 77-second cadence also
// had full coverage (stride 17); 83 changes the scattered traversal while
// retaining that property and is deliberately different from the M5Paper.
//
// NOT power-optimised. The M5Paper spends this interval in light sleep; this
// firmware stays awake in loop(). Battery life is a known open item for the
// Watchy port -- the design doc puts the sustainable autonomous cadence at
// roughly hourly, which this does not respect.
// How long the device stays awake for the serial console after boot, and after
// each byte received. See sleepUntilSomethingHappens().
static const unsigned long CONSOLE_AWAKE_MS = 60000;
static unsigned long g_lastSerialMs = 0;

static const unsigned long AUTO_RERUN_INTERVAL_MS = 83UL * 1000UL;

// Hours east of UTC written into the RTC at NTP sync. The chip holds no
// timezone of its own, so whatever offset is applied here is simply what
// $HOUR reads afterwards.
//
// Fixed, and matching the M5Paper's default (SystemManager's _timezone starts
// at 1) so the same script shows the same hour on both devices. That firmware
// can at least store a different value in NVS; this one has no settings store
// and no UI to reach one, so changing zone means changing this line. DST is
// not handled on either device -- the M5Paper passes daylightOffset_sec = 0
// too, so both are an hour out in summer.
static const int MP_TZ_OFFSET_HOURS = 1;
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

// Loads the script list written by the last sync into g_scripts.
static bool loadScriptIndex()
{
    g_scripts.clear();
    if (!g_scriptManager) return false;

    JsonDocument listDoc;
    if (!g_scriptManager->loadScriptList(listDoc)) {
        log_w("No local script list yet");
        return false;
    }
    if (!listDoc.is<JsonArray>()) {
        log_e("Local script list is not an array");
        return false;
    }
    for (JsonObject item : listDoc.as<JsonArray>()) {
        const char* id = item["id"].as<const char*>();
        if (!id || strlen(id) == 0) continue;
        ScriptEntry e;
        e.humanId = id;
        const char* nm = item["name"].as<const char*>();
        e.name   = (nm && strlen(nm)) ? nm : id;
        e.fileId = item["fileId"].as<String>();
        if (e.fileId.isEmpty() || e.fileId == "null") continue;
        g_scripts.push_back(e);
    }
    log_i("Script index: %u script(s) on device", (unsigned)g_scripts.size());
    return !g_scripts.empty();
}

// True while any of the four buttons is held.
//
// Used as the renderer's interrupt check, so a render abandons itself the
// moment someone presses -- the display list renderer already polls a callback
// between items for exactly this (the M5Paper has used it since the beginning),
// this firmware simply never supplied one.
static bool anyButtonDown()
{
    return digitalRead(BTN_BACK) == HIGH || digitalRead(BTN_MENU) == HIGH ||
           digitalRead(BTN_UP)   == HIGH || digitalRead(BTN_DOWN) == HIGH;
}

// Renders g_scripts[index] and pushes it to the panel.
//
// `state` carries this script's own $COUNTER in, and the time actually used
// back out, so the caller can persist both -- the same in/out arrangement the
// M5Paper's RenderController has.
static bool renderScript(int index, ScriptExecState& state)
{
    if (index < 0 || index >= (int)g_scripts.size()) return false;
    const ScriptEntry& scr = g_scripts[index];
    log_i("=== Rendering '%s' (%s) ===", scr.name.c_str(), scr.humanId.c_str());
    logHeap("before parse");

    String source;
    if (!g_scriptManager->loadScriptContent(scr.fileId, source) || source.isEmpty()) {
        log_e("Could not load content for '%s' (fileId %s)",
              scr.humanId.c_str(), scr.fileId.c_str());
        showRenderError(scr.name.c_str(), MP_MSG_SCRIPT_MISSING);
        return false;
    }

    MicroPatternsParser parser;
    parser.reset();
    if (!parser.parse(source)) {
        log_e("Parse failed for '%s':", scr.humanId.c_str());
        for (const String& e : parser.getErrors()) log_e("  %s", e.c_str());
        showRenderError(scr.name.c_str(), MP_MSG_PARSE_FAILED);
        return false;
    }
    source = String();   // the parser owns the tokens now; ~5KB back to the heap
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
    runtime.setCounter(state.counter);
    // Real wall-clock time, so $HOUR/$MINUTE/$SECOND move as they do in the
    // editor and on the M5Paper. This used to pass 0,0,0 unconditionally, which
    // froze every clock-driven script at midnight -- the runtime and the
    // variables were always wired, the platform simply never read the RTC.
    // With no chip or an unset one the zeros come back, and that is the right
    // fallback: a script drawing midnight is better than one drawing garbage.
    int rtcH = 0, rtcM = 0, rtcS = 0;
    if (!WatchyRTC::now(rtcH, rtcM, rtcS)) rtcH = rtcM = rtcS = 0;
    runtime.setTime(rtcH, rtcM, rtcS);
    state.hour = rtcH; state.minute = rtcM; state.second = rtcS;
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
    bool aborted = false;
    g_display.firstPage();
    do {
        DisplayListRenderer renderer(&g_canvas, parser.getAssets(), W, H);
        renderer.setInterruptCheckCallback(anyButtonDown);
        renderer.render(dl);
        if (anyButtonDown()) {
            // Leave WITHOUT calling nextPage(). The buffer holds a half-drawn
            // frame and nextPage() is what pushes it to the panel, so breaking
            // here costs the work but never shows it. The next firstPage()
            // starts clean.
            aborted = true;
            break;
        }
    } while (g_display.nextPage());
    unsigned long tRender = millis() - t0;

    if (aborted) {
        log_i("Render of '%s' abandoned after %lums: button pressed",
              scr.name.c_str(), tRender);
        // The de-ghost counter must not advance for a frame never shown.
        if (!fullRefreshThisTime && g_updatesSinceFull > 0) g_updatesSinceFull--;
        return false;
    }
    log_i("Panel update: %s (%d/%d until de-ghost)",
          fullRefreshThisTime ? "FULL (de-ghost)" : "fast partial",
          g_updatesSinceFull, WATCHY_DEGHOST_INTERVAL);

    log_i("Rendered '%s' in %lu ms (gen %lu + raster %lu)",
          scr.name.c_str(), tGen + tRender, tGen, tRender);
    logHeap("after render");
    return true;
}

// `announce` shows the script name on its own frame first. Only worth doing
// when the script actually CHANGES -- on a re-run you already know what you are
// looking at, and the title frame is a whole extra panel update.
static void showScript(int index, bool announce)
{
    if (g_scripts.empty()) {
        g_lastRenderMs = millis();
        showNotice(MP_MSG_NO_SCRIPTS, MP_MSG_NO_SCRIPTS_HOW, MP_MSG_NO_SCRIPTS_HOW2, "");
        return;
    }
    const int n = (int)g_scripts.size();
    g_currentScript = (index % n + n) % n;
    const String& humanId = g_scripts[g_currentScript].humanId;

    // $COUNTER belongs to the SCRIPT, not to the device, and it is read back
    // from SPIFFS rather than held in RAM. It used to be one global int
    // incremented per render: every script shared one sequence, so leaving a
    // script and coming back resumed wherever the others had got to, and a
    // reboot sent everything back to zero. This is the M5Paper's arrangement,
    // through the same ScriptManager and the same script_states.json format --
    // a script moved between the two devices keeps counting.
    ScriptExecState state;
    if (g_scriptManager && g_scriptManager->loadScriptExecutionState(humanId, state)
        && state.state_loaded) {
        state.counter++;
    } else {
        state.counter = 0;   // first run of this script on this device
    }

    if (announce) showScriptName(g_scripts[g_currentScript].name.c_str());
    // Survives a reboot, same as on the M5Paper.
    if (g_scriptManager) g_scriptManager->saveCurrentScriptId(humanId);
    g_lastRenderMs = millis();
    armRenderGuard(humanId);
    if (!renderScript(g_currentScript, state)) {
        disarmRenderGuard(false);
        // renderScript() has already shown the specific reason where it knows
        // one, and says nothing at all when the user simply pressed on.
        //
        // The counter is NOT saved here, so an abandoned render does not
        // consume a tick -- same principle as the de-ghost counter being wound
        // back for a frame that was never shown.
        log_i("Render did not complete for index %d", g_currentScript);
        return;
    }
    disarmRenderGuard(true);
    g_renderRecoverySafeMode = false;
    if (g_scriptManager) g_scriptManager->saveScriptExecutionState(humanId, state);
}

// Steps the selection and shows the new title, WITHOUT rendering.
//
// The render is armed for TITLE_SETTLE_MS later and re-armed by every further
// press, so holding down next pages through titles at the speed of the button.
// Previously each press rendered immediately and the following press had to
// wait for that render to finish -- roughly 750ms per title, and a partial
// frame of the wrong script in between.
static void browseScript(int delta)
{
    if (g_scripts.empty()) { showScript(0, true); return; }
    const int n = (int)g_scripts.size();
    const int from = (g_pendingScript >= 0) ? g_pendingScript : g_currentScript;
    g_pendingScript = ((from + delta) % n + n) % n;
    g_renderDueAt = millis() + TITLE_SETTLE_MS;
    showScriptName(g_scripts[g_pendingScript].name.c_str());
}

// Renders the browsed-to script once the presses have stopped.
static void renderIfSettled()
{
    if (g_pendingScript < 0) return;
    if ((long)(millis() - g_renderDueAt) < 0) return;
    if (anyButtonDown()) { g_renderDueAt = millis() + TITLE_SETTLE_MS; return; }

    const int target = g_pendingScript;
    g_pendingScript = -1;
    showScript(target, false);   // the title is already on screen
}

// Minimal serial channel, mirroring the M5Paper console: list / run N / next.
// Plus rtc / rtc-unset, which have no M5Paper counterpart: they exist to stage
// and inspect the clock state that otherwise needs the case opened.
static void pollSerial()
{
    static char line[64];
    static size_t len = 0;
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c < 0) break;
        g_lastSerialMs = millis();   // someone is typing: hold off sleep
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            line[len] = '\0';
            String cmd = String(line); cmd.trim(); cmd.toLowerCase();
            len = 0;
            if (cmd == "list") {
                for (size_t i = 0; i < g_scripts.size(); i++)
                    Serial.printf("MPCON|%u\t%s\t%s\n", (unsigned)i,
                                  g_scripts[i].humanId.c_str(), g_scripts[i].name.c_str());
                Serial.printf("MPCON|%u script(s)\n", (unsigned)g_scripts.size());
            } else if (cmd == "sync") {
                Serial.println("MPCON|ok sync");
                syncScripts(true);
            } else if (cmd == "next" || cmd == "prev") {
                // Through browseScript(), like the buttons: the console is meant
                // to mirror them, and routing it here means the title-browsing
                // behaviour can be exercised over a cable.
                Serial.printf("MPCON|ok %s\n", cmd.c_str());
                browseScript(cmd == "next" ? +1 : -1);
            } else if (cmd == "rtc") {
                int h = 0, m = 0, sec = 0;
                const bool ok = WatchyRTC::now(h, m, sec);
                uint8_t raw = 0; const bool haveRaw = WatchyRTC::statusByte(raw);
                Serial.printf("MPCON|rtc %s %s %02d:%02d:%02d flagreg=%s\n",
                              WatchyRTC::chipName(),
                              WatchyRTC::valid() ? "ok"
                                  : (WatchyRTC::forcedUntrusted() ? "unset(forced)" : "unset"),
                              ok ? h : 0, ok ? m : 0, ok ? sec : 0,
                              haveRaw ? String("0x" + String(raw, 16)).c_str() : "?");
            } else if (cmd == "rtc-unset") {
                // Stages the cold-clock state a battery pull produces, so the
                // "clock is not trustworthy -> go to NTP" branch can be tested
                // on every build instead of once with a screwdriver. Reboot (or
                // just watch the next sync) to see the path run.
                //
                // Does NOT itself re-sync: the point is to leave the watch in
                // the state a fresh one boots into.
                if (WatchyRTC::invalidate()) {
                    Serial.println("MPCON|ok rtc-unset -- clock marked untrusted (NVS override, not the chip's own flag: it is clear-only); reboot to exercise the NTP path");
                } else {
                    Serial.printf("MPCON|rtc-unset FAILED (chip: %s)\n", WatchyRTC::chipName());
                }
            } else if (cmd.startsWith("run ")) {
                int idx = cmd.substring(4).toInt();
                Serial.printf("MPCON|ok run %d\n", idx);
                showScript(idx, true);
            } else {
                Serial.println("MPCON|commands: list | run <index> | next | prev | sync | rtc | rtc-unset");
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

    // Counts toward the de-ghost budget: this is a fast waveform on real pixels
    // and it ghosts like any other. Not routed through beginPanelUpdate()
    // because that would promote an indicator to a full-screen flash when the
    // budget happens to expire on a button press.
    g_updatesSinceFull++;
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

// Centred message frame. Used for render failures, for the "nothing on this
// device yet" state and for sync progress.
//
// A render that fails used to leave whatever the clear produced -- a white
// screen, which is indistinguishable from a dead device. Say what happened and
// which script it was, so a failure is legible rather than alarming.
//
// Body text is size 2 like the title, not size 1: at 6px per glyph the
// explanation was unreadable on a 200x200 panel, which defeats the point of
// showing it. Size 2 is 12px per glyph, so only 16 glyphs fit across -- hence
// the word wrap, rather than one long line running off the edge.
static void drawWrapped(const char* text, int& y, int textSize)
{
    const int W = g_display.width();
    const int glyph = 6 * textSize;
    const int perLine = W / glyph;
    g_display.setTextSize(textSize);

    String rest(text);
    while (rest.length()) {
        String line = rest;
        if ((int)line.length() > perLine) {
            // Break on the last space that fits; hard-cut a single word that
            // is longer than a line.
            int cut = line.lastIndexOf(' ', perLine);
            if (cut <= 0) cut = perLine;
            line = rest.substring(0, cut);
            rest = rest.substring(cut);
            rest.trim();
        } else {
            rest = "";
        }
        const int w = (int)line.length() * glyph;
        g_display.setCursor(w < W ? (W - w) / 2 : 0, y);
        g_display.print(line);
        y += 8 * textSize + 4;
    }
}

static void showNotice(const char* title, const char* line1, const char* line2,
                       const char* hint)
{
    const int W = g_display.width();
    beginPanelUpdate(false);
    g_display.firstPage();
    do {
        g_display.fillScreen(GxEPD_WHITE);
        g_display.setTextColor(GxEPD_BLACK);

        int y = 26;
        g_display.setTextSize(2);
        g_display.setCursor((W - (int)strlen(title) * 12) / 2, y);
        g_display.print(title);
        y += 30;

        if (line1 && *line1) drawWrapped(line1, y, 2);
        if (line2 && *line2) drawWrapped(line2, y, 2);
        if (hint  && *hint)  { y += 8; drawWrapped(hint, y, 2); }
        g_display.setTextSize(1);
    } while (g_display.nextPage());
}

static void showRenderError(const char* scriptName, const char* reason)
{
    showNotice(MP_MSG_RENDER_ERROR, scriptName, reason, "");
}

// Full refresh: drive the panel through black and white to clear accumulated
// ghosting. The top-left gesture pairs it with syncScripts() -- the same
// "re-sync and repaint from scratch" the M5Paper's equivalent gesture performs.
static void fullRefresh()
{
    // Counter goes to ZERO, not to the interval. This function drives the panel
    // black then white, which IS the de-ghost -- the panel is clean when it
    // returns. Setting the counter to the interval told the very next render to
    // do a third full refresh, so one gesture produced black / white / flash,
    // which reads exactly like a failed attempt being retried.
    g_updatesSinceFull = 0;
    g_display.setFullWindow();
    for (int pass = 0; pass < 2; ++pass) {
        g_display.firstPage();
        do { g_display.fillScreen(pass == 0 ? GxEPD_BLACK : GxEPD_WHITE); }
        while (g_display.nextPage());
    }
}

// Sets the RTC from NTP. The M5Paper does this in SystemManager; there is no
// SystemManager on this device, so the same few steps live here.
//
// stopRadio() first for the reason mp_provisioning documents: BLE holds tens of
// KB of internal DRAM and the TLS-capable WiFi stack cannot get its own
// allocation while that is resident.
static bool syncTimeFromNTP()
{
    if (!g_networkManager) return false;

    MPProvisioning::stopRadio();
    if (!g_networkManager->connectWiFi(pdMS_TO_TICKS(15000))) {
        log_w("NTP: no WiFi; leaving the clock as it is");
        return false;
    }

    configTime((long)MP_TZ_OFFSET_HOURS * 3600, 0, "pool.ntp.org");

    struct tm t;
    // 10s: SNTP needs a round trip and often a DNS lookup first.
    const bool got = getLocalTime(&t, 10000);
    g_networkManager->disconnectWiFi();

    if (!got) {
        log_w("NTP: no reply within 10s");
        return false;
    }
    if (!WatchyRTC::set(t)) {
        log_e("NTP: got the time but could not write the RTC (%s)", WatchyRTC::chipName());
        return false;
    }

    log_i("NTP: RTC set to %02d:%02d:%02d (UTC%+d)", t.tm_hour, t.tm_min, t.tm_sec,
          MP_TZ_OFFSET_HOURS);
    Serial.printf("MPCON|ntp %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    return true;
}

// Pulls the script list and every script's content from the API.
//
// The procedure itself is mp_sync_scripts(), shared verbatim with the M5Paper
// (script_sync.cpp) -- this firmware supplies only the progress display. It runs
// synchronously on the Arduino loop task, where the M5Paper hands it to a
// dedicated FetchTask; that firmware has an interactive UI to keep responsive
// during a sync and this one does not.
static void syncScripts(bool announce)
{
    if (!g_networkManager || !g_scriptManager) return;

    if (announce) {
        showNotice(MP_MSG_SYNCING, MP_MSG_CONNECTING, "", "");
    }

    // The clock rides along with the deliberate sync gesture. Both of these
    // parts drift by minutes a month, nothing else on this device ever
    // corrects them, and the network is already being brought up -- so the one
    // moment the user has asked to go online is the cheapest place to do it.
    syncTimeFromNTP();

    logHeap("before sync");
    const ScriptSyncResult r = mp_sync_scripts(*g_networkManager, *g_scriptManager,
                                               /*fullRefresh=*/true, nullptr,
                                               [](const char* stage, void*) {
                                                   log_i("Sync: %s", stage);
                                                   Serial.printf("MPCON|sync %s\n", stage);
                                               });
    logHeap("after sync");
    log_i("Sync finished: %s (ok %d, failed %d, of %d)",
          r.message.c_str(), r.successCount, r.failCount, r.serverCount);

    if (r.status == FetchResultStatus::SUCCESS) {
        loadScriptIndex();
        return;
    }

    // Keep whatever is already on the device -- a failed sync must never be
    // worse than no sync. Only say so when there is nothing to fall back to.
    loadScriptIndex();
    if (g_scripts.empty()) {
        const char* why = (r.status == FetchResultStatus::NO_WIFI)
                              ? MP_MSG_SYNC_NO_WIFI
                              : r.message.c_str();
        showNotice(MP_MSG_SYNC_FAILED, why, "", MP_MSG_RETRY_SYNC);
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

    // PANEL-AS-INSTRUMENT probe. Default OFF; -DMP_PANEL_PROBE=1 re-enables.
    //
    // Draws a diagonal stripe pattern immediately after init(), before any
    // parser, runtime or renderer code can fail or hang. Written when serial
    // was proven useless on this device (the official InkWatchy image runs
    // visibly while emitting zero bytes over 60s), so the panel was the only
    // channel that could report progress.
    //
    //   stripes appear  -> init() returned AND the panel is driven correctly,
    //                      so a later blank screen is a script/render fault
    //   nothing          -> we never got here: init() blocked, or the app is
    //                      not executing at all
    //
    // Kept because that distinction was the hard part of this port and is not
    // reconstructable from a blank screen. Off by default: on a working device
    // it is just a confusing flash and a 2.5s delay at every boot.
#if MP_PANEL_PROBE
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
    Serial.println("MPCON|probe pattern pushed"); Serial.flush();
    delay(2500);   // hold it long enough to be seen before the first script
#endif

    // NVS DIAGNOSTIC ON THE PANEL.
    //
    // The Watchy emits nothing over serial, and the BLE reply path turned out
    // to be truncating long messages -- so both channels used to debug the
    // "write did not persist" failure were themselves unreliable. The panel is
    // the one output on this device that has been proven to work, so the
    // diagnostic goes there, and it runs BEFORE BLE is initialised so a BLE
    // fault cannot hide it.
    //
    // Remove once the NVS problem is understood.
#if MP_NVS_SCREEN
    {
        esp_err_t e = nvs_flash_init();
        bool erased = false;
        if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase(); e = nvs_flash_init(); erased = true;
        }
        // Which flash region does NVS actually believe it owns? If this is not
        // 0x9000 / 0x5000 then NVS is bound somewhere other than the partition
        // table we flash, which would explain writes that vanish.
        const esp_partition_t* part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);

        // Raw NVS round-trip, with NO BLE anywhere in the picture. This is the
        // bisect: if it fails here it is an NVS/flash problem, and if it
        // succeeds here but fails over BLE it is the BLE context.
        nvs_handle_t h; esp_err_t eo = nvs_open("bootdiag", NVS_READWRITE, &h);
        esp_err_t es = ESP_FAIL, ec = ESP_FAIL, eg = ESP_FAIL; uint8_t rv = 0;
        if (eo == ESP_OK) {
            es = nvs_set_u8(h, "k", 42);
            ec = nvs_commit(h);
            eg = nvs_get_u8(h, "k", &rv);
            nvs_close(h);
        }
        nvs_stats_t st; bool haveStats = (nvs_get_stats(NULL, &st) == ESP_OK);

        // Also print it: serial works on this board at 80MHz flash frequency,
        // so the probe no longer depends on somebody photographing the panel.
        Serial.printf("NVSPROBE init=%s erased=%d part=@0x%lx sz=0x%lx open=%s set=%s commit=%s get=%s value=%u used=%u free=%u total=%u\n",
                      esp_err_to_name(e), erased ? 1 : 0,
                      part ? (unsigned long)part->address : 0UL,
                      part ? (unsigned long)part->size : 0UL,
                      esp_err_to_name(eo), esp_err_to_name(es),
                      esp_err_to_name(ec), esp_err_to_name(eg), rv,
                      haveStats ? st.used_entries : 0,
                      haveStats ? st.free_entries : 0,
                      haveStats ? st.total_entries : 0);
        Serial.flush();

        beginPanelUpdate(true);
        g_display.firstPage();
        do {
            g_display.fillScreen(GxEPD_WHITE);
            g_display.setTextColor(GxEPD_BLACK);
            g_display.setTextSize(1);
            int y = 10;
            auto line = [&](const String& t) { g_display.setCursor(4, y); g_display.print(t); y += 12; };
            line("NVS BOOT PROBE (no BLE)");
            line(String("init: ") + esp_err_to_name(e));
            line(String("erased: ") + (erased ? "yes" : "no"));
            if (part) {
                char b[48];
                snprintf(b, sizeof(b), "part @0x%lx sz 0x%lx",
                         (unsigned long)part->address, (unsigned long)part->size);
                line(b);
            } else line("part: NOT FOUND");
            line(String("open: ") + esp_err_to_name(eo));
            line(String("set: ") + esp_err_to_name(es));
            line(String("commit: ") + esp_err_to_name(ec));
            line(String("get: ") + esp_err_to_name(eg));
            line(String("value: ") + rv + " (want 42)");
            if (haveStats) {
                line(String("used: ") + st.used_entries);
                line(String("free: ") + st.free_entries);
                line(String("total: ") + st.total_entries);
            } else {
                line("stats: unavailable");
            }
            line(String("heap: ") + ESP.getFreeHeap());
        } while (g_display.nextPage());
        delay(12000);   // long enough to read and photograph
    }
#endif

    // Loads stored credentials only. NO advertising window at boot -- the
    // window is opened by a real button press, below.
    MPProvisioning::begin();

    // Scripts live in SPIFFS, written by the last sync.
    g_scriptManager = new ScriptManager();
    if (!g_scriptManager->initialize()) {
        log_e("ScriptManager init failed (SPIFFS)");
    }
    g_networkManager = new MPNetworkManager(nullptr);   // no SystemManager on this device
    logHeap("managers up");

    // The clock. Probed after the managers because a first-boot watch needs the
    // network to set it, and before the first render because that render is
    // what reads it.
    if (WatchyRTC::begin()) {
        int h = 0, m = 0, sec = 0;
        const bool ok = WatchyRTC::now(h, m, sec);
        log_i("RTC: %s, %s, %02d:%02d:%02d", WatchyRTC::chipName(),
              WatchyRTC::valid() ? "time trusted" : "NOT SET", h, m, sec);
        Serial.printf("MPCON|rtc %s %s %02d:%02d:%02d\n", WatchyRTC::chipName(),
                      WatchyRTC::valid() ? "ok" : "unset", ok ? h : 0, ok ? m : 0,
                      ok ? sec : 0);

        // Only when the chip says its time is untrustworthy -- a fresh watch or
        // one whose battery was pulled. Boot must not otherwise wait on the
        // network, and the sync gesture keeps drift in check afterwards.
        if (!WatchyRTC::valid()) {
            log_i("RTC not set; going to NTP");
            syncTimeFromNTP();
        }
    } else {
        // Not fatal: scripts still render, they just see midnight.
        log_w("RTC: no chip answered on I2C; $HOUR/$MINUTE/$SECOND will be 0");
        Serial.println("MPCON|rtc none");
    }

    const bool haveLocal = loadScriptIndex();

    // Resolve the saved selection before accepting recovery commands. If the
    // RTC marker still names it, the previous boot died inside that script.
    // Skip forward once; after three consecutive render crashes, stop all
    // automatic rendering and leave the console/buttons alive for recovery.
    if (haveLocal) {
        String currentId;
        if (g_scriptManager->getCurrentScriptId(currentId) && currentId.length()) {
            for (size_t i = 0; i < g_scripts.size(); i++) {
                if (g_scripts[i].humanId == currentId) { g_currentScript = (int)i; break; }
            }
        }

        const bool unfinished = g_renderGuardMagic == RENDER_GUARD_MAGIC &&
            g_renderGuardScriptHash == scriptIdHash(g_scripts[g_currentScript].humanId);
        if (unfinished) {
            const String failedName = g_scripts[g_currentScript].name;
            if (g_renderCrashCount < 3) g_renderCrashCount++;
            disarmRenderGuard(false);
            g_continueRenderCrashStreak = true;

            if (g_renderCrashCount >= 3) {
                g_renderRecoverySafeMode = true;
                Serial.printf("MPCON|recovery safe-mode after %u render crashes; "
                              "last='%s' -- send: run <index>\n",
                              (unsigned)g_renderCrashCount, failedName.c_str());
            } else {
                g_currentScript = (g_currentScript + 1) % (int)g_scripts.size();
                Serial.printf("MPCON|recovery skipped crashed script '%s'; trying '%s'\n",
                              failedName.c_str(), g_scripts[g_currentScript].name.c_str());
            }
        } else if (g_renderGuardMagic == RENDER_GUARD_MAGIC) {
            // The script list or saved selection changed while the marker was
            // armed. It no longer identifies the boot target, so it is stale.
            disarmRenderGuard(true);
        }
    } else if (g_renderGuardMagic == RENDER_GUARD_MAGIC) {
        disarmRenderGuard(true);
    }

    // Recovery window: a saved script can abort before setup reaches loop(),
    // which otherwise makes the serial console impossible to use to select a
    // different one. Commands received here (notably `run <index>`) replace
    // /current_script.id before the normal boot render below.
    Serial.println("MPCON|recovery -- send: run <index>"); Serial.flush();
    const unsigned long recoveryUntil = millis() + 5000;
    while ((long)(millis() - recoveryUntil) < 0) {
        pollSerial();
        delay(10);
    }

    // A successful `run` in the window already rendered and persisted the new
    // selection, so do not render it a second time. In safe mode, do not render
    // anything automatically: the whole point is to keep loop() reachable.
    if (haveLocal) {
        if (g_renderRecoverySafeMode) {
            g_lastRenderMs = millis();
            showNotice("Recovery mode", "Repeated script crashes",
                       "send: run <index>", "serial console is active");
        } else if (g_lastRenderMs == 0) {
            showScript(g_currentScript, true);
        }
    } else {
        // Nothing on the device: a first sync is the only useful thing to do.
        // Deliberately NOT done when scripts already exist -- boot must be fast
        // and must not depend on the network being reachable.
        log_i("No local scripts; syncing from the server");
        syncScripts(true);
        showScript(0, true);
    }

    g_stage = 5;                                  // first script rendered; loop() runs
    Serial.println("MPCON|ready -- commands: list | run <index> | next | prev | sync | rtc | rtc-unset");
}

// Light sleep between renders.
//
// The loop was spinning at 20ms for the whole interval between re-renders,
// which on
// a watch is the difference between hours and days of battery: an ESP32 draws
// roughly 40mA awake and under 1mA in light sleep. The M5Paper has done this
// since the beginning (SystemManager::goToLightSleep); this is the same three
// wake sources, on this device's pins.
//
// Light sleep, not deep sleep, and that is a considered choice rather than a
// stepping stone. Deep sleep costs a full boot on every wake -- SPIFFS mount,
// script load, parse, GxEPD2 re-init -- which measured ~9s here. At the current
// 83s cadence that trades 83s at ~0.8mA (about 66mA-seconds) for 9s at ~100mA
// (about 900), so deep sleep would use more power, not less. It only wins once
// the interval is minutes rather than seconds, which is a product decision
// about how often a time-dependent script should advance.
//
// Sleep is skipped while a provisioning window is open: BLE needs its
// connection events serviced, and the window is only ever 20s.
static void sleepUntilSomethingHappens()
{
    if (MPProvisioning::windowOpen()) { delay(20); return; }
    if (Serial.available() > 0)       { return; }
    if (g_pendingScript >= 0)         { delay(10); return; }   // a render is due

    // Stay awake while someone is at the console.
    //
    // Serial cannot wake this device: esp_sleep_enable_uart_wakeup() only fires
    // if UART0 is clocked from a source that survives light sleep (REF_TICK /
    // XTAL), and Arduino 3.1 clocks it from APB, which is gated -- with no
    // supported way to change it after Serial.begin(). The M5Paper's equivalent
    // works only because Arduino 2.0.4 defaulted to REF_TICK. Worse, bytes sent
    // to a sleeping device are not merely late, they are dropped: the RX FIFO
    // is unclocked, so waking in short chunks and polling does not recover them
    // either. Both were tried on hardware.
    //
    // What does work: opening the port asserts DTR/RTS and resets the board, so
    // a debugger always arrives at a freshly booted device. Stay awake for a
    // window after boot, and re-arm it on every byte received, so a session
    // stays alive as long as it is being used and the watch sleeps the rest of
    // the time.
    if (millis() - g_lastSerialMs < CONSOLE_AWAKE_MS) { delay(20); return; }

    const unsigned long since = millis() - g_lastRenderMs;
    if (since >= AUTO_RERUN_INTERVAL_MS) return;          // due now; do not sleep
    const unsigned long remaining = AUTO_RERUN_INTERVAL_MS - since;

    // A button held down would wake us instantly and forever, so only sleep
    // once all four are released.
    if (digitalRead(BTN_BACK) == HIGH || digitalRead(BTN_MENU) == HIGH ||
        digitalRead(BTN_UP)   == HIGH || digitalRead(BTN_DOWN) == HIGH) {
        delay(20);
        return;
    }

    esp_sleep_enable_timer_wakeup((uint64_t)remaining * 1000ULL);

    // Buttons idle LOW on this board and go HIGH when pressed, the opposite of
    // the M5Paper's.
    gpio_wakeup_enable((gpio_num_t)BTN_BACK, GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN_MENU, GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN_UP,   GPIO_INTR_HIGH_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN_DOWN, GPIO_INTR_HIGH_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    Serial.flush();
    esp_light_sleep_start();

    // millis() is esp_timer-backed and is corrected across light sleep, so the
    // re-render deadline stays honest without any bookkeeping here.
}

void loop()
{
    pollSerial();
    MPProvisioning::tick();
    renderIfSettled();

    // Heartbeat. Without it, a device whose boot output was simply missed looks
    // exactly like a hung one -- which cost real time diagnosing this port. Any
    // capture started at any moment now shows within 5s whether the firmware is
    // alive, and how far it got.
    static unsigned long lastBeat = 0;
    // Every 60s, not 5s: the heartbeat is a diagnostic, and at 5s it would keep
    // waking the device and undo the light sleep below.
    if (millis() - lastBeat > 60000) {
        lastBeat = millis();
        Serial.printf("MPCON|alive t=%lus script=%d/%u heap=%u\n",
                      millis() / 1000, g_currentScript, (unsigned)g_scripts.size(),
                      ESP.getFreeHeap());
        Serial.flush();
    }

    // Periodic re-render, so time-dependent scripts keep advancing on their own.
    // Deliberately checked BEFORE the buttons: if a press arrives during the
    // render the button handler simply sees it on the next pass.
    if (!g_renderRecoverySafeMode &&
        millis() - g_lastRenderMs >= AUTO_RERUN_INTERVAL_MS) {
        log_i("Auto re-render after %lus idle", AUTO_RERUN_INTERVAL_MS / 1000);
        showScript(g_currentScript, false);   // no title: same script
    }

    // --- Buttons ----------------------------------------------------------
    //
    // Mirrors the M5Paper's controls onto the four corner buttons:
    //   top-right    previous script   (M5Paper UP)
    //   bottom-right next script       (M5Paper DOWN)
    //   bottom-left  re-run current    (M5Paper PUSH / confirm)
    //   top-left     hold 5s: sync scripts from the server, then full refresh
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

            // A press means someone is holding the watch, so make it
            // discoverable. Extends rather than stacks: the window always ends
            // 20s after the LAST press. The automatic 83s re-render does not
            // come through here, so the radio never comes up unasked.
            MPProvisioning::openWindow();
            continue;
        }

        if (down && b.wasDown && b.corner == CORNER_TL &&
            (millis() - b.downAt) >= BTN_LONG_PRESS_MS) {
            // Long press fires while still held, then swallows the release.
            log_i("Button: top-left held %dms -> sync from server", BTN_LONG_PRESS_MS);
            drawCornerIndicator(b.corner, false);
            // This gesture means "go get my scripts again", not "repaint the
            // panel" -- the de-ghost is just what a fresh start looks like.
            syncScripts(true);
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
                    browseScript(-1);
                    break;
                case CORNER_BR:
                    log_i("Button: bottom-right -> next script");
                    browseScript(+1);
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

    sleepUntilSomethingHappens();
}

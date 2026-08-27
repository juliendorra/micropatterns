#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <M5EPD.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" // For mutex
// #include "event_defs.h" // No direct dependency found in this header, but if added later, path would need adjustment.

enum ActivityIndicatorType {
    ACTIVITY_PUSH,
    ACTIVITY_UP,
    ACTIVITY_DOWN
};

// LOCKING MODEL -- two levels, deliberately.
//
// There used to be a single mutex ("_epdMutex") guarding both the big script
// framebuffer AND every panel transaction. RenderTask holds that lock for the
// WHOLE render (parse + display-list generation + rasterization + push, 8-10 s),
// so every UI acknowledgement -- the button activity indicator, the script-name
// banner -- timed out against it and never reached the panel. That is exactly
// the "device seems stuck, not responding to the buttons" report: the indicator
// code ran, failed to take the lock, logged an error and returned.
//
//   _canvasMutex  long-held. Guards the 540x960 script framebuffer (_canvas).
//                 RenderTask holds it for the entire render via lockEPD().
//   _panelMutex   short-held. Guards actual EPD hardware transactions and the
//                 small scratch canvas (_indicatorCanvas) used for region
//                 pushes. Never held across a render.
//
// Lock ORDER is always _canvasMutex then _panelMutex. Nothing takes them the
// other way round, so there is no cycle.
//
// The indicator/banner paths take ONLY _panelMutex, and draw ONLY into
// _indicatorCanvas -- never into _canvas. Both properties matter: taking only
// the panel lock is what makes them instant during a render, and staying off
// _canvas is what stops them racing the in-flight rasterizer (and what stops
// the indicator being baked into the script image that gets pushed later).
//
// WAVEFORM POLICY -- see the long comment above pushScriptCanvasLocked() in
// display_manager.cpp for the reasoning and the measurement caveat.
class DisplayManager {
public:
    // The MicroPatterns DSL is effectively 1 bit per pixel: micropatterns_drawing.h
    // defines DRAWING_COLOR_WHITE = 0 and DRAWING_COLOR_BLACK = 15 and no script can
    // ever produce any of the 14 intermediate greys. GC16 (16-grey waveform) is
    // therefore paying for tones that are never on screen.
    //
    // DU: "transitions from any graytone to black or white only", Low ghosting,
    // ~260 ms panel time vs GC16's ~450 ms (M5EPD_Driver.h, from the IT8951
    // waveform documentation). It is exactly the mode this content wants.
    // DU4 (~120 ms, Medium ghosting) is also legal for us -- it supports pixel
    // states [0 10 20 30], and our two values 0 and 15 map to the two endpoints
    // of that set -- and is what the indicator/banner paths already use. It is
    // left as the documented faster/dirtier alternative; flip this constant to
    // UPDATE_MODE_DU4 to try it on device.
    static constexpr m5epd_update_mode_t SCRIPT_FAST_UPDATE_MODE = UPDATE_MODE_DU;

    // Fast 1-bit waveforms do not fully settle the pigment, so residue from
    // previous frames accumulates. Every SCRIPT_DEGHOST_INTERVAL-th *fast* panel
    // update, the next full-canvas script push uses GC16 instead, which repaints
    // the whole panel and clears the accumulated ghost. Indicator and banner
    // pushes count toward the same budget: they are fast waveforms too, and the
    // GC16 that eventually lands is full-screen, so it cleans their regions as
    // well. 8 is a deliberately conservative starting point -- with DU's "Low"
    // ghosting it costs ~450/8 = ~56 ms amortised per render while saving
    // ~190 ms of panel time on each of the other seven.
    static constexpr uint16_t SCRIPT_DEGHOST_INTERVAL = 8;

    DisplayManager();
    ~DisplayManager();

    bool initializeEPD(); // Initializes EPD, sets rotation, etc.

    // Thread-safe methods for UI updates
    void showMessage(const String& text, int y_offset, uint16_t color, bool full_update = false, bool clear_first = false);
    void pushCanvasUpdate(int32_t x, int32_t y, m5epd_update_mode_t mode); // Pass x,y for partial updates
    void clearScreen(uint16_t color = 0); // Default to white

    // Push the main canvas when the CALLER ALREADY HOLDS the canvas lock
    // (RenderTask). Takes only the panel lock, so it cannot self-deadlock the
    // way pushCanvasUpdate() would from inside a lockEPD() section.
    void pushMainCanvasLocked(m5epd_update_mode_t mode, int32_t x = 0, int32_t y = 0);

    // Push a FINISHED SCRIPT RENDER. Same locking contract as
    // pushMainCanvasLocked() (caller already holds the canvas lock), but picks
    // the waveform itself according to the de-ghosting policy above instead of
    // hardcoding GC16. This is the RenderTask path.
    void pushScriptCanvasLocked();

    // True if BOTH locks are free right now. MainControlTask uses this before
    // light sleep; with a single mutex the old lockEPD(5ms) probe covered the
    // panel too, and splitting the lock would otherwise have let the device
    // sleep with a panel transaction in flight.
    bool isDisplayIdle(TickType_t timeout);

    // Draw one line of text as a narrow full-width band, straight to the panel,
    // without touching the script framebuffer. Works during a render.
    void showBanner(const String& text, int y_offset, uint16_t color);

    // Provides direct access to the canvas for complex drawing (e.g., by RenderController)
    // Access to this canvas MUST be synchronized externally: hold lockEPD().
    M5EPD_Canvas* getCanvas();

    // Utility
    int getWidth();
    int getHeight();

    // Canvas-lock control for external raw canvas access (RenderTask).
    bool lockEPD(TickType_t timeout = portMAX_DELAY);
    void unlockEPD();

    // Methods for specific UI indicators. These take only the short panel lock
    // and are safe to call while a render is in flight.
    void drawStartupIndicator();
    void drawActivityIndicator(ActivityIndicatorType type = ACTIVITY_PUSH);

private:
    M5EPD_Canvas _canvas;
    M5EPD_Canvas _indicatorCanvas; // Scratch canvas for region pushes (indicators, banners)
    SemaphoreHandle_t _canvasMutex; // Long-held: guards _canvas
    SemaphoreHandle_t _panelMutex;  // Short-held: guards EPD transactions + _indicatorCanvas

    bool _isInitialized;
    // Cached geometry so indicator/banner paths can compute positions without
    // touching _canvas (which may be locked by an in-flight render).
    int _canvasW;
    int _canvasH;

    // Number of fast (non-GC16) panel updates issued since the last full-panel
    // GC16. Only ever touched while holding _panelMutex. Seeded to the interval
    // so the FIRST script push after boot is a GC16 -- we do not know what the
    // panel was showing before (the boot path deliberately does not Clear()).
    uint16_t _fastUpdatesSinceRefresh;

    // Counter maintenance. Both require _panelMutex held.
    void noteFastUpdateLocked();
    void noteFullRefreshLocked();

    // Draws `text` into _indicatorCanvas and pushes it. Caller MUST hold _panelMutex.
    void drawBannerLocked(const String& text, int y_offset, uint16_t color);
};

#endif // DISPLAY_MANAGER_H

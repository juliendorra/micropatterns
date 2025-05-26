#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Watchy.h>        // For Watchy specific functionalities
#include <GxEPD2_BW.h>     // For GxEPD2 display class
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" // For mutex
// #include "event_defs.h" // No direct dependency found in this header, but if added later, path would need adjustment.

enum ActivityIndicatorType {
    ACTIVITY_PUSH,
    ACTIVITY_UP,
    ACTIVITY_DOWN
};

class DisplayManager {
public:
    DisplayManager();
    ~DisplayManager();

    bool initializeEPD(); // Initializes EPD, sets rotation, etc. (Watchy: initGxEPD)

    // Thread-safe methods for UI updates
    // Watchy uses GxEPD_BLACK, GxEPD_WHITE. full_update concept maps to display.display(bool partial)
    void showMessage(const String& text, int y_offset, uint16_t color, bool full_update = false, bool clear_first = false);
    // void pushCanvasUpdate(int32_t x, int32_t y, m5epd_update_mode_t mode); // Pass x,y for partial updates - Replace with display.displayWindow or display(true)
    void pushCanvasUpdate(bool partial = true); // Simplified for Watchy
    void clearScreen(uint16_t color = GxEPD_WHITE); // Default to white

    // Provides direct access to the canvas for complex drawing (e.g., by RenderController)
    // Access to this canvas MUST be synchronized externally if used by multiple tasks concurrently
    // OR DisplayManager methods that use it must be internally synchronized.
    // This method itself is safe, but using the canvas is not.
    // M5EPD_Canvas* getCanvas(); // Original
    GxEPD2_GFX* getCanvas();   // Changed for Watchy - Watchy::display is a GxEPD2_GFX derivative

    // Utility
    int getWidth(); // Watchy: display.width()
    int getHeight();

    // Mutex control for external raw canvas access
    bool lockEPD(TickType_t timeout = portMAX_DELAY);
    void unlockEPD();

    // Methods for specific UI indicators
    void drawStartupIndicator();
    void drawActivityIndicator(ActivityIndicatorType type = ACTIVITY_PUSH); // Needs Watchy adaptation

private:
    // M5EPD_Canvas _canvas; // Original
    // M5EPD_Canvas _indicatorCanvas; // Canvas for temporary indicators (startup, activity) - To be removed or re-implemented
    SemaphoreHandle_t _epdMutex; // Mutex to protect EPD hardware access and Watchy::display object

    bool _isInitialized;

    // _drawTextInternal removed, logic moved to showMessage
};

#endif // DISPLAY_MANAGER_H

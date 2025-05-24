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

class DisplayManager {
public:
    DisplayManager();
    ~DisplayManager();

    bool initializeEPD(); // Initializes EPD, sets rotation, etc.

    // Thread-safe methods for UI updates
    void showMessage(const String& text, int y_offset, uint16_t color, bool full_update = false, bool clear_first = false);
    void pushCanvasUpdate(int32_t x, int32_t y, m5epd_update_mode_t mode); // Pass x,y for partial updates
    void clearScreen(uint16_t color = 0); // Default to white

    // Provides direct access to the canvas for complex drawing (e.g., by RenderController)
    // Access to this canvas MUST be synchronized externally if used by multiple tasks concurrently
    // OR DisplayManager methods that use it must be internally synchronized.
    // This method itself is safe, but using the canvas is not.
    M5EPD_Canvas* getCanvas();

    // Utility
    int getWidth();
    int getHeight();

    // Mutex control for external raw canvas access
    bool lockEPD(TickType_t timeout = portMAX_DELAY);
    void unlockEPD();

    // Methods for specific UI indicators
    void drawStartupIndicator();
    void drawActivityIndicator(ActivityIndicatorType type = ACTIVITY_PUSH);

private:
    M5EPD_Canvas _canvas;
    M5EPD_Canvas _indicatorCanvas; // Canvas for temporary indicators (startup, activity)
    SemaphoreHandle_t _epdMutex; // Mutex to protect EPD hardware access and canvas object

    bool _isInitialized;

    // _drawTextInternal removed, logic moved to showMessage
};

#endif // DISPLAY_MANAGER_H
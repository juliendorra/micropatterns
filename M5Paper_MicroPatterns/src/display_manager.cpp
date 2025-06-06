#include "display_manager.h"
#include "esp32-hal-log.h"

DisplayManager::DisplayManager() : _canvas(&M5.EPD), _indicatorCanvas(&M5.EPD), _isInitialized(false)
{
    _epdMutex = xSemaphoreCreateMutex();
    if (_epdMutex == NULL)
    {
        log_e("DisplayManager: Failed to create EPD mutex!");
    }
}

DisplayManager::~DisplayManager()
{
    if (_epdMutex != NULL)
    {
        vSemaphoreDelete(_epdMutex);
    }
    // Note: _canvas is a member object, its destructor will be called.
    // If it were a pointer, we might delete it here.
}

bool DisplayManager::initializeEPD()
{
    if (_isInitialized)
        return true;

    if (xSemaphoreTake(_epdMutex, portMAX_DELAY) == pdTRUE)
    {
        // M5.enableEPDPower(); // Ensure EPD power is on, might be done in main system init
        // delay(100);

        // EPD begin (pins are usually fixed for M5Paper, can be passed if configurable)
        // M5.EPD.begin(M5EPD_SCK_PIN, M5EPD_MOSI_PIN, M5EPD_MISO_PIN, M5EPD_CS_PIN, M5EPD_BUSY_PIN);
        // Simpler: M5.EPD.begin(); if default pins are okay.
        // The M5EPD library handles this internally.
        // We just need to ensure M5.EPD object is valid.

        M5.EPD.SetRotation(M5EPD_Driver::ROTATE_90);
        // M5.EPD.Clear(true); // Initial full clear - REMOVED to preserve screen content

        // Create the canvas frame buffer
        // Canvas dimensions should match rotated screen
        if (!_canvas.createCanvas(540, 960))
        { // M5Paper 540x960
            log_e("DisplayManager: Failed to create canvas framebuffer!");
            xSemaphoreGive(_epdMutex);
            return false;
        }
        _canvas.setTextSize(3);         // Default text size
        _canvas.setTextColor(15);       // Default to black
        _canvas.setTextDatum(TC_DATUM); // Top-center for drawString

        _isInitialized = true;
        log_i("DisplayManager initialized EPD and Canvas (%d x %d).", _canvas.width(), _canvas.height());

        xSemaphoreGive(_epdMutex);
        return true;
    }
    log_e("DisplayManager::initializeEPD failed to take mutex.");
    return false;
}

// _drawTextInternal removed, logic incorporated into showMessage.

void DisplayManager::showMessage(const String &text, int y_offset, uint16_t color, bool full_update, bool clear_first)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot show message.");
        return;
    }
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    { // Wait up to 500ms
        if (clear_first)
        {
            _canvas.fillCanvas(0); // White
        }
        else
        {
            // Draw a small white background rectangle behind the text
            // Ensure text properties are set as expected by getTextBounds and drawString
            // Default text size 3 and TC_DATUM are set in initializeEPD.
            // If other parts of the code change these, they might need to be reset here.
            // _canvas.setTextDatum(TC_DATUM) is set in initializeEPD.
            // _canvas.setTextSize(3) is set in initializeEPD.

            uint16_t text_w = _canvas.textWidth(text);
            uint16_t text_h = _canvas.fontHeight(); // Using current font height

            // Calculate top-left (x1, y1) of the text based on TC_DATUM
            // For TC_DATUM, drawString(text, x_center, y_top)
            // So, x1 = x_center - (text_w / 2)
            // And y1 = y_top
            int16_t x1 = (_canvas.width() / 2) - (text_w / 2);
            int16_t y1 = y_offset;

            const int16_t padding = 5; // Padding around the text for the background
            // Draw the white rectangle.
            _canvas.fillRect(x1 - padding, y1 - padding, text_w + (2 * padding), text_h + (2 * padding), 0 /* WHITE */);
        }

        // Draw the actual text
        _canvas.setTextColor(color); // Set text color
        // _canvas.setTextDatum(TC_DATUM); // Ensure datum is correct if changed elsewhere
        _canvas.drawString(text, _canvas.width() / 2, y_offset);
        log_i("DisplayManager: Drawing message: \"%s\"", text.c_str());

        _canvas.pushCanvas(0, 0, full_update ? UPDATE_MODE_GC16 : UPDATE_MODE_DU4);

        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::showMessage failed to take mutex for: %s", text.c_str());
    }
}

void DisplayManager::pushCanvasUpdate(int32_t x, int32_t y, m5epd_update_mode_t mode)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot push canvas update.");
        return;
    }
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        _canvas.pushCanvas(x, y, mode);
        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::pushCanvasUpdate failed to take mutex.");
    }
}

void DisplayManager::clearScreen(uint16_t color)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot clear screen.");
        return;
    }
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        _canvas.fillCanvas(color);
        // Typically, a clearScreen implies a full update.
        _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::clearScreen failed to take mutex.");
        // Mutex was not taken, so no need to give it back.
    }
}

void DisplayManager::drawStartupIndicator()
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot draw startup indicator.");
        return;
    }
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(20)) == pdTRUE) // Reduced timeout
    {
        // Define indicator properties
        const int32_t indicator_width = 384;
        const int32_t indicator_height = 64;
        const int32_t outline_thickness = 12;

        // 1. Define region for the canvas push (this is the visible part of the indicator)
        int32_t region_w = indicator_width;
        int32_t region_h = indicator_height;
        int32_t region_screen_x = _canvas.width() / 2 - region_w / 2;
        int32_t region_screen_y = 0; // Top edge

        // 2. Draw on main _canvas for consistency (used as fallback)
        // Outer black rectangle
        _canvas.fillRect(region_screen_x, region_screen_y, region_w, region_h, 15); // BLACK
        // Inner white rectangle (flush with top edge)
        _canvas.fillRect(region_screen_x + outline_thickness,
                         region_screen_y, // Flush with top
                         region_w - (2 * outline_thickness),
                         region_h - outline_thickness,
                         0); // WHITE

        // 3. Create a temporary canvas for the indicator region and push it for partial update
        // M5EPD_Canvas tempIndicatorCanvas(&M5.EPD); // Replaced with member _indicatorCanvas
        if (_indicatorCanvas.createCanvas(region_w, region_h))
        {
            // Draw on the temporary canvas (coordinates relative to this small canvas)
            // Outer black rectangle
            _indicatorCanvas.fillRect(0, 0, region_w, region_h, 15); // BLACK
            // Inner white rectangle (flush with top edge of temp canvas)
            _indicatorCanvas.fillRect(outline_thickness,
                                      0, // Flush with top
                                      region_w - (2 * outline_thickness),
                                      region_h - outline_thickness,
                                      0); // WHITE
            _indicatorCanvas.pushCanvas(region_screen_x, region_screen_y, UPDATE_MODE_GC16);
            _indicatorCanvas.deleteCanvas();
            log_i("DisplayManager: Drew startup indicator rectangle using temporary canvas for partial update.");
        }
        else
        {
            log_e("DisplayManager: Failed to create temporary canvas for startup indicator rectangle. Pushing full canvas.");
            // Fallback: push the main canvas (which has the indicator drawn on it)
            _canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        }

        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::drawStartupIndicator failed to take mutex.");
    }
}

void DisplayManager::drawActivityIndicator(ActivityIndicatorType type)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot draw activity indicator.");
        return;
    }
    // Increased timeout from 20ms to 100ms
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        // Define indicator properties
        const int32_t indicator_visible_width = 64; // Width of the rectangle on screen
        const int32_t indicator_height = 256;       // Total height of the rectangle
        const int32_t outline_thickness = 12;
        log_d("drawActivityIndicator: Type: %d", type);
        log_d("  Properties: vis_width=%d, height=%d, outline=%d", indicator_visible_width, indicator_height, outline_thickness);

        // Position calculation variables
        const int32_t screen_width = _canvas.width();
        const int32_t screen_height = _canvas.height();
        const int32_t screen_center_y = screen_height / 2;
        const int32_t half_indicator_height = indicator_height / 2;
        log_d("  Screen: width=%d, height=%d, center_y=%d", screen_width, screen_height, screen_center_y);
        log_d("  Indicator derived: half_height=%d", half_indicator_height);

        // Step 1: Determine the top Y of an indicator if it were centered on the screen.
        // Its center would be screen_center_y with an offset to account
        // for the button centered based on the case, not the screen

        const int32_t centered_indicator_top_y = screen_center_y - half_indicator_height + 25;

        log_d("  Calculated: centered_indicator_top_y=%d (screen_center_y %d - half_indicator_height %d)", centered_indicator_top_y, screen_center_y, half_indicator_height);

        // Step 2: Define the "PUSH" position.
        const int32_t push_case_top_y = centered_indicator_top_y;

        log_d("  Calculated: push_case_top_y=%d (centered_indicator_top_y %d - half_indicator_height %d)", push_case_top_y, centered_indicator_top_y, 0);

        int32_t current_indicator_top_y;
        switch (type)
        {
        case ACTIVITY_PUSH:
            // PUSH is at push_case_top_y.
            current_indicator_top_y = push_case_top_y;
            log_d("  Activity Type PUSH: current_indicator_top_y=%d", current_indicator_top_y);
            break;
        case ACTIVITY_UP:
            // Requirement: "UP, the rectangle should be higher than PUSH by the size of the rectangle".
            // So, UP_top_Y = push_case_top_y - indicator_height.
            current_indicator_top_y = push_case_top_y - indicator_height;
            log_d("  Activity Type UP: current_indicator_top_y=%d (push_case_top_y %d - indicator_height %d)", current_indicator_top_y, push_case_top_y, indicator_height);
            break;
        case ACTIVITY_DOWN:
            // Requirement: "DOWN, the rectangle should lower than PUSH by the size of the rectangle".
            // So, DOWN_top_Y = push_case_top_y + indicator_height.
            current_indicator_top_y = push_case_top_y + indicator_height;
            log_d("  Activity Type DOWN: current_indicator_top_y=%d (push_case_top_y %d + indicator_height %d)", current_indicator_top_y, push_case_top_y, indicator_height);
            break;
        default:
            // Fallback to PUSH position if type is somehow invalid
            log_w("DisplayManager: Unknown ActivityIndicatorType %d, defaulting to PUSH position.", type);
            current_indicator_top_y = push_case_top_y;
            log_d("  Activity Type UNKNOWN (default PUSH): current_indicator_top_y=%d", current_indicator_top_y);
            break;
        }

        // 1. Define region for the canvas push (this is the visible part of the indicator)
        // The region uses the full indicator_height for drawing.
        int32_t region_w = indicator_visible_width;
        int32_t region_h = indicator_height;
        int32_t region_screen_x = screen_width - region_w; // Align to right screen edge
        int32_t region_screen_y = current_indicator_top_y; // Use the calculated top Y for the current type
        log_d("  Final Region: w=%d, h=%d, screen_x=%d, screen_y=%d", region_w, region_h, region_screen_x, region_screen_y);

        // 2. Draw on main _canvas for consistency (used as fallback)
        // Outer black rectangle part
        _canvas.fillRect(region_screen_x, region_screen_y, region_w, region_h, 15); // BLACK
        // Inner white rectangle part (flush with right screen edge, black border on LEFT)
        _canvas.fillRect(region_screen_x + outline_thickness, // Black border on LEFT
                         region_screen_y + outline_thickness,
                         region_w - outline_thickness, // Width of white area, leaves black on right edge of main rect
                         region_h - (2 * outline_thickness),
                         0); // WHITE

        // 3. Create a temporary canvas for the indicator region and push it for partial update
        // M5EPD_Canvas tempIndicatorCanvas(&M5.EPD); // Replaced with member _indicatorCanvas
        if (_indicatorCanvas.createCanvas(region_w, region_h))
        {
            // Draw on the temporary canvas (coordinates relative to this small canvas, so top-left is 0,0)
            // Outer black rectangle part
            _indicatorCanvas.fillRect(0, 0, region_w, region_h, 15); // BLACK
            // Inner white rectangle part (black border on LEFT side of this temp canvas)
            _indicatorCanvas.fillRect(outline_thickness, // Black border on LEFT
                                      outline_thickness,
                                      region_w - outline_thickness, // Width of white area
                                      region_h - (2 * outline_thickness),
                                      0); // WHITE
            _indicatorCanvas.pushCanvas(region_screen_x, region_screen_y, UPDATE_MODE_DU4);
            _indicatorCanvas.deleteCanvas();
            log_i("DisplayManager: Drew activity indicator rectangle (type %d at Y:%d) using temporary canvas for partial update.", type, region_screen_y);
        }
        else
        {
            log_e("DisplayManager: Failed to create temporary canvas for activity indicator rectangle. Pushing full canvas.");
            // Fallback: push the main canvas (which has the indicator drawn on it)
            _canvas.pushCanvas(0, 0, UPDATE_MODE_DU4);
        }

        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::drawActivityIndicator failed to take mutex.");
    }
}

M5EPD_Canvas *DisplayManager::getCanvas()
{
    // This method itself doesn't need mutex if it just returns the pointer.
    // The CALLER must ensure thread safety when using the canvas.
    // If DisplayManager methods are the *only* way to draw, then they handle mutex.
    // If RenderController draws directly, it must coordinate or DisplayManager needs
    // beginDraw/endDraw methods that take/give the mutex.
    // For now, return pointer. RenderController will need to be careful or use a DisplayManager-provided mutex.
    // A safer approach: RenderController asks DisplayManager to draw things, or DisplayManager
    // provides lock/unlock methods.
    // Let's assume RenderController will use this canvas within a locked section.
    // The RenderTask will call DisplayManager::lockCanvas(), then RenderController uses getCanvas(), then RenderTask calls DisplayManager::unlockCanvas().
    // This requires lock/unlock methods.
    // For now, just return it. The RenderTask will own the DisplayManager calls for rendering.
    return &_canvas;
}

int DisplayManager::getWidth()
{
    return _isInitialized ? _canvas.width() : 0;
}

int DisplayManager::getHeight()
{
    return _isInitialized ? _canvas.height() : 0;
}

bool DisplayManager::lockEPD(TickType_t timeout)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot lock EPD.");
        return false;
    }
    if (_epdMutex == NULL)
    {
        log_e("DisplayManager EPD mutex is NULL, cannot lock.");
        return false;
    }
    if (xSemaphoreTake(_epdMutex, timeout) == pdTRUE)
    {
        return true;
    }
    log_e("DisplayManager::lockEPD failed to take mutex.");
    return false;
}

void DisplayManager::unlockEPD()
{
    if (!_isInitialized)
    {
        // log_w("DisplayManager not initialized, cannot unlock EPD (but proceeding).");
        // Allow unlock even if not initialized, in case mutex was somehow taken.
    }
    if (_epdMutex == NULL)
    {
        log_e("DisplayManager EPD mutex is NULL, cannot unlock.");
        return;
    }
    xSemaphoreGive(_epdMutex);
}
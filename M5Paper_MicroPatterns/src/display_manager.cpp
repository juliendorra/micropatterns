#include "display_manager.h"
#include "esp32-hal-log.h"

DisplayManager::DisplayManager() : _canvas(&M5.EPD), _isInitialized(false)
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
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        // Define indicator properties
        int32_t main_canvas_centerX = _canvas.width() / 2;
        int32_t main_canvas_centerY = 0;         // On the top edge
        int32_t outer_radius = 64;               // Diameter 128px
        int32_t inner_radius = outer_radius - 8; // 8px thick outline

        // 1. Draw on main _canvas for consistency
        _canvas.fillCircle(main_canvas_centerX, main_canvas_centerY, outer_radius, 15); // BLACK (outline)
        _canvas.fillCircle(main_canvas_centerX, main_canvas_centerY, inner_radius, 0);  // WHITE (inside)

        // 2. Define region for the small canvas push
        int32_t region_w = 2 * outer_radius;
        int32_t region_h = outer_radius; // Height is radius for a half-circle at the top
        int32_t region_screen_x = main_canvas_centerX - outer_radius;
        int32_t region_screen_y = 0; // Top edge

        // 3. Create a temporary canvas for the indicator region and push it
        M5EPD_Canvas indicatorCanvas(&M5.EPD);
        if (indicatorCanvas.createCanvas(region_w, region_h))
        {
            // Draw on the temporary canvas (coordinates relative to this small canvas)
            indicatorCanvas.fillCircle(region_w / 2, 0, outer_radius, 15); // BLACK outline
            indicatorCanvas.fillCircle(region_w / 2, 0, inner_radius, 0);  // WHITE inside
            indicatorCanvas.pushCanvas(region_screen_x, region_screen_y, UPDATE_MODE_DU4);
            indicatorCanvas.deleteCanvas();
            log_i("DisplayManager: Drew startup indicator using temporary canvas for partial update.");
        }
        else
        {
            log_e("DisplayManager: Failed to create temporary canvas for startup indicator. Pushing full canvas.");
            // Fallback: push the main canvas (which has the indicator drawn on it)
            _canvas.pushCanvas(0, 0, UPDATE_MODE_DU4);
        }

        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::drawStartupIndicator failed to take mutex.");
    }
}

void DisplayManager::drawActivityIndicator()
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot draw activity indicator.");
        return;
    }
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        // Define indicator properties
        // Full circle center for drawing on main_canvas (pops out from right edge)
        int32_t circle_center_x_on_main_canvas = _canvas.width();
        int32_t circle_center_y_on_main_canvas = _canvas.height() / 2;
        int32_t outer_radius = 64;               // Diameter 128px
        int32_t inner_radius = outer_radius - 8; // 8px thick outline

        // 1. Draw directly on main _canvas for consistency
        // This draws the full circle, half of which is off-screen to the right.
        _canvas.fillCircle(circle_center_x_on_main_canvas, circle_center_y_on_main_canvas, outer_radius, 15); // BLACK (outline)
        _canvas.fillCircle(circle_center_x_on_main_canvas, circle_center_y_on_main_canvas, inner_radius, 0);  // WHITE (inside)

        // 2. Define the region on screen that shows the visible half of the circle
        int32_t region_screen_x = _canvas.width() - outer_radius;                // Left edge of the visible part
        int32_t region_screen_y = circle_center_y_on_main_canvas - outer_radius; // Top edge of the visible part
        int32_t region_w = outer_radius;                                         // Width of the visible part is one radius
        int32_t region_h = 2 * outer_radius;                                     // Height of the visible part is diameter

        // 3. Create a temporary canvas for the indicator region and push it
        M5EPD_Canvas indicatorCanvas(&M5.EPD);
        if (indicatorCanvas.createCanvas(region_w, region_h))
        {
            // Draw on the temporary canvas (coordinates relative to this small canvas)
            // For a circle emerging from the right, its center on the small canvas is (region_w, region_h / 2)
            indicatorCanvas.fillCircle(region_w, region_h / 2, outer_radius, 15); // BLACK outline
            indicatorCanvas.fillCircle(region_w, region_h / 2, inner_radius, 0);  // WHITE inside
            indicatorCanvas.pushCanvas(region_screen_x, region_screen_y, UPDATE_MODE_DU4);
            indicatorCanvas.deleteCanvas();
            log_i("DisplayManager: Drew activity indicator using temporary canvas for partial update.");
        }
        else
        {
            log_e("DisplayManager: Failed to create temporary canvas for activity indicator. Pushing full canvas.");
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
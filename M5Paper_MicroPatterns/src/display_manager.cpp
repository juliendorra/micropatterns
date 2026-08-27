#include "display_manager.h"
#include "esp32-hal-log.h"

DisplayManager::DisplayManager() : _canvas(&M5.EPD), _indicatorCanvas(&M5.EPD), _isInitialized(false),
                                   _canvasW(540), _canvasH(960)
{
    _canvasMutex = xSemaphoreCreateMutex();
    _panelMutex = xSemaphoreCreateMutex();
    if (_canvasMutex == NULL || _panelMutex == NULL)
    {
        log_e("DisplayManager: Failed to create EPD mutexes!");
    }
}

DisplayManager::~DisplayManager()
{
    if (_canvasMutex != NULL)
    {
        vSemaphoreDelete(_canvasMutex);
    }
    if (_panelMutex != NULL)
    {
        vSemaphoreDelete(_panelMutex);
    }
}

bool DisplayManager::initializeEPD()
{
    if (_isInitialized)
        return true;

    if (xSemaphoreTake(_canvasMutex, portMAX_DELAY) == pdTRUE)
    {
        M5.EPD.SetRotation(M5EPD_Driver::ROTATE_90);
        // M5.EPD.Clear(true); // Initial full clear - REMOVED to preserve screen content

        // Create the canvas frame buffer
        if (!_canvas.createCanvas(540, 960))
        { // M5Paper 540x960
            log_e("DisplayManager: Failed to create canvas framebuffer!");
            xSemaphoreGive(_canvasMutex);
            return false;
        }
        _canvas.setTextSize(3);         // Default text size
        _canvas.setTextColor(15);       // Default to black
        _canvas.setTextDatum(TC_DATUM); // Top-center for drawString

        _canvasW = _canvas.width();
        _canvasH = _canvas.height();

        _isInitialized = true;
        log_i("DisplayManager initialized EPD and Canvas (%d x %d).", _canvasW, _canvasH);

        xSemaphoreGive(_canvasMutex);
        return true;
    }
    log_e("DisplayManager::initializeEPD failed to take mutex.");
    return false;
}

// --- Banner: text straight to the panel, no script framebuffer involved ------
//
// Caller MUST hold _panelMutex. Draws one line of text into the scratch canvas
// as a full-width band and pushes only that band. Used both as showMessage()'s
// fallback while a render holds the canvas lock, and directly by callers that
// must not disturb the script framebuffer.
void DisplayManager::drawBannerLocked(const String &text, int y_offset, uint16_t color)
{
    const int16_t padding = 5;

    _indicatorCanvas.setTextSize(3);
    _indicatorCanvas.setTextDatum(TC_DATUM);

    int32_t band_h = _indicatorCanvas.fontHeight() + (2 * padding);
    // Defensive clamp: fontHeight() is read before the scratch buffer exists.
    // The project loads no custom fonts (default GLCD, 8 px * textSize 3 = 24),
    // but a zero or absurd value here would mean a failed createCanvas.
    if (band_h < 24 + (2 * padding)) band_h = 24 + (2 * padding);
    if (band_h > 120) band_h = 120;
    int32_t band_w = _canvasW;
    int32_t band_y = y_offset - padding;
    if (band_y < 0) band_y = 0;
    if (band_y + band_h > _canvasH) band_y = _canvasH - band_h;
    if (band_y < 0) return;

    if (!_indicatorCanvas.createCanvas(band_w, band_h))
    {
        log_e("DisplayManager: Failed to create banner canvas for \"%s\".", text.c_str());
        return;
    }
    _indicatorCanvas.setTextSize(3);
    _indicatorCanvas.setTextDatum(TC_DATUM);
    _indicatorCanvas.fillCanvas(0); // WHITE background behind the text
    _indicatorCanvas.setTextColor(color);
    _indicatorCanvas.drawString(text, band_w / 2, padding);
    // DU4 is the fast waveform; a banner is transient text, it does not need GC16.
    _indicatorCanvas.pushCanvas(0, band_y, UPDATE_MODE_DU4);
    _indicatorCanvas.deleteCanvas();
    log_i("DisplayManager: Banner \"%s\" pushed at y=%d.", text.c_str(), (int)band_y);
}

void DisplayManager::showBanner(const String &text, int y_offset, uint16_t color)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot show banner.");
        return;
    }
    if (xSemaphoreTake(_panelMutex, pdMS_TO_TICKS(1500)) == pdTRUE)
    {
        drawBannerLocked(text, y_offset, color);
        xSemaphoreGive(_panelMutex);
    }
    else
    {
        log_e("DisplayManager::showBanner failed to take panel mutex for: %s", text.c_str());
    }
}

void DisplayManager::showMessage(const String &text, int y_offset, uint16_t color, bool full_update, bool clear_first)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot show message.");
        return;
    }
    // Try the full-canvas path first. If a render owns the canvas, fall back to
    // a band pushed straight to the panel instead of dropping the message on the
    // floor -- silently losing it is what made script switches look dead.
    if (xSemaphoreTake(_canvasMutex, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        log_i("DisplayManager: canvas busy (render in flight); showing \"%s\" as a banner.", text.c_str());
        showBanner(text, y_offset, color);
        return;
    }

    if (clear_first)
    {
        _canvas.fillCanvas(0); // White
    }
    else
    {
        // Draw a small white background rectangle behind the text.
        uint16_t text_w = _canvas.textWidth(text);
        uint16_t text_h = _canvas.fontHeight(); // Using current font height

        // For TC_DATUM, drawString(text, x_center, y_top)
        int16_t x1 = (_canvasW / 2) - (text_w / 2);
        int16_t y1 = y_offset;

        const int16_t padding = 5; // Padding around the text for the background
        _canvas.fillRect(x1 - padding, y1 - padding, text_w + (2 * padding), text_h + (2 * padding), 0 /* WHITE */);
    }

    // Draw the actual text
    _canvas.setTextColor(color); // Set text color
    _canvas.drawString(text, _canvasW / 2, y_offset);
    log_i("DisplayManager: Drawing message: \"%s\"", text.c_str());

    if (xSemaphoreTake(_panelMutex, portMAX_DELAY) == pdTRUE)
    {
        _canvas.pushCanvas(0, 0, full_update ? UPDATE_MODE_GC16 : UPDATE_MODE_DU4);
        xSemaphoreGive(_panelMutex);
    }

    xSemaphoreGive(_canvasMutex);
}

void DisplayManager::pushCanvasUpdate(int32_t x, int32_t y, m5epd_update_mode_t mode)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot push canvas update.");
        return;
    }
    if (xSemaphoreTake(_canvasMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        pushMainCanvasLocked(mode, x, y);
        xSemaphoreGive(_canvasMutex);
    }
    else
    {
        log_e("DisplayManager::pushCanvasUpdate failed to take canvas mutex.");
    }
}

void DisplayManager::pushMainCanvasLocked(m5epd_update_mode_t mode, int32_t x, int32_t y)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot push canvas.");
        return;
    }
    // Caller holds _canvasMutex. Only the panel transaction needs serializing
    // against indicator/banner pushes, and that is a short wait.
    if (xSemaphoreTake(_panelMutex, portMAX_DELAY) == pdTRUE)
    {
        _canvas.pushCanvas(x, y, mode);
        xSemaphoreGive(_panelMutex);
    }
}

bool DisplayManager::isDisplayIdle(TickType_t timeout)
{
    if (!_isInitialized || _canvasMutex == NULL || _panelMutex == NULL)
    {
        return false;
    }
    if (xSemaphoreTake(_canvasMutex, timeout) != pdTRUE)
    {
        return false;
    }
    bool panelFree = (xSemaphoreTake(_panelMutex, timeout) == pdTRUE);
    if (panelFree)
    {
        xSemaphoreGive(_panelMutex);
    }
    xSemaphoreGive(_canvasMutex);
    return panelFree;
}

void DisplayManager::clearScreen(uint16_t color)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot clear screen.");
        return;
    }
    if (xSemaphoreTake(_canvasMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        _canvas.fillCanvas(color);
        pushMainCanvasLocked(UPDATE_MODE_GC16);
        xSemaphoreGive(_canvasMutex);
    }
    else
    {
        log_e("DisplayManager::clearScreen failed to take canvas mutex.");
    }
}

void DisplayManager::drawStartupIndicator()
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot draw startup indicator.");
        return;
    }
    // Panel lock only -- see the LOCKING MODEL note in display_manager.h.
    if (xSemaphoreTake(_panelMutex, pdMS_TO_TICKS(1500)) == pdTRUE)
    {
        const int32_t indicator_width = 384;
        const int32_t indicator_height = 64;
        const int32_t outline_thickness = 12;

        int32_t region_w = indicator_width;
        int32_t region_h = indicator_height;
        int32_t region_screen_x = _canvasW / 2 - region_w / 2;
        int32_t region_screen_y = 0; // Top edge

        if (_indicatorCanvas.createCanvas(region_w, region_h))
        {
            // Outer black rectangle
            _indicatorCanvas.fillRect(0, 0, region_w, region_h, 15); // BLACK
            // Inner white rectangle (flush with top edge of scratch canvas)
            _indicatorCanvas.fillRect(outline_thickness,
                                      0, // Flush with top
                                      region_w - (2 * outline_thickness),
                                      region_h - outline_thickness,
                                      0); // WHITE
            _indicatorCanvas.pushCanvas(region_screen_x, region_screen_y, UPDATE_MODE_GC16);
            _indicatorCanvas.deleteCanvas();
            log_i("DisplayManager: Drew startup indicator rectangle (region push).");
        }
        else
        {
            log_e("DisplayManager: Failed to create scratch canvas for startup indicator.");
        }

        xSemaphoreGive(_panelMutex);
    }
    else
    {
        log_e("DisplayManager::drawStartupIndicator failed to take panel mutex.");
    }
}

void DisplayManager::drawActivityIndicator(ActivityIndicatorType type)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot draw activity indicator.");
        return;
    }
    // Panel lock only. This used to take the single long-held EPD mutex with a
    // 100 ms timeout, which a render holds for 8-10 s -- so the one piece of
    // instant feedback the device had never appeared while it was busy, which is
    // precisely when the user needs it.
    if (xSemaphoreTake(_panelMutex, pdMS_TO_TICKS(1500)) == pdTRUE)
    {
        // Define indicator properties
        const int32_t indicator_visible_width = 64; // Width of the rectangle on screen
        const int32_t indicator_height = 256;       // Total height of the rectangle
        const int32_t outline_thickness = 12;

        // Position calculation. Uses cached geometry, never _canvas -- the
        // render owns that buffer right now.
        const int32_t screen_width = _canvasW;
        const int32_t screen_height = _canvasH;
        const int32_t screen_center_y = screen_height / 2;
        const int32_t half_indicator_height = indicator_height / 2;

        // Top Y of an indicator centred on the screen, with an offset because
        // the buttons are centred on the case, not on the screen.
        const int32_t centered_indicator_top_y = screen_center_y - half_indicator_height + 25;
        const int32_t push_case_top_y = centered_indicator_top_y;

        int32_t current_indicator_top_y;
        switch (type)
        {
        case ACTIVITY_PUSH:
            current_indicator_top_y = push_case_top_y;
            break;
        case ACTIVITY_UP:
            // UP sits one full rectangle higher than PUSH.
            current_indicator_top_y = push_case_top_y - indicator_height;
            break;
        case ACTIVITY_DOWN:
            // DOWN sits one full rectangle lower than PUSH.
            current_indicator_top_y = push_case_top_y + indicator_height;
            break;
        default:
            log_w("DisplayManager: Unknown ActivityIndicatorType %d, defaulting to PUSH position.", type);
            current_indicator_top_y = push_case_top_y;
            break;
        }

        int32_t region_w = indicator_visible_width;
        int32_t region_h = indicator_height;
        int32_t region_screen_x = screen_width - region_w; // Align to right screen edge
        int32_t region_screen_y = current_indicator_top_y;

        if (_indicatorCanvas.createCanvas(region_w, region_h))
        {
            // Outer black rectangle part
            _indicatorCanvas.fillRect(0, 0, region_w, region_h, 15); // BLACK
            // Inner white rectangle part (black border on LEFT side of this scratch canvas)
            _indicatorCanvas.fillRect(outline_thickness, // Black border on LEFT
                                      outline_thickness,
                                      region_w - outline_thickness, // Width of white area
                                      region_h - (2 * outline_thickness),
                                      0); // WHITE
            _indicatorCanvas.pushCanvas(region_screen_x, region_screen_y, UPDATE_MODE_DU4);
            _indicatorCanvas.deleteCanvas();
            log_i("DisplayManager: Drew activity indicator (type %d at Y:%d) as a region push.", type, region_screen_y);
        }
        else
        {
            log_e("DisplayManager: Failed to create scratch canvas for activity indicator.");
        }

        xSemaphoreGive(_panelMutex);
    }
    else
    {
        log_e("DisplayManager::drawActivityIndicator failed to take panel mutex.");
    }
}

M5EPD_Canvas *DisplayManager::getCanvas()
{
    // Returns the raw script framebuffer. The CALLER must hold the canvas lock
    // (lockEPD/unlockEPD) for the whole time it draws into it. RenderTask does.
    return &_canvas;
}

int DisplayManager::getWidth()
{
    return _isInitialized ? _canvasW : 0;
}

int DisplayManager::getHeight()
{
    return _isInitialized ? _canvasH : 0;
}

bool DisplayManager::lockEPD(TickType_t timeout)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot lock EPD.");
        return false;
    }
    if (_canvasMutex == NULL)
    {
        log_e("DisplayManager canvas mutex is NULL, cannot lock.");
        return false;
    }
    if (xSemaphoreTake(_canvasMutex, timeout) == pdTRUE)
    {
        return true;
    }
    log_e("DisplayManager::lockEPD failed to take canvas mutex.");
    return false;
}

void DisplayManager::unlockEPD()
{
    if (_canvasMutex == NULL)
    {
        log_e("DisplayManager canvas mutex is NULL, cannot unlock.");
        return;
    }
    xSemaphoreGive(_canvasMutex);
}

#include "display_manager.h"
#include "esp32-hal-log.h" // Keep for logging

// Watchy::display is the global display object
// GxEPD2_DISPLAY_CLASS *gGxEPD2_Type = &Watchy::display; // Example of getting the specific type if needed

DisplayManager::DisplayManager() : _isInitialized(false)
{
    _epdMutex = xSemaphoreCreateMutex();
    if (_epdMutex == NULL)
    {
        log_e("DisplayManager: Failed to create EPD mutex!");
    }
    // _canvas and _indicatorCanvas are removed, so no specific constructor logic for them.
    // Watchy::display is initialized by Watchy::initGxEPD() in main.
}

DisplayManager::~DisplayManager()
{
    if (_epdMutex != NULL)
    {
        vSemaphoreDelete(_epdMutex);
    }
    // No canvas objects owned by this class to delete/destroy
}

bool DisplayManager::initializeEPD() // Renamed from init for consistency, was initializeM5PaperEPD
{
    if (_isInitialized)
        return true;

    // Watchy::init() and Watchy::initGxEPD() should be called in main setup() BEFORE this.
    // This method is now more about setting DisplayManager's internal state
    // and applying Watchy-specific display settings if any are managed here.

    if (xSemaphoreTake(_epdMutex, portMAX_DELAY) == pdTRUE)
    {
        // Watchy display is 200x200
        // Set rotation (e.g., 1 for portrait, Watchy default is landscape)
        // Watchy::display.setRotation(1); // 0=0deg, 1=90deg, 2=180deg, 3=270deg

        // Set text properties for Watchy::display
        // Note: GxEPD2 uses Adafruit_GFX text functions.
        // Colors are GxEPD_BLACK, GxEPD_WHITE.
        Watchy::display.setTextSize(1); // Default text size (GxEPD2 default is 1)
        Watchy::display.setTextColor(GxEPD_BLACK);
        // Watchy::display.setTextDatum(TC_DATUM); // setTextDatum is not standard in GxEPD or Adafruit_GFX.
                                                // Alignment is typically handled by cursor positioning and getTextBounds.
                                                // For now, assume Top-Left and adjust drawing logic.

        // No separate canvas creation like _canvas.createCanvas(W, H) for Watchy::display
        // Its dimensions are fixed (200x200 for standard Watchy)

        _isInitialized = true;
        log_i("DisplayManager initialized for Watchy. Display dimensions: %dx%d", Watchy::display.width(), Watchy::display.height());

        xSemaphoreGive(_epdMutex);
        return true;
    }
    log_e("DisplayManager::initializeEPD failed to take mutex.");
    return false;
}

// _drawTextInternal removed, logic incorporated into showMessage.

void DisplayManager::showMessage(const String &text, int y_offset, uint16_t color, bool full_update, bool clear_first)
{
    // Color will be GxEPD_BLACK or GxEPD_WHITE. Original M5Paper color (0-15) needs mapping.
    // For simplicity, map non-white to GxEPD_BLACK.
    uint16_t actual_color = (color == GxEPD_WHITE) ? GxEPD_WHITE : GxEPD_BLACK;

    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot show message.");
        return;
    }
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    { 
        if (clear_first)
        {
            Watchy::display.fillScreen(GxEPD_WHITE); 
        }
        else
        {
            // Draw a white background rectangle behind the text
            // Adafruit_GFX text functions require setting cursor, text color, size first.
            // Then use getTextBounds to find out w,h.
            // Watchy::display.setTextDatum(TC_DATUM); // No direct equivalent, handle manually
            // Watchy::display.setTextSize(3); // Example size, adjust as needed, or use value from init
            
            int16_t x1_b, y1_b; // Dummy values for getTextBounds x,y origin
            uint16_t text_w_b, text_h_b; // For text bounds
            Watchy::display.getTextBounds(text, 0, 0, &x1_b, &y1_b, &text_w_b, &text_h_b);

            // Center text: x_pos = (display_width - text_width) / 2
            int16_t x_pos = (Watchy::display.width() - text_w_b) / 2;
            int16_t y_pos = y_offset; // Using y_offset as the top Y for text

            const int16_t padding = 5; 
            Watchy::display.fillRect(x_pos - padding, y_pos - padding, text_w_b + (2 * padding), text_h_b + (2 * padding), GxEPD_WHITE);
        }

        // Draw the actual text
        Watchy::display.setTextColor(actual_color); 
        // Watchy::display.setTextDatum(TC_DATUM); // No direct equivalent
        // Calculate x for centered text based on textWidth (from getTextBounds)
        int16_t x1_t, y1_t;
        uint16_t text_w_t, text_h_t;
        Watchy::display.getTextBounds(text, 0, 0, &x1_t, &y1_t, &text_w_t, &text_h_t);
        int16_t x_centered = (Watchy::display.width() - text_w_t) / 2;

        Watchy::display.setCursor(x_centered, y_offset); // y_offset is the top baseline
        Watchy::display.print(text);
        log_i("DisplayManager: Drawing message: \"%s\"", text.c_str());

        Watchy::display.display(!full_update); // display(true) for partial, display(false) for full

        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::showMessage failed to take mutex for: %s", text.c_str());
    }
}

// void DisplayManager::pushCanvasUpdate(int32_t x, int32_t y, m5epd_update_mode_t mode)
void DisplayManager::pushCanvasUpdate(bool partial)
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot push canvas update.");
        return;
    }
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        // Watchy::display.displayWindow(x,y,w,h) could be used if bounds are known
        Watchy::display.display(partial); // true for partial update
        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::pushCanvasUpdate failed to take mutex.");
    }
}

void DisplayManager::clearScreen(uint16_t color)
{
    // color will be GxEPD_WHITE or GxEPD_BLACK
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot clear screen.");
        return;
    }
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        Watchy::display.fillScreen(color);
        Watchy::display.display(false); // Full update for a clearScreen
        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::clearScreen failed to take mutex.");
    }
}

void DisplayManager::drawStartupIndicator()
{
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot draw startup indicator.");
        return;
    }
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(100)) == pdTRUE) // Increased timeout slightly
    {
        // Define indicator properties for Watchy (200x200 screen)
        const int32_t indicator_width = 100; // Smaller for Watchy
        const int32_t indicator_height = 10; // Thin bar
        const int32_t outline_thickness = 2;

        int32_t region_screen_x = Watchy::display.width() / 2 - indicator_width / 2;
        int32_t region_screen_y = 0; // Top edge

        // Draw directly on Watchy::display
        Watchy::display.fillRect(region_screen_x, region_screen_y, indicator_width, indicator_height, GxEPD_BLACK);
        Watchy::display.fillRect(region_screen_x + outline_thickness,
                         region_screen_y + outline_thickness,
                         indicator_width - (2 * outline_thickness),
                         indicator_height - (2 * outline_thickness),
                         GxEPD_WHITE);

        // Update the specific window region
        Watchy::display.displayWindow(region_screen_x, region_screen_y, indicator_width, indicator_height);
        // Or a general partial update: Watchy::display.display(true); 
        
        log_i("DisplayManager: Drew startup indicator rectangle.");
        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::drawStartupIndicator failed to take mutex.");
    }
}

void DisplayManager::drawActivityIndicator(ActivityIndicatorType type)
{
    // This function needs significant redesign for Watchy's screen and button layout.
    // For now, let's draw a simple small square on the right edge, position varies by type.
    if (!_isInitialized)
    {
        log_e("DisplayManager not initialized, cannot draw activity indicator.");
        return;
    }
    if (xSemaphoreTake(_epdMutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        const int32_t indicator_size = 10; // Small square
        int32_t region_screen_x = Watchy::display.width() - indicator_size - 2; // 2px from edge
        int32_t region_screen_y;

        switch (type)
        {
        case ACTIVITY_UP:
            region_screen_y = 2; // Top
            break;
        case ACTIVITY_DOWN:
            region_screen_y = Watchy::display.height() - indicator_size - 2; // Bottom
            break;
        case ACTIVITY_PUSH:
        default:
            region_screen_y = Watchy::display.height() / 2 - indicator_size / 2; // Middle
            break;
        }
        
        // Clear the area first (e.g. with white)
        // To avoid ghosting, it's better to clear a slightly larger area if this is redrawn often
        Watchy::display.fillRect(region_screen_x -1, region_screen_y -1, indicator_size + 2, indicator_size + 2, GxEPD_WHITE);
        Watchy::display.fillRect(region_screen_x, region_screen_y, indicator_size, indicator_size, GxEPD_BLACK);

        // Update a small window or do a general partial update
        Watchy::display.displayWindow(region_screen_x -1, region_screen_y -1, indicator_size + 2, indicator_size + 2);
        // Watchy::display.display(true); // Partial update

        log_i("DisplayManager: Drew activity indicator (type %d).", type);
        xSemaphoreGive(_epdMutex);
    }
    else
    {
        log_e("DisplayManager::drawActivityIndicator failed to take mutex.");
    }
}


// M5EPD_Canvas *DisplayManager::getCanvas()
GxEPD2_GFX* DisplayManager::getCanvas()
{
    // Return a pointer to the global Watchy display object.
    // Callers must manage synchronization using lockEPD/unlockEPD.
    return &Watchy::display;
}

int DisplayManager::getWidth()
{
    return _isInitialized ? Watchy::display.width() : 0;
}

int DisplayManager::getHeight()
{
    return _isInitialized ? Watchy::display.height() : 0;
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

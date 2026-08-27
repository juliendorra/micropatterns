// Host implementation of the firmware's DisplayManager.
//
// The firmware header M5Paper_MicroPatterns/src/display_manager.h is used
// VERBATIM (it parses fine once M5EPD.h / freertos are shimmed). Only the
// method bodies are replaced here, and only the ones the renderer core
// actually calls. Everything else is intentionally left undefined: if a future
// change makes DisplayListRenderer depend on more of DisplayManager, this file
// will fail to link and that dependency will be visible rather than silently
// emulated.
#include "display_manager.h"
#include "host_display_manager.h"

namespace {
int g_canvasWidth = 960;
int g_canvasHeight = 540;
}

void hostSetCanvasSize(int width, int height) {
    g_canvasWidth = width;
    g_canvasHeight = height;
}

DisplayManager::DisplayManager() : _epdMutex(nullptr), _isInitialized(false) {}

DisplayManager::~DisplayManager() {}

bool DisplayManager::initializeEPD() {
    _canvas.createCanvas((int16_t)g_canvasWidth, (int16_t)g_canvasHeight);
    _canvas.fillCanvas(0);
    _isInitialized = true;
    return true;
}

M5EPD_Canvas* DisplayManager::getCanvas() { return &_canvas; }

int DisplayManager::getWidth() { return (int)_canvas.width(); }

int DisplayManager::getHeight() { return (int)_canvas.height(); }

bool DisplayManager::lockEPD(TickType_t timeout) { (void)timeout; return true; }

void DisplayManager::unlockEPD() {}

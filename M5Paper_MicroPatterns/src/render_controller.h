#ifndef RENDER_CONTROLLER_H
#define RENDER_CONTROLLER_H

#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "display_manager.h"
#include "event_defs.h"     // For RenderJobData, RenderResultData
#include "display_list_renderer.h" // New include

class RenderController
{
public:
    RenderController(DisplayManager &displayMgr);
    ~RenderController();

    RenderResultData renderScript(const RenderJobData &job);
    void requestInterrupt();

private:
    DisplayManager &_displayMgr;
    MicroPatternsParser _parser;
    MicroPatternsRuntime *_runtime; // For display list generation
    DisplayListRenderer *_renderer; // For rendering the display list

    volatile bool _interrupt_requested_for_runtime_or_renderer;

    // Callback for interrupt checking (passed to runtime and renderer)
    bool checkInterrupt();
};

#endif // RENDER_CONTROLLER_H
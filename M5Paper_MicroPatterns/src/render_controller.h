#ifndef RENDER_CONTROLLER_H
#define RENDER_CONTROLLER_H

#include "mp_program.h"
#include "micropatterns_runtime.h"
#include "display_manager.h"
#include "event_defs.h"     // For RenderJobData, RenderResultData
#include "display_list_renderer.h"

// Runs a COMPILED program through the runtime and the rasterizer. Parsing is
// no longer this class's job: RenderTask asks ScriptManager::loadProgram()
// for the program, which comes from the compiled cache written at sync time
// (or, if that is missing or stale, from a one-off compile that is then
// cached). See mp_program.h.
class RenderController
{
public:
    RenderController(DisplayManager &displayMgr);
    ~RenderController();

    RenderResultData renderScript(const String& script_id, const MpProgram& program, const ScriptExecState& initial_state);
    void requestInterrupt();

private:
    DisplayManager &_displayMgr;
    MicroPatternsRuntime *_runtime; // For display list generation
    DisplayListRenderer *_renderer; // For rendering the display list

    volatile bool _interrupt_requested_for_runtime_or_renderer;

    // Callback for interrupt checking (passed to runtime and renderer)
    bool checkInterrupt();
};

#endif // RENDER_CONTROLLER_H

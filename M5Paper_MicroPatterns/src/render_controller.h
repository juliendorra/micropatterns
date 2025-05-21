#ifndef RENDER_CONTROLLER_H
#define RENDER_CONTROLLER_H

#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "display_manager.h"
#include "script_manager.h" // For ScriptExecState, though it's in event_defs.h
#include "event_defs.h"     // For RenderJob, RenderResult, ScriptExecState

class RenderController
{
public:
    RenderController(DisplayManager &displayMgr);
    ~RenderController();

    // Renders the script specified in the job.
    // Returns a RenderResultData indicating success, failure, or interruption.
    RenderResultData renderScript(const RenderJobData &job); // Changed RenderJob to RenderJobData

    // Signals the currently running MicroPatternsRuntime to interrupt its execution.
    void requestInterrupt();

private:
    DisplayManager &_displayMgr;
    MicroPatternsParser _parser;
    MicroPatternsRuntime *_runtime; // Dynamically allocated for each script run

    // Interrupt flag for the runtime
    volatile bool _interrupt_requested_for_runtime;

    // Callback for MicroPatternsDrawing interrupt check
    bool checkRuntimeInterrupt();
};

#endif // RENDER_CONTROLLER_H
#include "render_controller.h"
#include "esp32-hal-log.h"

RenderController::RenderController(DisplayManager& displayMgr)
    : _displayMgr(displayMgr), _runtime(nullptr), _interrupt_requested_for_runtime(false) {
}

RenderController::~RenderController() {
    delete _runtime; // Ensure cleanup if an instance exists
}

bool RenderController::checkRuntimeInterrupt() {
    // This function is passed to MicroPatternsDrawing
    return _interrupt_requested_for_runtime;
}

RenderResultData RenderController::renderScript(const RenderJobData& job) { // Changed RenderJob to RenderJobData
    log_i("RenderController: Starting render for script ID: %s", job.script_id.c_str());
    _interrupt_requested_for_runtime = false; // Reset interrupt flag for this job

    RenderResultData result; // Changed RenderResult to RenderResultData
    if (job.script_id.isEmpty()) {
        log_e("RenderController: Received job with empty script_id. Aborting render.");
        result.script_id = "";
        result.success = false;
        result.interrupted = false;
        result.error_message = "Render job had an empty script ID.";
        // final_state will be default from ScriptExecState in RenderResultData
        return result;
    }
    result.script_id = job.script_id;
    result.success = false;
    result.interrupted = false;
    // Only set final_state, as initial_state doesn't exist in RenderResult
    result.final_state = job.initial_state;   // Start final state from initial

    // 1. Parse Script
    _parser.reset();
    if (!_parser.parse(job.script_content)) {
        log_e("RenderController: Script parsing failed for ID: %s", job.script_id.c_str());
        // Collect parse errors into result.error_message
        String errors_str;
        for (const String& err : _parser.getErrors()) {
            errors_str += err + "\n";
        }
        result.error_message = "Parse failed: " + errors_str;
        // Display parse errors using DisplayManager
        // _displayMgr.showMessage("Parse Error: " + job.script_id, 50, 15, true, true);
        // int y_pos = 100;
        // for (const String& err : _parser.getErrors()) {
        //     _displayMgr.showMessage(err, y_pos, 15, false, false); // Don't clear, don't full update each line
        //     y_pos += 30;
        // }
        // _displayMgr.pushCanvasUpdate(UPDATE_MODE_GC16); // Push all messages
        return result; // Return with success=false
    }
    log_i("RenderController: Script '%s' parsed successfully.", job.script_id.c_str());

    // 2. Prepare Runtime
    // Delete old runtime if it exists
    if (_runtime) {
        delete _runtime;
        _runtime = nullptr;
    }
    // Create new runtime instance. Canvas is obtained from DisplayManager.
    // The DisplayManager's canvas should be used.
    _runtime = new MicroPatternsRuntime(_displayMgr.getCanvas(), _parser.getAssets());
    _runtime->setCommands(&_parser.getCommands());
    _runtime->setDeclaredVariables(&_parser.getDeclaredVariables());

    // Set interrupt callback for the runtime (which passes it to drawing)
    _runtime->setInterruptCheckCallback([this]() { return this->checkRuntimeInterrupt(); });


    // 3. Set initial state for the script from the job
    _runtime->setCounter(job.initial_state.counter);
    _runtime->setTime(job.initial_state.hour, job.initial_state.minute, job.initial_state.second);
    log_i("RenderController: Executing script '%s' - Initial Counter: %d, Time: %02d:%02d:%02d",
          job.script_id.c_str(), job.initial_state.counter, job.initial_state.hour, job.initial_state.minute, job.initial_state.second);


    // 4. Execute Script
    // The runtime's execute() method will call drawing methods, which will use the interrupt check.
    // The canvas operations (clear, draw, push) are encapsulated within runtime/drawing.
    // DisplayManager's mutex should be handled by the RenderTask before calling this.
    
    unsigned long executionStartTime = millis();
    _runtime->execute(); // This includes drawing and pushing canvas via MicroPatternsDrawing
    unsigned long executionDuration = millis() - executionStartTime;

    // 5. Check for interruption status from runtime
    if (_runtime->isInterrupted()) {
        log_i("RenderController: Script '%s' execution was interrupted.", job.script_id.c_str());
        result.interrupted = true;
        result.success = false; // Interrupted means not fully successful
        result.error_message = "Rendering interrupted by user.";
    } else {
        log_i("RenderController: Script '%s' execution and rendering took %lu ms.", job.script_id.c_str(), executionDuration);
        result.success = true; // Assuming no other errors from runtime
    }

    // 6. Collect final state
    // Get counter and time values from the runtime
    result.final_state.counter = _runtime->getCounter();
    _runtime->getTime(result.final_state.hour, result.final_state.minute, result.final_state.second);
    result.final_state.state_loaded = true; // Mark that this state is from an execution

    // Runtime is deleted and recreated per script, so no need to clear its internal interrupt flag here.
    // _interrupt_requested_for_runtime is reset at the start of this function.
    
    return result;
}

void RenderController::requestInterrupt() {
    log_i("RenderController: Interrupt requested.");
    _interrupt_requested_for_runtime = true;
    if (_runtime) {
        // The runtime itself doesn't have a requestInterrupt method in the plan.
        // The interrupt is checked via the callback.
        // If runtime had its own flag, we'd set it here: _runtime->requestInterrupt();
    }
}
#include "render_controller.h"
#include "main.h" // For g_renderInterruptRequested
#include "esp32-hal-log.h"

RenderController::RenderController(DisplayManager& displayMgr)
    : _displayMgr(displayMgr), _runtime(nullptr), _renderer(nullptr),
      _interrupt_requested_for_runtime_or_renderer(false) {
}

RenderController::~RenderController() {
    delete _runtime;
    delete _renderer;
}

bool RenderController::checkInterrupt() {
    // HOT PATH. micropatterns_drawing.cpp polls this callback from its scanline
    // loops in fillRect/fillCircle/drawAsset, so it must be nothing but loads.
    //
    // It used to fall through to xEventGroupGetBits(g_renderTaskEventFlags) on
    // every unlatched call -- a FreeRTOS critical section, tens of thousands of
    // times per render (and, before the drawing layer moved to per-scanline
    // polling, millions). MainControlTask now sets the plain flag
    // g_renderInterruptRequested (main.cpp) alongside RENDER_INTERRUPT_BIT, so we
    // answer from memory alone and still abort on the same signal. Measured on
    // the host harness: +1%..+3.3% of rasterization time for the lock-shaped
    // check versus this one, and it was +95%..+154% under per-pixel polling.
    if (_interrupt_requested_for_runtime_or_renderer) {
        return true;
    }
    if (g_renderInterruptRequested) {
        _interrupt_requested_for_runtime_or_renderer = true; // latch
        return true;
    }
    return false;
}

RenderResultData RenderController::renderScript(const String& script_id, const MpProgram& program, const ScriptExecState& initial_state) {
    log_i("RenderController: Starting render for script ID: %s", script_id.c_str());
    _interrupt_requested_for_runtime_or_renderer = false; // Reset latched interrupt flag

    RenderResultData result;
    result.script_id = script_id;
    result.success = false;
    result.interrupted = false;
    result.final_state = initial_state;

    if (script_id.isEmpty()) {
        result.error_message = "Render job had an empty script ID.";
        log_e("RenderController: %s", result.error_message.c_str());
        return result;
    }
    if (program.code.empty() && program.assets.empty()) {
        result.error_message = "Render job had an empty program.";
        log_e("RenderController: %s for script ID %s", result.error_message.c_str(), script_id.c_str());
        return result;
    }

    // 2. Prepare and Run Runtime to generate Display List
    if (_runtime) delete _runtime;
    _runtime = new MicroPatternsRuntime(_displayMgr.getWidth(), _displayMgr.getHeight(), program);
    _runtime->setInterruptCheckCallback([this]() { return this->checkInterrupt(); });
    _runtime->setCounter(initial_state.counter);
    _runtime->setTime(initial_state.hour, initial_state.minute, initial_state.second);

    unsigned long generationStartTime = millis();
    _runtime->generateDisplayList();
    unsigned long generationDuration = millis() - generationStartTime;

    if (_runtime->isInterrupted()) {
        result.interrupted = true;
        result.error_message = "Display list generation interrupted.";
        log_i("RenderController: %s for script '%s'", result.error_message.c_str(), script_id.c_str());
        // Final state might be partially updated by runtime before interrupt
        result.final_state.counter = _runtime->getCounter();
        _runtime->getTime(result.final_state.hour, result.final_state.minute, result.final_state.second);
        result.final_state.state_loaded = true;
        return result;
    }
    log_i("RenderController: Display list generation for '%s' took %lu ms. List size: %d (program %u instr, %u B)",
          script_id.c_str(), generationDuration, _runtime->getDisplayList().size(),
          (unsigned)program.code.size(), (unsigned)program.byteSize());

    // 3. Prepare and Run DisplayListRenderer
    if (_renderer) delete _renderer;
    _renderer = new DisplayListRenderer(_displayMgr.getCanvas(), _displayMgr.getWidth(), _displayMgr.getHeight());
    _renderer->setInterruptCheckCallback([this]() { return this->checkInterrupt(); });
    
    unsigned long renderStartTime = millis();
    _renderer->render(_runtime->getDisplayList()); // This clears canvas and draws items
    unsigned long renderDuration = millis() - renderStartTime;

    // Check for interrupt again (renderer might also check it)
    if (checkInterrupt()) { // Check our flag, renderer might have set it via callback
        result.interrupted = true;
        // result.success will be handled below
        result.error_message = "Rendering process interrupted.";
        log_i("RenderController: %s for script '%s'", result.error_message.c_str(), script_id.c_str());
    } else {
        log_i("RenderController: Display list rendering for '%s' took %lu ms.", script_id.c_str(), renderDuration);
        // result.success will be set based on result.interrupted below
    }

    // Final success/interrupted status determination
    if (result.interrupted) { // If interrupted at any stage (runtime or renderer)
        result.success = false;
    } else {
        result.success = true; // Not interrupted, assume success unless other errors occurred
    }
    
    // Final state from runtime (variables might have changed during display list generation)
    result.final_state.counter = _runtime->getCounter();
    _runtime->getTime(result.final_state.hour, result.final_state.minute, result.final_state.second);
    result.final_state.state_loaded = true;
    
    return result;
}

void RenderController::requestInterrupt() {
    log_i("RenderController: Interrupt requested.");
    _interrupt_requested_for_runtime_or_renderer = true;
    // Runtime and Renderer will check this flag via the callback.
}
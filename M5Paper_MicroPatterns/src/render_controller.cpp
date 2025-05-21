#include "render_controller.h"
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
    return _interrupt_requested_for_runtime_or_renderer;
}

RenderResultData RenderController::renderScript(const RenderJobData& job) {
    log_i("RenderController: Starting render for script ID: %s", job.script_id.c_str());
    _interrupt_requested_for_runtime_or_renderer = false; // Reset interrupt flag

    RenderResultData result;
    result.script_id = job.script_id;
    result.success = false;
    result.interrupted = false;
    result.final_state = job.initial_state;

    if (job.script_id.isEmpty()) {
        result.error_message = "Render job had an empty script ID.";
        log_e("RenderController: %s", result.error_message.c_str());
        return result;
    }

    // 1. Parse Script
    _parser.reset();
    if (!_parser.parse(job.script_content)) {
        String errors_str;
        for (const String& err : _parser.getErrors()) { errors_str += err + "\n"; }
        result.error_message = "Parse failed: " + errors_str;
        log_e("RenderController: Script parsing failed for ID %s. Errors:\n%s", job.script_id.c_str(), errors_str.c_str());
        return result;
    }
    log_i("RenderController: Script '%s' parsed successfully.", job.script_id.c_str());

    // 2. Prepare and Run Runtime to generate Display List
    if (_runtime) delete _runtime;
    _runtime = new MicroPatternsRuntime(_displayMgr.getWidth(), _displayMgr.getHeight(), _parser.getAssets());
    _runtime->setCommands(&_parser.getCommands());
    _runtime->setDeclaredVariables(&_parser.getDeclaredVariables());
    _runtime->setInterruptCheckCallback([this]() { return this->checkInterrupt(); });
    _runtime->setCounter(job.initial_state.counter);
    _runtime->setTime(job.initial_state.hour, job.initial_state.minute, job.initial_state.second);

    unsigned long generationStartTime = millis();
    _runtime->generateDisplayList();
    unsigned long generationDuration = millis() - generationStartTime;

    if (_runtime->isInterrupted()) {
        result.interrupted = true;
        result.error_message = "Display list generation interrupted.";
        log_i("RenderController: %s for script '%s'", result.error_message.c_str(), job.script_id.c_str());
        // Final state might be partially updated by runtime before interrupt
        result.final_state.counter = _runtime->getCounter();
        _runtime->getTime(result.final_state.hour, result.final_state.minute, result.final_state.second);
        result.final_state.state_loaded = true;
        return result;
    }
    log_i("RenderController: Display list generation for '%s' took %lu ms. List size: %d",
          job.script_id.c_str(), generationDuration, _runtime->getDisplayList().size());

    // 3. Prepare and Run DisplayListRenderer
    if (_renderer) delete _renderer;
    _renderer = new DisplayListRenderer(_displayMgr, _parser.getAssets(), _displayMgr.getWidth(), _displayMgr.getHeight());
    _renderer->setInterruptCheckCallback([this]() { return this->checkInterrupt(); });
    
    unsigned long renderStartTime = millis();
    _renderer->render(_runtime->getDisplayList()); // This clears canvas and draws items
    unsigned long renderDuration = millis() - renderStartTime;

    // Check for interrupt again (renderer might also check it)
    if (checkInterrupt()) { // Check our flag, renderer might have set it via callback
        result.interrupted = true;
        result.error_message = "Rendering process interrupted.";
        log_i("RenderController: %s for script '%s'", result.error_message.c_str(), job.script_id.c_str());
    } else {
        log_i("RenderController: Display list rendering for '%s' took %lu ms.", job.script_id.c_str(), renderDuration);
        result.success = true; // If not interrupted and no other errors reported by renderer (future)
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
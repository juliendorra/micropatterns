#include "main.h"
#include "esp32-hal-log.h"
#include "esp_task_wdt.h" // For watchdog

// --- Task Handles ---
TaskHandle_t g_mainControlTaskHandle = NULL;
TaskHandle_t g_inputTaskHandle = NULL;
TaskHandle_t g_renderTaskHandle = NULL;
TaskHandle_t g_fetchTaskHandle = NULL;

// --- Queue Handles ---
QueueHandle_t g_inputEventQueue = NULL;
QueueHandle_t g_renderCommandQueue = NULL;
QueueHandle_t g_renderStatusQueue = NULL;
QueueHandle_t g_fetchCommandQueue = NULL;
QueueHandle_t g_fetchStatusQueue = NULL;

// --- Event Group Handles ---
EventGroupHandle_t g_appEventGroup = NULL;
// const EventBits_t WIFI_CONNECTED_BIT = (1 << 0); // Example
// const EventBits_t FETCH_INTERRUPT_REQUESTED_BIT = (1 << 1); // Example
EventGroupHandle_t g_renderTaskEventFlags = NULL;
const EventBits_t RENDER_INTERRUPT_BIT = (1 << 0);


// --- Global Manager Instances ---
// Instantiated in setup()
SystemManager* g_systemManager = nullptr;
InputManager* g_inputManager = nullptr;
DisplayManager* g_displayManager = nullptr;
ScriptManager* g_scriptManager = nullptr;
NetworkManager* g_networkManager = nullptr;
// RenderController is instantiated by RenderTask as needed, or can be global if RenderTask always uses one.
// For now, RenderTask will create its own RenderController instance.

// --- Constants ---
#define FRESH_START_THRESHOLD 10 // Perform full refresh every 10 reboots (approx)
// Calculate capacity for script content JSON: content length + structural overhead + parsing buffer
#define SCRIPT_CONTENT_JSON_CAPACITY (MAX_SCRIPT_CONTENT_LEN + 512)
const TickType_t MAIN_LOOP_IDLE_DELAY = pdMS_TO_TICKS(50);
const TickType_t SLEEP_IDLE_THRESHOLD_MS = 30000; // 30 seconds of inactivity before sleep

// --- Main Setup ---
void setup() {
    // Initialize M5Paper hardware components.
    // M5.begin() handles:
    // - Serial.begin()
    // - M5.Power.begin() (enables main power, ext power)
    // - M5.EPD.begin() (enables EPD power, inits EPD driver)
    // - M5.RTC.begin()
    // - M5.TP.begin() (Touch Panel)
    // Parameters: (SerialEnable=true, SDEnable=false, EnableI2C=true, EPDEnable=true, WireEnable=true)
    // SD card is not used, so SDEnable=false. Serial is used for logging. I2C for RTC/Touch. EPD is essential.
    M5.begin(true, false, true, true, true);
    log_i("M5.begin() completed.");

    // Perform minimal early hardware setup (NVS, ISR service) after M5.begin ensures Serial is up.
    SysInit_EarlyHardware();

    // Initialize Watchdog for the main setup/initialization phase
    esp_task_wdt_init(60, true); // 60s timeout, panic on timeout during setup
    esp_task_wdt_add(NULL);      // Add current task (setup) to watchdog
    esp_task_wdt_reset();

    // 1. Create Queues and Event Groups
    g_inputEventQueue = xQueueCreate(10, sizeof(InputEvent));
    g_renderCommandQueue = xQueueCreate(1, sizeof(RenderJobQueueItem)); // Use RenderJobQueueItem
    g_renderStatusQueue = xQueueCreate(1, sizeof(RenderResultQueueItem)); // Use RenderResultQueueItem
    g_fetchCommandQueue = xQueueCreate(1, sizeof(FetchJob)); // FetchJob is simple, no Strings
    g_fetchStatusQueue = xQueueCreate(1, sizeof(FetchResultQueueItem)); // Use FetchResultQueueItem
    g_renderTaskEventFlags = xEventGroupCreate();

    if (!g_inputEventQueue || !g_renderCommandQueue || !g_renderStatusQueue ||
        !g_fetchCommandQueue || !g_fetchStatusQueue || !g_renderTaskEventFlags) {
        log_e("FATAL: Failed to create one or more FreeRTOS objects (queues/event groups). Halting.");
        while(1) vTaskDelay(portMAX_DELAY); // Halt
    }
    esp_task_wdt_reset();

    // 2. Initialize Manager Classes
    // Order can be important if there are dependencies in constructors.
    g_displayManager = new DisplayManager();
    if (!g_displayManager || !g_displayManager->initializeEPD()) { // Initialize EPD early for messages
        log_e("FATAL: DisplayManager initialization failed. Halting.");
        // No point trying to display error if display failed.
        while(1) vTaskDelay(portMAX_DELAY); // Halt
    }
    g_displayManager->showMessage("System Booting...", 100, 15, true, true);
    esp_task_wdt_reset();

    g_systemManager = new SystemManager();
    if (!g_systemManager || !g_systemManager->initialize()) { // Loads NVS settings
        log_e("FATAL: SystemManager initialization failed.");
        g_displayManager->showMessage("SysMgr Fail!", 150, 15, false, false);
        while(1) vTaskDelay(portMAX_DELAY);
    }
    esp_task_wdt_reset();

    g_scriptManager = new ScriptManager();
    if (!g_scriptManager || !g_scriptManager->initialize()) { // Initializes SPIFFS
        log_e("FATAL: ScriptManager initialization failed.");
        g_displayManager->showMessage("ScrMgr Fail!", 150, 15, false, false);
        while(1) vTaskDelay(portMAX_DELAY);
    }
    esp_task_wdt_reset();
    
    g_networkManager = new NetworkManager(g_systemManager); // Pass SystemManager if needed for config
    if (!g_networkManager) {
        log_e("FATAL: NetworkManager instantiation failed.");
        g_displayManager->showMessage("NetMgr Fail!", 150, 15, false, false);
        while(1) vTaskDelay(portMAX_DELAY);
    }
    // NetworkManager doesn't have an init method in the plan, connects on demand.
    esp_task_wdt_reset();

    g_inputManager = new InputManager(g_inputEventQueue);
    if (!g_inputManager || !g_inputManager->initialize()) { // Sets up GPIOs and ISRs
        log_e("FATAL: InputManager initialization failed.");
        g_displayManager->showMessage("InpMgr Fail!", 150, 15, false, false);
        while(1) vTaskDelay(portMAX_DELAY);
    }
    esp_task_wdt_reset();

    // 3. Create Tasks
    xTaskCreatePinnedToCore(MainControlTask_Function, "MainCtrlTask", MAIN_CONTROL_TASK_STACK_SIZE, NULL, MAIN_CONTROL_TASK_PRIORITY, &g_mainControlTaskHandle, 1);
    xTaskCreatePinnedToCore(InputTask_Function, "InputTask", INPUT_TASK_STACK_SIZE, NULL, INPUT_TASK_PRIORITY, &g_inputTaskHandle, 1); // Core 1 for responsiveness
    xTaskCreatePinnedToCore(RenderTask_Function, "RenderTask", RENDER_TASK_STACK_SIZE, NULL, RENDER_TASK_PRIORITY, &g_renderTaskHandle, 0); // Core 0 for rendering
    xTaskCreatePinnedToCore(FetchTask_Function, "FetchTask", FETCH_TASK_STACK_SIZE, NULL, FETCH_TASK_PRIORITY, &g_fetchTaskHandle, 0);    // Core 0 for network

    if (!g_mainControlTaskHandle || !g_inputTaskHandle || !g_renderTaskHandle || !g_fetchTaskHandle) {
        log_e("FATAL: Failed to create one or more tasks. Halting.");
        g_displayManager->showMessage("Task Fail!", 150, 15, false, false);
        while(1) vTaskDelay(portMAX_DELAY);
    }

    log_i("Setup complete. Tasks created. Managers initialized.");
    g_displayManager->showMessage("Setup OK", 200, 15, false, false);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Show setup message

    // Setup task no longer needs WDT monitoring. Tasks will manage their own.
    esp_task_wdt_delete(NULL);

    // The `loop()` function is not used in FreeRTOS projects.
    // MainControlTask_Function will take over the role of the main application loop.
    // Delete this task (setup) as it's done.
    vTaskDelete(NULL);
}

// Helper function to queue a render job
// if useAsIsState is true, it uses the state directly from getScriptForExecution (for WiFi fail recovery)
// if useAsIsState is false, it increments counter (if loaded) and uses current RTC time (for user-initiated re-render/next script etc.)
// Returns true if a job was successfully queued, false otherwise.
static bool triggerScriptRender(const String& humanIdToRender, bool useAsIsState, AppState& currentAppState_ref, String& currentLoadedScriptId_ref) {
    if (humanIdToRender.isEmpty()) {
        log_w("triggerScriptRender: humanIdToRender is empty. Cannot render.");
        // Attempt to load and render default script as a fallback
        RenderJobData defaultJobData; // script_content is no longer a member
        ScriptExecState defaultScriptState;
        // getScriptForExecution with empty ID should provide default script details (ID, FileID, State)
        if (g_scriptManager->getScriptForExecution(defaultJobData.script_id, defaultJobData.file_id, defaultScriptState)) { // No script_content
            if (!defaultJobData.script_id.isEmpty()) {
                log_i("triggerScriptRender: humanIdToRender was empty, attempting to render default script '%s'", defaultJobData.script_id.c_str());
                // For default script, always use 'fresh' state (useAsIsState = false effectively)
                defaultJobData.initial_state = defaultScriptState; // Base state
                if (defaultScriptState.state_loaded) { // Default script state_loaded should be false from getScriptForExecution
                    defaultJobData.initial_state.counter++;
                } else {
                    defaultJobData.initial_state.counter = 0;
                }
                RTC_Time now_time = g_systemManager->getTime();
                defaultJobData.initial_state.hour = now_time.hour;
                defaultJobData.initial_state.minute = now_time.min;
                defaultJobData.initial_state.second = now_time.sec;

                RenderJobQueueItem defaultJobQueueItem;
                defaultJobQueueItem.fromRenderJobData(defaultJobData);
                if (xQueueSend(g_renderCommandQueue, &defaultJobQueueItem, pdMS_TO_TICKS(100)) == pdTRUE) {
                    currentAppState_ref = AppState::RENDERING_SCRIPT;
                    currentLoadedScriptId_ref = defaultJobData.script_id;
                    return true;
                } else {
                    log_e("triggerScriptRender: Failed to send render job for default script '%s'.", defaultJobData.script_id.c_str());
                    g_displayManager->showMessage("Render Q Fail", 150, 15, false, false);
                    return false;
                }
            } else {
                 log_e("triggerScriptRender: getScriptForExecution also failed to provide a default script ID.");
                 g_displayManager->showMessage("No Script ID", 150, 15, false, false);
                 return false;
            }
        } else {
            log_e("triggerScriptRender: Failed to get default script details.");
            g_displayManager->showMessage("Default Load Fail", 150, 15, false, false);
            return false;
        }
    }

    // Avoid queuing a new "standard" render if one is already in progress. Recovery renders can proceed.
    if (currentAppState_ref == AppState::RENDERING_SCRIPT && !useAsIsState) {
        log_w("triggerScriptRender: Standard render requested for '%s' while already rendering. Ignoring.", humanIdToRender.c_str());
        return false;
    }

    RenderJobData jobData; // script_content is no longer a member
    ScriptExecState scriptState; // This will be the state from storage
    String fileId;
    // String scriptContentForJob; // REMOVED - content not handled here

    // getScriptForExecution now returns humanId, fileId, and initial_state.
    // Content will be loaded by RenderTask.
    if (g_scriptManager->getScriptForExecution(jobData.script_id, fileId, scriptState)) { // No scriptContentForJob
        // Ensure jobData.script_id is the one we intended, or the one getScriptForExecution resolved to (e.g. default)
        // If humanIdToRender was valid, jobData.script_id should match it.
        // If humanIdToRender was invalid and getScriptForExecution gave default, jobData.script_id is default.
        jobData.file_id = fileId;
        // jobData.script_content = scriptContentForJob; // REMOVED
        jobData.initial_state = scriptState; // Base state from storage

        if (!useAsIsState) { // Standard render: increment counter (if loaded), use current RTC time
            if (scriptState.state_loaded) {
                jobData.initial_state.counter++; // Increment the counter from the loaded state
            } else {
                jobData.initial_state.counter = 0; // Ensure counter is 0 if state wasn't loaded (first run for this script)
            }
            RTC_Time now_time = g_systemManager->getTime();
            jobData.initial_state.hour = now_time.hour;
            jobData.initial_state.minute = now_time.min;
            jobData.initial_state.second = now_time.sec;
        }
        // If useAsIsState is true, jobData.initial_state is already set correctly to scriptState (from storage).

        log_i("triggerScriptRender: Queuing render for '%s' (resolved from '%s'), file_id '%s', counter %d. useAsIsState: %s",
              jobData.script_id.c_str(), humanIdToRender.c_str(), jobData.file_id.c_str(), jobData.initial_state.counter, useAsIsState ? "true" : "false");

        RenderJobQueueItem jobQueueItem;
        jobQueueItem.fromRenderJobData(jobData); // This copies human_id, file_id, initial_state

        if (xQueueSend(g_renderCommandQueue, &jobQueueItem, pdMS_TO_TICKS(100)) == pdTRUE) {
            currentAppState_ref = AppState::RENDERING_SCRIPT;
            currentLoadedScriptId_ref = jobData.script_id; // Update the main task's tracker
            return true;
        } else {
            log_e("triggerScriptRender: Failed to send render job for '%s'.", jobData.script_id.c_str());
            g_displayManager->showMessage("Render Q Fail", 150, 15, false, false);
            return false;
        }
    } else {
        // This case should ideally be rare if getScriptForExecution always falls back to default.
        log_e("triggerScriptRender: Failed to get script details for '%s' (even default).", humanIdToRender.c_str());
        g_displayManager->showMessage("Script Load Fail", 150, 15, false, false);
        return false;
    }
}


// --- Main Control Task ---
void MainControlTask_Function(void *pvParameters) {
    esp_task_wdt_init(30, true); // Panic on WDT timeout
    esp_task_wdt_add(NULL);

    AppState currentState = AppState::IDLE;
    TickType_t lastActivityTime = xTaskGetTickCount();
    String currentLoadedScriptId = ""; // Keep track of what's supposedly loaded/rendered

    // Initial actions on boot/restart:
    // 1. Potentially sync time
    RTC_Time rtcTime = g_systemManager->getTime();
    if (rtcTime.hour == 0 && rtcTime.min == 0 && rtcTime.sec == 0) { // RTC likely not set
        g_displayManager->showMessage("Syncing Time...", 50, 15, true, true);
        if (g_systemManager->syncTimeWithNTP(*g_networkManager)) {
            g_displayManager->showMessage("Time Synced", 100, 15);
            vTaskDelay(pdMS_TO_TICKS(1000)); // Delay only on success
        } else {
            g_displayManager->showMessage("Time Sync Fail", 100, 15, false, false); // Brief message
            // No long delay here, attempt to re-render current script
            if (!currentLoadedScriptId.isEmpty()) {
                log_i("MainCtrl: NTP sync failed. Re-rendering current script '%s' with saved state.", currentLoadedScriptId.c_str());
                triggerScriptRender(currentLoadedScriptId, true, currentState, currentLoadedScriptId); // true for useAsIsState
            } else {
                log_w("MainCtrl: NTP sync failed, but no current script loaded to re-render.");
                // If no script is loaded, the message "Time Sync Fail" will be shown briefly.
                // Consider a small delay to ensure message is visible if no re-render happens.
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }
    esp_task_wdt_reset();

    // 2. Initial script load and render
    // The initial script ID will be determined by getScriptForExecution,
    // which might be the last saved, the first in list, or default.
    // For the very first render, we want 'fresh' state (counter 0, current time).
    String initialHumanIdToLoad = "";
    String initialFileIdUnused; // Not used here, but needed for signature
    ScriptExecState tempInitialState;

    // Get initial script ID and state, content is not loaded here.
    if(g_scriptManager->getScriptForExecution(initialHumanIdToLoad, initialFileIdUnused, tempInitialState)){
        // initialHumanIdToLoad is now set by getScriptForExecution
    } else {
        // Fallback if getScriptForExecution fails catastrophically (should not happen if default is robust)
        // This path should ideally not be hit if getScriptForExecution always returns a default.
        initialHumanIdToLoad = ScriptManager::DEFAULT_SCRIPT_ID; // Use known default
    }

    if (!initialHumanIdToLoad.isEmpty()) {
        log_i("MainCtrl: Initial script determined as '%s'. Triggering render.", initialHumanIdToLoad.c_str());
        triggerScriptRender(initialHumanIdToLoad, false, currentState, currentLoadedScriptId); // false for useAsIsState (fresh)
    } else {
        log_e("MainCtrl: Failed to determine an initial script to load. This should not happen.");
        g_displayManager->showMessage("Initial Load Fail", 150, 15, true, true);
    }
    lastActivityTime = xTaskGetTickCount();
    esp_task_wdt_reset();

    // 3. Check for full refresh intent
    g_systemManager->incrementFreshStartCounter();
    if (g_systemManager->getFreshStartCounter() == 1 || g_systemManager->getFreshStartCounter() > FRESH_START_THRESHOLD) {
        log_i("MainCtrl: Full refresh intended (counter: %d).", g_systemManager->getFreshStartCounter());
        g_systemManager->setFullRefreshIntended(true);
        if (g_systemManager->getFreshStartCounter() > FRESH_START_THRESHOLD) {
            g_systemManager->resetFreshStartCounter(); // Reset counter after threshold
            g_systemManager->incrementFreshStartCounter(); // Set to 1 for next cycle
        }
        if (!g_systemManager->saveSettings()) { // Persist counter and intent
            log_e("MainCtrl: Failed to save settings after fresh start counter update!");
        }
    }
    esp_task_wdt_reset();

    // 4. Initial fetch check
    int lf_year, lf_month, lf_day, lf_hour, lf_min;
    g_systemManager->getLastFetchTimestamp(lf_year, lf_month, lf_day, lf_hour, lf_min);
    RTC_Date currentDate = g_systemManager->getDate();
    RTC_Time currentTime = g_systemManager->getTime();
    bool timeForFetch = false;
    if (lf_year == -1 || currentDate.year != lf_year || currentDate.mon != lf_month || currentDate.day != lf_day) {
        timeForFetch = true; // Different day or never fetched
    } else { // Same day, check time interval (e.g., 2 hours)
        int elapsed_minutes = (currentTime.hour - lf_hour) * 60 + (currentTime.min - lf_min);
        if (elapsed_minutes < 0) elapsed_minutes += 24 * 60; // Crossed midnight
        if (elapsed_minutes >= 120) timeForFetch = true;
    }

    if (g_systemManager->isFullRefreshIntended() || timeForFetch) {
        log_i("MainCtrl: Triggering initial fetch (FullRefresh: %s, TimeForFetch: %s)",
              g_systemManager->isFullRefreshIntended() ? "Yes" : "No", timeForFetch ? "Yes" : "No");
        FetchJob fetchJob;
        fetchJob.full_refresh = g_systemManager->isFullRefreshIntended();
        if (xQueueSend(g_fetchCommandQueue, &fetchJob, pdMS_TO_TICKS(100)) == pdTRUE) {
            currentState = AppState::FETCHING_DATA;
            // DisplayManager message handled by FetchTask or here
            g_displayManager->showMessage(fetchJob.full_refresh ? "Full Refresh..." : "Fetching...", 200, 15);
        } else {
            log_e("MainCtrl: Failed to send initial fetch job.");
        }
    }
    lastActivityTime = xTaskGetTickCount();
    esp_task_wdt_reset();


    // --- Main Loop ---
    for (;;) {
        esp_task_wdt_reset();
        InputEvent inputEvent;
        RenderResultQueueItem renderResultItem; // Use RenderResultQueueItem
        FetchResultQueueItem fetchResultItem;   // Use FetchResultQueueItem
        
        // Log current state periodically (every 30 seconds)
        static TickType_t lastLogTime = 0;
        TickType_t now = xTaskGetTickCount();
        if (now - lastLogTime > pdMS_TO_TICKS(30000)) {
            log_i("MainCtrl: State=%d, CurrentScript='%s'", (int)currentState, currentLoadedScriptId.c_str());
            lastLogTime = now;
        }

        // Check Input Queue (non-blocking)
        if (xQueueReceive(g_inputEventQueue, &inputEvent, 0) == pdTRUE) {
            lastActivityTime = xTaskGetTickCount();
            log_i("MainCtrl: Received input event: %d", (int)inputEvent.type);
            // Stop ongoing render or fetch if significant input
            if (currentState == AppState::RENDERING_SCRIPT) {
                log_i("MainCtrl: Input received during render. Requesting interrupt.");
                xEventGroupSetBits(g_renderTaskEventFlags, RENDER_INTERRUPT_BIT);
            }
            if (currentState == AppState::FETCHING_DATA) {
                // Fetch task interrupt is more complex, might need a flag for NetworkManager
                // For now, let FetchTask complete or handle its own interrupt via NetworkManager flag.
                log_i("MainCtrl: Input received during fetch. Fetch task should handle via its interrupt flag.");
            }

            if (inputEvent.type == InputEventType::NEXT_SCRIPT || inputEvent.type == InputEventType::PREVIOUS_SCRIPT) {
                String selectedHumanId, selectedName;
                if (g_scriptManager->selectNextScript(inputEvent.type == InputEventType::PREVIOUS_SCRIPT, selectedHumanId, selectedName)) {
                    g_displayManager->showMessage(selectedName, 250, 15, true, true);
                    vTaskDelay(pdMS_TO_TICKS(500)); // Display selection message

                    // currentLoadedScriptId will be updated by triggerScriptRender if successful
                    log_i("MainCtrl: Selected script '%s'. Triggering render.", selectedHumanId.c_str());
                    triggerScriptRender(selectedHumanId, false, currentState, currentLoadedScriptId); // false for useAsIsState (fresh)
                } else {
                    // selectNextScript failed or returned default
                    log_w("MainCtrl: selectNextScript failed or no scripts available. Current script: '%s'", currentLoadedScriptId.c_str());
                    // Optionally, re-render current if selection failed but a script is loaded, or render default.
                    // For now, if selection fails, it might have already set to default.
                    // If currentLoadedScriptId is still valid, we could re-render it.
                    // If selectNextScript sets currentLoadedScriptId to default, triggerScriptRender will handle it.
                    if (!selectedHumanId.isEmpty()){ // If selectNextScript provided an ID (even default)
                         triggerScriptRender(selectedHumanId, false, currentState, currentLoadedScriptId);
                    }
                }
            } else if (inputEvent.type == InputEventType::CONFIRM_ACTION) {
                log_i("MainCtrl: Confirm action received. Attempting to re-render current script '%s'.", currentLoadedScriptId.c_str());
                if (!currentLoadedScriptId.isEmpty()) {
                    triggerScriptRender(currentLoadedScriptId, false, currentState, currentLoadedScriptId); // false for useAsIsState (fresh)
                } else {
                    log_w("MainCtrl: No script currently loaded. Cannot re-render on CONFIRM_ACTION.");
                    // Attempt to render default if no script is loaded
                    triggerScriptRender(ScriptManager::DEFAULT_SCRIPT_ID, false, currentState, currentLoadedScriptId);
                }
            }
            // For any other input type, or if a render wasn't triggered, manage state.
            // If a render was triggered, currentState is already RENDERING_SCRIPT.
            // If not, and we are not fetching, go to IDLE.
            if (currentState != AppState::FETCHING_DATA && currentState != AppState::RENDERING_SCRIPT) {
                currentState = AppState::IDLE;
            }
        } // Closes the 'if (xQueueReceive(g_inputEventQueue...' block.

        // Check Render Status Queue (non-blocking)
        if (xQueueReceive(g_renderStatusQueue, &renderResultItem, 0) == pdTRUE) { // Use renderResultItem
            lastActivityTime = xTaskGetTickCount();

            String received_script_id(renderResultItem.script_id); // Construct String from char[]
            String received_error_message(renderResultItem.error_message); // Construct String from char[]

            log_i("MainCtrl: Received render result for '%s'. Success: %s, Interrupted: %s",
                  received_script_id.c_str(), renderResultItem.success ? "Yes":"No", renderResultItem.interrupted ? "Yes":"No");
            
            if (renderResultItem.success) {
                g_scriptManager->saveScriptExecutionState(received_script_id, renderResultItem.final_state);
                if (currentState == AppState::RENDERING_SCRIPT) { // If we were rendering
                    currentState = AppState::IDLE;
                    log_i("MainCtrl: Render successful for '%s'. State -> IDLE.", received_script_id.c_str());
                }
            } else if (renderResultItem.interrupted) {
                g_displayManager->showMessage("Render Interrupted", 200, 15, false, false); // Brief, no clear
                vTaskDelay(pdMS_TO_TICKS(100)); // Short delay
                if (currentState == AppState::RENDERING_SCRIPT) { // If we were rendering
                    currentState = AppState::IDLE;
                    log_i("MainCtrl: Render interrupted for '%s'. State -> IDLE.", received_script_id.c_str());
                }
            } else { // Render failed (not success, not interrupted)
                g_displayManager->showMessage("Render Fail: " + received_script_id, 200, 15, false, false); // Brief, no clear
                if (!received_error_message.isEmpty()) {
                    log_e("Render Error for '%s': %s", received_script_id.c_str(), received_error_message.c_str());
                }
                vTaskDelay(pdMS_TO_TICKS(100)); // Short delay for message visibility

                String scriptToRetry = "";
                if (!received_script_id.isEmpty()) {
                    scriptToRetry = received_script_id;
                    log_i("MainCtrl: Render failed for '%s'. Re-rendering with saved state.", scriptToRetry.c_str());
                } else if (!currentLoadedScriptId.isEmpty()) {
                    scriptToRetry = currentLoadedScriptId;
                    log_i("MainCtrl: Render failed (unknown script_id in result). Re-rendering current '%s' with saved state.", scriptToRetry.c_str());
                } else {
                    log_w("MainCtrl: Render failed, and no script ID available to re-render. Attempting default.");
                    // scriptToRetry remains empty, triggerScriptRender handles "" as default
                }
                
                bool retryQueued = triggerScriptRender(scriptToRetry, true, currentState, currentLoadedScriptId);
                
                if (!retryQueued) {
                    // Retry was attempted but failed to queue. If we were in RENDERING_SCRIPT state from the failed job, transition to IDLE.
                    if (currentState == AppState::RENDERING_SCRIPT) {
                        currentState = AppState::IDLE;
                        log_w("MainCtrl: Render failed for '%s', and retry also failed to queue. State -> IDLE.", scriptToRetry.c_str());
                    }
                }
                // If retryQueued is true, triggerScriptRender already set currentState = AppState::RENDERING_SCRIPT for the new job.
            }
        }

        // Check Fetch Status Queue (non-blocking)
        if (xQueueReceive(g_fetchStatusQueue, &fetchResultItem, 0) == pdTRUE) { // Use fetchResultItem
            lastActivityTime = xTaskGetTickCount();
            
            String fetch_message(fetchResultItem.message); // Construct String from char[]
            log_i("MainCtrl: Received fetch result. Status: %d, Message: %s", (int)fetchResultItem.status, fetch_message.c_str());

            if (fetchResultItem.status == FetchResultStatus::NO_WIFI) {
                log_w("MainCtrl: Fetch failed (NO_WIFI). Silently skipping. No EPD message, no re-render.");
                // User requested to not show message on EPD and not re-render script.
                // Logging to console is maintained.
            } else if (fetchResultItem.status == FetchResultStatus::SUCCESS) {
                g_displayManager->showMessage("Fetch: " + fetch_message, 350, 15, false, true); // Show success message
                vTaskDelay(pdMS_TO_TICKS(1000)); // Display success message for a bit

                g_systemManager->updateLastFetchTimestamp();
                if (g_systemManager->isFullRefreshIntended()) {
                    g_systemManager->setFullRefreshIntended(false);
                }
                if (!g_systemManager->saveSettings()) {
                    log_e("MainCtrl: Failed to save settings after successful fetch!");
                }

                if (fetchResultItem.new_scripts_available) {
                    g_displayManager->showMessage("New Scripts!", 400, 15);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    bool currentStillValid = false;
                    DynamicJsonDocument listDoc(JSON_DOC_CAPACITY_SCRIPT_LIST);
                    if(g_scriptManager->loadScriptList(listDoc) && listDoc.is<JsonArray>()){
                        for(JsonObject item : listDoc.as<JsonArray>()){
                            if(item["id"].as<String>() == currentLoadedScriptId){
                                currentStillValid = true;
                                break;
                            }
                        }
                    }
                    if(!currentStillValid && !currentLoadedScriptId.isEmpty()){
                        log_i("MainCtrl: Current script '%s' no longer in list after fetch. Rendering first available.", currentLoadedScriptId.c_str());
                        RenderJobData newJobData; // script_content is no longer a member
                        ScriptExecState state;
                        // Get script ID, file ID, and state. Content is not loaded here.
                        if(g_scriptManager->getScriptForExecution(newJobData.script_id, newJobData.file_id, state)){
                            if (newJobData.script_id.isEmpty()) { // script_id is humanId
                                log_e("MainCtrl: getScriptForExecution (after fetch) returned empty human_id. Aborting render.");
                            } else {
                                newJobData.initial_state = state;
                                if (state.state_loaded) newJobData.initial_state.counter++; else newJobData.initial_state.counter = 0;
                                RTC_Time now_time = g_systemManager->getTime();
                                newJobData.initial_state.hour = now_time.hour; newJobData.initial_state.minute = now_time.min; newJobData.initial_state.second = now_time.sec;
                                
                                RenderJobQueueItem newJobQueueItem;
                                newJobQueueItem.fromRenderJobData(newJobData); // Copies human_id, file_id, initial_state
                                if (xQueueSend(g_renderCommandQueue, &newJobQueueItem, pdMS_TO_TICKS(100)) == pdTRUE) {
                                    currentState = AppState::RENDERING_SCRIPT;
                                    currentLoadedScriptId = newJobData.script_id; // humanId
                                }
                            }
                        }
                    }
                }
            } else if (fetchResultItem.status == FetchResultStatus::INTERRUPTED_BY_USER) {
                g_displayManager->showMessage("Fetch Interrupted", 200, 15, false, false);
                vTaskDelay(pdMS_TO_TICKS(100));
                // User interrupted, new action likely to follow.
            } else { // Other non-success, non-NO_WIFI, non-INTERRUPTED statuses (e.g., GENUINE_ERROR)
                g_displayManager->showMessage("Fetch Err: " + fetch_message, 200, 15, false, false); // Brief, no clear
                vTaskDelay(pdMS_TO_TICKS(100)); // Short delay for message visibility

                if (!currentLoadedScriptId.isEmpty()) {
                    log_i("MainCtrl: Fetch failed (Error: %s). Re-rendering current script '%s' with saved state.", fetch_message.c_str(), currentLoadedScriptId.c_str());
                    triggerScriptRender(currentLoadedScriptId, true, currentState, currentLoadedScriptId);
                } else {
                    log_w("MainCtrl: Fetch failed (Error: %s), but no current script loaded. Attempting default render.", fetch_message.c_str());
                    triggerScriptRender("", true, currentState, currentLoadedScriptId); // "" triggers default
                }
            }

            // If a fetch operation completed:
            // - If the system was purely in FETCHING_DATA state (and no re-render was triggered), transition to IDLE.
            // - If a RENDER operation was started (current state is RENDERING_SCRIPT), keep that state.
            if (currentState == AppState::FETCHING_DATA) {
                 currentState = AppState::IDLE;
            }
        }
        
        // Sleep Management
        if (currentState == AppState::IDLE && (xTaskGetTickCount() - lastActivityTime) > pdMS_TO_TICKS(SLEEP_IDLE_THRESHOLD_MS)) {
            log_i("MainCtrl: Idle timeout. Going to light sleep.");
            // Removed: g_displayManager->showMessage("Sleeping...", 450, 15, false, true);
            vTaskDelay(pdMS_TO_TICKS(500)); // Delay kept, might be for other purposes or harmless
            
            esp_task_wdt_delete(NULL);
            g_systemManager->goToLightSleep(SystemManager::DEFAULT_SLEEP_DURATION_S); // Use constant
            esp_task_wdt_init(30, true);
            esp_task_wdt_add(NULL);

            lastActivityTime = xTaskGetTickCount();
            log_i("MainCtrl: Woke up. Cause: %d", g_systemManager->getWakeupCause());
            // Removed: g_displayManager->showMessage("Awake!", 50, 15, true, true);
            // Removed: vTaskDelay(pdMS_TO_TICKS(500));

            String scriptIdAfterWakeup = currentLoadedScriptId; // Default to current
            if (scriptIdAfterWakeup.isEmpty()) {
                // If no script was loaded before sleep, try to get the default/first one
                String tempWakeFileIdUnused; // Not used here
                ScriptExecState tempWakeState;
                // Get script ID, file ID (unused), and state. Content not loaded here.
                if(g_scriptManager->getScriptForExecution(scriptIdAfterWakeup, tempWakeFileIdUnused, tempWakeState)){
                    // scriptIdAfterWakeup is now set
                } else {
                    scriptIdAfterWakeup = ScriptManager::DEFAULT_SCRIPT_ID;
                }
            }

            if (!scriptIdAfterWakeup.isEmpty()) {
                // For timer wakeup, useAsIsState = false (fresh render, increments counter, updates time)
                // For GPIO wakeup, an InputEvent will be generated and handled, leading to a fresh render.
                // So, any direct render after wakeup should be 'fresh'.
                log_i("MainCtrl: Woke up. Triggering render for script '%s'.", scriptIdAfterWakeup.c_str());
                triggerScriptRender(scriptIdAfterWakeup, false, currentState, currentLoadedScriptId);
            } else {
                log_e("MainCtrl: Woke up, but failed to determine a script to render.");
            }
            
            // 2. If timer wakeup, check if fetch is due
            if (g_systemManager->getWakeupCause() == ESP_SLEEP_WAKEUP_TIMER) {
                int ly, lm, ld, lh, lmin;
                g_systemManager->getLastFetchTimestamp(ly, lm, ld, lh, lmin);
                RTC_Date cd = g_systemManager->getDate();
                RTC_Time ct = g_systemManager->getTime();
                bool fetchDue = false;
                if (ly == -1 || cd.year != ly || cd.mon != lm || cd.day != ld) {
                    fetchDue = true;
                } else {
                    int elapsed = (ct.hour - lh) * 60 + (ct.min - lmin);
                    if (elapsed < 0) elapsed += 24 * 60;
                    if (elapsed >= 120) fetchDue = true;
                }
                if (fetchDue) {
                    log_i("MainCtrl: Triggering fetch after timer wakeup.");
                    FetchJob fetchJob;
                    fetchJob.full_refresh = g_systemManager->isFullRefreshIntended();
                     if (xQueueSend(g_fetchCommandQueue, &fetchJob, pdMS_TO_TICKS(100)) == pdTRUE) {
                        if (currentState != AppState::RENDERING_SCRIPT) currentState = AppState::FETCHING_DATA;
                        // Display message about fetch starting
                         g_displayManager->showMessage(fetchJob.full_refresh ? "Full Refresh..." : "Fetching...", 200, 15, false, false); // Don't clear if rendering
                    }
                }
            }
        }
        vTaskDelay(MAIN_LOOP_IDLE_DELAY); // Small delay to prevent busy-waiting
    }
}

// --- Input Task ---
// Global reference to the raw input queue for InputManager's ISR.
// This is initialized to NULL here and set by the InputManager's constructor
// to point to its internal raw input queue.
QueueHandle_t g_im_raw_queue_ref = NULL;

void InputTask_Function(void *pvParameters) {
    esp_task_wdt_init(30, true); // Panic on WDT timeout
    esp_task_wdt_add(NULL);

    if (g_inputManager) {
        g_inputManager->taskFunction(); // This is a blocking loop
    } else {
        log_e("InputTask: g_inputManager is NULL!");
    }
    // Should not reach here if taskFunction is an infinite loop
    log_e("InputTask_Function exiting unexpectedly!");
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

// --- Render Task ---
void RenderTask_Function(void *pvParameters) {
    esp_task_wdt_init(60, true); // Longer timeout for rendering, panic on WDT timeout
    esp_task_wdt_add(NULL);

    RenderController renderCtrl(*g_displayManager); // Create RenderController instance for this task

    RenderJobQueueItem jobItem; // Use RenderJobQueueItem
    // WDT timeout for RenderTask is 60s. We'll use a 30s queue receive timeout.
    const TickType_t queueReceiveTimeout = pdMS_TO_TICKS(30000);

    for (;;) {
        esp_task_wdt_reset(); // Reset WDT at the start of each loop iteration.
        if (xQueueReceive(g_renderCommandQueue, &jobItem, queueReceiveTimeout) == pdTRUE) {
            // Construct RenderJobData. Script content will be loaded into jobData.script_content.
            RenderJobData jobDataForRenderCtrl; // This will hold all data for RenderController
            jobDataForRenderCtrl.script_id = String(jobItem.human_id);
            jobDataForRenderCtrl.file_id = String(jobItem.file_id);
            jobDataForRenderCtrl.initial_state = jobItem.initial_state;
            
            String script_content_for_parser; // Local string to hold content for parser

            log_i("RenderTask: Received job for human_id: %s, file_id: %s", jobDataForRenderCtrl.script_id.c_str(), jobDataForRenderCtrl.file_id.c_str());

            // Load script content
            if (jobDataForRenderCtrl.file_id == ScriptManager::DEFAULT_SCRIPT_ID) {
                log_i("RenderTask: Using built-in default script content for '%s'", jobDataForRenderCtrl.script_id.c_str());
                // Get default content directly from ScriptManager constant or method
                // For simplicity, assuming ScriptManager::loadScriptContent handles DEFAULT_SCRIPT_ID correctly by returning built-in content.
                // Or, we can assign it directly here:
                script_content_for_parser = ScriptManager::DEFAULT_SCRIPT_CONTENT; // Access public static member
            } else if (!g_scriptManager->loadScriptContent(jobDataForRenderCtrl.file_id, script_content_for_parser)) {
                log_e("RenderTask: Failed to load script content for fileId: %s (humanId: %s)", jobDataForRenderCtrl.file_id.c_str(), jobDataForRenderCtrl.script_id.c_str());
                RenderResultData errorResultData;
                errorResultData.script_id = jobDataForRenderCtrl.script_id;
                errorResultData.success = false;
                errorResultData.interrupted = false;
                errorResultData.error_message = "RenderTask: Failed to load script content.";
                // final_state will be default
                
                RenderResultQueueItem errorResultQueueItem;
                errorResultQueueItem.fromRenderResultData(errorResultData);
                if (xQueueSend(g_renderStatusQueue, &errorResultQueueItem, pdMS_TO_TICKS(100)) != pdTRUE) {
                    log_e("RenderTask: Failed to send error render status for %s", jobDataForRenderCtrl.script_id.c_str());
                }
                continue; // Skip to next job
            }
            log_i("RenderTask: Content loaded for script ID: %s", jobDataForRenderCtrl.script_id.c_str());
            
            RenderResultData resultData; // To store result from RenderController

            // Create a temporary RenderJobData that includes the script_content for the parser
            // This is a bit awkward as RenderJobData struct itself doesn't have script_content anymore.
            // RenderController::renderScript expects a struct with content.
            // Let's define a temporary struct or pass content separately to renderCtrl.renderScript.
            // For minimal change to RenderController, we'll pass content separately.
            // Modifying RenderController::renderScript signature:
            // RenderResultData renderScript(const String& scriptId, const String& fileId, const String& scriptContent, const ScriptExecState& initialState);

            // Create a temporary struct for RenderController, or adapt RenderController
            // For now, let's assume RenderController's renderScript method is adapted to take content separately.
            // Or, we make a local struct for RenderController that *does* include content.
            // Let's make RenderController::renderScript take content as a separate parameter.
            // This requires changing RenderController.h and .cpp.
            // For now, to proceed with this file's changes, we'll assume RenderController is adapted.
            // The RenderJobData passed to renderCtrl.renderScript will be jobDataForRenderCtrl,
            // and script_content_for_parser will be passed as an additional argument.
            // This means RenderController.h/cpp needs:
            // RenderResultData renderScript(const RenderJobData& job_meta_data, const String& script_content_payload);

            if (g_displayManager->lockEPD(pdMS_TO_TICKS(1000))) { // Lock EPD, 1s timeout
                // Clear any pending interrupt bit before starting
                xEventGroupClearBits(g_renderTaskEventFlags, RENDER_INTERRUPT_BIT);
                
                // Pass jobDataForRenderCtrl (meta) and script_content_for_parser (payload)
                // This requires RenderController::renderScript to be updated.
                // For this commit, we'll assume RenderController is updated.
                // A struct specifically for RenderController input might be cleaner.
                // Let's assume RenderController::renderScript is updated to:
                // renderScript(const String& script_id, const String& script_content, const ScriptExecState& initial_state)
                // (file_id is not directly needed by parser/runtime if content is provided)
                
                resultData = renderCtrl.renderScript(jobDataForRenderCtrl.script_id, script_content_for_parser, jobDataForRenderCtrl.initial_state);
                
                // Check if MainControlTask signaled an interrupt during the process
                EventBits_t uxBits = xEventGroupGetBits(g_renderTaskEventFlags);
                if (uxBits & RENDER_INTERRUPT_BIT) {
                    log_i("RenderTask: Interrupt bit was set by MainControlTask during/after render.");
                    renderCtrl.requestInterrupt(); // Signal interrupt to RenderController
                    // Result data will be updated by renderCtrl if interrupt occurred
                    xEventGroupClearBits(g_renderTaskEventFlags, RENDER_INTERRUPT_BIT); // Clear the bit
                }

                // After rendering is complete (or interrupted), push the canvas
                // The DisplayListRenderer now handles clearing the canvas.
                // The final push to EPD happens here.
                // Directly call canvas push since mutex is already held by RenderTask.
                M5EPD_Canvas* canvas = g_displayManager->getCanvas();
                if (canvas) {
                    canvas->pushCanvas(0, 0, UPDATE_MODE_GC16); // Or appropriate mode
                } else {
                    log_e("RenderTask: Failed to get canvas from DisplayManager.");
                }
                
                g_displayManager->unlockEPD(); // Unlock EPD
            } else {
                log_e("RenderTask: Failed to lock EPD for rendering script %s", jobDataForRenderCtrl.script_id.c_str());
                resultData.script_id = jobDataForRenderCtrl.script_id; // Populate for error reporting
                resultData.success = false;
                resultData.interrupted = false; // Not interrupted by user, but by system issue
                resultData.error_message = "Failed to acquire display lock for rendering.";
                resultData.final_state = jobDataForRenderCtrl.initial_state; // Preserve initial state on this type of error
            }

            RenderResultQueueItem resultQueueItem;
            resultQueueItem.fromRenderResultData(resultData); // Convert to char[] based for queue

            if (xQueueSend(g_renderStatusQueue, &resultQueueItem, pdMS_TO_TICKS(100)) != pdTRUE) {
                log_e("RenderTask: Failed to send render status for %s", jobDataForRenderCtrl.script_id.c_str());
            }
        }
    }
}

// --- Fetch Task ---
void FetchTask_Function(void *pvParameters) {
    esp_task_wdt_init(120, true); // Long timeout for network ops, panic on WDT timeout
    esp_task_wdt_add(NULL);

    volatile bool user_interrupt_flag_for_network_manager = false;
    if (g_networkManager) {
        g_networkManager->setInterruptFlag(&user_interrupt_flag_for_network_manager);
    }

    FetchJob job; // FetchJob is simple, no Strings, can remain as is
    DynamicJsonDocument serverListDoc(JSON_DOC_CAPACITY_SCRIPT_LIST);
    DynamicJsonDocument scriptContentDoc(SCRIPT_CONTENT_JSON_CAPACITY); // From event_defs.h capacity
    
    // WDT timeout for FetchTask is 120s. We'll use a 60s queue receive timeout.
    const TickType_t queueReceiveTimeout = pdMS_TO_TICKS(60000);

    for (;;) {
        esp_task_wdt_reset(); // Reset WDT at the start of each loop iteration.
        user_interrupt_flag_for_network_manager = false; // Reset before waiting for new job

        if (xQueueReceive(g_fetchCommandQueue, &job, queueReceiveTimeout) == pdTRUE) {
            log_i("FetchTask: Received job. Full Refresh: %s", job.full_refresh ? "Yes" : "No");
            
            // Clear JsonDocuments before use for this job
            serverListDoc.clear();
            scriptContentDoc.clear();

            FetchResultData resultData; // Use FetchResultData for internal logic
            resultData.new_scripts_available = false; // Default
            resultData.status = FetchResultStatus::GENUINE_ERROR; // Default
            
            // Connect WiFi
            if (!g_networkManager->connectWiFi()) { // connectWiFi uses its internal interrupt flag
                resultData.status = FetchResultStatus::NO_WIFI;
                resultData.message = "WiFi Connect Fail";
            } else {
                // Perform fetch operations
                // JsonDocument serverListDoc; // Moved declaration outside loop
                // JsonDocument scriptContentDoc; // Moved declaration outside loop
            
                if (job.full_refresh) {
                    g_scriptManager->clearAllScriptData(); // Clear local data first for full refresh
                    esp_task_wdt_reset();
                }

                resultData.status = g_networkManager->fetchScriptList(serverListDoc);
                esp_task_wdt_reset();

                if (resultData.status == FetchResultStatus::SUCCESS) {
                        JsonArray serverList = serverListDoc.as<JsonArray>(); // This is safe if serverListDoc is an array
                        log_i("FetchTask: Fetched server list with %d scripts.", serverList.size());
                        
                        JsonDocument localListDoc; // Use JsonDocument
                        bool localListExists = g_scriptManager->loadScriptList(localListDoc);
                        if (!localListExists || localListDoc.as<JsonArray>().size() != serverList.size()) {
                        resultData.new_scripts_available = true;
                    }

                        // Before saving, check if list is valid
                        log_d("FetchTask: Before saveScriptList - serverListDoc type: isArray=%d, isObject=%d, isNull=%d, size=%d", serverListDoc.is<JsonArray>(), serverListDoc.is<JsonObject>(), serverListDoc.isNull(), serverListDoc.is<JsonArray>() ? serverListDoc.as<JsonArray>().size() : 0);
                        
                        esp_task_wdt_reset(); // Reset WDT before saving script list
                        if (!serverListDoc.is<JsonArray>()) {
                            log_e("FetchTask: Server list document (serverListDoc) is not a JSON array before saving!");
                            resultData.status = FetchResultStatus::GENUINE_ERROR;
                            resultData.message = "Invalid List Format (Pre-Save Check)";
                        } else {
                            // Extra verification of list structure
                            JsonArrayConst serverListConstView = serverListDoc.as<JsonArrayConst>(); // Use const view for safety
                            bool listIsValid = true;
                            
                            // Check that it's not empty
                            if (serverListConstView.size() == 0) {
                                log_w("FetchTask: Server returned empty script list");
                                // Continue anyway, empty array is valid
                            }
                            
                            // Log the list structure
                            log_i("FetchTask: Downloaded list contains %u scripts", serverListConstView.size());
                            
                            // Attempt to save list to filesystem
                            if (!g_scriptManager->saveScriptList(serverListDoc)) {
                                log_e("FetchTask: Failed to save script list to filesystem");
                                
                                // Try to diagnose the error
                                log_e("FetchTask: serverListDoc state after saveScriptList failed: isArray=%d, isObject=%d, isNull=%d, size=%d",
                                     serverListDoc.is<JsonArray>(), serverListDoc.is<JsonObject>(), serverListDoc.isNull(), serverListDoc.is<JsonArray>() ? serverListDoc.as<JsonArray>().size() : 0);
                                
                                if (serverListDoc.is<JsonArray>()) { // Check again, in case it was valid but save failed for other reasons
                                    log_e("FetchTask: List (serverListDoc) contains %u items but save failed",
                                          serverListDoc.as<JsonArray>().size());
                                    
                                    // Detailed logging of the first few items
                                    int itemsToLog = min(3, (int)serverListDoc.as<JsonArray>().size());
                                    for (int i = 0; i < itemsToLog; i++) {
                                        JsonVariant item = serverListDoc.as<JsonArray>()[i];
                                        if (item.is<JsonObject>()) {
                                            JsonObject obj = item.as<JsonObject>();
                                            const char* id = obj["id"].as<const char*>();
                                            log_e("FetchTask: Item %d - id: %s", i, id ? id : "null");
                                        } else {
                                            log_e("FetchTask: Item %d is not an object", i);
                                        }
                                    }
                                }
                                
                                resultData.status = FetchResultStatus::GENUINE_ERROR;
                                resultData.message = "Save List Fail";
                            } else {
                                log_i("FetchTask: Successfully saved script list to filesystem");
                                
                                // Now fetch content for each script
                                bool allContentFetched = true;
                                int successCount = 0;
                                int failCount = 0;
                                
                                for (JsonObject scriptInfo : serverList) {
                                    esp_task_wdt_reset();
                                    
                                    // Check if script has valid ID
                                    const char* humanId = scriptInfo["id"].as<const char*>();
                                    if (!humanId || strlen(humanId) == 0) {
                                        log_w("FetchTask: Skipping script with missing/empty ID");
                                        failCount++;
                                        continue;
                                    }
                                    
                                    log_i("FetchTask: Fetching content for script '%s'", humanId);
                                    
                                    // Fetch script content
                                    FetchResultStatus contentStatus = g_networkManager->fetchScriptContent(humanId, scriptContentDoc);
                                    
                                    if (contentStatus == FetchResultStatus::SUCCESS) {
                                        // Double-check content validity
                                        if (!scriptContentDoc.is<JsonObject>() ||
                                            !scriptContentDoc["content"].is<const char*>()) {
                                            
                                            log_e("FetchTask: Invalid content response structure for '%s'", humanId);
                                            allContentFetched = false;
                                            failCount++;
                                            continue;
                                        }
                                        
                                        const char* content = scriptContentDoc["content"].as<const char*>();
                                        if (!content || strlen(content) == 0) {
                                            log_e("FetchTask: Empty content for '%s'", humanId);
                                            allContentFetched = false;
                                            failCount++;
                                            continue;
                                        }
                                        
                                        // Determine fileId - ensure we have a valid short id for storage
                                        String fileId;
                                        if (!scriptInfo["fileId"].isNull() && scriptInfo["fileId"].is<const char*>()) {
                                            fileId = scriptInfo["fileId"].as<String>();
                                            // Check if fileId is empty, "null", or doesn't follow our format (s+number)
                                            if (fileId.isEmpty() || fileId == "null" || !fileId.startsWith("s")) {
                                                log_w("FetchTask: Script '%s' has invalid fileId '%s', generating short fileId",
                                                      humanId, fileId.c_str());
                                                
                                                // Generate a new short fileId - format: "s" + number
                                                fileId = g_scriptManager->generateShortFileId(humanId);
                                                
                                                // Update the fileId in both places to ensure consistency
                                                scriptInfo["fileId"] = fileId;
                                                scriptContentDoc["fileId"] = fileId;
                                            }
                                        } else {
                                            log_w("FetchTask: Script '%s' has no fileId field or it's not a string, generating short fileId", humanId);
                                            
                                            // Generate a new short fileId - format: "s" + number
                                            fileId = g_scriptManager->generateShortFileId(humanId);
                                            
                                            // Add the fileId to both documents
                                            scriptInfo["fileId"] = fileId;
                                            scriptContentDoc["fileId"] = fileId;
                                        }
                                        
                                        // Save content to filesystem
                                        log_i("FetchTask: Saving content for '%s' (length: %u bytes)",
                                             humanId, strlen(content));
                                        
                                        esp_task_wdt_reset(); // Reset WDT before saving script content
                                        if (!g_scriptManager->saveScriptContent(fileId, content)) {
                                            log_e("FetchTask: Failed to save content for '%s'", humanId);
                                            allContentFetched = false;
                                            failCount++;
                                        } else {
                                            log_i("FetchTask: Successfully saved content for '%s'", humanId);
                                            successCount++;
                                        }
                                    } else if (contentStatus == FetchResultStatus::INTERRUPTED_BY_USER) {
                                        log_i("FetchTask: Content fetch for '%s' interrupted by user", humanId);
                                        resultData.status = FetchResultStatus::INTERRUPTED_BY_USER;
                                        allContentFetched = false;
                                        break; // Exit the for loop
                                    } else { // Other errors (GENUINE_ERROR, NO_WIFI)
                                        log_e("FetchTask: Failed to fetch content for '%s' (status: %d)",
                                             humanId, (int)contentStatus);
                                        allContentFetched = false;
                                        failCount++;
                                    }
                                    
                                    // Check for manual interrupt
                                    if (user_interrupt_flag_for_network_manager) {
                                        log_i("FetchTask: Script content fetch loop interrupted by user");
                                        break;
                                    }
                                }
                                
                                // Log final fetch statistics
                                log_i("FetchTask: Content fetch complete - Success: %d, Failed: %d, Total: %d",
                                     successCount, failCount, (int)serverList.size());

                        if (resultData.status != FetchResultStatus::INTERRUPTED_BY_USER) {
                            if (allContentFetched) {
                                resultData.message = job.full_refresh ? "Full Refresh OK" : "Fetch OK";
                                resultData.status = FetchResultStatus::SUCCESS;
                                esp_task_wdt_reset(); // Reset WDT before cleanup
                                g_scriptManager->cleanupOrphanedContent(serverList);
                                esp_task_wdt_reset(); // Reset WDT before next cleanup
                                g_scriptManager->cleanupOrphanedStates(serverList);
                            } else {
                                resultData.message = "Partial Fetch";
                                resultData.status = FetchResultStatus::GENUINE_ERROR;
                            }
                        }
                    }
                } // Closes the 'else' block for 'if (!serverListDoc.is<JsonArray>())' or 'if (!g_scriptManager->saveScriptList(serverListDoc))'
                } // Closes the main 'if (resultData.status == FetchResultStatus::SUCCESS)' block (started at line 621)
                else if (resultData.status == FetchResultStatus::INTERRUPTED_BY_USER) {
                     resultData.message = "Fetch Interrupted";
                } else {
                    resultData.message = "Fetch List Fail";
                }
                g_networkManager->disconnectWiFi();
            } // End if WiFi connected

            FetchResultQueueItem resultQueueItem;
            resultQueueItem.fromFetchResultData(resultData); // Convert to char[] based for queue

            if (xQueueSend(g_fetchStatusQueue, &resultQueueItem, pdMS_TO_TICKS(100)) != pdTRUE) {
                log_e("FetchTask: Failed to send fetch status.");
            }
        } // End if receive from queue
    } // End for(;;)
}


// --- Main Loop (not used in FreeRTOS) ---
void loop() {
    // This function is not called when using FreeRTOS.
    // Tasks handle all ongoing operations.
    vTaskDelay(portMAX_DELAY); // Should not be reached
}
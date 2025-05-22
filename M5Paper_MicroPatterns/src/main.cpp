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
        } else {
            g_displayManager->showMessage("Time Sync Fail", 100, 15);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_task_wdt_reset();

    // 2. Initial script load and render
    RenderJobData initialRenderJobData; // Use RenderJobData for internal logic
    ScriptExecState initialScriptState;
    // Get humanId, fileId, content, and state
    if (g_scriptManager->getScriptForExecution(initialRenderJobData.script_id, initialRenderJobData.file_id, initialRenderJobData.script_content, initialScriptState)) {
        if (initialRenderJobData.script_id.isEmpty()) { // script_id is humanId
            log_e("MainCtrl: getScriptForExecution returned empty human_id for initial load. Aborting initial render.");
            g_displayManager->showMessage("Empty ID Fail", 150, 15);
        } else {
            initialRenderJobData.initial_state = initialScriptState;
            // Increment counter for first boot execution if state was loaded, or set to 0 if not
            if (initialScriptState.state_loaded) {
                initialRenderJobData.initial_state.counter++;
            } else {
                initialRenderJobData.initial_state.counter = 0;
            }
            // Update time from RTC for this first execution
            RTC_Time now_time = g_systemManager->getTime();
            initialRenderJobData.initial_state.hour = now_time.hour;
            initialRenderJobData.initial_state.minute = now_time.min;
            initialRenderJobData.initial_state.second = now_time.sec;

            log_i("MainCtrl: Initial render job for script '%s', counter %d", initialRenderJobData.script_id.c_str(), initialRenderJobData.initial_state.counter);
            
            RenderJobQueueItem initialRenderJobQueueItem;
            initialRenderJobQueueItem.fromRenderJobData(initialRenderJobData);

            if (xQueueSend(g_renderCommandQueue, &initialRenderJobQueueItem, pdMS_TO_TICKS(100)) == pdTRUE) {
                currentState = AppState::RENDERING_SCRIPT;
                currentLoadedScriptId = initialRenderJobData.script_id;
            } else {
                log_e("MainCtrl: Failed to send initial render job.");
                g_displayManager->showMessage("Render Q Fail", 150, 15);
            }
        }
    } else {
        log_e("MainCtrl: Failed to get initial script for execution.");
        g_displayManager->showMessage("Script Load Fail", 150, 15);
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
        g_systemManager->saveSettings(); // Persist counter and intent
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

                    currentLoadedScriptId = selectedHumanId; // Update tracking

                    // Prepare and send a render job for the newly selected script
                    RenderJobData newRenderJobData;
                    ScriptExecState newScriptState;
                    String newFileId;
                    String tempContent; // script_content from getScriptForExecution, not strictly needed for queue item

                    if (g_scriptManager->getScriptForExecution(selectedHumanId, newFileId, tempContent, newScriptState)) {
                        if (selectedHumanId.isEmpty()) {
                            log_e("MainCtrl: getScriptForExecution returned empty human_id for '%s'. Aborting render.", selectedHumanId.c_str());
                            currentState = AppState::IDLE;
                        } else {
                            newRenderJobData.script_id = selectedHumanId;
                            newRenderJobData.file_id = newFileId;
                            newRenderJobData.initial_state = newScriptState;
                            // script_content is not set in newRenderJobData as RenderTask loads it.

                            if (newScriptState.state_loaded) {
                                newRenderJobData.initial_state.counter++;
                            } else {
                                newRenderJobData.initial_state.counter = 0;
                            }
                            RTC_Time now_time = g_systemManager->getTime();
                            newRenderJobData.initial_state.hour = now_time.hour;
                            newRenderJobData.initial_state.minute = now_time.min;
                            newRenderJobData.initial_state.second = now_time.sec;

                            log_i("MainCtrl: Queuing render job for selected script '%s', file_id '%s', counter %d",
                                  newRenderJobData.script_id.c_str(), newRenderJobData.file_id.c_str(), newRenderJobData.initial_state.counter);
                            
                            RenderJobQueueItem newRenderJobQueueItem;
                            newRenderJobQueueItem.fromRenderJobData(newRenderJobData);

                            if (xQueueSend(g_renderCommandQueue, &newRenderJobQueueItem, pdMS_TO_TICKS(100)) == pdTRUE) {
                                currentState = AppState::RENDERING_SCRIPT;
                            } else {
                                log_e("MainCtrl: Failed to send render job for selected script '%s'.", selectedHumanId.c_str());
                                g_displayManager->showMessage("Render Q Fail", 150, 15);
                                currentState = AppState::IDLE;
                            }
                        }
                    } else {
                        log_e("MainCtrl: Failed to get script details for execution for selected script '%s'.", selectedHumanId.c_str());
                        g_displayManager->showMessage("Script Load Fail", 150, 15);
                        currentState = AppState::IDLE;
                    }
                }
            } else if (inputEvent.type == InputEventType::CONFIRM_ACTION) {
                log_i("MainCtrl: Confirm action received. Attempting to re-render current script.");

                if (currentState == AppState::RENDERING_SCRIPT) { // Only block if already rendering
                    log_w("MainCtrl: Already rendering. Ignoring CONFIRM_ACTION for re-render.");
                } else if (currentLoadedScriptId.isEmpty()) {
                    log_w("MainCtrl: No script currently loaded. Cannot re-render on CONFIRM_ACTION.");
                    // If fetching, currentState remains FETCHING_DATA. If IDLE, remains IDLE.
                } else {
                    // Re-render the current script
                    RenderJobData rerenderJobData;
                    ScriptExecState currentScriptState;
                    String currentFileId;
                    String tempContent; // script_content from getScriptForExecution, not strictly needed for queue item

                    if (g_scriptManager->getScriptForExecution(currentLoadedScriptId, currentFileId, tempContent, currentScriptState)) {
                        rerenderJobData.script_id = currentLoadedScriptId;
                        rerenderJobData.file_id = currentFileId;
                        rerenderJobData.initial_state = currentScriptState;

                        if (currentScriptState.state_loaded) {
                            rerenderJobData.initial_state.counter++;
                        } else {
                            // If state wasn't loaded (e.g. first time for this script), start counter at 0.
                            // getScriptForExecution should initialize counter to 0 if not loaded.
                            // This ensures counter increments correctly or starts fresh.
                            rerenderJobData.initial_state.counter = currentScriptState.counter; // Use value from getScriptForExecution
                            if (!currentScriptState.state_loaded) rerenderJobData.initial_state.counter = 0; // Explicitly 0 if not loaded
                            else rerenderJobData.initial_state.counter++; // Increment if loaded
                        }
                        RTC_Time now_time = g_systemManager->getTime();
                        rerenderJobData.initial_state.hour = now_time.hour;
                        rerenderJobData.initial_state.minute = now_time.min;
                        rerenderJobData.initial_state.second = now_time.sec;

                        log_i("MainCtrl: Queuing re-render job for script '%s', file_id '%s', counter %d",
                              rerenderJobData.script_id.c_str(), rerenderJobData.file_id.c_str(), rerenderJobData.initial_state.counter);
                        
                        RenderJobQueueItem rerenderJobQueueItem;
                        rerenderJobQueueItem.fromRenderJobData(rerenderJobData);

                        if (xQueueSend(g_renderCommandQueue, &rerenderJobQueueItem, pdMS_TO_TICKS(100)) == pdTRUE) {
                            currentState = AppState::RENDERING_SCRIPT;
                        } else {
                            log_e("MainCtrl: Failed to send re-render job for script '%s'.", currentLoadedScriptId.c_str());
                            g_displayManager->showMessage("Re-Render Q Fail", 150, 15);
                            currentState = AppState::IDLE;
                        }
                    } else {
                        log_e("MainCtrl: Failed to get script details for execution for re-render of script '%s'.", currentLoadedScriptId.c_str());
                        g_displayManager->showMessage("Script Load Fail", 150, 15);
                        currentState = AppState::IDLE;
                    }
                }
            }
                 // For any other input type, if not fetching or rendering, go to IDLE.
                 if (currentState != AppState::FETCHING_DATA && currentState != AppState::RENDERING_SCRIPT) {
                    currentState = AppState::IDLE;
                 }
            }

        // Check Render Status Queue (non-blocking)
        if (xQueueReceive(g_renderStatusQueue, &renderResultItem, 0) == pdTRUE) { // Use renderResultItem
            lastActivityTime = xTaskGetTickCount();

            String received_script_id(renderResultItem.script_id); // Construct String from char[]
            String received_error_message(renderResultItem.error_message); // Construct String from char[]

            log_i("MainCtrl: Received render result for '%s'. Success: %s, Interrupted: %s",
                  received_script_id.c_str(), renderResultItem.success ? "Yes":"No", renderResultItem.interrupted ? "Yes":"No");
            
            if (renderResultItem.success && !renderResultItem.interrupted) {
                g_scriptManager->saveScriptExecutionState(received_script_id, renderResultItem.final_state);
            } else if (renderResultItem.interrupted) {
                g_displayManager->showMessage("Render Interrupted", 300, 15, false, true);
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else { // Render failed
                g_displayManager->showMessage("Render Failed: " + received_script_id, 300, 15, false, true);
                if (!received_error_message.isEmpty()) {
                    log_e("Render Error: %s", received_error_message.c_str());
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            if (currentState == AppState::RENDERING_SCRIPT) currentState = AppState::IDLE;
        }

        // Check Fetch Status Queue (non-blocking)
        if (xQueueReceive(g_fetchStatusQueue, &fetchResultItem, 0) == pdTRUE) { // Use fetchResultItem
            lastActivityTime = xTaskGetTickCount();
            
            String fetch_message(fetchResultItem.message); // Construct String from char[]

            log_i("MainCtrl: Received fetch result. Status: %d", (int)fetchResultItem.status);
            g_displayManager->showMessage("Fetch: " + fetch_message, 350, 15, false, true);
            vTaskDelay(pdMS_TO_TICKS(1500));

            if (fetchResultItem.status == FetchResultStatus::SUCCESS) {
                g_systemManager->updateLastFetchTimestamp();
                if (g_systemManager->isFullRefreshIntended()) {
                    g_systemManager->setFullRefreshIntended(false);
                }
                g_systemManager->saveSettings();

                if (fetchResultItem.new_scripts_available) {
                    g_displayManager->showMessage("New Scripts!", 400, 15);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    bool currentStillValid = false;
                    JsonDocument listDoc;
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
                        RenderJobData newJobData; ScriptExecState state; // Use RenderJobData
                        if(g_scriptManager->getScriptForExecution(newJobData.script_id, newJobData.file_id, newJobData.script_content, state)){
                            if (newJobData.script_id.isEmpty()) { // script_id is humanId
                                log_e("MainCtrl: getScriptForExecution (after fetch) returned empty human_id. Aborting render.");
                            } else {
                                newJobData.initial_state = state;
                                if (state.state_loaded) newJobData.initial_state.counter++; else newJobData.initial_state.counter = 0;
                                RTC_Time now_time = g_systemManager->getTime();
                                newJobData.initial_state.hour = now_time.hour; newJobData.initial_state.minute = now_time.min; newJobData.initial_state.second = now_time.sec;
                                
                                RenderJobQueueItem newJobQueueItem;
                                newJobQueueItem.fromRenderJobData(newJobData); // This now copies human_id, file_id, initial_state
                                if (xQueueSend(g_renderCommandQueue, &newJobQueueItem, pdMS_TO_TICKS(100)) == pdTRUE) {
                                    currentState = AppState::RENDERING_SCRIPT;
                                    currentLoadedScriptId = newJobData.script_id; // humanId
                                }
                            }
                        }
                    }
                }
            }
            // If a fetch operation completed:
            // - If the system was purely in FETCHING_DATA state, transition to IDLE.
            // - If a RENDER operation was started during the fetch (current state is RENDERING_SCRIPT),
            //   then keep the state as RENDERING_SCRIPT.
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

            RenderJobData wakeRenderJobData; // Use RenderJobData
            ScriptExecState wakeScriptState;
            if (g_scriptManager->getScriptForExecution(wakeRenderJobData.script_id, wakeRenderJobData.file_id, wakeRenderJobData.script_content, wakeScriptState)) {
                if (wakeRenderJobData.script_id.isEmpty()) { // script_id is humanId
                    log_e("MainCtrl: getScriptForExecution (after wakeup) returned empty human_id. Aborting render.");
                } else {
                    wakeRenderJobData.initial_state = wakeScriptState;
                    if (g_systemManager->getWakeupCause() == ESP_SLEEP_WAKEUP_TIMER) {
                         if (wakeScriptState.state_loaded) wakeRenderJobData.initial_state.counter++;
                         else wakeRenderJobData.initial_state.counter = 0;
                    }
                    RTC_Time now_time = g_systemManager->getTime();
                    wakeRenderJobData.initial_state.hour = now_time.hour;
                    wakeRenderJobData.initial_state.minute = now_time.min;
                    wakeRenderJobData.initial_state.second = now_time.sec;

                    RenderJobQueueItem wakeRenderJobQueueItem;
                    wakeRenderJobQueueItem.fromRenderJobData(wakeRenderJobData); // This now copies human_id, file_id, initial_state
                    if (xQueueSend(g_renderCommandQueue, &wakeRenderJobQueueItem, pdMS_TO_TICKS(100)) == pdTRUE) {
                        currentState = AppState::RENDERING_SCRIPT;
                        currentLoadedScriptId = wakeRenderJobData.script_id; // humanId
                    }
                }
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
            // Construct RenderJobData, loading script content here
            RenderJobData jobData;
            jobData.script_id = String(jobItem.human_id); // This is human_id
            jobData.file_id = String(jobItem.file_id);
            jobData.initial_state = jobItem.initial_state;

            log_i("RenderTask: Received job for human_id: %s, file_id: %s", jobData.script_id.c_str(), jobData.file_id.c_str());

            // Check if it's the default script
            if (jobData.file_id == "default_fallback_script") {
                log_i("RenderTask: Using built-in default script content for '%s'", jobData.script_id.c_str());
                // Default script content will be provided by ScriptManager
                if (!g_scriptManager->loadScriptContent(jobData.file_id, jobData.script_content)) {
                    // Even if file load fails, we should still have the default content in some cases
                    if (jobData.script_content.isEmpty()) {
                        // This shouldn't happen with our changes to getScriptForExecution
                        log_w("RenderTask: Default script content is empty, attempting to load built-in content");
                        // Get default content directly from ScriptManager constant
                        String defaultId, defaultFileId;
                        ScriptExecState defaultState;
                        g_scriptManager->getScriptForExecution(defaultId, defaultFileId, jobData.script_content, defaultState);
                    }
                }
            } else if (!g_scriptManager->loadScriptContent(jobData.file_id, jobData.script_content)) {
                log_e("RenderTask: Failed to load script content for fileId: %s (humanId: %s)", jobData.file_id.c_str(), jobData.script_id.c_str());
                RenderResultData errorResultData;
                errorResultData.script_id = jobData.script_id;
                errorResultData.success = false;
                errorResultData.interrupted = false;
                errorResultData.error_message = "RenderTask: Failed to load script content.";
                // final_state will be default
                
                RenderResultQueueItem errorResultQueueItem;
                errorResultQueueItem.fromRenderResultData(errorResultData);
                if (xQueueSend(g_renderStatusQueue, &errorResultQueueItem, pdMS_TO_TICKS(100)) != pdTRUE) {
                    log_e("RenderTask: Failed to send error render status for %s", jobData.script_id.c_str());
                }
                continue; // Skip to next job
            }
            log_i("RenderTask: Content loaded for script ID: %s", jobData.script_id.c_str());
            
            RenderResultData resultData; // To store result from RenderController

            if (g_displayManager->lockEPD(pdMS_TO_TICKS(1000))) { // Lock EPD, 1s timeout
                // Clear any pending interrupt bit before starting
                xEventGroupClearBits(g_renderTaskEventFlags, RENDER_INTERRUPT_BIT);
                
                resultData = renderCtrl.renderScript(jobData); // This will use the canvas and push it
                
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
                    canvas->pushCanvas(0, 0, UPDATE_MODE_GLD16); // Or appropriate mode
                } else {
                    log_e("RenderTask: Failed to get canvas from DisplayManager.");
                }
                
                g_displayManager->unlockEPD(); // Unlock EPD
            } else {
                log_e("RenderTask: Failed to lock EPD for rendering script %s", jobData.script_id.c_str());
                resultData.script_id = jobData.script_id; // Populate for error reporting
                resultData.success = false;
                resultData.interrupted = false; // Not interrupted by user, but by system issue
                resultData.error_message = "Failed to acquire display lock for rendering.";
                // final_state will be default
            }

            RenderResultQueueItem resultQueueItem;
            resultQueueItem.fromRenderResultData(resultData); // Convert to char[] based for queue

            if (xQueueSend(g_renderStatusQueue, &resultQueueItem, pdMS_TO_TICKS(100)) != pdTRUE) {
                log_e("RenderTask: Failed to send render status for %s", jobData.script_id.c_str());
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
    JsonDocument serverListDoc; // Use JsonDocument - declare outside loop if reused, or inside if per-job
    JsonDocument scriptContentDoc; // Use JsonDocument - declare outside loop if reused, or inside if per-job
    
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
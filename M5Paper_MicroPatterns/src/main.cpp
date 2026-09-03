#include "main.h"
#include "esp32-hal-log.h"
#include "esp_task_wdt.h" // For watchdog
#include "esp_heap_caps.h" // heap integrity probe in RenderTask
#include "serial_console.h" // Serial command channel (list/run scripts over USB)
#include "mp_provisioning.h"   // BLE provisioning, shared with the Watchy firmware
#include "script_sync.h"        // Server sync procedure, shared with the Watchy firmware
#include "mp_messages.h"       // Panel wording, shared with the Watchy firmware
#include "mp_browse_policy.h"  // Browse/settle rules, shared with the Watchy firmware

// --- Browsing ---------------------------------------------------------------
//
// The settle window, the held-button cap and the "which script is chosen" state
// used to be open-coded in MainControlTask, in parallel with the Watchy's own
// copy of the same three ideas. Both copies were arrived at by watching a panel
// and guessing, and each was fixed on one device while the other kept the bug.
// Commit a51e4cf moved the rules into MpBrowsePolicy, where the host harness
// drives them with a clock it can move by hand; this is the M5Paper half of
// that move.
//
// ADAPTER. The policy indexes scripts by position, this firmware names them by
// humanId, and the two are reconciled by tracking the id alongside rather than
// by mirroring the list into an index array. That is much the smaller change:
// ScriptManager already owns the ordering, the wrap-around and -- crucially --
// persisting the choice to flash so a reboot or a 77s wake resumes the same
// script (selectNextScript / saveCurrentScriptId). Making the policy
// authoritative would have meant lifting all of that into main.cpp for no gain,
// because what was actually broken here was never the ordering: it was the
// timing. So the policy owns WHEN, ScriptManager owns WHICH, and g_pendingId is
// the one string that carries identity across the settle window.
static MpBrowsePolicy g_browse;
static String         g_pendingId;   // the humanId behind g_browse.pending()

// Input EDGES seen so far, monotonically increasing. This is what rule 4 needs:
// a render is abandoned by a NEW instruction, not by a button that merely reads
// low. On this device that count is free and exact -- InputTask disables a pin's
// interrupt on the press and re-enables it only after seeing the release, so
// every item on g_inputEventQueue is one edge. Serial-console run requests
// arrive on the same queue and count too: they are equally "something new
// arrived after this render began", which is the question abortRequested() is
// really asking.
static uint32_t g_pressCount = 0;

// Live button level, for rule 3. Active LOW: InputManager configures all three
// pins with pull-ups and a negative-edge interrupt.
static bool anyButtonDown()
{
    return digitalRead(BUTTON_UP_PIN)   == LOW ||
           digitalRead(BUTTON_DOWN_PIN) == LOW ||
           digitalRead(BUTTON_PUSH_PIN) == LOW;
}

#if MP_BENCH
#include "bench/mp_bench.h" // env:m5paper-bench only; compiled out otherwise
#endif

// --- Task Handles ---
TaskHandle_t g_mainControlTaskHandle = NULL;
TaskHandle_t g_inputTaskHandle = NULL;
TaskHandle_t g_serialConsoleTaskHandle = NULL;
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
// See main.h for why this exists alongside the event bit: the rasterizer polls
// the interrupt callback from its scanline loops, and a plain flag load keeps a
// FreeRTOS critical section (xEventGroupGetBits) off that path entirely.
volatile bool g_renderInterruptRequested = false;


// --- Global Manager Instances ---
// Instantiated in setup()
SystemManager* g_systemManager = nullptr;
InputManager* g_inputManager = nullptr;
DisplayManager* g_displayManager = nullptr;
ScriptManager* g_scriptManager = nullptr;
MPNetworkManager* g_networkManager = nullptr;
// RenderController is instantiated by RenderTask as needed, or can be global if RenderTask always uses one.
// For now, RenderTask will create its own RenderController instance.

// --- Constants ---
#define FRESH_START_THRESHOLD 10 // Perform full refresh every 10 reboots (approx)
// Calculate capacity for script content JSON: content length + structural overhead + parsing buffer
const TickType_t MAIN_LOOP_IDLE_DELAY = pdMS_TO_TICKS(50);
const TickType_t SLEEP_IDLE_THRESHOLD_MS = 3000; // 3 seconds of inactivity before sleep

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

#if MP_BENCH
    // Benchmark firmware (env:m5paper-bench) only. Diverts the whole device
    // into the deterministic on-device benchmark: no WiFi, no S3 fetch, no
    // SPIFFS, no input handling -- just the render path, timed.
    // Compiled out entirely in the normal build.
    MPBench_Start();
    return; // loop() is a no-op; the bench task owns the device from here.
#endif

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
    // g_displayManager->showMessage("System Booting...", 100, 15, true, true); // Replaced by startup indicator
    g_displayManager->drawStartupIndicator(); // Draw startup indicator without clearing screen
    esp_task_wdt_reset();

    g_systemManager = new SystemManager();
    if (!g_systemManager || !g_systemManager->initialize()) { // Loads NVS settings
        log_e("FATAL: SystemManager initialization failed.");
        g_displayManager->showMessage(MP_MSG_STARTUP_FAILED, 150, 15, false, false);
        while(1) vTaskDelay(portMAX_DELAY);
    }
    esp_task_wdt_reset();

    g_scriptManager = new ScriptManager();
    if (!g_scriptManager || !g_scriptManager->initialize()) { // Initializes SPIFFS
        log_e("FATAL: ScriptManager initialization failed.");
        g_displayManager->showMessage(MP_MSG_STARTUP_FAILED, 150, 15, false, false);
        while(1) vTaskDelay(portMAX_DELAY);
    }
    esp_task_wdt_reset();
    
    g_networkManager = new MPNetworkManager(g_systemManager); // Pass SystemManager if needed for config
    if (!g_networkManager) {
        log_e("FATAL: MPNetworkManager instantiation failed.");
        g_displayManager->showMessage(MP_MSG_STARTUP_FAILED, 150, 15, false, false);
        while(1) vTaskDelay(portMAX_DELAY);
    }
    // MPNetworkManager doesn't have an init method in the plan, connects on demand.
    esp_task_wdt_reset();

    g_inputManager = new InputManager(g_inputEventQueue);
    if (!g_inputManager || !g_inputManager->initialize()) { // Sets up GPIOs and ISRs
        log_e("FATAL: InputManager initialization failed.");
        g_displayManager->showMessage(MP_MSG_STARTUP_FAILED, 150, 15, false, false);
        while(1) vTaskDelay(portMAX_DELAY);
    }
    esp_task_wdt_reset();

    // The browse rules, shared with the Watchy; the numbers, deliberately not.
    {
        MpBrowsePolicy::Config cfg;
        // How long a script title stays up, alone, before its render starts.
        // Long enough to press again and move on; short enough not to feel like
        // a delay. The Watchy uses the same 450, but as its own constant: they
        // agree today by measurement, not by contract.
        cfg.titleSettleMs   = 450;
        // The longest a held button may postpone a settled render. Rule 3: a
        // contact that never reads low would otherwise hold the device on a
        // title indefinitely, which from the bench is indistinguishable from a
        // crash and was in fact reported as one.
        cfg.buttonHoldCapMs = 1500;
        // NO policy-driven periodic re-render on this device.
        //
        // The Watchy stays awake and needs the policy to tell it when 83s have
        // passed. The M5Paper does not stay awake: MainControlTask goes into
        // light sleep after 3s idle and the SoC timer wakes it 77s later
        // (SystemManager::DEFAULT_SLEEP_DURATION_S), and the wake path already
        // triggers a fresh render so time- and counter-dependent scripts
        // advance. A second deadline running in parallel with the sleep timer
        // would be a duplicate of it that could only ever disagree -- exactly
        // the class of bug this file is being cured of -- and it could not even
        // fire, since the task is not running while asleep. Zero means "the
        // caller has its own cadence"; poll() then only ever answers for a
        // settled title.
        cfg.autoRerunMs     = 0;
        g_browse.configure(cfg);
    }
    esp_task_wdt_reset();

    // 3. Create Tasks
    xTaskCreatePinnedToCore(MainControlTask_Function, "MainCtrlTask", MAIN_CONTROL_TASK_STACK_SIZE, NULL, MAIN_CONTROL_TASK_PRIORITY, &g_mainControlTaskHandle, 1);
    xTaskCreatePinnedToCore(InputTask_Function, "InputTask", INPUT_TASK_STACK_SIZE, NULL, INPUT_TASK_PRIORITY, &g_inputTaskHandle, 1); // Core 1 for responsiveness
    xTaskCreatePinnedToCore(RenderTask_Function, "RenderTask", RENDER_TASK_STACK_SIZE, NULL, RENDER_TASK_PRIORITY, &g_renderTaskHandle, 0); // Core 0 for rendering
    xTaskCreatePinnedToCore(FetchTask_Function, "FetchTask", FETCH_TASK_STACK_SIZE, NULL, FETCH_TASK_PRIORITY, &g_fetchTaskHandle, 0);    // Core 0 for network
    xTaskCreatePinnedToCore(SerialConsoleTask_Function, "SerialConTask", SERIAL_CONSOLE_TASK_STACK_SIZE, NULL, SERIAL_CONSOLE_TASK_PRIORITY, &g_serialConsoleTaskHandle, 1); // Core 1, alongside input

    if (!g_mainControlTaskHandle || !g_inputTaskHandle || !g_renderTaskHandle || !g_fetchTaskHandle || !g_serialConsoleTaskHandle) {
        log_e("FATAL: Failed to create one or more tasks. Halting.");
        g_displayManager->showMessage(MP_MSG_STARTUP_FAILED, 150, 15, false, false);
        while(1) vTaskDelay(portMAX_DELAY);
    }

    // Loads stored credentials only. NO advertising window at boot: a boot is
    // not a request to provision, and a timer wake even less so. The window is
    // opened by a real button press -- see MainControlTask.
    MPProvisioning::begin();

    log_i("Setup complete. Tasks created. Managers initialized.");
    // g_displayManager->showMessage("Setup OK", 200, 15, false, false); // Removed to preserve screen
    // vTaskDelay(pdMS_TO_TICKS(1000)); // Show setup message - Removed

    // Setup task no longer needs WDT monitoring. Tasks will manage their own.
    esp_task_wdt_delete(NULL);

    // The `loop()` function is not used in FreeRTOS projects.
    // MainControlTask_Function will take over the role of the main application loop.
    // Delete this task (setup) as it's done.
    vTaskDelete(NULL);
}

// Tells the policy how many scripts there are.
//
// It refuses to act on an empty list, so this has to be right before the first
// press; but it only changes when a sync brings scripts in or takes them away,
// so it is called at task start and after a fetch rather than per press. The
// list read is cached in ScriptManager, but it is still a JSON document copy
// and that is not something to pay for on every button.
static void refreshBrowseScriptCount()
{
    JsonDocument listDoc;
    int n = 0;
    if (g_scriptManager && g_scriptManager->loadScriptList(listDoc) && listDoc.is<JsonArray>()) {
        n = (int)listDoc.as<JsonArray>().size();
    }
    // A device with no list still has the built-in default script to browse, and
    // a policy told there are zero scripts would answer Nothing forever -- so the
    // floor is one, not zero.
    g_browse.setScriptCount(n > 0 ? n : 1);
    log_i("Browse policy: %d script(s) selectable.", g_browse.scriptCount());
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
                    // See the note at the other xQueueSend below: the render
                    // brackets are opened here, at the single point where a job
                    // actually reaches RenderTask.
                    g_browse.renderStarted(millis(), g_pressCount);
                    return true;
                } else {
                    log_e("triggerScriptRender: Failed to send render job for default script '%s'.", defaultJobData.script_id.c_str());
                    g_displayManager->showMessage(MP_MSG_RENDER_ERROR, 150, 15, false, false);
                    return false;
                }
            } else {
                 log_e("triggerScriptRender: getScriptForExecution also failed to provide a default script ID.");
                 g_displayManager->showMessage(MP_MSG_NO_SCRIPTS, 150, 15, false, false);
                 return false;
            }
        } else {
            log_e("triggerScriptRender: Failed to get default script details.");
            g_displayManager->showMessage(MP_MSG_NO_SCRIPTS, 150, 15, false, false);
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
    // humanIdToRender is passed through, not just logged: this used to resolve
    // whatever /current_script.id held, so the argument was decoration. It
    // matched only because every caller writes the id to flash first (see
    // selectNextScript / the serial console path); a caller that forgot would
    // have rendered the wrong script with no sign of it in the logs, which say
    // "resolved from" on the next line.
    if (g_scriptManager->getScriptForExecution(jobData.script_id, fileId, scriptState, humanIdToRender)) { // No scriptContentForJob
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
            // Open the render bracket HERE, not at any of the eight call sites,
            // because this is the one place a job actually reaches RenderTask.
            // renderStarted() records the press count as it stands now, which is
            // what lets abortRequested() tell a genuinely new press from a
            // button that was already down when the render began (rule 4).
            g_browse.renderStarted(millis(), g_pressCount);
            return true;
        } else {
            log_e("triggerScriptRender: Failed to send render job for '%s'.", jobData.script_id.c_str());
            g_displayManager->showMessage(MP_MSG_RENDER_ERROR, 150, 15, false, false);
            return false;
        }
    } else {
        // This case should ideally be rare if getScriptForExecution always falls back to default.
        log_e("triggerScriptRender: Failed to get script details for '%s' (even default).", humanIdToRender.c_str());
        g_displayManager->showMessage(MP_MSG_RENDER_ERROR, 150, 15, false, false);
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

    // How many times in a row the SAME script has come back from RenderTask
    // unsuccessful. The render-result handler below uses it to stop retrying a
    // script that cannot be rendered at all; a success, or moving to another
    // script, clears it so a later transient failure still gets its retry.
    String lastFailedScriptId = "";
    int consecutiveRenderFailures = 0;

    // Initial actions on boot/restart:
    // 1. Potentially sync time
    RTC_Time rtcTime = g_systemManager->getTime();
    if (rtcTime.hour == 0 && rtcTime.min == 0 && rtcTime.sec == 0) { // RTC likely not set
        // log_i("MainCtrl: RTC not set. Attempting NTP sync."); // EPD Message removed
        if (g_systemManager->syncTimeWithNTP(*g_networkManager)) {
            // log_i("MainCtrl: NTP sync successful."); // EPD Message and delay removed
        } else {
            // log_i("MainCtrl: NTP sync failed."); // EPD Message removed
            // No long delay here, attempt to re-render current script
            if (!currentLoadedScriptId.isEmpty()) {
                log_i("MainCtrl: NTP sync failed. Re-rendering current script '%s' with saved state.", currentLoadedScriptId.c_str());
                triggerScriptRender(currentLoadedScriptId, true, currentState, currentLoadedScriptId); // true for useAsIsState
            } else {
                log_w("MainCtrl: NTP sync failed, but no current script loaded to re-render.");
                // Delay removed
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
        // Past the deepest call this task makes (script list JSON + SPIFFS), so
        // this is the number that matters. Logged because the previous 4096-byte
        // budget failed silently, as a boot loop, the moment a refactor added
        // 160 bytes to this frame.
        log_i("MainCtrl: stack high-water mark %u bytes free",
              (unsigned)uxTaskGetStackHighWaterMark(NULL));
        triggerScriptRender(initialHumanIdToLoad, false, currentState, currentLoadedScriptId); // false for useAsIsState (fresh)
    } else {
        log_e("MainCtrl: Failed to determine an initial script to load. This should not happen.");
        g_displayManager->showMessage(MP_MSG_NO_SCRIPTS, 150, 15, true, true);
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
            g_displayManager->showMessage(MP_MSG_SYNCING, 200, 15);
        } else {
            log_e("MainCtrl: Failed to send initial fetch job.");
        }
    }
    lastActivityTime = xTaskGetTickCount();
    esp_task_wdt_reset();


    // --- Main Loop ---
    // Title browsing: a script change shows its name immediately but does not
    // start the render until the presses stop. Holding a render off for a
    // moment is what makes it possible to page through titles at the speed of
    // the button rather than the speed of the renderer. The deadline that used
    // to live here, next to a copy of the id, is g_browse's now.
    refreshBrowseScriptCount();

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

        MPProvisioning::tick();   // closes the provisioning window when it expires

        // Ask the policy what to do about the title on screen.
        //
        // Everything that used to be spelled out here -- the deadline, and the
        // fact that a render must not be started on top of one already running
        // -- is in MpBrowsePolicy::poll(), which the harness tests against a
        // clock it can move. This is only the hands.
        //
        // The live button level is passed in for rule 3, which is new behaviour
        // on this device: a finger still on the button extends the window, so
        // the render waits for the user to actually settle rather than firing
        // under their thumb. That extension is only safe because it is capped:
        // InputTask leaves a held pin's interrupt disabled until it sees the
        // release, so a contact stuck low produces no further events at all and
        // an uncapped extension would be a hang with no way out.
        //
        // Guarded rather than filtered afterwards: poll() COMMITS the selection
        // when it answers Render, so calling it in a state that cannot act on
        // the answer would drop the title on the floor. The policy has its own
        // in-flight interlock; this one covers the window where the two views
        // of "is a render running" could still disagree, which is precisely the
        // preemptive-IDLE handoff below.
        if (currentState != AppState::RENDERING_SCRIPT &&
            g_browse.poll(millis(), anyButtonDown(), g_pressCount) == MpBrowseAction::Render) {
            const String toRender = g_pendingId;
            g_pendingId = "";
            log_i("MainCtrl: Title settled on '%s'. Triggering render.", toRender.c_str());
            triggerScriptRender(toRender, false, currentState, currentLoadedScriptId);
        }

        // Check Input Queue (non-blocking)
        if (xQueueReceive(g_inputEventQueue, &inputEvent, 0) == pdTRUE) {
            lastActivityTime = xTaskGetTickCount();

            // A button press is a person standing at the device, so it is the
            // moment to be discoverable. Timer wakes never reach here, so the
            // radio never comes up unasked. Repeated presses extend rather than
            // stack: the window always ends 20s after the LAST press.
            MPProvisioning::openWindow();
            log_i("MainCtrl: Received input event: %d", (int)inputEvent.type);

            // One edge, by construction -- see g_pressCount at the top of this
            // file. Counted before anything below looks at it, because
            // abortRequested() is asking "did something new arrive since this
            // render began", and this event IS that something.
            g_pressCount++;

            // Activity indicator is now drawn by InputManager::taskFunction with specific type

            // Stop ongoing render or fetch if significant input
            if (currentState == AppState::RENDERING_SCRIPT && g_browse.abortRequested(g_pressCount)) {
                // Rule 4: abandoned by a NEW press, never by a button that is
                // merely down. The distinction did not exist on this device
                // before -- the test was "an event arrived", which happens to
                // be equivalent here because a held pin produces no further
                // events -- but asking the policy is what keeps the two
                // firmwares from drifting apart again, and it is now the same
                // question the Watchy asks from inside its page loop.
                log_i("MainCtrl: New input during render. Requesting interrupt.");
                // Plain flag first: this is the one the rasterizer's inner loops
                // actually poll, so it is what makes the abort land quickly.
                g_renderInterruptRequested = true;
                xEventGroupSetBits(g_renderTaskEventFlags, RENDER_INTERRUPT_BIT);
                // Preemptively change state to allow new render to be queued.
                // The RenderTask will eventually send its (now hopefully interrupted) status.
                currentState = AppState::IDLE;
                // Close the render bracket at the same moment, and with
                // completed=false: a frame abandoned mid-raster is not a frame
                // the user saw (rule 5). Leaving the policy to find out only
                // when the stale result arrives would block the next title's
                // render behind a render nobody is waiting for any more.
                g_browse.renderFinished(millis(), false);
                log_i("MainCtrl: State changed to IDLE preemptively due to render interrupt request.");
            }
            if (currentState == AppState::FETCHING_DATA) { // Check if currently fetching
                // Fetch task interrupt is more complex, might need a flag for MPNetworkManager
                // For now, let FetchTask complete or handle its own interrupt via MPNetworkManager flag.
                log_i("MainCtrl: Input received during fetch. Fetch task should handle via its interrupt flag.");
            }

            if (inputEvent.type == InputEventType::NEXT_SCRIPT || inputEvent.type == InputEventType::PREVIOUS_SCRIPT) {
                String selectedHumanId, selectedName;
                const int delta = (inputEvent.type == InputEventType::PREVIOUS_SCRIPT) ? -1 : +1;
                if (g_scriptManager->selectNextScript(inputEvent.type == InputEventType::PREVIOUS_SCRIPT, selectedHumanId, selectedName)) {
                    // The policy is stepped for its state machine, not for its
                    // arithmetic: ScriptManager has already walked and wrapped
                    // the list and told us the id. What browse() does that
                    // matters is refuse to arm the settle window -- see
                    // titleDrawn() below.
                    const bool wasBrowsing = g_browse.browsing();
                    g_browse.browse(delta, millis());
                    g_pendingId = selectedHumanId;
                    // Name now, render later. This used to be followed by a
                    // blocking vTaskDelay(500), which meant every press cost
                    // half a second before the next one was even read -- so
                    // paging through titles ran at two per second at best, and
                    // each one started a render that the next press then had to
                    // interrupt.
                    //
                    // full_update=false, i.e. DU4 rather than GC16. A title is a
                    // transient frame of black text on white and has no use for
                    // 16 greys; GC16 is the slowest waveform on this panel and
                    // holds the panel mutex for the whole of it, so paying it
                    // per press made browsing SLOWER than the Watchy, whose
                    // equivalent has always used a fast update. The canvas is
                    // still cleared first, so the title lands on white rather
                    // than over the outgoing script.
                    // First title of a burst clears the whole panel so the name
                    // lands on white rather than over the outgoing script. Every
                    // title after that only repaints the band -- the panel is
                    // already white, and a band push touches 34 rows instead of
                    // 960.
                    g_displayManager->clearActivityIndicators();
                    if (!wasBrowsing) {
                        g_displayManager->showMessage(selectedName, 250, 15, false, true);
                    } else {
                        g_displayManager->showBanner(selectedName, 250, 15);
                    }
                    // ONLY NOW is the window armed, and only titleDrawn() can
                    // arm it. Both showMessage() and showBanner() block on the
                    // panel until the frame is out, and on this panel that is
                    // hundreds of milliseconds -- the same order as the window
                    // itself. Arming before the push, which is what a plain
                    // "deadline = now + 450" does wherever it is written, spends
                    // the whole window driving the very frame it exists to leave
                    // up. That is rule 1, and it is the bug that made a title
                    // vanish the instant it appeared once loading a compiled
                    // program stopped taking long enough to hide it.
                    g_browse.titleDrawn(millis());
                    log_i("MainCtrl: Selected '%s'; render in %ums unless another press arrives.",
                          selectedHumanId.c_str(), (unsigned)g_browse.config().titleSettleMs);
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
            } else if (inputEvent.type == InputEventType::RUN_SCRIPT_BY_ID) {
                // Serial console asked for a specific script by human id.
                String requestedId(inputEvent.script_id);
                if (requestedId.isEmpty()) {
                    log_w("MainCtrl: RUN_SCRIPT_BY_ID with empty id. Ignoring.");
                } else {
                    log_i("MainCtrl: Serial console requested script '%s'. Triggering render.", requestedId.c_str());
                    // Persist it so a later wake/reboot resumes the same script,
                    // matching what button-driven selection does.
                    g_scriptManager->saveCurrentScriptId(requestedId);
                    triggerScriptRender(requestedId, false, currentState, currentLoadedScriptId);
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

            // Close the render bracket, but ONLY for a render we still believe
            // is in flight.
            //
            // An interrupted render already closed it at the interrupt request
            // above, and went preemptively IDLE at the same moment. Its result
            // then arrives late, by which time the next title may have started
            // a render of its own -- and closing the bracket unconditionally
            // here would close THAT one, on the strength of a result belonging
            // to a render nobody is waiting for. The AppState check is what
            // distinguishes the two: RENDERING_SCRIPT means the bracket this
            // result belongs to is still open.
            if (currentState == AppState::RENDERING_SCRIPT) {
                g_browse.renderFinished(millis(), renderResultItem.success);
            }


            // A result for a script we have already left is stale: the user
            // pressed on, and acting on it would repaint the old script over the
            // title of the new one -- which is the "previous render appears
            // briefly anyway" flicker.
            if (g_browse.browsing() ||
                (!currentLoadedScriptId.isEmpty() && received_script_id != currentLoadedScriptId)) {
                log_i("MainCtrl: Ignoring stale render result for '%s' (now on '%s').",
                      received_script_id.c_str(),
                      g_browse.browsing() ? g_pendingId.c_str() : currentLoadedScriptId.c_str());
                if (currentState == AppState::RENDERING_SCRIPT) currentState = AppState::IDLE;
            } else if (renderResultItem.success) {
                // A script that rendered is not a failing script any more: a
                // later failure of it starts its retry budget from scratch.
                lastFailedScriptId = "";
                consecutiveRenderFailures = 0;
                g_scriptManager->saveScriptExecutionState(received_script_id, renderResultItem.final_state);
                if (currentState == AppState::RENDERING_SCRIPT) { // If we were rendering
                    currentState = AppState::IDLE;
                    log_i("MainCtrl: Render successful for '%s'. State -> IDLE.", received_script_id.c_str());
                }
            } else if (renderResultItem.interrupted) {
                // Deliberately silent. A render is only ever interrupted because
                // the user pressed a button, and the press has already replaced
                // the screen with the new script's title -- so "Render stopped"
                // arrived a moment later and painted over the title the user was
                // reading, announcing something they had just done on purpose.
                if (currentState == AppState::RENDERING_SCRIPT) { // If we were rendering
                    currentState = AppState::IDLE;
                    log_i("MainCtrl: Render interrupted for '%s'. State -> IDLE.", received_script_id.c_str());
                }
            } else { // Render failed (not success, not interrupted)
                String scriptToRetry = "";
                if (!received_script_id.isEmpty()) {
                    scriptToRetry = received_script_id;
                } else if (!currentLoadedScriptId.isEmpty()) {
                    scriptToRetry = currentLoadedScriptId;
                    log_i("MainCtrl: Render failed (unknown script_id in result); attributing it to current '%s'.", scriptToRetry.c_str());
                } else {
                    log_w("MainCtrl: Render failed, and no script ID available.");
                    // scriptToRetry remains empty, triggerScriptRender handles "" as default
                }

                // Count consecutive failures PER SCRIPT, and stop after the
                // second one.
                //
                // This branch used to re-queue unconditionally. For a script
                // with no content on the device, or one that will never compile,
                // RenderTask fails identically every time, so the device sat in
                // a loop repainting two error banners forever -- burning the
                // panel's fast-update budget and never letting the user read the
                // error. The Watchy shows one error frame and stops; this now
                // does the same, while still giving a genuinely transient
                // failure (busy panel, full queue) the one retry it deserves.
                if (scriptToRetry == lastFailedScriptId) {
                    consecutiveRenderFailures++;
                } else {
                    lastFailedScriptId = scriptToRetry;
                    consecutiveRenderFailures = 1;
                }
                const RenderFailure failureKind = renderResultItem.failure;
                const bool permanentFailure = (failureKind == RenderFailure::SCRIPT_MISSING ||
                                               failureKind == RenderFailure::COMPILE_FAILED);
                const bool giveUp = permanentFailure || consecutiveRenderFailures >= 2;

                // Three lines: what happened, which script, and -- when
                // RenderTask could tell them apart -- why. MP_MSG_SCRIPT_MISSING
                // and MP_MSG_PARSE_FAILED are the same two reasons the Watchy
                // prints; they were previously only in the serial log here, so a
                // user with no cable could not tell a missing script from a
                // broken one. One line for all three would be up to 33 glyphs
                // and this panel fits 30 at text size 3 (6x8 font x3 = 18px per
                // glyph across 540px), so the ends were being clipped.
                g_displayManager->showMessage(MP_MSG_RENDER_ERROR, 200, 15, false, false);
                g_displayManager->showMessage(received_script_id, 250, 15, false, false);
                if (failureKind == RenderFailure::SCRIPT_MISSING) {
                    g_displayManager->showMessage(MP_MSG_SCRIPT_MISSING, 300, 15, false, false);
                } else if (failureKind == RenderFailure::COMPILE_FAILED) {
                    g_displayManager->showMessage(MP_MSG_PARSE_FAILED, 300, 15, false, false);
                }
                if (!received_error_message.isEmpty()) {
                    log_e("Render Error for '%s': %s", received_script_id.c_str(), received_error_message.c_str());
                }
                vTaskDelay(pdMS_TO_TICKS(100)); // Short delay for message visibility

                if (giveUp) {
                    log_e("MainCtrl: Render failed for '%s' (%s, attempt %d). Not retrying; leaving the error on screen.",
                          scriptToRetry.c_str(),
                          permanentFailure ? "permanent" : "repeated",
                          consecutiveRenderFailures);
                    if (currentState == AppState::RENDERING_SCRIPT) currentState = AppState::IDLE;
                } else {
                    log_i("MainCtrl: Render failed for '%s'. Re-rendering once with saved state.", scriptToRetry.c_str());
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
        }

        // Check Fetch Status Queue (non-blocking)
        if (xQueueReceive(g_fetchStatusQueue, &fetchResultItem, 0) == pdTRUE) { // Use fetchResultItem
            lastActivityTime = xTaskGetTickCount();
            
            String fetch_message(fetchResultItem.message); // Construct String from char[]
            log_i("MainCtrl: Received fetch result. Status: %d, Message: %s", (int)fetchResultItem.status, fetch_message.c_str());

            // Record the ATTEMPT, not just the success.
            //
            // Both gates that decide whether to fetch -- isFullRefreshIntended()
            // and the last-fetch timestamp -- used to advance only on SUCCESS.
            // So when the server is unreachable, both stayed permanently true
            // and the device re-attempted WiFi + fetch on EVERY boot and EVERY
            // 77s wake, forever, with no backoff. That is the "why is it trying
            // so often" behaviour: it was not retrying a failure deliberately,
            // it simply never recorded that it had tried.
            //
            // Stamping the timestamp on a completed attempt restores the normal
            // 120-minute cadence for failures too. The full-refresh INTENT is
            // deliberately left set, so when connectivity does come back the
            // refresh still happens -- we throttle the retry, we do not forget
            // the intent.
            //
            // Interrupted and restart-requested outcomes are NOT attempts: the
            // user cut it short, so they must not push the next try out by two
            // hours.
            if (fetchResultItem.status != FetchResultStatus::INTERRUPTED_BY_USER &&
                fetchResultItem.status != FetchResultStatus::RESTART_REQUESTED &&
                fetchResultItem.status != FetchResultStatus::SUCCESS) {
                log_i("MainCtrl: Fetch attempt failed; recording attempt time so the retry backs off.");
                g_systemManager->updateLastFetchTimestamp();
                if (!g_systemManager->saveSettings()) {
                    log_e("MainCtrl: Failed to save settings after failed fetch attempt!");
                }
            }

            if (fetchResultItem.status == FetchResultStatus::NO_WIFI) {
                log_w("MainCtrl: Fetch failed (NO_WIFI). Silently skipping. No EPD message, no re-render.");
                // User requested to not show message on EPD and not re-render script.
                // Logging to console is maintained.
            } else if (fetchResultItem.status == FetchResultStatus::SUCCESS) {
                g_displayManager->showMessage(fetch_message, 350, 15, false, true); // Show success message
                vTaskDelay(pdMS_TO_TICKS(1000)); // Display success message for a bit

                g_systemManager->updateLastFetchTimestamp();
                if (g_systemManager->isFullRefreshIntended()) {
                    g_systemManager->setFullRefreshIntended(false);
                }
                if (!g_systemManager->saveSettings()) {
                    log_e("MainCtrl: Failed to save settings after successful fetch!");
                }

                if (fetchResultItem.new_scripts_available) {
                    // The only moment the number of selectable scripts can
                    // change, so the only moment the policy needs telling.
                    refreshBrowseScriptCount();
                    g_displayManager->showMessage("New scripts", 400, 15);
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
                g_displayManager->showMessage(MP_MSG_SYNC_STOPPED, 200, 15, false, false);
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
        // Do NOT sleep while the BLE provisioning window is open.
        //
        // Light sleep suspends the radio, so advertising stops -- but millis()
        // keeps running, so the window expires anyway. With a 3s idle timeout
        // that left a window measured in seconds, which is not something a
        // person can connect to. Provisioning is rare and explicitly requested,
        // so staying awake for it is the right trade.
        if (MPProvisioning::windowOpen()) {
            lastActivityTime = xTaskGetTickCount();   // hold off the idle timer
        }

        // Nor while a title is settling. The 3s idle threshold has always been
        // comfortably longer than the 450ms window, so this never bit; but rule
        // 3 lets a held button push a settled render out by up to 1.5s more, and
        // sleeping on top of a title the user is still choosing would strand the
        // panel showing a name whose script never rendered. The Watchy makes the
        // same check before its own sleep.
        if (g_browse.browsing()) {
            lastActivityTime = xTaskGetTickCount();
        }

        if (currentState == AppState::IDLE && !MPProvisioning::windowOpen() &&
            (xTaskGetTickCount() - lastActivityTime) > pdMS_TO_TICKS(SLEEP_IDLE_THRESHOLD_MS)) {
            log_i("MainCtrl: Idle timeout. Checking EPD mutex before light sleep.");

            // Probe BOTH display locks with a very short timeout: the canvas lock
            // (a render in flight) and the panel lock (an indicator/banner push in
            // flight). Sleeping through either would be wrong.
            if (g_displayManager->isDisplayIdle(pdMS_TO_TICKS(5))) {
                
                log_i("MainCtrl: EPD Mutex free. Going to light sleep.");
                // User requested to not show "Sleeping..." message on EPD.
                vTaskDelay(pdMS_TO_TICKS(10)); // Short delay before sleep
                
                esp_task_wdt_delete(NULL); // Stop WDT for MainControlTask before sleeping
                g_systemManager->goToLightSleep(SystemManager::DEFAULT_SLEEP_DURATION_S); // Use constant
                // --- Device Wakes Up Here ---
                esp_task_wdt_init(30, true); // Re-initialize WDT for MainControlTask
                esp_task_wdt_add(NULL);

                lastActivityTime = xTaskGetTickCount(); // Reset activity time after waking up
                log_i("MainCtrl: Woke up. Cause: %d", g_systemManager->getWakeupCause());
                // User requested to not show "Awake!" message on EPD.
                
                // Activity indicator on GPIO wakeup is handled by InputManager.
                
                String scriptIdAfterWakeup = currentLoadedScriptId; // Default to current
                if (scriptIdAfterWakeup.isEmpty()) {
                    // If no script was loaded before sleep, try to get the default/first one
                    String tempWakeFileIdUnused; // Not used here
                    ScriptExecState tempWakeState;
                    if(g_scriptManager->getScriptForExecution(scriptIdAfterWakeup, tempWakeFileIdUnused, tempWakeState)){
                        // scriptIdAfterWakeup is now set
                    } else {
                        scriptIdAfterWakeup = ScriptManager::DEFAULT_SCRIPT_ID;
                    }
                }

                if (!scriptIdAfterWakeup.isEmpty()) {
                    // For timer wakeup, useAsIsState = false (fresh render, increments counter, updates time)
                    // For GPIO wakeup, an InputEvent will be generated and handled by InputManager,
                    // which then sends a logical event to MainControlTask. MainControlTask will then
                    // trigger a fresh render based on that input.
                    // So, any direct render after wakeup (especially timer) should be 'fresh'.
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
                         g_displayManager->showMessage(MP_MSG_SYNCING, 200, 15, false, false); // Don't clear if rendering
                    }
                }
            }
            } else { // lockEPD failed
                log_e("MainCtrl: EPD Mutex is held by another task before attempting sleep! Aborting sleep cycle. Will retry later.");
                // Prevent immediate re-sleep attempt by resetting activity time
                lastActivityTime = xTaskGetTickCount();
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

    // Warm the compiled-program cache once, so renders load programs instead
    // of parsing. Nothing to do when every stored program is already fresh.
    if (g_scriptManager) {
        const int n = g_scriptManager->compileAllPrograms(false);
        if (n) log_i("RenderTask: compiled %d program(s) at start", n);
    }

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
            
            log_i("RenderTask: Received job for human_id: %s, file_id: %s", jobDataForRenderCtrl.script_id.c_str(), jobDataForRenderCtrl.file_id.c_str());

            // The compiled program: from the cache written at sync time, or a
            // one-off compile of the source if that is missing or stale. The
            // built-in default is compiled from its literal every time.
            MpProgram program;
            String programError;
            ScriptManager::LoadReason programReason = ScriptManager::LoadReason::OK;
            bool haveProgram;
            if (jobDataForRenderCtrl.file_id == ScriptManager::DEFAULT_SCRIPT_ID) {
                log_i("RenderTask: Using built-in default script for '%s'", jobDataForRenderCtrl.script_id.c_str());
                haveProgram = ScriptManager::compileSource(ScriptManager::DEFAULT_SCRIPT_CONTENT, program, &programError);
                if (!haveProgram) programReason = ScriptManager::LoadReason::COMPILE_FAILED;
            } else {
                haveProgram = g_scriptManager->loadProgram(jobDataForRenderCtrl.file_id, program, &programError, &programReason);
            }
            if (!haveProgram) {
                log_e("RenderTask: No program for fileId: %s (humanId: %s): %s", jobDataForRenderCtrl.file_id.c_str(), jobDataForRenderCtrl.script_id.c_str(), programError.c_str());
                RenderResultData errorResultData;
                errorResultData.script_id = jobDataForRenderCtrl.script_id;
                errorResultData.success = false;
                errorResultData.interrupted = false;
                // Say WHICH kind of failure this is, so MainControlTask can stop
                // instead of re-queueing a job that will fail the same way.
                // ScriptManager reports the reason as a value; the Watchy
                // switches on the same enum, so the two firmwares cannot drift
                // apart over a reworded message.
                switch (programReason) {
                    case ScriptManager::LoadReason::COMPILE_FAILED:
                        errorResultData.failure = RenderFailure::COMPILE_FAILED; break;
                    case ScriptManager::LoadReason::STORAGE_UNAVAILABLE:
                        errorResultData.failure = RenderFailure::TRANSIENT; break;
                    default:
                        errorResultData.failure = RenderFailure::SCRIPT_MISSING; break;
                }
                errorResultData.error_message = programError.isEmpty() ? String("RenderTask: Failed to load script program.") : programError;
                // final_state will be default
                
                RenderResultQueueItem errorResultQueueItem;
                errorResultQueueItem.fromRenderResultData(errorResultData);
                if (xQueueSend(g_renderStatusQueue, &errorResultQueueItem, pdMS_TO_TICKS(100)) != pdTRUE) {
                    log_e("RenderTask: Failed to send error render status for %s", jobDataForRenderCtrl.script_id.c_str());
                }
                continue; // Skip to next job
            }
            log_i("RenderTask: Program ready for script ID: %s (%u instructions)", jobDataForRenderCtrl.script_id.c_str(), (unsigned)program.code.size());
            
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
                // Clear any pending interrupt request before starting.
                // NOTE (still open, see docs/analysis/m5paper-render-interrupt-bug.md):
                // a request raised between the job being queued and this clear is
                // dropped. A render epoch counter remains the better design.
                g_renderInterruptRequested = false;
                xEventGroupClearBits(g_renderTaskEventFlags, RENDER_INTERRUPT_BIT);
                
                // Pass jobDataForRenderCtrl (meta) and script_content_for_parser (payload)
                // This requires RenderController::renderScript to be updated.
                // For this commit, we'll assume RenderController is updated.
                // A struct specifically for RenderController input might be cleaner.
                // Let's assume RenderController::renderScript is updated to:
                // renderScript(const String& script_id, const String& script_content, const ScriptExecState& initial_state)
                // (file_id is not directly needed by parser/runtime if content is provided)
                
                resultData = renderCtrl.renderScript(jobDataForRenderCtrl.script_id, program, jobDataForRenderCtrl.initial_state);
                
                // Check if MainControlTask signaled an interrupt during the process
                EventBits_t uxBits = xEventGroupGetBits(g_renderTaskEventFlags);
                if (uxBits & RENDER_INTERRUPT_BIT) {
                    log_i("RenderTask: RENDER_INTERRUPT_BIT was set by MainControlTask. Overriding result to interrupted.");
                    resultData.interrupted = true;
                    resultData.success = false;
                    if (resultData.error_message.isEmpty()) {
                        resultData.error_message = "Render interrupted by MainControlTask signal.";
                    }
                    // Ensure RenderController's internal flag is also set, though its main processing is done.
                    // This helps if RenderController is queried later about its interrupt state for this job.
                    renderCtrl.requestInterrupt();
                    g_renderInterruptRequested = false;
                    xEventGroupClearBits(g_renderTaskEventFlags, RENDER_INTERRUPT_BIT); // Clear the bit
                } else if (resultData.interrupted) {
                    // This case means RenderController itself detected an interrupt (e.g., via its checkInterrupt polling the event group).
                    // Ensure success is false.
                    log_i("RenderTask: RenderController reported interruption. Ensuring success is false.");
                    resultData.success = false;
                    if (resultData.error_message.isEmpty()) {
                       resultData.error_message = "Render interrupted (reported by RenderController).";
                   }
                }
                // If no RENDER_INTERRUPT_BIT was set by MainControlTask post-render,
                // and RenderController didn't report an interruption, then resultData.interrupted remains as
                // determined by RenderController (likely false), and resultData.success also remains as determined.


                // --- Post-render health probe -----------------------------
                // RenderTask has a RENDER_TASK_STACK_SIZE-byte stack (bytes, not
                // words -- the ESP32 FreeRTOS port takes usStackDepth in bytes,
                // despite the comment in main.h). Rendering recurses through
                // MicroPatternsRuntime::processCommandForDisplayList and calls
                // into the rasterizer's deep-frame primitives, so the margin is
                // worth knowing rather than guessing. heap_caps_check_integrity_all
                // then says whether anything has stomped an allocator header --
                // which is the difference between "a task blocked" and "memory
                // was corrupted", the two hypotheses a task-watchdog abort with a
                // corrupted backtrace cannot distinguish on its own.
                {
                    UBaseType_t stackFreeWords = uxTaskGetStackHighWaterMark(NULL);
                    bool heapOk = heap_caps_check_integrity_all(true);
                    log_i("RenderTask: post-render probe -- stack high-water %u bytes free of %u, "
                          "free heap %u (largest block %u), heap integrity %s",
                          (unsigned)(stackFreeWords * sizeof(StackType_t)),
                          (unsigned)RENDER_TASK_STACK_SIZE,
                          (unsigned)esp_get_free_heap_size(),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                          heapOk ? "OK" : "CORRUPT");
                    if (!heapOk) {
                        log_e("RenderTask: HEAP CORRUPTION detected after rendering '%s'.",
                              resultData.script_id.c_str());
                    }
                }

                // After rendering is complete (or interrupted), push the canvas
                // The DisplayListRenderer now handles clearing the canvas.
                // The final push to EPD happens here.
                // Directly call canvas push since mutex is already held by RenderTask.
                M5EPD_Canvas* canvas = g_displayManager->getCanvas();
                if (!canvas) {
                    log_e("RenderTask: Failed to get canvas from DisplayManager.");
                } else if (resultData.interrupted) {
                    // Do NOT push an interrupted render. The canvas holds a partial
                    // image of the script we just abandoned; pushing it repaints the
                    // panel with stale content -- which is what painted the previous
                    // script over the new one's title on every script switch.
                    // The incoming render owns the panel now.
                    log_i("RenderTask: Render interrupted for '%s'. Skipping canvas push to avoid painting stale content.",
                          resultData.script_id.c_str());
                } else {
                    // Panel transactions are serialized separately from the canvas
                    // lock we are holding, so an activity indicator pushed during
                    // this render cannot collide with this push.
                    //
                    // Was a hardcoded UPDATE_MODE_GC16. Script output is pure
                    // black/white (DRAWING_COLOR_WHITE=0 / DRAWING_COLOR_BLACK=15,
                    // nothing in between), so the 16-grey waveform was paying for
                    // tones that never appear. pushScriptCanvasLocked() uses the
                    // fast 1-bit waveform and folds in the periodic full GC16
                    // de-ghost -- see DisplayManager::SCRIPT_FAST_UPDATE_MODE.
                    // Bracketed so the serial log says whether a stall is in
                    // compute or in the panel transaction. Without both lines a
                    // hang after the last RenderController message is ambiguous.
                    log_i("RenderTask: pushing canvas for '%s'...", resultData.script_id.c_str());
                    unsigned long pushStart = millis();
                    g_displayManager->pushScriptCanvasLocked();
                    log_i("RenderTask: canvas push for '%s' took %lu ms.",
                          resultData.script_id.c_str(), millis() - pushStart);
                }
                
                g_displayManager->unlockEPD(); // Unlock EPD
            } else {
                log_e("RenderTask: Failed to lock EPD for rendering script %s", jobDataForRenderCtrl.script_id.c_str());
                resultData.script_id = jobDataForRenderCtrl.script_id; // Populate for error reporting
                resultData.success = false;
                resultData.interrupted = false; // Not interrupted by user, but by system issue
                // A busy panel is the textbook transient failure: whoever holds
                // the lock will let go, so the one retry MainControlTask allows
                // is exactly the right response here.
                resultData.failure = RenderFailure::TRANSIENT;
                resultData.error_message = "Failed to acquire display lock for rendering.";
                resultData.final_state = jobDataForRenderCtrl.initial_state; // Preserve initial state on this type of error
            }

            RenderResultQueueItem resultQueueItem;
            log_d("RenderTask: Before fromRenderResultData for '%s': resultData.success=%s, resultData.interrupted=%s",
                  resultData.script_id.c_str(), resultData.success ? "true" : "false", resultData.interrupted ? "true" : "false");

            resultQueueItem.fromRenderResultData(resultData); // Convert to char[] based for queue

            log_d("RenderTask: Before xQueueSend for '%s': resultQueueItem.success=%s, resultQueueItem.interrupted=%s",
                  resultQueueItem.script_id, resultQueueItem.success ? "true" : "false", resultQueueItem.interrupted ? "true" : "false");

            if (xQueueSend(g_renderStatusQueue, &resultQueueItem, pdMS_TO_TICKS(100)) != pdTRUE) {
                log_e("RenderTask: Failed to send render status for %s", jobDataForRenderCtrl.script_id.c_str());
            }
        } // Closes if (xQueueReceive...)
    } // Closes for (;;)
} // Closes RenderTask_Function
// --- Fetch Task ---
void FetchTask_Function(void *pvParameters) {
    esp_task_wdt_init(120, true); // Long timeout for network ops, panic on WDT timeout
    esp_task_wdt_add(NULL);

    volatile bool user_interrupt_flag_for_network_manager = false;

    FetchJob job; // FetchJob is simple, no Strings, can remain as is

    // WDT timeout for FetchTask is 120s. We'll use a 60s queue receive timeout.
    const TickType_t queueReceiveTimeout = pdMS_TO_TICKS(60000);

    for (;;) {
        esp_task_wdt_reset(); // Reset WDT at the start of each loop iteration.
        user_interrupt_flag_for_network_manager = false; // Reset before waiting for new job

        if (xQueueReceive(g_fetchCommandQueue, &job, queueReceiveTimeout) == pdTRUE) {
            log_i("FetchTask: Received job. Full Refresh: %s", job.full_refresh ? "Yes" : "No");

            // The sync procedure itself is shared with the Watchy -- see
            // script_sync.cpp. This task only owns the queue, the watchdog and
            // the interrupt flag.
            const ScriptSyncResult sync = mp_sync_scripts(*g_networkManager,
                                                          *g_scriptManager,
                                                          job.full_refresh,
                                                          &user_interrupt_flag_for_network_manager);

            FetchResultData resultData;
            resultData.status = sync.status;
            resultData.message = sync.message;
            resultData.new_scripts_available = sync.newScriptsAvailable;

            FetchResultQueueItem resultQueueItem;
            resultQueueItem.fromFetchResultData(resultData);

            if (xQueueSend(g_fetchStatusQueue, &resultQueueItem, pdMS_TO_TICKS(100)) != pdTRUE) {
                log_e("FetchTask: Failed to send fetch status.");
            }
        } // End if receive from queue
    } // End for(;;)
} // Closes FetchTask_Function

// The loop() function is not used extensively in FreeRTOS projects where tasks manage operations.
// However, it's a standard Arduino entry point. Keep it minimal.
// Since setup() calls vTaskDelete(NULL) for the task running setup/loop, this loop()
// is effectively not run by the main Arduino task after setup completes.
void loop() {
    vTaskDelay(portMAX_DELAY);
}
#include "main.h"

// --- Default Script (if loading from SPIFFS fails) ---
const char *default_script = R"(
DEFINE PATTERN NAME="girafe" WIDTH=20 HEIGHT=20 DATA="1100000000000000000010001110000111000100000111111000110011100011111100000001111000111110001100111110000011100111100111000010010011111100000001110001111111000010111110011111100011111111000011111010111111110110011100100111011001100010011000010000111100001110000000011111100000100100001111111011000001100111111100111100111001111110011110011110111111001111001111100111100011100111110000100000010000111000"

DEFINE PATTERN NAME="background" WIDTH=10 HEIGHT=10 DATA="0010001001000101000010001000000101000000001000000001000000011000000011000001111000001000001000000000"

VAR $center_x
VAR $center_y
VAR $secondplusone
VAR $rotation
VAR $rotationdeux
VAR $size
var $squareside

LET $size = 10 + $COUNTER % 15
LET $secondplusone = 1 + $SECOND
LET $rotation = 360 * 60 / $secondplusone
LET $rotationdeux = $minute + 360 * 60 / $secondplusone 
LET $squareside = 20

LET $center_x = $width/2 
LET $center_y = $height/2

FILL NAME="background"
FILL_RECT WIDTH=$width HEIGHT=$height X=0 Y=0

TRANSLATE DX=$center_x DY=$center_y

SCALE FACTOR=$size

ROTATE DEGREES=$rotation

COLOR NAME=WHITE
DRAW NAME="girafe" WIDTH=20 HEIGHT=20 X=0 Y=0

ROTATE DEGREES=$rotationdeux

COLOR NAME=BLACK
DRAW NAME="girafe" WIDTH=20 HEIGHT=20 X=0 Y=0

ROTATE DEGREES=$rotation

COLOR NAME=WHITE
DRAW NAME="girafe" WIDTH=20 HEIGHT=20 X=0 Y=0

ROTATE DEGREES=$rotationdeux

COLOR NAME=BLACK
DRAW NAME="girafe" WIDTH=20 HEIGHT=20 X=0 Y=0    
)";

// Global objects
M5EPD_Canvas canvas(&M5.EPD);
MicroPatternsParser parser;
MicroPatternsRuntime *runtime = nullptr;
RTC_DATA_ATTR int last_fetch_hour = -1; // -1 indicates no previous fetch
RTC_DATA_ATTR int last_fetch_minute = -1;
RTC_DATA_ATTR int last_fetch_day = -1;
RTC_DATA_ATTR int last_fetch_month = -1;
RTC_DATA_ATTR int last_fetch_year = -1;
RTC_Time time_struct;                    // To store time from RTC
RTC_Date date_struct;                    // To store date from RTC
RTC_DATA_ATTR int freshStartCounter = 0; // Counter for triggering full data refresh
RTC_DATA_ATTR bool g_full_refresh_intended = false; // True if next fetch should be a full refresh
String currentScriptId = "";             // ID of the script to execute
String currentScriptContent = "";        // Content of the script to execute

#define FRESH_START_THRESHOLD 10 // Perform full refresh every 10 reboots (approx)

// --- Tasking Globals ---
TaskHandle_t g_fetchTaskHandle = NULL;
SemaphoreHandle_t g_fetchRequestSemaphore = NULL; // Binary semaphore to trigger fetch
QueueHandle_t g_displayMessageQueue = NULL;       // Queue for messages from fetch task to display task

volatile bool g_fetch_task_in_progress = false;
volatile bool g_user_interrupt_signal_for_fetch_task = false; // Signal to fetch task to stop (hard stop)
volatile bool g_fetch_restart_pending = false;                // Signal to fetch task to restart gracefully

struct DisplayMsg
{
    char text[64]; // Increased size for longer messages
    int y_offset;
    uint16_t color;
    bool clear_canvas_first;
    bool push_full_update; // GC16 vs DU4
};

// Function Prototypes
bool selectNextScript(bool moveUp);
bool loadScriptToExecute();
void displayMessage(const String &msg, int y_offset, uint16_t color);
void displayParseErrors();
void handleWakeupAndScriptExecution(uint8_t raw_gpio_from_isr); // Modified signature
void fetchTaskFunction(void *pvParameters);
FetchResultStatus perform_fetch_operations(bool isFullRefresh); // Modified signature
bool shouldPerformFetch(const char* caller);  // Helper to check if fetch should be performed based on time criteria
void clearAllScriptDataFromSPIFFS();      // New helper function

// Interrupt handling for wakeup pin is defined in global_setting.cpp

// Helper function to check if a fetch should be performed based on time criteria
bool shouldPerformFetch(const char* caller) {
    if (g_full_refresh_intended) {
        log_i("%s: Allowing fetch: Full refresh is intended.", caller);
        return true;
    }

    // Get current time and date from RTC
    RTC_Time currentTime;
    RTC_Date currentDate;
    M5.RTC.getTime(&currentTime);
    M5.RTC.getDate(&currentDate);

    bool allowFetch = false;
    int elapsed_minutes = 0;

    if (last_fetch_year == -1 || last_fetch_month == -1 || last_fetch_day == -1)
    { // First fetch or old version data
        log_i("%s: Allowing fetch: No previous full fetch date stored.", caller);
        allowFetch = true;
    }
    else if (currentDate.year != last_fetch_year ||
             currentDate.mon != last_fetch_month ||
             currentDate.day != last_fetch_day)
    {
        log_i("%s: Allowing fetch: Date changed (Last: %d-%02d-%02d, Now: %d-%02d-%02d).",
              caller, last_fetch_year, last_fetch_month, last_fetch_day,
              currentDate.year, currentDate.mon, currentDate.day);
        allowFetch = true;
    }
    else
    {
        // Date is the same, check time interval
        if (last_fetch_hour != -1)
        {
            if (currentTime.hour < last_fetch_hour)
            { // Crossed midnight (should be caught by date check, but defensive)
                elapsed_minutes = (currentTime.hour + 24 - last_fetch_hour) * 60 + (currentTime.min - last_fetch_minute);
            }
            else
            {
                elapsed_minutes = (currentTime.hour - last_fetch_hour) * 60 + (currentTime.min - last_fetch_minute);
            }
        }
        else
        {
            log_w("%s: Inconsistent RTC data: Date set but hour not. Allowing fetch.", caller);
            allowFetch = true;
        }

        if (elapsed_minutes >= 120)
        {
            log_i("%s: Allowing fetch: Same date, but >= 120 minutes passed (elapsed: %d min).", caller, elapsed_minutes);
            allowFetch = true;
        }
        else
        {
            log_i("%s: Skipping fetch: Same date and < 120 minutes passed since last successful fetch (Last: %02d:%02d, Now: %02d:%02d, Elapsed: %d min).",
                  caller, last_fetch_hour, last_fetch_minute, currentTime.hour, currentTime.min, elapsed_minutes);
            allowFetch = false;
        }
    }
    
    return allowFetch;
}

void clearAllScriptDataFromSPIFFS() {
    log_w("clearAllScriptDataFromSPIFFS: Clearing all script data (list.json and content files).");

    // Delete list.json
    if (SPIFFS.exists("/scripts/list.json")) {
        if (SPIFFS.remove("/scripts/list.json")) {
            log_i("clearAllScriptDataFromSPIFFS: Deleted /scripts/list.json");
        } else {
            log_e("clearAllScriptDataFromSPIFFS: Failed to delete /scripts/list.json");
        }
    } else {
        log_i("clearAllScriptDataFromSPIFFS: /scripts/list.json not found, no need to delete.");
    }

    // Delete all files in /scripts/content/
    File root = SPIFFS.open("/scripts/content");
    if (root) {
        if (root.isDirectory()) {
            File entry = root.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String pathComponent = entry.name();
                    String fullPathToDelete;
                    if (pathComponent.startsWith("/")) {
                        fullPathToDelete = pathComponent;
                    } else {
                        fullPathToDelete = String("/scripts/content/") + pathComponent;
                    }
                    log_i("clearAllScriptDataFromSPIFFS: Attempting to delete: %s", fullPathToDelete.c_str());
                    if (!SPIFFS.remove(fullPathToDelete.c_str())) {
                        log_e("clearAllScriptDataFromSPIFFS: Failed to delete %s", fullPathToDelete.c_str());
                    }
                }
                entry.close();
                entry = root.openNextFile();
            }
        } else {
            log_w("clearAllScriptDataFromSPIFFS: /scripts/content is not a directory.");
        }
        root.close();
    } else {
        log_w("clearAllScriptDataFromSPIFFS: Could not open /scripts/content directory.");
    }
    log_i("clearAllScriptDataFromSPIFFS: Script data clearing completed.");
}


void setup()
{
    SysInit_Start();

    // Initialize watchdog for the main task (Core 1) early
    esp_task_wdt_init(30, false); // 30 second timeout, don't panic on timeout
    esp_task_wdt_add(NULL);       // Add current task to watchdog

    canvas.createCanvas(540, 960);
    if (!canvas.frameBuffer())
    {
        log_e("CRITICAL: Failed to create canvas framebuffer! Restarting device.");
        // No point trying to draw an error message if canvas itself failed.
        // A restart is the most robust option.
        esp_restart(); // This function does not return.
    }
    log_i("Canvas created: %d x %d", canvas.width(), canvas.height());
    esp_task_wdt_reset();

    // --- Step 1: Immediate Script Execution on Boot ---
    log_i("Setup: Attempting initial script execution before refresh checks.");
    handleWakeupAndScriptExecution(0); // Pass 0 to indicate timer/boot wakeup
    esp_task_wdt_reset();              // Reset watchdog after initial script execution

    // Increment freshStartCounter after initial display, so its value reflects this boot cycle
    freshStartCounter++;
    log_i("MicroPatterns M5Paper - Fresh Start Counter: %d (Threshold: %d)",
          freshStartCounter, FRESH_START_THRESHOLD);

    // --- Step 2: Full Refresh Logic (if needed) ---
    // Condition 1: First boot after RTC_DATA_ATTR variable is initialized (e.g., after full power cycle or new flash where counter is 0)
    // Condition 2: Counter reaches or exceeds the defined threshold
    if (freshStartCounter == 1)
    {
        log_i("Fresh Start: Counter is 1 (first boot cycle or RTC reset). Intending full data refresh.");
        g_full_refresh_intended = true; // Set the RTC flag
        if (canvas.frameBuffer()) { // Display message about full refresh intent
            canvas.fillCanvas(0);
            displayMessage("Full Refresh Pending...", 100, 15);
            canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            vTaskDelay(pdMS_TO_TICKS(1000)); // Show message
        }
    }
    else if (freshStartCounter > FRESH_START_THRESHOLD)
    {
        log_i("Fresh Start: Threshold (%d) reached/exceeded (Counter: %d). Intending full data refresh.", FRESH_START_THRESHOLD, freshStartCounter);
        g_full_refresh_intended = true; // Set the RTC flag
        freshStartCounter = 1; // Reset counter to 1 to restart the cycle towards threshold
        log_i("Fresh Start: Counter reset to 1.");
        if (canvas.frameBuffer()) { // Display message
            canvas.fillCanvas(0);
            displayMessage("Full Refresh Pending...", 100, 15);
            canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
            vTaskDelay(pdMS_TO_TICKS(1000)); // Show message
        }
    }
    // Ensure g_full_refresh_intended is false on the very first boot if freshStartCounter was 0 and became 1,
    // but we don't want an immediate full refresh without user scripts yet.
    // The above logic sets it to true if freshStartCounter IS 1.
    // If freshStartCounter was 0 (initial RTC state) and became 1, and no scripts exist,
    // g_full_refresh_intended will be true, and shouldPerformFetch will allow the first fetch.
    // This seems correct.

    esp_task_wdt_reset();

    // --- Step 3: Create FreeRTOS objects ---
    g_displayMessageQueue = xQueueCreate(5, sizeof(DisplayMsg));
    if (g_displayMessageQueue == NULL)
    {
        log_e("Failed to create display message queue!");
    }
    g_fetchRequestSemaphore = xSemaphoreCreateBinary();
    if (g_fetchRequestSemaphore == NULL)
    {
        log_e("Failed to create fetch request semaphore!");
    }

    // Create the fetching task, pinned to Core 0
    xTaskCreatePinnedToCore(
        fetchTaskFunction,  // Task function
        "FetchTask",        // Name of the task
        8192,               // Stack size in words (increased for WiFi/HTTPS)
        NULL,               // Task input parameter
        1,                  // Priority of the task (1 is common for app tasks)
        &g_fetchTaskHandle, // Task handle
        0                   // Core where the task should run (0)
    );
    if (g_fetchTaskHandle == NULL)
    {
        log_e("Failed to create fetch task!");
    }

    // --- Initial Fetch Check on Boot ---
    log_i("Setup: Checking fetch criteria on boot...");
    if (shouldPerformFetch("Setup")) { // shouldPerformFetch now checks g_full_refresh_intended first
        log_i("Setup: Triggering initial fetch request.");
        if (g_fetchRequestSemaphore != NULL) {
            xSemaphoreGive(g_fetchRequestSemaphore);
        } else {
            log_e("Setup: Fetch request semaphore is NULL, cannot trigger initial fetch.");
        }
    } else {
        log_i("Setup: No fetch triggered on boot (full refresh not intended and time criteria not met).");
    }
    // --- End Initial Fetch Check ---
}

void loop()
{
    // Reset watchdog at the start of each loop iteration
    esp_task_wdt_reset();

    // Process any messages from the fetch task for display
    DisplayMsg msg;
    if (g_displayMessageQueue != NULL && xQueueReceive(g_displayMessageQueue, &msg, 0) == pdTRUE)
    { // Non-blocking check
        if (msg.clear_canvas_first)
            canvas.fillCanvas(0); // Usually white
        displayMessage(msg.text, msg.y_offset, msg.color);
        canvas.pushCanvas(0, 0, msg.push_full_update ? UPDATE_MODE_GC16 : UPDATE_MODE_DU4);

        // Reset watchdog after potentially expensive canvas push
        esp_task_wdt_reset();

        // Brief delay for important messages, but use vTaskDelay instead of blocking delay
        if (msg.push_full_update && (strcmp(msg.text, "Fetch OK!") == 0 || strstr(msg.text, "Failed") != NULL || strstr(msg.text, "Interrupted") != NULL))
        {
            // Use vTaskDelay instead of delay to allow other tasks to run
            vTaskDelay(pdMS_TO_TICKS(500)); // Reduced from 1000ms to 500ms
            esp_task_wdt_reset();           // Reset watchdog after delay
        }
    }

    // Reset watchdog before potential wakeup handling (which can be expensive)
    esp_task_wdt_reset();

    // Process wakeup if either:
    // 1. We haven't handled it yet this wake cycle
    // 2. We just came out of sleep (g_wakeup_handled is reset in goToLightSleep after waking up)
    // This ensures we catch button presses even if they happen during fetches

    // Atomically load the current value of wakeup_pin set by ISR
    uint8_t pin_val_from_isr = __atomic_load_n(&wakeup_pin, __ATOMIC_SEQ_CST);

    if (!g_wakeup_handled)
    { // Process wakeup only once per cycle
        // Pass the pin_val_from_isr (could be 0 if timer wakeup or ISR didn't set)
        // handleWakeupAndScriptExecution will perform debouncing and clear the pin if handled.
        handleWakeupAndScriptExecution(pin_val_from_isr);
        g_wakeup_handled = true; // Mark wakeup as handled for this cycle
        esp_task_wdt_reset();    // Reset watchdog after potentially expensive wakeup handling
    }

    if (g_fetch_task_in_progress)
    {
        log_i("Loop: Fetch task is in progress, delaying sleep.");
        // Yield for a short period to allow fetch task to run
        esp_task_wdt_reset(); // Reset watchdog before delay
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    else
    {
        esp_task_wdt_reset(); // Reset watchdog before sleep
        goToLightSleep();
        // We'll only reach here after waking from sleep
        esp_task_wdt_reset(); // Reset watchdog after waking up
    }
}

void displayParseErrors()
{
    const auto &errors = parser.getErrors();
    canvas.fillCanvas(0); // White background
    displayMessage("Parse Error: " + currentScriptId, 50, 15);
    int y_pos = 100;
    for (const String &err : errors)
    {
        log_e("  %s", err.c_str());
        displayMessage(err, y_pos, 15);
        y_pos += 30;
        if (y_pos > canvas.height() - 50)
            break;
    }
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void handleWakeupAndScriptExecution(uint8_t raw_gpio_from_isr) // Modified signature
{
    bool scriptChangeRequest = false;
    bool moveUpDirection = false;
    // Fetch triggering is now handled in setup() and after timer wakeups in goToLightSleep()
    // bool fetchFromServerAfterExecution = false; // Removed

    // Capture initial fetch task state to make decisions based on state at entry
    // This is still useful for deciding whether to signal a restart if a button is pressed *during* a fetch.
    bool initial_fetch_task_state_is_progress = g_fetch_task_in_progress;
    bool signaled_restart_to_existing_fetch_this_cycle = false;

    uint8_t processed_gpio_event = 0; // Will hold the pin if it's a valid, debounced button event

    // 1. Determine Wakeup Reason & Intent (includes debouncing)
    if (raw_gpio_from_isr != 0)
    {
        // Attempt to "claim" this specific pin event from the ISR.
        // We try to change wakeup_pin from raw_gpio_from_isr to 0.
        // If successful, we own this event for debouncing.
        uint8_t expected_pin_val = raw_gpio_from_isr;
        if (__atomic_compare_exchange_n(&wakeup_pin, &expected_pin_val, (uint8_t)0, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        {
            // Successfully claimed and cleared the global wakeup_pin. Now debounce.
            uint32_t current_time = millis(); // Safe to call millis() in task context
            // Check debounce time OR handle millis() overflow
            if (((current_time - g_last_button_time) >= DEBOUNCE_TIME_MS || current_time < g_last_button_time))
            {
                g_last_button_time = current_time;        // Update time of last valid press
                processed_gpio_event = raw_gpio_from_isr; // This is a valid, debounced button event
                log_i("Wakeup: Button event for GPIO %d accepted (debounced).", processed_gpio_event);
            }
            else
            {
                log_i("Wakeup: Debounced out GPIO %d event.", raw_gpio_from_isr);
                // Event is debounced out, processed_gpio_event remains 0.
            }
        }
        else
        {
            // wakeup_pin was not raw_gpio_from_isr when we tried to clear it.
            // This means it might have been cleared by another call to this function (if g_wakeup_handled was false),
            // or changed by another ISR if the first ISR was slow to be processed by loop().
            // Given current ISR logic (only sets if wakeup_pin is 0), this case should be rare.
            // Treat as no valid button event for *this specific call*.
            log_w("Wakeup: Button event for GPIO %d was stale or changed during processing, ignoring.", raw_gpio_from_isr);
            // processed_gpio_event remains 0.
        }
    }

    // Proceed with logic based on processed_gpio_event
    if (processed_gpio_event != 0)
    {
        // This log is slightly redundant if the one above fired, but confirms it's being handled.
        log_i("Handling action for debounced GPIO %d.", processed_gpio_event);

        if (initial_fetch_task_state_is_progress) // Check against the state at the beginning of this function call
        {
            log_i("User action during fetch. Signaling fetch task to restart.");
            g_fetch_restart_pending = true;                       // Signal a graceful restart
            signaled_restart_to_existing_fetch_this_cycle = true; // Mark that this cycle signaled a restart
        }

        if (processed_gpio_event == BUTTON_UP_PIN)
        {
            log_i("Button UP (GPIO %d) detected.", processed_gpio_event);
            scriptChangeRequest = true;
            moveUpDirection = true;
            // fetchFromServerAfterExecution = true; // Removed - Fetch not triggered by script change
        }
        else if (processed_gpio_event == BUTTON_DOWN_PIN)
        {
            log_i("Button DOWN (GPIO %d) detected.", processed_gpio_event);
            scriptChangeRequest = true;
            moveUpDirection = false;
            // fetchFromServerAfterExecution = true; // Removed - Fetch not triggered by script change
        }
        else if (processed_gpio_event == BUTTON_PUSH_PIN)
        {
            log_i("Button PUSH (GPIO %d) detected.", processed_gpio_event);
            if (initial_fetch_task_state_is_progress)
            {
                log_i("PUSH button pressed during fetch. Signaling fetch task to restart.");
                g_fetch_restart_pending = true;
                signaled_restart_to_existing_fetch_this_cycle = true;
                esp_task_wdt_reset();
            }
            // For PUSH, just re-run current script. No fetch by default.
        }
    }
    else
    {
        log_i("Wakeup: Timer, or button event was debounced/stale.");
    }

    // 2. Handle Script Selection if Requested (for UP/DOWN)
    if (scriptChangeRequest)
    {
        if (!selectNextScript(moveUpDirection))
        {
            log_e("Failed to select next script. Will attempt to run current/default.");
        }
    }

    // 3. Load Script Content (current, newly selected, or default)
    if (!loadScriptToExecute())
    {
        log_w("Failed to load script from SPIFFS. Using default script as last resort.");
        currentScriptContent = default_script;
        currentScriptId = "default";
    }

    // 4. Parse and Prepare Runtime
    log_i("Preparing to execute script ID: %s", currentScriptId.c_str());
    parser.reset();
    if (!parser.parse(currentScriptContent))
    {
        log_e("Script parsing failed for ID: %s", currentScriptId.c_str());
        displayParseErrors();
        if (runtime)
        {
            delete runtime;
            runtime = nullptr;
        }
    }
    else
    {
        log_i("Script '%s' parsed successfully.", currentScriptId.c_str());
        if (runtime)
        {
            delete runtime;
        }
        runtime = new MicroPatternsRuntime(&canvas, parser.getAssets());
        runtime->setCommands(&parser.getCommands());
        runtime->setDeclaredVariables(&parser.getDeclaredVariables());
    }

    // 5. Execute Script (if runtime is valid)
    if (runtime)
    {
        // Reset watchdog timer before executing script to avoid timeouts
        esp_task_wdt_reset();

        int counterForThisExecution;
        int hourForThisExecution, minuteForThisExecution, secondForThisExecution;
        bool isResumeWakeup = (processed_gpio_event == 0); // True if timer wakeup, false if button press

        // Attempt to load persisted state for the current script
        bool stateLoaded = loadScriptExecutionState(currentScriptId.c_str(), counterForThisExecution, hourForThisExecution, minuteForThisExecution, secondForThisExecution);

        if (isResumeWakeup && stateLoaded) {
            // Resume wakeup and state was loaded: use the persisted state directly
            log_i("Resuming script '%s' with persisted state: C=%d, T=%02d:%02d:%02d",
                  currentScriptId.c_str(), counterForThisExecution, hourForThisExecution, minuteForThisExecution, secondForThisExecution);
            // Time and counter are already set from loaded state.
        } else {
            // Button press, or no persisted state, or first run for this script:
            // Use current RTC time.
            M5.RTC.getTime(&time_struct);
            hourForThisExecution = time_struct.hour;
            minuteForThisExecution = time_struct.min;
            secondForThisExecution = time_struct.sec;

            if (stateLoaded) {
                // State was loaded, but it's a button press, so increment the counter for this new execution.
                counterForThisExecution++;
            } else {
                // No state loaded (e.g., new script, or script_states.json cleared), start counter from 0.
                counterForThisExecution = 0;
            }
            log_i("Executing script '%s' with new/updated state: C=%d, T=%02d:%02d:%02d",
                  currentScriptId.c_str(), counterForThisExecution, hourForThisExecution, minuteForThisExecution, secondForThisExecution);
        }

        runtime->setTime(hourForThisExecution, minuteForThisExecution, secondForThisExecution);
        runtime->setCounter(counterForThisExecution);

        log_i("Executing script '%s' - Using Counter: %d, Time: %02d:%02d:%02d",
              currentScriptId.c_str(), counterForThisExecution, hourForThisExecution, minuteForThisExecution, secondForThisExecution);

        unsigned long executionStartTime = millis();
        runtime->execute(); // This includes drawing and pushing canvas
        unsigned long executionEndTime = millis();
        unsigned long executionDuration = executionEndTime - executionStartTime;

        log_i("Script '%s' execution and rendering took %lu ms.", currentScriptId.c_str(), executionDuration);

        // Save the state that was *used* for this execution.
        // This counter (counterForThisExecution) is the one that produced the current image.
        // If resumed, it's the same. If button pressed, it's the incremented one.
        saveScriptExecutionState(currentScriptId.c_str(), counterForThisExecution, hourForThisExecution, minuteForThisExecution, secondForThisExecution);

        // Reset watchdog timer after execution and state saving
        esp_task_wdt_reset();
        log_i("Finished execution for script '%s', counter %d.", currentScriptId.c_str(), counterForThisExecution);
    }
    else
    {
        log_e("Runtime not initialized, skipping execution. Check for parse errors.");
    }

    // 6. Trigger Fetch from Server if Requested - REMOVED
    // Fetch triggering is now handled in setup() and after timer wakeups in goToLightSleep()
}

void displayMessage(const String &msg, int y_offset, uint16_t color)
{
    if (!canvas.frameBuffer())
        return;
    canvas.setTextSize(3);
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextColor(color);
    canvas.drawString(msg, canvas.width() / 2, y_offset);
    log_i("Displaying message: %s", msg.c_str());
}

void fetchTaskFunction(void *pvParameters)
{
    DisplayMsg msg_to_send;
    log_i("FetchTask started and pinned to core %d", xPortGetCoreID());

    // Initialize watchdog for this task
    esp_task_wdt_init(30, false); // 30 second timeout, don't panic on timeout
    esp_task_wdt_add(NULL);       // Add current task to watchdog

    for (;;)
    {
        // Reset watchdog timer at the start of each cycle of the main loop of FetchTask
        esp_task_wdt_reset();

        // Explicitly unsubscribe FetchTask from WDT before blocking
        esp_err_t wdt_del_err = esp_task_wdt_delete(NULL);
        if (wdt_del_err != ESP_OK && wdt_del_err != ESP_ERR_NOT_FOUND)
        { // ESP_ERR_NOT_FOUND is okay.
            log_e("FetchTask: Failed to delete WDT for self before semaphore, error %d (%s)", wdt_del_err, esp_err_to_name(wdt_del_err));
        }

        BaseType_t semTakeResult = xSemaphoreTake(g_fetchRequestSemaphore, portMAX_DELAY);

        // Re-subscribe FetchTask to WDT immediately after unblocking
        esp_err_t wdt_add_err = esp_task_wdt_add(NULL);
        if (wdt_add_err != ESP_OK)
        {
            log_e("FetchTask: CRITICAL - Failed to re-add WDT for self after semaphore, error %d (%s). Task may not be WDT monitored.", wdt_add_err, esp_err_to_name(wdt_add_err));
            // Consider recovery or panic if this happens, e.g., esp_restart();
        }
        // Reset WDT immediately after re-subscribing. This is also critical.
        esp_task_wdt_reset();

        if (g_fetchRequestSemaphore != NULL && semTakeResult == pdTRUE)
        {
            g_fetch_task_in_progress = true; // Set flag immediately
            // Reset watchdog timer again after setting flag and before heavy operations
            esp_task_wdt_reset();
            log_i("FetchTask: Semaphore taken, g_fetch_task_in_progress=true. Starting fetch operation.");
            bool restart_fetch_immediately;
            bool first_attempt_in_cycle = true;

            do
            {
                restart_fetch_immediately = false;
                // g_user_interrupt_signal_for_fetch_task is for hard stop, cleared by perform_fetch_operations or if it triggers
                // g_fetch_restart_pending is for soft restart, cleared when acted upon below or by perform_fetch_operations

                bool is_full_refresh_for_this_attempt = g_full_refresh_intended; // Capture intent for this attempt

                // Send "Fetching scripts..." or "Restarting fetch..." message to UI task
                if (g_displayMessageQueue != NULL)
                {
                    if (first_attempt_in_cycle)
                    {
                        if (is_full_refresh_for_this_attempt) {
                            strncpy(msg_to_send.text, "Full Refreshing...", sizeof(msg_to_send.text) - 1);
                        } else {
                            strncpy(msg_to_send.text, "Fetching scripts...", sizeof(msg_to_send.text) - 1);
                        }
                    }
                    else
                    {
                        strncpy(msg_to_send.text, "Restarting fetch...", sizeof(msg_to_send.text) - 1);
                    }
                    msg_to_send.text[sizeof(msg_to_send.text) - 1] = '\0';
                    msg_to_send.y_offset = 50;
                    msg_to_send.color = 15;
                    msg_to_send.clear_canvas_first = false;
                    msg_to_send.push_full_update = false; // DU4 for quick message
                    xQueueSend(g_displayMessageQueue, &msg_to_send, pdMS_TO_TICKS(100));
                }
                first_attempt_in_cycle = false;

                FetchResultStatus fetch_status = perform_fetch_operations(is_full_refresh_for_this_attempt);

                // Prepare result message based on status
                bool send_final_message = true;
                msg_to_send.y_offset = 100; // Standard y_offset for result messages
                msg_to_send.color = 15;
                msg_to_send.clear_canvas_first = false;
                msg_to_send.push_full_update = true; // GC16 for final status

                switch (fetch_status)
                {
                case FETCH_SUCCESS:
                    if (is_full_refresh_for_this_attempt) {
                        strncpy(msg_to_send.text, "Full Refresh OK!", sizeof(msg_to_send.text) - 1);
                        g_full_refresh_intended = false; // Clear the intent flag
                        log_i("FetchTask: Full refresh completed successfully. Intent cleared.");
                    } else {
                        strncpy(msg_to_send.text, "Fetch OK!", sizeof(msg_to_send.text) - 1);
                        log_i("FetchTask: Incremental fetch completed successfully.");
                    }
                    disconnectWiFi(); // Disconnect on final success

                    // Update last successful fetch time and date
                    {
                        RTC_Time currentTime;
                        RTC_Date currentDate;
                        M5.RTC.getTime(&currentTime);
                        M5.RTC.getDate(&currentDate);
                        last_fetch_hour = currentTime.hour;
                        last_fetch_minute = currentTime.min;
                        last_fetch_day = currentDate.day;
                        last_fetch_month = currentDate.mon;
                        last_fetch_year = currentDate.year;
                        log_i("Updated last fetch timestamp to %d-%02d-%02d %02d:%02d",
                              last_fetch_year, last_fetch_month, last_fetch_day,
                              last_fetch_hour, last_fetch_minute);
                    }
                    break;
                case FETCH_GENUINE_ERROR:
                    strncpy(msg_to_send.text, "Fetch Failed!", sizeof(msg_to_send.text) - 1);
                    log_e("FetchTask: Failed with genuine error.");
                    disconnectWiFi(); // Disconnect on final error
                    break;
                case FETCH_INTERRUPTED_BY_USER: // Hard interrupt
                    strncpy(msg_to_send.text, "Fetch Interrupted!", sizeof(msg_to_send.text) - 1);
                    log_i("FetchTask: Hard interrupted by user.");
                    // disconnectWiFi() should have been called by perform_fetch_operations
                    break;
                case FETCH_NO_WIFI:
                    // This case now covers "SSID not found" and general "connection failed".
                    // The specific log in perform_fetch_operations will detail the cause.
                    strncpy(msg_to_send.text, "WiFi connection failed, skipping fetch", sizeof(msg_to_send.text) - 1);
                    msg_to_send.push_full_update = false; // DU4 for transient message
                    log_w("FetchTask: No WiFi connection or SSID not found, skipping fetch operation.");
                    disconnectWiFi(); // Ensure WiFi is off
                    break;
                case FETCH_RESTART_REQUESTED:
                    log_i("FetchTask: Restart requested. Looping.");
                    // UI message for restarting is handled by the next iteration's "Restarting fetch..."
                    send_final_message = false;      // Don't send a "final" message yet
                    g_fetch_restart_pending = false; // Consume the flag
                    restart_fetch_immediately = true;
                    esp_task_wdt_reset();           // Reset watchdog before delay
                    vTaskDelay(pdMS_TO_TICKS(200)); // Brief pause before restarting
                    break;
                }
                msg_to_send.text[sizeof(msg_to_send.text) - 1] = '\0';

                if (send_final_message && g_displayMessageQueue != NULL)
                {
                    xQueueSend(g_displayMessageQueue, &msg_to_send, pdMS_TO_TICKS(100));
                }

                // g_fetch_task_in_progress = false; // Moved to after the do...while loop

            } while (restart_fetch_immediately);

            // Make sure we're truly done with all operations
            esp_task_wdt_reset();

            g_fetch_task_in_progress = false; // Clear flag after all attempts for this semaphore signal are done
            // g_user_interrupt_signal_for_fetch_task should be false here unless a hard interrupt occurred.
            // g_fetch_restart_pending is false.
            log_i("FetchTask: Operation cycle finished, g_fetch_task_in_progress=false. Waiting for semaphore.");
        }
    }
} // Closing brace for fetchTaskFunction

// Helper struct for temporarily storing script data during full refresh
struct TempScriptData {
    String humanId;
    String name;
    String content;
};

FetchResultStatus perform_fetch_operations(bool isFullRefresh)
{
    // Clear hard interrupt signal for this specific attempt. Restart signal is handled differently.
    g_user_interrupt_signal_for_fetch_task = false;

    log_i("perform_fetch_operations: Starting. Full Refresh Mode: %s", isFullRefresh ? "YES" : "NO");

    // Reset watchdog before potentially long WiFi connection
    esp_task_wdt_reset();

    // Instead of scanning first, try to connect directly
    // This is more reliable as scanning can sometimes miss networks
    log_i("perform_fetch_operations: Attempting to connect to WiFi SSID '%s'...", WIFI_SSID);
    
    // Reset watchdog before potentially long WiFi connection
    esp_task_wdt_reset();
    
    // Try to connect directly without scanning first
    bool wifiConnected = connectToWiFi(); // connectToWiFi checks g_user_interrupt_signal_for_fetch_task
    
    if (g_user_interrupt_signal_for_fetch_task) {
        log_i("perform_fetch_operations: connectToWiFi hard-interrupted.");
        // connectToWiFi should have handled disconnect
        return FETCH_INTERRUPTED_BY_USER;
    }
    
    if (g_fetch_restart_pending) {
        log_i("perform_fetch_operations: Restart requested after connectToWiFi attempt.");
        // WiFi might be on or off depending on connectToWiFi result. If on, keep it on.
        return FETCH_RESTART_REQUESTED;
    }

    if (!wifiConnected) {
        log_w("perform_fetch_operations: WiFi connection failed. SSID '%s' may not be available.", WIFI_SSID);
        disconnectWiFi();  // Ensure WiFi is off
        return FETCH_NO_WIFI;
    }
    
    // WiFi is now connected (we checked above)

    // If we reach here, WiFi is connected.
    log_i("perform_fetch_operations: WiFi connected. Proceeding with fetch.");

    WiFiClientSecure httpsClient;
    httpsClient.setCACert(rootCACertificate);
    HTTPClient http;
    bool overall_fetch_successful = true; // Assume success unless something fails

    // --- Variables for both full and incremental refresh ---
    DynamicJsonDocument serverListDoc(4096); // To store the list fetched from server
    DynamicJsonDocument finalSpiifsListDoc(8192); // To build the list to be saved to SPIFFS
    JsonArray finalSpiifsArray = finalSpiifsListDoc.to<JsonArray>();
    std::set<String> assignedFileIdsThisFetch; // Track fileIds used in this fetch to ensure uniqueness

    // --- Fetch Script List from Server (common for both modes) ---
    String listUrl = String(API_BASE_URL) + "/api/device/scripts";
    log_i("perform_fetch_operations: Fetching script list from: %s", listUrl.c_str());

    if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
    if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

    if (!http.begin(httpsClient, listUrl)) {
        log_e("perform_fetch_operations: HTTPClient begin failed for list URL!");
        disconnectWiFi(); // Ensure WiFi is disconnected on error
        return FETCH_GENUINE_ERROR;
    }
    esp_task_wdt_reset();
    int httpCode = http.GET();
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_task_wdt_reset();

    if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
    if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

    if (httpCode == HTTP_CODE_OK) {
        String serverJsonPayload = http.getString();
        if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
        if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

        DeserializationError error = deserializeJson(serverListDoc, serverJsonPayload);
        if (error) {
            log_e("perform_fetch_operations: Failed to parse server script list JSON: %s", error.c_str());
            overall_fetch_successful = false;
        } else if (!serverListDoc.is<JsonArray>()) {
            log_e("perform_fetch_operations: Server script list JSON is not an array.");
            overall_fetch_successful = false;
        }
    } else {
        log_e("perform_fetch_operations: HTTP error fetching script list: %d (%s)", httpCode, http.errorToString(httpCode).c_str());
        overall_fetch_successful = false;
    }
    http.end();

    if (!overall_fetch_successful) {
        return FETCH_GENUINE_ERROR; // Disconnect will be handled by caller task for GENUINE_ERROR
    }
    if (!serverListDoc.is<JsonArray>() || serverListDoc.as<JsonArray>().size() == 0) {
        log_w("perform_fetch_operations: Server script list is empty or invalid. Saving an empty list locally.");
        if (isFullRefresh) {
            clearAllScriptDataFromSPIFFS(); // Clear old data if full refresh and server list is empty
        }
        DynamicJsonDocument emptyListDoc(JSON_ARRAY_SIZE(0)); // Create an empty array
        emptyListDoc.to<JsonArray>();
        if (!saveScriptList(emptyListDoc)) {
             log_e("perform_fetch_operations: Failed to save empty script list to SPIFFS.");
             return FETCH_GENUINE_ERROR;
        }
        // Also clean up script states if we're saving an empty list
        DynamicJsonDocument emptyStatesDoc(JSON_OBJECT_SIZE(0));
        emptyStatesDoc.to<JsonObject>(); // Create an empty object
        File statesFile = SPIFFS.open(SCRIPT_STATES_PATH, FILE_WRITE);
        if (statesFile) {
            serializeJson(emptyStatesDoc, statesFile);
            statesFile.close();
            log_i("perform_fetch_operations: Cleared script states due to empty server list.");
        } else {
            log_e("perform_fetch_operations: Failed to open script_states.json for clearing.");
        }
        return FETCH_SUCCESS; // Successfully processed an empty list from server
    }

    // --- Branch logic for Full Refresh vs Incremental ---
    if (isFullRefresh) {
        log_i("perform_fetch_operations: --- Full Refresh Path ---");
        std::vector<TempScriptData> fetchedScripts;
        JsonArray serverArray = serverListDoc.as<JsonArray>();
        log_i("perform_fetch_operations (Full): Processing %d scripts from server list for content fetch.", serverArray.size());

        // IMPORTANT: First fetch ALL script contents BEFORE clearing SPIFFS
        // This ensures we don't wipe existing scripts if the fetch fails
        int scriptCounter = 0;
        for (JsonObject serverScriptInfo : serverArray) {
            if (scriptCounter++ % 3 == 0) esp_task_wdt_reset();
            if (g_user_interrupt_signal_for_fetch_task) { disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
            if (g_fetch_restart_pending) { return FETCH_RESTART_REQUESTED; }

            const char *humanId = serverScriptInfo["id"];
            const char *humanName = serverScriptInfo["name"];
            if (!humanId || strlen(humanId) == 0) {
                log_w("perform_fetch_operations (Full): Skipping script from server with missing 'id'.");
                continue;
            }

            String scriptUrl = String(API_BASE_URL) + "/api/scripts/" + humanId;
            if (!http.begin(httpsClient, scriptUrl)) {
                log_e("perform_fetch_operations (Full): HTTPClient begin failed for script content URL: %s", scriptUrl.c_str());
                overall_fetch_successful = false;
                esp_task_wdt_reset(); // Reset watchdog before potentially breaking
                break; // Abort full refresh
            }
            int scriptHttpCode = http.GET();
            vTaskDelay(pdMS_TO_TICKS(10)); esp_task_wdt_reset();
            
            if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
            if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

            if (scriptHttpCode == HTTP_CODE_OK) {
                String scriptPayload = http.getString();
                http.end(); // End connection for this script content fetch
                
                DynamicJsonDocument scriptDoc(8192);
                DeserializationError scriptError = deserializeJson(scriptDoc, scriptPayload);
                if (scriptError) {
                    log_e("perform_fetch_operations (Full): Failed to parse script content JSON for humanId %s: %s", humanId, scriptError.c_str());
                    overall_fetch_successful = false; break;
                }
                const char *scriptContentFetched = scriptDoc["content"];
                if (scriptContentFetched) {
                    fetchedScripts.push_back({String(humanId), String(humanName ? humanName : humanId), String(scriptContentFetched)});
                    log_i("perform_fetch_operations (Full): Successfully fetched content for humanId %s", humanId);
                } else {
                    log_e("perform_fetch_operations (Full): Missing 'content' field for humanId %s.", humanId);
                    overall_fetch_successful = false; break;
                }
            } else {
                http.end(); // End connection on error
                log_w("perform_fetch_operations (Full): HTTP error %d (%s) fetching script content for humanId %s.",
                      scriptHttpCode, http.errorToString(scriptHttpCode).c_str(), humanId);
                overall_fetch_successful = false; break;
            }
        } // End loop for fetching all contents

        if (!overall_fetch_successful || fetchedScripts.empty()) {
            log_e("perform_fetch_operations (Full): Aborting full refresh due to errors in fetching all script contents or no scripts fetched.");
            return FETCH_GENUINE_ERROR;
        }

        // All content fetched successfully, NOW clear old SPIFFS data and save new
        log_i("perform_fetch_operations (Full): All %d script contents fetched successfully. NOW clearing old data and saving new.", fetchedScripts.size());
        clearAllScriptDataFromSPIFFS();
        esp_task_wdt_reset();

        int fileIdCounter = 0;
        for (const auto& scriptData : fetchedScripts) {
            String newFileId = "s" + String(fileIdCounter++);
            if (saveScriptContent(newFileId.c_str(), scriptData.content.c_str())) {
                JsonObject newEntry = finalSpiifsArray.createNestedObject();
                newEntry["id"] = scriptData.humanId;
                newEntry["name"] = scriptData.name;
                newEntry["fileId"] = newFileId;
                log_i("perform_fetch_operations (Full): Saved script content for humanId %s with fileId %s", scriptData.humanId.c_str(), newFileId.c_str());
            } else {
                log_e("perform_fetch_operations (Full): Failed to save script content for humanId %s (new fileId %s). Critical error during save phase.", scriptData.humanId.c_str(), newFileId.c_str());
                // This is a critical error after clearing, might leave system in bad state.
                // For now, mark as overall failure.
                overall_fetch_successful = false;
                break;
            }
        }
        if (!overall_fetch_successful) {
            log_e("perform_fetch_operations (Full): Failed during saving phase after clearing SPIFFS. System may be inconsistent.");
            return FETCH_GENUINE_ERROR; // Indicate a serious problem
        }

    } else { // --- Incremental Refresh Path ---
        log_i("perform_fetch_operations: --- Incremental Refresh Path ---");
        DynamicJsonDocument oldSpiifsListDoc(8192);
        bool oldListLoaded = loadScriptList(oldSpiifsListDoc);
        if (oldListLoaded) {
            log_i("perform_fetch_operations (Inc): Successfully loaded existing list.json to help preserve fileIds (%d entries).", oldSpiifsListDoc.as<JsonArray>().size());
        } else {
            log_w("perform_fetch_operations (Inc): Could not load existing list.json or it was empty/invalid.");
        }
        vTaskDelay(pdMS_TO_TICKS(10));

        JsonArray serverArray = serverListDoc.as<JsonArray>();
        log_i("perform_fetch_operations (Inc): Processing %d scripts from server list.", serverArray.size());
        int scriptCounter = 0;

        for (JsonObject serverScriptInfo : serverArray) {
            // (Logic from existing incremental path - reconcile, fetch missing, assign fileIds)
            // This part needs to be carefully merged from the original perform_fetch_operations
            // For brevity, assuming the existing complex reconciliation logic is here.
            // Key parts:
            // - Determine fileId (reuse from oldSpiifsListDoc or generate new)
            // - Check if content exists or needs fetching (based on existence or ideally lastModified)
            // - Fetch content if needed
            // - Add to finalSpiifsArray
            // - Ensure assignedFileIdsThisFetch is updated

            // Reset watchdog periodically during script processing
            if (scriptCounter++ % 3 == 0) {
                esp_task_wdt_reset();
            }
            if (g_user_interrupt_signal_for_fetch_task) { disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
            if (g_fetch_restart_pending) { return FETCH_RESTART_REQUESTED; }

            const char *humanId = serverScriptInfo["id"];
            const char *humanName = serverScriptInfo["name"];
            // const char *serverLastModified = serverScriptInfo["lastModified"]; // If using lastModified checks

            if (!humanId || strlen(humanId) == 0) {
                log_w("perform_fetch_operations (Inc): Skipping script from server with missing 'id'.");
                continue;
            }

            String determinedFileId = "";
            bool foundInOldList = false;

            if (oldListLoaded && oldSpiifsListDoc.is<JsonArray>()) {
                for (JsonObject oldScriptInfo : oldSpiifsListDoc.as<JsonArray>()) {
                    const char *oldHumanId = oldScriptInfo["id"];
                    const char *oldFileId = oldScriptInfo["fileId"];
                    if (oldHumanId && oldFileId && strcmp(oldHumanId, humanId) == 0) {
                        if (assignedFileIdsThisFetch.count(oldFileId)) {
                            log_w("perform_fetch_operations (Inc): FileId '%s' for humanId '%s' from old list already assigned. Generating new.", oldFileId, humanId);
                        } else {
                            determinedFileId = oldFileId;
                            foundInOldList = true;
                            break;
                        }
                    }
                }
            }

            if (foundInOldList) {
                log_i("perform_fetch_operations (Inc): Reusing fileId '%s' for humanId '%s'.", determinedFileId.c_str(), humanId);
                assignedFileIdsThisFetch.insert(determinedFileId);
            } else {
                int tempIdCounter = 0;
                do {
                    determinedFileId = "s" + String(tempIdCounter++);
                } while (assignedFileIdsThisFetch.count(determinedFileId));
                log_i("perform_fetch_operations (Inc): Assigning new fileId '%s' for humanId '%s'.", determinedFileId.c_str(), humanId);
                assignedFileIdsThisFetch.insert(determinedFileId);
            }

            String contentPath = "/scripts/content/" + determinedFileId;
            bool contentAvailableOrFetched = false;

            // TODO: Add lastModified check here if server provides it and we store it.
            // For now, just check existence.
            if (SPIFFS.exists(contentPath.c_str())) {
                log_d("perform_fetch_operations (Inc): Content for humanId '%s' (fileId '%s') already exists.", humanId, determinedFileId.c_str());
                contentAvailableOrFetched = true;
            } else {
                log_i("perform_fetch_operations (Inc): Content for humanId '%s' (fileId '%s') missing, fetching.", humanId, determinedFileId.c_str());
                String scriptUrl = String(API_BASE_URL) + "/api/scripts/" + humanId;
                if (!http.begin(httpsClient, scriptUrl)) {
                    log_e("perform_fetch_operations (Inc): HTTPClient begin failed for script content URL: %s", scriptUrl.c_str());
                    assignedFileIdsThisFetch.erase(determinedFileId); // Release ID
                    continue; // Skip this script
                }
                int scriptHttpCode = http.GET();
                vTaskDelay(pdMS_TO_TICKS(10)); esp_task_wdt_reset();
                http.end();

                if (g_user_interrupt_signal_for_fetch_task) { disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                if (g_fetch_restart_pending) { return FETCH_RESTART_REQUESTED; }

                if (scriptHttpCode == HTTP_CODE_OK) {
                    String scriptPayload = http.getString();
                    DynamicJsonDocument scriptDoc(8192);
                    DeserializationError scriptError = deserializeJson(scriptDoc, scriptPayload);
                    if (scriptError) {
                        log_e("perform_fetch_operations (Inc): Failed to parse script content JSON for %s: %s", humanId, scriptError.c_str());
                    } else {
                        const char *scriptContentFetched = scriptDoc["content"];
                        if (scriptContentFetched) {
                            if (saveScriptContent(determinedFileId.c_str(), scriptContentFetched)) {
                                contentAvailableOrFetched = true;
                            } else {
                                log_e("perform_fetch_operations (Inc): Failed to save content for %s (fileId %s).", humanId, determinedFileId.c_str());
                            }
                        } else {
                            log_e("perform_fetch_operations (Inc): Missing 'content' for %s.", humanId);
                        }
                    }
                } else {
                    log_w("perform_fetch_operations (Inc): HTTP error %d fetching content for %s.", scriptHttpCode, humanId);
                }
            }

            if (contentAvailableOrFetched) {
                JsonObject newEntry = finalSpiifsArray.createNestedObject();
                newEntry["id"] = humanId;
                newEntry["name"] = humanName ? humanName : humanId;
                newEntry["fileId"] = determinedFileId;
                // newEntry["lastModified"] = serverLastModified; // If storing this
            } else {
                log_w("perform_fetch_operations (Inc): Script %s (fileId %s) excluded, content unavailable/save failed.", humanId, determinedFileId.c_str());
                assignedFileIdsThisFetch.erase(determinedFileId);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    } // End of Full vs Incremental branch

    // --- Common operations for both paths (Cleanup, Save List) ---
    esp_task_wdt_reset();

    // Cleanup Orphaned Script Content Files (only if not a full refresh, as full refresh clears all first)
    if (!isFullRefresh) {
        log_i("perform_fetch_operations: Cleaning up orphaned script content files (Incremental Mode)...");
        File root = SPIFFS.open("/scripts/content");
        if (root && root.isDirectory()) {
            File entry = root.openNextFile();
            while (entry) {
                if (g_user_interrupt_signal_for_fetch_task) { root.close(); entry.close(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                if (g_fetch_restart_pending) { root.close(); entry.close(); return FETCH_RESTART_REQUESTED; }
                if (!entry.isDirectory()) {
                    String entryName = entry.name();
                    String pathToRemove;
                    String fileIdFromPath;
                    if (entryName.startsWith("/")) { pathToRemove = entryName; }
                    else { pathToRemove = "/scripts/content/" + entryName; }
                    fileIdFromPath = pathToRemove.substring(pathToRemove.lastIndexOf('/') + 1);

                    bool foundInFinalList = false;
                    for (JsonObject scriptInFinalList : finalSpiifsArray) {
                        if (fileIdFromPath == scriptInFinalList["fileId"].as<String>()) {
                            foundInFinalList = true;
                            break;
                        }
                    }
                    if (!foundInFinalList) {
                        log_i("perform_fetch_operations: Removing orphaned script content: %s", pathToRemove.c_str());
                        if (!SPIFFS.remove(pathToRemove)) {
                            log_e("perform_fetch_operations: Failed to remove %s", pathToRemove.c_str());
                        }
                    }
                }
                entry.close();
                entry = root.openNextFile();
            }
            root.close();
        } else {
            log_w("perform_fetch_operations: Could not open /scripts/content for cleanup or not a directory.");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }


    // Save the New list.json
    if (!saveScriptList(finalSpiifsListDoc)) {
        log_e("perform_fetch_operations: Failed to save final script list to SPIFFS.");
        overall_fetch_successful = false; // This is a critical failure
    } else {
        // State Cleanup Logic (common for both full and incremental, happens after successful list save)
        log_i("perform_fetch_operations: Cleaning up orphaned script execution states...");
        DynamicJsonDocument currentStatesDoc(2048);
        File statesFile = SPIFFS.open(SCRIPT_STATES_PATH, FILE_READ);
        bool statesLoaded = false;
        if (!statesFile || statesFile.isDirectory() || statesFile.size() == 0) {
            if(statesFile) statesFile.close();
            log_w("perform_fetch_operations: %s not found, is directory, or empty. No states to clean.", SCRIPT_STATES_PATH);
        } else {
            DeserializationError error = deserializeJson(currentStatesDoc, statesFile);
            statesFile.close();
            if (error) {
                log_e("perform_fetch_operations: Failed to parse %s for cleanup: %s. Skipping cleanup.", SCRIPT_STATES_PATH, error.c_str());
            } else if (!currentStatesDoc.is<JsonObject>()) {
                log_e("perform_fetch_operations: %s content not a JSON object. Skipping cleanup.", SCRIPT_STATES_PATH);
            } else {
                statesLoaded = true;
            }
        }

        if (statesLoaded) {
            std::set<String> validHumanIds;
            for (JsonObject scriptInfo : finalSpiifsArray) { // Use finalSpiifsArray directly
                 const char* humanId = scriptInfo["id"];
                 if (humanId) validHumanIds.insert(String(humanId));
            }
            log_i("Found %d valid human script IDs in the final list for state cleanup.", validHumanIds.size());
            JsonObject statesRoot = currentStatesDoc.as<JsonObject>();
            bool statesModified = false;
            std::vector<String> keysToRemove;
            for (JsonPair kv : statesRoot) {
                String scriptIdKey = kv.key().c_str();
                if (validHumanIds.find(scriptIdKey) == validHumanIds.end()) {
                    keysToRemove.push_back(scriptIdKey);
                    statesModified = true;
                }
            }
            if (statesModified) {
                log_i("Removing states for %d orphaned script IDs:", keysToRemove.size());
                for (const String& key : keysToRemove) {
                    log_i("  - Removing state for: %s", key.c_str());
                    statesRoot.remove(key);
                }
                statesFile = SPIFFS.open(SCRIPT_STATES_PATH, FILE_WRITE);
                if (!statesFile) {
                    log_e("perform_fetch_operations: Failed to open %s for writing cleaned states.", SCRIPT_STATES_PATH);
                } else {
                     if (serializeJson(currentStatesDoc, statesFile) > 0) {
                         log_i("Successfully saved cleaned-up script states to %s.", SCRIPT_STATES_PATH);
                     } else {
                         log_e("Failed to write updated states to %s.", SCRIPT_STATES_PATH);
                     }
                     statesFile.close();
                }
            } else {
                log_i("No orphaned script states found to remove.");
            }
        }
    }

    // Final checks before returning
    if (g_user_interrupt_signal_for_fetch_task) { disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
    if (g_fetch_restart_pending) { return FETCH_RESTART_REQUESTED; }

    return overall_fetch_successful ? FETCH_SUCCESS : FETCH_GENUINE_ERROR;
}

// Selects the next script ID based on the direction (up/down)
bool selectNextScript(bool moveUp)
{
    DynamicJsonDocument listDoc(4096);

    if (!loadScriptList(listDoc))
    {
        log_e("Cannot select next script, failed to load script list from SPIFFS.");
        return false;
    }
    if (!listDoc.is<JsonArray>() || listDoc.as<JsonArray>().size() == 0)
    {
        log_e("Cannot select next script, list is empty or not an array after loading. Fetch will not be triggered from here.");
        // Fetch is no longer triggered from here.
        // The main loop will proceed, and loadScriptToExecute will handle the missing/bad list,
        // potentially falling back to default. A fetch might occur later if it's a boot/timer-wakeup.
        return false;
    }

    JsonArray scriptList = listDoc.as<JsonArray>();
    vTaskDelay(pdMS_TO_TICKS(10)); // Yield after SPIFFS load

    String loadedCurrentId;
    loadCurrentScriptId(loadedCurrentId);

    int currentIndex = -1;
    for (int i = 0; i < scriptList.size(); i++)
    {
        JsonObject scriptInfo = scriptList[i];
        const char *idJson = scriptInfo["id"];
        if (idJson && loadedCurrentId == idJson)
        {
            currentIndex = i;
            break;
        }
    }

    int nextIndex = 0;
    if (scriptList.size() > 0)
    {
        if (currentIndex != -1)
        {
            int delta = moveUp ? -1 : 1;
            nextIndex = (currentIndex + delta + scriptList.size()) % scriptList.size();
        }
        else
        {
            nextIndex = moveUp ? scriptList.size() - 1 : 0;
        }
        JsonObject nextScriptInfo = scriptList[nextIndex];
        const char *nextId = nextScriptInfo["id"];
        const char *nextName = nextScriptInfo["name"];

        if (nextId)
        {
            log_i("Selected script index: %d, ID: %s, Name: %s", nextIndex, nextId, nextName ? nextName : "N/A");
            if (saveCurrentScriptId(nextId))
            {
                vTaskDelay(pdMS_TO_TICKS(10)); // Yield after SPIFFS save
                canvas.fillCanvas(0);
                displayMessage((nextName ? nextName : nextId), 150, 15);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
                vTaskDelay(pdMS_TO_TICKS(10));   // Yield after push
                vTaskDelay(pdMS_TO_TICKS(1500)); // Changed from delay()
                return true;
            }
            else
            {
                log_e("Failed to save the new current script ID: %s", nextId);
                return false;
            }
        }
        else
        {
            log_e("Script at index %d has no ID.", nextIndex);
            return false;
        }
    }
    else
    {
        // This case should be caught by the earlier check, but defensive.
        log_e("Script list is empty, cannot select.");
        return false;
    }
}

// Loads the current script ID and its content into global variables
bool loadScriptToExecute()
{
    String humanReadableScriptIdFromSpiffs; // ID from /current_script.id
    bool spiffsIdLoaded = false;
    String determinedFileId = "";
    String finalHumanIdForExecution = ""; // This will be set to the ID we actually try to load content for

    // 1. Load human-readable ID from /current_script.id
    if (loadCurrentScriptId(humanReadableScriptIdFromSpiffs) && humanReadableScriptIdFromSpiffs.length() > 0)
    {
        log_i("loadScriptToExecute: Current human-readable script ID from SPIFFS: '%s'", humanReadableScriptIdFromSpiffs.c_str());
        spiffsIdLoaded = true;
    }
    else
    {
        log_w("loadScriptToExecute: No current script ID found in SPIFFS or it was empty. Will try first from list.");
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // 2. Load the script list
    DynamicJsonDocument listDoc(4096);
    bool listIsProblematic = false; // Flag if list.json seems to have issues
    JsonArray scriptList;
    bool hasScriptsInList = false;

    if (loadScriptList(listDoc) && listDoc.is<JsonArray>() && listDoc.as<JsonArray>().size() > 0)
    {
        scriptList = listDoc.as<JsonArray>();
        hasScriptsInList = scriptList.size() > 0;
        log_i("loadScriptToExecute: Script list loaded with %d entries.", scriptList.size());
        
        if (hasScriptsInList) {
            JsonObject firstEntryTest = scriptList[0];
            const char *testId = firstEntryTest["id"];
            const char *testFileId = firstEntryTest["fileId"];
            if (!testId || strlen(testId) == 0 || !testFileId || strlen(testFileId) == 0)
            {
                log_w("loadScriptToExecute: First script in list.json is missing 'id' or 'fileId'. List may be problematic.");
                listIsProblematic = true;
            }
        }
    }
    else
    {
        log_e("loadScriptToExecute: Failed to load script list, or list is empty/invalid. Marking as problematic.");
        listIsProblematic = true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // 3. Try to use humanReadableScriptIdFromSpiffs if it was loaded and list is not problematic
    bool foundAndMappedSpiffsId = false;
    if (spiffsIdLoaded && !listIsProblematic && hasScriptsInList)
    {
        bool idExistsInList = false;
        for (JsonObject scriptInfo : scriptList)
        {
            const char *hId = scriptInfo["id"];
            if (hId && humanReadableScriptIdFromSpiffs == hId)
            {
                idExistsInList = true;
                const char *fId = scriptInfo["fileId"];
                if (fId && strlen(fId) > 0)
                {
                    determinedFileId = fId;
                    finalHumanIdForExecution = humanReadableScriptIdFromSpiffs;
                    foundAndMappedSpiffsId = true;
                    log_i("loadScriptToExecute: Found fileId '%s' for humanId '%s' (from SPIFFS preference).", determinedFileId.c_str(), finalHumanIdForExecution.c_str());
                }
                else
                {
                    log_w("loadScriptToExecute: HumanId '%s' (from SPIFFS preference) found in list but has no valid fileId.", humanReadableScriptIdFromSpiffs.c_str());
                    // Don't mark list as problematic here, just try the first script instead
                }
                break;
            }
        }
        if (!idExistsInList)
        {
            log_w("loadScriptToExecute: HumanId '%s' (from SPIFFS preference) not found in the script list. Will try first from list.", humanReadableScriptIdFromSpiffs.c_str());
        }
    }

    // 4. If spiffsId wasn't used, try first script from list (if list is usable)
    if (!foundAndMappedSpiffsId && !listIsProblematic && hasScriptsInList)
    {
        log_i("loadScriptToExecute: Attempting to use first script from the list.");
        JsonObject firstScript = scriptList[0];
        const char *firstHumanId = firstScript["id"];
        const char *firstFileId = firstScript["fileId"];

        if (firstHumanId && strlen(firstHumanId) > 0 && firstFileId && strlen(firstFileId) > 0)
        {
            finalHumanIdForExecution = firstHumanId;
            determinedFileId = firstFileId;
            log_i("loadScriptToExecute: Using first script: humanId '%s', fileId '%s'", finalHumanIdForExecution.c_str(), determinedFileId.c_str());
            if (!saveCurrentScriptId(finalHumanIdForExecution.c_str()))
            {
                log_w("loadScriptToExecute: Failed to save the first script's humanId ('%s') as current to SPIFFS.", finalHumanIdForExecution.c_str());
            }
        }
        else
        {
            // This case implies listIsProblematic should have been true.
            log_e("loadScriptToExecute: First script in list has no valid humanId or fileId, despite earlier checks. List is problematic.");
            listIsProblematic = true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 5. If we still don't have a fileId (due to list problems or empty list), check if we need to trigger a fetch
    if (determinedFileId.length() == 0)
    {
        // Only use default script if we have no scripts at all on the device
        if (!hasScriptsInList) {
            log_w("loadScriptToExecute: No scripts found in list. Using default script as last resort.");
            currentScriptId = "default";
            currentScriptContent = default_script;
            
            // Fetch is no longer triggered from here.
            log_w("loadScriptToExecute: No scripts found in list or list problematic. Using default script. Fetch will not be triggered from here.");
            // A fetch might occur later if it's a boot/timer-wakeup scenario and shouldPerformFetch allows.
            return true; // Return true since we're using the default script
        }
        
        // If listIsProblematic but hasScriptsInList is false, the above block handles it.
        // If listIsProblematic is true AND hasScriptsInList is true, it means we have a list, but it's flawed.
        // We will proceed to try and load based on finalHumanIdForExecution / determinedFileId if they were set.
        // If they weren't set (e.g. first script in problematic list was bad), determinedFileId will be empty.

        // If determinedFileId is still empty at this point, it means:
        // - List was problematic and first entry was bad, OR
        // - SPIFFS ID was loaded but not found in a problematic list, OR
        // - Some other edge case where we couldn't map to a fileId.
        // In this scenario, we fall back to default.
        if (determinedFileId.length() == 0) { // This check is now more encompassing
            log_w("loadScriptToExecute: Could not determine a fileId due to list issues or missing entries. Using default script.");
            currentScriptId = "default";
            currentScriptContent = default_script;
            // Fetch is not triggered from here.
            return true;
        }
    }

    // 6. Load script content using the determined fileId
    currentScriptId = finalHumanIdForExecution;
    log_i("loadScriptToExecute: Attempting to load content for humanId '%s' using fileId '%s'", currentScriptId.c_str(), determinedFileId.c_str());
    if (!loadScriptContent(determinedFileId.c_str(), currentScriptContent))
    {
        log_e("loadScriptToExecute: Failed to load script content for fileId '%s' (humanId '%s'). This file should exist according to list.json.", determinedFileId.c_str(), currentScriptId.c_str());
        
        // Try to use default script only if we have no scripts at all
        if (!hasScriptsInList) { // This implies list.json was empty or didn't exist
            log_w("loadScriptToExecute: No scripts available (list empty/missing). Using default script as last resort.");
            currentScriptId = "default";
            currentScriptContent = default_script;
            // Fetch is not triggered from here.
            return true;
        }
        
        // If content is missing for a script that list.json says should exist:
        log_e("loadScriptToExecute: Content for humanId '%s' (fileId '%s') is missing, but list.json entry exists. Using default script.", currentScriptId.c_str(), determinedFileId.c_str());
        currentScriptId = "default"; // Fallback to default
        currentScriptContent = default_script;
        // Fetch is not triggered from here. A future fetch (on boot/timer) should repair this.
        return true; // Return true as we are providing default script
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    log_i("loadScriptToExecute: Successfully set up script humanId '%s' (fileId '%s') for execution.", currentScriptId.c_str(), determinedFileId.c_str());
    return true;
}
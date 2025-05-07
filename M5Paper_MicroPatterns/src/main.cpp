#include <M5EPD.h>
#include "systeminit.h" // For M5Paper hardware init
#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "micropatterns_drawing.h" // Drawing depends on runtime state
#include "global_setting.h"        // For settings, WiFi, SPIFFS, sleep
#include "driver/rtc_io.h"         // For rtc_gpio functions
#include "driver/gpio.h"           // For gpio_num_t
#include <esp_sleep.h>             // Include for sleep functions like esp_sleep_get_gpio_wakeup_status
#include <HTTPClient.h>            // For fetching scripts
#include <ArduinoJson.h>           // For parsing JSON
#include <SPIFFS.h>                // For saving/loading scripts
#include <WiFi.h>                  // Include WiFi header
#include <WiFiClientSecure.h>      // For HTTPS client configuration
#include <set>                     // For std::set

// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

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
RTC_DATA_ATTR int executionCounter = 0; // Use RTC memory to persist counter across sleep
RTC_Time time_struct;                   // To store time from RTC
String currentScriptId = "";            // ID of the script to execute
String currentScriptContent = "";       // Content of the script to execute

// --- Tasking Globals ---
TaskHandle_t g_fetchTaskHandle = NULL;
SemaphoreHandle_t g_fetchRequestSemaphore = NULL; // Binary semaphore to trigger fetch
QueueHandle_t g_displayMessageQueue = NULL;       // Queue for messages from fetch task to display task

volatile bool g_fetch_task_in_progress = false;
volatile bool g_user_interrupt_signal_for_fetch_task = false; // Signal to fetch task to stop (hard stop)
volatile bool g_fetch_restart_pending = false; // Signal to fetch task to restart gracefully

struct DisplayMsg
{
    char text[64]; // Increased size for longer messages
    int y_offset;
    uint16_t color;
    bool clear_canvas_first;
    bool push_full_update; // GC16 vs DU4
};

enum FetchResultStatus
{
    FETCH_SUCCESS,
    FETCH_GENUINE_ERROR,
    FETCH_INTERRUPTED_BY_USER, // Hard interrupt
    FETCH_NO_WIFI,
    FETCH_RESTART_REQUESTED    // Graceful restart
};

// Function Prototypes
bool selectNextScript(bool moveUp);
bool loadScriptToExecute();
void displayMessage(const String &msg, int y_offset = 50, uint16_t color = 15);
void displayParseErrors();
void handleWakeupAndScriptExecution();
void fetchTaskFunction(void *pvParameters);
FetchResultStatus perform_fetch_operations(); // Renamed and modified fetchAndStoreScripts

// Interrupt handling for wakeup pin is defined in global_setting.cpp

void setup()
{
    // Initialize M5Paper hardware, RTC, SPIFFS, etc.
    SysInit_Start();

    log_i("MicroPatterns M5Paper - Wakeup Cycle %d", executionCounter);

    canvas.createCanvas(540, 960);
    if (!canvas.frameBuffer())
    {
        log_e("Failed to create canvas framebuffer!");
        displayMessage("Canvas Error!");
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        while (1)
            delay(1000); // Halt
    }
    log_i("Canvas created: %d x %d", canvas.width(), canvas.height());

    // Create FreeRTOS objects
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
}

void loop()
{
    // Process any messages from the fetch task for display
    DisplayMsg msg;
    if (g_displayMessageQueue != NULL && xQueueReceive(g_displayMessageQueue, &msg, 0) == pdTRUE)
    { // Non-blocking check
        if (msg.clear_canvas_first)
            canvas.fillCanvas(0); // Usually white
        displayMessage(msg.text, msg.y_offset, msg.color);
        canvas.pushCanvas(0, 0, msg.push_full_update ? UPDATE_MODE_GC16 : UPDATE_MODE_DU4);
        // Brief delay for important messages, helps user see them
        if (msg.push_full_update && (strcmp(msg.text, "Fetch OK!") == 0 || strstr(msg.text, "Failed") != NULL || strstr(msg.text, "Interrupted") != NULL))
        {
            delay(1000);
        }
    }

    // Only process wakeup if it hasn't been handled yet this wake cycle
    if (!g_wakeup_handled) {
        handleWakeupAndScriptExecution();
        g_wakeup_handled = true; // Mark wakeup as handled
    }

    if (g_fetch_task_in_progress) {
        log_i("Loop: Fetch task is in progress, delaying sleep.");
        // Yield for a short period to allow fetch task to run and prevent busy-waiting in main loop.
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        goToLightSleep();
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

void handleWakeupAndScriptExecution()
{
    bool scriptChangeRequest = false;
    bool moveUpDirection = false;
    // Initialize fetchFromServerAfterExecution to false.
    // It will only be set true if a button requests it AND no fetch is currently active/being restarted.
    bool fetchFromServerAfterExecution = false;

    // Capture initial fetch task state to make decisions based on state at entry
    bool initial_fetch_task_state_is_progress = g_fetch_task_in_progress;
    bool signaled_restart_to_existing_fetch_this_cycle = false;

    // 1. Determine Wakeup Reason & Intent
    uint8_t current_wakeup_pin_val = wakeup_pin; // Capture volatile read
    if (current_wakeup_pin_val != 0)
    {
        wakeup_pin = 0; // Consume the event for this cycle of handleWakeupAndScriptExecution
        log_i("Wakeup caused by GPIO %d", current_wakeup_pin_val);

        if (initial_fetch_task_state_is_progress) // Check against the state at the beginning of this function call
        {
            log_i("User action during fetch. Signaling fetch task to restart.");
            g_fetch_restart_pending = true; // Signal a graceful restart
            signaled_restart_to_existing_fetch_this_cycle = true; // Mark that this cycle signaled a restart
            // g_user_interrupt_signal_for_fetch_task = true; // This would be for a hard stop
            // No delay here, fetch task will check the flag. UI continues immediately.
        }

        if (current_wakeup_pin_val == BUTTON_UP_PIN)
        {
            log_i("Button UP (GPIO %d) detected.", current_wakeup_pin_val);
            scriptChangeRequest = true;
            moveUpDirection = true;
            // Only set fetchFromServerAfterExecution if no fetch was initially in progress.
            if (!initial_fetch_task_state_is_progress) {
                fetchFromServerAfterExecution = true;
            }
        }
        else if (current_wakeup_pin_val == BUTTON_DOWN_PIN)
        {
            log_i("Button DOWN (GPIO %d) detected.", current_wakeup_pin_val);
            scriptChangeRequest = true;
            moveUpDirection = false;
            // Only set fetchFromServerAfterExecution if no fetch was initially in progress.
            if (!initial_fetch_task_state_is_progress) {
                fetchFromServerAfterExecution = true;
            }
        }
        else if (current_wakeup_pin_val == BUTTON_PUSH_PIN)
        {
            log_i("Button PUSH (GPIO %d) detected.", current_wakeup_pin_val);
            // For PUSH, just re-run current script. No fetch by default.
        }
    }
    else
    {
        log_i("Wakeup caused by timer or unknown source.");
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
        log_w("Failed to load script from SPIFFS. Using default script.");
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
        M5.RTC.getTime(&time_struct);
        executionCounter++;

        log_i("Executing script '%s' - Cycle: %d, Time: %02d:%02d:%02d",
              currentScriptId.c_str(), executionCounter, time_struct.hour, time_struct.min, time_struct.sec);

        runtime->setTime(time_struct.hour, time_struct.min, time_struct.sec);
        runtime->setCounter(executionCounter);
        runtime->execute(); // This includes drawing and pushing canvas
        log_i("Finished execution for cycle #%d", executionCounter);
    }
    else
    {
        log_e("Runtime not initialized, skipping execution. Check for parse errors.");
    }

    // 6. Trigger Fetch from Server if Requested
    if (fetchFromServerAfterExecution) // True if UP/DOWN pressed AND no fetch was active at handler start
    {
        // At this point, we know:
        // 1. An UP or DOWN button was pressed.
        // 2. No fetch was in progress when this handler started (initial_fetch_task_state_is_progress was false).
        // Therefore, signaled_restart_to_existing_fetch_this_cycle is also false.
        // We are clear to request a new fetch.
        log_i("Requesting new fetch (button press, no initial fetch active).");

        // It's still theoretically possible g_fetch_task_in_progress became true due to some other mechanism
        // between the start of this handler and now, though unlikely with the current single-threaded handler design for wakeups.
        // A simple check for logging:
        if (g_fetch_task_in_progress) {
             log_w("Warning: Queuing new fetch, but g_fetch_task_in_progress is now true. This might lead to overlapping requests if not handled by fetch task.");
        }

        if (g_fetchRequestSemaphore != NULL)
        {
            xSemaphoreGive(g_fetchRequestSemaphore);
            log_i("Fetch request semaphore given.");
        }
        else
        {
            log_e("Fetch request semaphore is NULL, cannot trigger fetch.");
        }
        // UI task no longer displays "Fetching..." directly. Fetch task sends message.
    }
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

    for (;;)
    {
        if (g_fetchRequestSemaphore != NULL && xSemaphoreTake(g_fetchRequestSemaphore, portMAX_DELAY) == pdTRUE)
        {
            g_fetch_task_in_progress = true; // Set flag immediately
            log_i("FetchTask: Semaphore taken, g_fetch_task_in_progress=true. Starting fetch operation.");
            bool restart_fetch_immediately;
            bool first_attempt_in_cycle = true;

            do
            {
                restart_fetch_immediately = false;
                // g_fetch_task_in_progress = true; // Moved up
                // g_user_interrupt_signal_for_fetch_task is for hard stop, cleared by perform_fetch_operations or if it triggers
                // g_fetch_restart_pending is for soft restart, cleared when acted upon below or by perform_fetch_operations

                // Send "Fetching scripts..." or "Restarting fetch..." message to UI task
                if (g_displayMessageQueue != NULL)
                {
                    if (first_attempt_in_cycle)
                    {
                        strncpy(msg_to_send.text, "Fetching scripts...", sizeof(msg_to_send.text) - 1);
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

                FetchResultStatus fetch_status = perform_fetch_operations(); // This function checks g_fetch_restart_pending

                // Prepare result message based on status
                bool send_final_message = true;
                msg_to_send.y_offset = 100; // Standard y_offset for result messages
                msg_to_send.color = 15;
                msg_to_send.clear_canvas_first = false;
                msg_to_send.push_full_update = true; // GC16 for final status

                switch (fetch_status)
                {
                case FETCH_SUCCESS:
                    strncpy(msg_to_send.text, "Fetch OK!", sizeof(msg_to_send.text) - 1);
                    log_i("FetchTask: Completed successfully.");
                    disconnectWiFi(); // Disconnect on final success
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
                    strncpy(msg_to_send.text, "Fetch: No WiFi", sizeof(msg_to_send.text) - 1);
                    log_w("FetchTask: No WiFi connection.");
                    // disconnectWiFi() should have been called by perform_fetch_operations or connectToWiFi
                    break;
                case FETCH_RESTART_REQUESTED:
                    log_i("FetchTask: Restart requested. Looping.");
                    // UI message for restarting is handled by the next iteration's "Restarting fetch..."
                    send_final_message = false; // Don't send a "final" message yet
                    g_fetch_restart_pending = false; // Consume the flag
                    restart_fetch_immediately = true;
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

            g_fetch_task_in_progress = false; // Clear flag after all attempts for this semaphore signal are done
            // g_user_interrupt_signal_for_fetch_task should be false here unless a hard interrupt occurred.
            // g_fetch_restart_pending is false.
            log_i("FetchTask: Operation cycle finished, g_fetch_task_in_progress=false. Waiting for semaphore.");
        }
        else
        {
            // Semaphore not available or timeout (if not portMAX_DELAY)
            vTaskDelay(pdMS_TO_TICKS(10)); // Small delay if semaphore take fails for some reason
        }
    }
}

FetchResultStatus perform_fetch_operations()
{
    // Clear hard interrupt signal for this specific attempt. Restart signal is handled differently.
    g_user_interrupt_signal_for_fetch_task = false;

    bool wifiConnected = connectToWiFi(); // connectToWiFi checks g_user_interrupt_signal_for_fetch_task

    if (g_user_interrupt_signal_for_fetch_task) { // Check if connectToWiFi was hard-interrupted
        log_i("perform_fetch_operations: connectToWiFi hard-interrupted.");
        // connectToWiFi should have handled disconnect
        return FETCH_INTERRUPTED_BY_USER;
    }
    if (g_fetch_restart_pending) { // Check for restart request immediately after connection attempt
        log_i("perform_fetch_operations: Restart requested after connectToWiFi attempt.");
        // WiFi might be on or off depending on connectToWiFi result. If on, keep it on.
        return FETCH_RESTART_REQUESTED;
    }

    if (!wifiConnected) {
        log_e("perform_fetch_operations: WiFi connection failed.");
        return FETCH_NO_WIFI;
    }

    // If we reach here, WiFi is connected.
    log_i("perform_fetch_operations: WiFi connected. Proceeding with fetch.");

    WiFiClientSecure httpsClient;
    httpsClient.setCACert(rootCACertificate);
    HTTPClient http;
    bool overall_fetch_successful = true;

    // 0. Load existing SPIFFS list to help preserve fileIds
    DynamicJsonDocument oldSpiifsListDoc(8192); // Increased size to match finalSpiifsListDoc
    bool oldListLoaded = loadScriptList(oldSpiifsListDoc);
    if (oldListLoaded) {
        log_i("perform_fetch_operations: Successfully loaded existing list.json to help preserve fileIds (%d entries).", oldSpiifsListDoc.as<JsonArray>().size());
    } else {
        log_w("perform_fetch_operations: Could not load existing list.json or it was empty/invalid. New fileIds will be generated for all scripts.");
        // oldSpiifsListDoc will be empty or invalid, checks later will handle this.
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Yield after SPIFFS load

    // 1. Fetch Script List from Server
    String listUrl = String(API_BASE_URL) + "/api/scripts";
    log_i("perform_fetch_operations: Fetching script list from: %s", listUrl.c_str());

    if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
    if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

    if (!http.begin(httpsClient, listUrl)) {
        log_e("perform_fetch_operations: HTTPClient begin failed for list URL!");
        return FETCH_GENUINE_ERROR;
    }

    int httpCode = http.GET();
    vTaskDelay(pdMS_TO_TICKS(10));

    if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
    if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

    DynamicJsonDocument serverListDoc(4096); // To store the list fetched from server
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
        log_e("perform_fetch_operations: HTTP error fetching script list: %d", httpCode);
        overall_fetch_successful = false;
    }
    http.end(); // End connection for list fetch

    if (!overall_fetch_successful) {
        // No need to disconnectWiFi here, caller task will handle it for GENUINE_ERROR
        return FETCH_GENUINE_ERROR;
    }

    // 2. Reconcile with Local State, Fetch Content, and Build Final SPIFFS List
    DynamicJsonDocument finalSpiifsListDoc(8192); // Increased size for safety
    JsonArray finalSpiifsArray = finalSpiifsListDoc.to<JsonArray>();
    std::set<String> assignedFileIdsThisFetch; // Track fileIds used in this fetch to ensure uniqueness

    if (serverListDoc.is<JsonArray>()) {
        JsonArray serverArray = serverListDoc.as<JsonArray>();
        log_i("perform_fetch_operations: Processing %d scripts from server list.", serverArray.size());

        for (JsonObject serverScriptInfo : serverArray) {
            if (g_user_interrupt_signal_for_fetch_task) { disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
            if (g_fetch_restart_pending) { return FETCH_RESTART_REQUESTED; }

            const char *humanId = serverScriptInfo["id"];
            const char *humanName = serverScriptInfo["name"];

            if (!humanId || strlen(humanId) == 0) {
                log_w("perform_fetch_operations: Skipping script from server with missing 'id'.");
                continue;
            }

            String determinedFileId = "";
            bool foundInOldList = false;

            // Try to find this humanId in the old SPIFFS list to reuse its fileId
            if (oldListLoaded && oldSpiifsListDoc.is<JsonArray>()) {
                for (JsonObject oldScriptInfo : oldSpiifsListDoc.as<JsonArray>()) {
                    const char* oldHumanId = oldScriptInfo["id"];
                    const char* oldFileId = oldScriptInfo["fileId"];
                    if (oldHumanId && oldFileId && strcmp(oldHumanId, humanId) == 0 && strlen(oldFileId) > 0) {
                        // Check if this oldFileId is already taken by another humanId in this new fetch (e.g. if old list had duplicates)
                        if (assignedFileIdsThisFetch.count(oldFileId)) {
                            log_w("perform_fetch_operations: fileId '%s' for humanId '%s' from old list is already assigned in this fetch. Will generate new ID.", oldFileId, humanId);
                            // Fall through to generate new ID
                        } else {
                            determinedFileId = oldFileId;
                            foundInOldList = true;
                            break;
                        }
                    }
                }
            }

            if (foundInOldList) {
                log_i("perform_fetch_operations: Reusing fileId '%s' for humanId '%s'.", determinedFileId.c_str(), humanId);
                assignedFileIdsThisFetch.insert(determinedFileId);
            } else {
                // Generate a new fileId, ensuring it's unique for this fetch cycle
                int tempIdCounter = 0;
                do {
                    determinedFileId = "s" + String(tempIdCounter++);
                } while (assignedFileIdsThisFetch.count(determinedFileId));
                log_i("perform_fetch_operations: Assigning new fileId '%s' for humanId '%s'.", determinedFileId.c_str(), humanId);
                assignedFileIdsThisFetch.insert(determinedFileId);
            }

            String contentPath = "/scripts/content/" + determinedFileId;
            bool contentAvailableOrFetched = false;

            if (SPIFFS.exists(contentPath.c_str())) {
                log_d("perform_fetch_operations: Content for humanId '%s' (fileId '%s') already exists locally at %s.", humanId, determinedFileId.c_str(), contentPath.c_str());
                contentAvailableOrFetched = true;
            } else {
                log_i("perform_fetch_operations: Content for humanId '%s' (fileId '%s') missing locally at %s, fetching from server.", humanId, determinedFileId.c_str(), contentPath.c_str());
                String scriptUrl = String(API_BASE_URL) + "/api/scripts/" + humanId;

                if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }
                if (!http.begin(httpsClient, scriptUrl)) {
                    log_e("perform_fetch_operations: HTTPClient begin failed for script content URL: %s", scriptUrl.c_str());
                    assignedFileIdsThisFetch.erase(determinedFileId); // Release ID if fetch setup fails
                    continue; // Skip this script
                }
                
                int scriptHttpCode = http.GET();
                vTaskDelay(pdMS_TO_TICKS(10));

                if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

                if (scriptHttpCode == HTTP_CODE_OK) {
                    String scriptPayload = http.getString();
                    if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                    if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

                    DynamicJsonDocument scriptDoc(8192); // Ensure large enough for script content
                    DeserializationError scriptError = deserializeJson(scriptDoc, scriptPayload);
                    if (scriptError) {
                        log_e("perform_fetch_operations: Failed to parse script content JSON for humanId %s: %s", humanId, scriptError.c_str());
                    } else {
                        const char *scriptContentFetched = scriptDoc["content"];
                        if (scriptContentFetched) {
                            if (saveScriptContent(determinedFileId.c_str(), scriptContentFetched)) {
                                contentAvailableOrFetched = true;
                            } else {
                                log_e("perform_fetch_operations: Failed to save script content for fileId %s (humanId %s).", determinedFileId.c_str(), humanId);
                            }
                        } else {
                            log_e("perform_fetch_operations: Missing 'content' field for humanId %s.", humanId);
                        }
                    }
                } else {
                    log_w("perform_fetch_operations: HTTP error %d fetching script content for humanId %s. Script will be excluded.", scriptHttpCode, humanId);
                }
                http.end(); // End connection for this script content fetch
            }

            if (contentAvailableOrFetched) {
                JsonObject newEntry = finalSpiifsArray.createNestedObject();
                newEntry["id"] = humanId;
                newEntry["name"] = humanName ? humanName : humanId; // Fallback for name
                newEntry["fileId"] = determinedFileId;
                // No validScriptIndex++ here, assignedFileIdsThisFetch tracks used IDs.
            } else {
                log_w("perform_fetch_operations: Script humanId '%s' (intended fileId '%s') excluded from final list as content is unavailable or save failed.", humanId, determinedFileId.c_str());
                assignedFileIdsThisFetch.erase(determinedFileId); // Release the fileId if script is not included
            }
            vTaskDelay(pdMS_TO_TICKS(50)); // Small delay between script processing
        }
    }

    // 3. Cleanup Orphaned Script Content Files
    log_i("perform_fetch_operations: Cleaning up orphaned script content files...");
    File root = SPIFFS.open("/scripts/content");
    if (root && root.isDirectory()) {
        File entry = root.openNextFile();
        while(entry) {
            if (g_user_interrupt_signal_for_fetch_task) { root.close(); entry.close(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
            if (g_fetch_restart_pending) { root.close(); entry.close(); return FETCH_RESTART_REQUESTED; }

            if (!entry.isDirectory()) {
                String entryName = entry.name(); // This might be relative or absolute depending on SPIFFS lib version
                String pathToRemove;
                String fileIdFromPath;

                if (entryName.startsWith("/")) { // Already an absolute path
                    pathToRemove = entryName;
                } else { // Relative path, construct absolute
                    pathToRemove = "/scripts/content/" + entryName;
                }
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
            entry.close(); // Close current entry
            entry = root.openNextFile(); // Open next
        }
        root.close(); // Close root directory
    } else {
        log_w("perform_fetch_operations: Could not open /scripts/content directory for cleanup or it's not a directory.");
    }
    vTaskDelay(pdMS_TO_TICKS(10));


    // 4. Save the New list.json
    if (!saveScriptList(finalSpiifsListDoc)) { // Uses the new saveScriptList(DynamicJsonDocument&)
        log_e("perform_fetch_operations: Failed to save final script list to SPIFFS.");
        overall_fetch_successful = false;
    }

    // Final checks before returning success/error
    if (g_user_interrupt_signal_for_fetch_task) { disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
    if (g_fetch_restart_pending) { return FETCH_RESTART_REQUESTED; }

    if (overall_fetch_successful) {
        return FETCH_SUCCESS;
    }
    return FETCH_GENUINE_ERROR;
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
        log_e("Cannot select next script, list is empty or not an array after loading.");
        // Attempt to trigger a fetch if list is empty/invalid, as it might be recoverable
        log_w("selectNextScript: Triggering fetch due to empty/invalid script list.");
        if (g_fetchRequestSemaphore != NULL) {
            xSemaphoreGive(g_fetchRequestSemaphore);
        }
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
                vTaskDelay(pdMS_TO_TICKS(10)); // Yield after push
                delay(1500);
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
    if (loadCurrentScriptId(humanReadableScriptIdFromSpiffs) && humanReadableScriptIdFromSpiffs.length() > 0) {
        log_i("loadScriptToExecute: Current human-readable script ID from SPIFFS: '%s'", humanReadableScriptIdFromSpiffs.c_str());
        spiffsIdLoaded = true;
    } else {
        log_w("loadScriptToExecute: No current script ID found in SPIFFS or it was empty. Will try first from list.");
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // 2. Load the script list
    DynamicJsonDocument listDoc(4096);
    bool listIsProblematic = false; // Flag if list.json seems to have issues
    JsonArray scriptList;

    if (loadScriptList(listDoc) && listDoc.is<JsonArray>() && listDoc.as<JsonArray>().size() > 0) {
        scriptList = listDoc.as<JsonArray>();
        log_i("loadScriptToExecute: Script list loaded with %d entries.", scriptList.size());
        JsonObject firstEntryTest = scriptList[0];
        const char* testId = firstEntryTest["id"];
        const char* testFileId = firstEntryTest["fileId"];
        if (!testId || strlen(testId) == 0 || !testFileId || strlen(testFileId) == 0) {
            log_w("loadScriptToExecute: First script in list.json is missing 'id' or 'fileId'. List may be problematic.");
            listIsProblematic = true;
        }
    } else {
        log_e("loadScriptToExecute: Failed to load script list, or list is empty/invalid. Marking as problematic.");
        listIsProblematic = true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // 3. Try to use humanReadableScriptIdFromSpiffs if it was loaded and list is not problematic
    bool foundAndMappedSpiffsId = false;
    if (spiffsIdLoaded && !listIsProblematic) {
        bool idExistsInList = false;
        for (JsonObject scriptInfo : scriptList) {
            const char *hId = scriptInfo["id"];
            if (hId && humanReadableScriptIdFromSpiffs == hId) {
                idExistsInList = true;
                const char *fId = scriptInfo["fileId"];
                if (fId && strlen(fId) > 0) {
                    determinedFileId = fId;
                    finalHumanIdForExecution = humanReadableScriptIdFromSpiffs;
                    foundAndMappedSpiffsId = true;
                    log_i("loadScriptToExecute: Found fileId '%s' for humanId '%s' (from SPIFFS preference).", determinedFileId.c_str(), finalHumanIdForExecution.c_str());
                } else {
                    log_w("loadScriptToExecute: HumanId '%s' (from SPIFFS preference) found in list but has no valid fileId.", humanReadableScriptIdFromSpiffs.c_str());
                    listIsProblematic = true; // Mark list as problematic
                }
                break;
            }
        }
        if (!idExistsInList) {
            log_w("loadScriptToExecute: HumanId '%s' (from SPIFFS preference) not found in the script list. Will try first from list.", humanReadableScriptIdFromSpiffs.c_str());
        }
    }

    // 4. If spiffsId wasn't used or list is problematic, try first script from list (if list is usable)
    if (!foundAndMappedSpiffsId && !listIsProblematic && scriptList.size() > 0) {
        log_i("loadScriptToExecute: Attempting to use first script from the list.");
        JsonObject firstScript = scriptList[0]; // Already checked firstEntryTest for validity
        const char *firstHumanId = firstScript["id"];
        const char *firstFileId = firstScript["fileId"];

        // This check is redundant if listIsProblematic is false, but good for safety
        if (firstHumanId && strlen(firstHumanId) > 0 && firstFileId && strlen(firstFileId) > 0) {
            finalHumanIdForExecution = firstHumanId;
            determinedFileId = firstFileId;
            log_i("loadScriptToExecute: Using first script: humanId '%s', fileId '%s'", finalHumanIdForExecution.c_str(), determinedFileId.c_str());
            if (!saveCurrentScriptId(finalHumanIdForExecution.c_str())) {
                log_w("loadScriptToExecute: Failed to save the first script's humanId ('%s') as current to SPIFFS.", finalHumanIdForExecution.c_str());
            }
        } else {
            // This case implies listIsProblematic should have been true.
            log_e("loadScriptToExecute: First script in list has no valid humanId or fileId, despite earlier checks. List is problematic.");
            listIsProblematic = true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 5. If we still don't have a fileId (due to list problems or empty list), fallback to default.
    if (determinedFileId.length() == 0) {
        log_e("loadScriptToExecute: Could not determine a valid script/fileId from list.json or list was unusable.");
        currentScriptId = spiffsIdLoaded ? humanReadableScriptIdFromSpiffs : "default";
        currentScriptContent = "";
        
        if (listIsProblematic) { // This covers empty, corrupt, or unreadable list.json
            if (spiffsIdLoaded) {
                log_w("loadScriptToExecute: List.json seems problematic. Clearing current_script.id ('%s') to avoid getting stuck.", humanReadableScriptIdFromSpiffs.c_str());
                saveCurrentScriptId("");
            } else {
                log_w("loadScriptToExecute: List.json seems problematic. No specific current_script.id was loaded to clear.");
            }
            log_w("loadScriptToExecute: Triggering a fetch operation to rebuild problematic list.json.");
            if (g_fetchRequestSemaphore != NULL) {
                xSemaphoreGive(g_fetchRequestSemaphore);
                log_i("loadScriptToExecute: Fetch request semaphore given due to problematic list.");
            } else {
                log_e("loadScriptToExecute: Fetch request semaphore is NULL, cannot trigger fetch for problematic list.");
            }
        }
        return false;
    }

    // 6. Load script content using the determined fileId
    currentScriptId = finalHumanIdForExecution;
    log_i("loadScriptToExecute: Attempting to load content for humanId '%s' using fileId '%s'", currentScriptId.c_str(), determinedFileId.c_str());
    if (!loadScriptContent(determinedFileId.c_str(), currentScriptContent)) {
        log_e("loadScriptToExecute: Failed to load script content for fileId '%s' (humanId '%s'). This file should exist according to list.json.", determinedFileId.c_str(), currentScriptId.c_str());
        currentScriptContent = "";
        // Trigger fetch to repair missing content
        log_w("loadScriptToExecute: Triggering a fetch operation to attempt to download missing content for '%s'.", currentScriptId.c_str());
        if (g_fetchRequestSemaphore != NULL) {
            xSemaphoreGive(g_fetchRequestSemaphore);
            log_i("loadScriptToExecute: Fetch request semaphore given for missing content file.");
        } else {
            log_e("loadScriptToExecute: Fetch request semaphore is NULL, cannot trigger fetch for missing content.");
        }
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    log_i("loadScriptToExecute: Successfully set up script humanId '%s' (fileId '%s') for execution.", currentScriptId.c_str(), determinedFileId.c_str());
    return true;
}
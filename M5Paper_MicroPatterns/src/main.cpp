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

    handleWakeupAndScriptExecution();
    goToLightSleep();
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
    bool fetchFromServerAfterExecution = false;

    // 1. Determine Wakeup Reason & Intent
    uint8_t current_wakeup_pin_val = wakeup_pin; // Capture volatile read
    if (current_wakeup_pin_val != 0)
    {
        wakeup_pin = 0; // Consume the event for this cycle of handleWakeupAndScriptExecution
        log_i("Wakeup caused by GPIO %d", current_wakeup_pin_val);

        if (g_fetch_task_in_progress)
        {
            log_i("User action during fetch. Signaling fetch task to restart.");
            g_fetch_restart_pending = true; // Signal a graceful restart
            // g_user_interrupt_signal_for_fetch_task = true; // This would be for a hard stop
            // No delay here, fetch task will check the flag. UI continues immediately.
        }

        if (current_wakeup_pin_val == BUTTON_UP_PIN)
        {
            log_i("Button UP (GPIO %d) detected.", current_wakeup_pin_val);
            scriptChangeRequest = true;
            moveUpDirection = true;
            fetchFromServerAfterExecution = true;
        }
        else if (current_wakeup_pin_val == BUTTON_DOWN_PIN)
        {
            log_i("Button DOWN (GPIO %d) detected.", current_wakeup_pin_val);
            scriptChangeRequest = true;
            moveUpDirection = false;
            fetchFromServerAfterExecution = true;
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
    if (fetchFromServerAfterExecution)
    {
        if (g_fetch_task_in_progress)
        {
            log_i("Requesting new fetch while one is in progress. Current fetch will be signaled to interrupt if not already.");
            // The g_user_interrupt_signal_for_fetch_task might have already been set above.
            // The fetch task will clear this signal at the beginning of its new operation.
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
            log_i("FetchTask: Semaphore taken, starting fetch operation.");
            bool restart_fetch_immediately;
            bool first_attempt_in_cycle = true;

            do
            {
                restart_fetch_immediately = false;
                g_fetch_task_in_progress = true;
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

                g_fetch_task_in_progress = false;

            } while (restart_fetch_immediately);

            // g_user_interrupt_signal_for_fetch_task should be false here unless a hard interrupt occurred.
            // g_fetch_restart_pending is false.
            log_i("FetchTask: Operation cycle finished, going back to wait for semaphore.");
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

    // 1. Fetch Script List
    String listUrl = String(API_BASE_URL) + "/api/scripts";
    log_i("perform_fetch_operations: Fetching script list from: %s", listUrl.c_str());

    // Check for hard interrupt or soft restart before beginning HTTP transaction
    if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
    if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

    if (!http.begin(httpsClient, listUrl))
    {
        log_e("perform_fetch_operations: HTTPClient begin failed for list URL!");
        // disconnectWiFi(); // WiFi will be disconnected by caller task if this is a final error
        return FETCH_GENUINE_ERROR;
    }

    int httpCode = http.GET();
    vTaskDelay(pdMS_TO_TICKS(10)); // Small yield after blocking call

    // Check for hard interrupt or soft restart after HTTP GET
    if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
    if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

    if (httpCode == HTTP_CODE_OK)
    {
        String payload = http.getString(); // This is the raw JSON string of the script list from API
        // Check for hard interrupt or soft restart after getting string
        if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
        if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

        log_d("perform_fetch_operations: Script list payload from API: %s", payload.c_str());

        // saveScriptList now parses payload, generates fileIds, and saves the augmented list to SPIFFS
        if (!saveScriptList(payload.c_str()))
        {
            log_e("perform_fetch_operations: Failed to save augmented script list to SPIFFS.");
            overall_fetch_successful = false; // Mark as partial failure
        }

        // Check for hard interrupt or soft restart after saving list
        if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
        if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

        if (overall_fetch_successful)
        {
            // Now load the augmented list from SPIFFS to get fileIds for fetching content
            DynamicJsonDocument augmentedListDoc(4096); // Doc to hold the list from SPIFFS
            if (!loadScriptList(augmentedListDoc)) { // loadScriptList loads from /scripts/list.json
                log_e("perform_fetch_operations: Failed to load augmented script list from SPIFFS after saving.");
                overall_fetch_successful = false;
            }
            // Check for hard interrupt or soft restart after loading augmented list
            if (g_user_interrupt_signal_for_fetch_task) { http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
            if (g_fetch_restart_pending) { http.end(); return FETCH_RESTART_REQUESTED; }

            if (overall_fetch_successful && augmentedListDoc.is<JsonArray>())
            {
                JsonArray scriptListFromSpiffs = augmentedListDoc.as<JsonArray>();
                log_i("perform_fetch_operations: Loaded %d scripts from augmented list. Fetching content...", scriptListFromSpiffs.size());

                for (JsonObject scriptInfo : scriptListFromSpiffs)
                {
                    // Check for hard interrupt or soft restart at the start of each script fetch iteration
                    if (g_user_interrupt_signal_for_fetch_task) { overall_fetch_successful = false; http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                    if (g_fetch_restart_pending) { overall_fetch_successful = false; http.end(); return FETCH_RESTART_REQUESTED; }

                    const char *humanId = scriptInfo["id"];
                    const char *fileId = scriptInfo["fileId"]; // Get the generated fileId

                    if (!humanId || !fileId)
                    {
                        log_w("perform_fetch_operations: Skipping script with missing 'id' or 'fileId' in augmented list.");
                        continue;
                    }

                    String scriptUrl = String(API_BASE_URL) + "/api/scripts/" + humanId; // Fetch API using humanId
                    log_d("perform_fetch_operations: Fetching script content for humanId '%s' (fileId '%s') from: %s", humanId, fileId, scriptUrl.c_str());

                    http.end(); // End previous connection before starting new one
                    // Check for hard interrupt or soft restart before new http.begin
                    if (g_user_interrupt_signal_for_fetch_task) { overall_fetch_successful = false; http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                    if (g_fetch_restart_pending) { overall_fetch_successful = false; http.end(); return FETCH_RESTART_REQUESTED; }

                    if (!http.begin(httpsClient, scriptUrl))
                    {
                        log_e("perform_fetch_operations: HTTPClient begin failed for script URL: %s", scriptUrl.c_str());
                        overall_fetch_successful = false;
                        continue;
                    }

                    int scriptHttpCode = http.GET();
                    vTaskDelay(pdMS_TO_TICKS(10));

                    // Check for hard interrupt or soft restart after script GET
                    if (g_user_interrupt_signal_for_fetch_task) { overall_fetch_successful = false; http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                    if (g_fetch_restart_pending) { overall_fetch_successful = false; http.end(); return FETCH_RESTART_REQUESTED; }

                    if (scriptHttpCode == HTTP_CODE_OK)
                    {
                        String scriptPayload = http.getString();
                        // Check for hard interrupt or soft restart after script getString
                        if (g_user_interrupt_signal_for_fetch_task) { overall_fetch_successful = false; http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                        if (g_fetch_restart_pending) { overall_fetch_successful = false; http.end(); return FETCH_RESTART_REQUESTED; }

                        DynamicJsonDocument scriptDoc(8192); // Increased size for script content
                        DeserializationError scriptError = deserializeJson(scriptDoc, scriptPayload);
                        // Check for hard interrupt or soft restart after script deserialize
                        if (g_user_interrupt_signal_for_fetch_task) { overall_fetch_successful = false; http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                        if (g_fetch_restart_pending) { overall_fetch_successful = false; http.end(); return FETCH_RESTART_REQUESTED; }

                        if (scriptError)
                        {
                            log_e("perform_fetch_operations: Failed to parse script content JSON for humanId %s: %s", humanId, scriptError.c_str());
                            overall_fetch_successful = false;
                        }
                        else
                        {
                            const char *scriptContentFetched = scriptDoc["content"];
                            if (scriptContentFetched)
                            {
                                // Save content using fileId
                                if (!saveScriptContent(fileId, scriptContentFetched))
                                {
                                    log_e("perform_fetch_operations: Failed to save script content for fileId %s (humanId %s).", fileId, humanId);
                                    overall_fetch_successful = false;
                                }
                                vTaskDelay(pdMS_TO_TICKS(5));
                                // Check for hard interrupt or soft restart after saving script content
                                if (g_user_interrupt_signal_for_fetch_task) { overall_fetch_successful = false; http.end(); disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
                                if (g_fetch_restart_pending) { overall_fetch_successful = false; http.end(); return FETCH_RESTART_REQUESTED; }
                            }
                            else
                            {
                                log_e("perform_fetch_operations: Missing 'content' field for humanId %s.", humanId);
                                overall_fetch_successful = false;
                            }
                        }
                    }
                    else
                    {
                        log_e("perform_fetch_operations: HTTP error fetching script humanId %s: %d", humanId, scriptHttpCode);
                        overall_fetch_successful = false;
                    }
                    http.end(); // End connection for this script
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            } else if (overall_fetch_successful) { // If overall_fetch_successful is true but augmentedListDoc is not array
                 log_e("perform_fetch_operations: Augmented script list from SPIFFS is not an array or failed to load.");
                 overall_fetch_successful = false;
            }
        }
    }
    else
    {
        log_e("perform_fetch_operations: HTTP error fetching script list: %d", httpCode);
        overall_fetch_successful = false;
    }

    http.end(); // Final cleanup of HTTP client

    // Final checks before returning success/error
    // If a hard interrupt occurred at any point and wasn't caught by an earlier return:
    if (g_user_interrupt_signal_for_fetch_task) { disconnectWiFi(); return FETCH_INTERRUPTED_BY_USER; }
    // If a restart was requested and wasn't caught:
    if (g_fetch_restart_pending) { return FETCH_RESTART_REQUESTED; }


    // If we reach here, the operation completed (or failed genuinely) without interrupts/restarts
    // WiFi is disconnected by the caller task (fetchTaskFunction) for final states like SUCCESS or GENUINE_ERROR.
    // However, if an error occurred and WiFi is still on, it's good to turn it off here.
    // But for FETCH_SUCCESS, we want the caller to handle disconnect.
    // Let's ensure WiFi is disconnected only on GENUINE_ERROR here if it wasn't a restart/interrupt.
    if (!overall_fetch_successful && !g_fetch_restart_pending && !g_user_interrupt_signal_for_fetch_task) {
         // disconnectWiFi(); // Caller will handle disconnect for GENUINE_ERROR
    }


    if (overall_fetch_successful)
    {
        return FETCH_SUCCESS;
    }
    // If not success, and not interrupted or restart_pending, it's a genuine error.
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
    bool listIsPotentiallyCorrupt = false; // Flag if list.json seems to have issues with fileIds
    JsonArray scriptList; // Will be assigned if list loads successfully

    if (loadScriptList(listDoc) && listDoc.is<JsonArray>() && listDoc.as<JsonArray>().size() > 0) {
        scriptList = listDoc.as<JsonArray>();
        log_i("loadScriptToExecute: Script list loaded with %d entries.", scriptList.size());
        // Initial check: does the first item have a fileId? If not, list is likely bad.
        JsonObject firstEntryTest = scriptList[0];
        const char* testId = firstEntryTest["id"];
        const char* testFileId = firstEntryTest["fileId"];
        if (!testId || strlen(testId) == 0 || !testFileId || strlen(testFileId) == 0) {
            log_w("loadScriptToExecute: First script in list.json is missing 'id' or 'fileId'. List may be corrupted.");
            listIsPotentiallyCorrupt = true;
        }
    } else {
        log_e("loadScriptToExecute: Failed to load script list, or list is empty/invalid. Marking as potentially corrupt.");
        listIsPotentiallyCorrupt = true; // Mark list as corrupt if loading failed or list is unusable
        // scriptList remains unassigned or empty. Logic will proceed to fallback.
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // 3. Try to use humanReadableScriptIdFromSpiffs if it was loaded
    bool foundAndMappedSpiffsId = false;
    if (spiffsIdLoaded) {
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
                    listIsPotentiallyCorrupt = true; // Mark list as problematic
                }
                break; // Found the entry for humanReadableScriptIdFromSpiffs
            }
        }
        if (!idExistsInList) {
            log_w("loadScriptToExecute: HumanId '%s' (from SPIFFS preference) not found in the script list. Will try first from list.", humanReadableScriptIdFromSpiffs.c_str());
        }
        // If ID existed but fileId was invalid, foundAndMappedSpiffsId remains false.
    }

    // 4. If spiffsId wasn't used (not loaded, not found, or no fileId for it), try first script from list
    if (!foundAndMappedSpiffsId && scriptList.size() > 0) {
        log_i("loadScriptToExecute: Attempting to use first script from the list.");
        JsonObject firstScript = scriptList[0];
        const char *firstHumanId = firstScript["id"];
        const char *firstFileId = firstScript["fileId"];

        if (firstHumanId && strlen(firstHumanId) > 0 && firstFileId && strlen(firstFileId) > 0) {
            finalHumanIdForExecution = firstHumanId;
            determinedFileId = firstFileId;
            log_i("loadScriptToExecute: Using first script: humanId '%s', fileId '%s'", finalHumanIdForExecution.c_str(), determinedFileId.c_str());
            // Save this first script's humanId as the current one for next boot
            if (!saveCurrentScriptId(finalHumanIdForExecution.c_str())) {
                log_w("loadScriptToExecute: Failed to save the first script's humanId ('%s') as current to SPIFFS.", finalHumanIdForExecution.c_str());
            }
        } else {
            log_e("loadScriptToExecute: First script in list has no valid humanId or fileId. List is confirmed problematic.");
            listIsPotentiallyCorrupt = true; // Confirm list is problematic
            // finalHumanIdForExecution and determinedFileId remain empty
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 5. If we still don't have a fileId, list.json is bad or empty. Fallback to default.
    if (determinedFileId.length() == 0) {
        log_e("loadScriptToExecute: Could not determine a valid script/fileId from list.json or list was unusable.");
        // Set currentScriptId for handleWakeupAndScriptExecution to use for logging/default script context
        currentScriptId = spiffsIdLoaded ? humanReadableScriptIdFromSpiffs : "default";
        currentScriptContent = ""; // Signal to use default_script
        
        if (listIsPotentiallyCorrupt) {
            if (spiffsIdLoaded) {
                // If current_script.id was loaded and list is corrupt, clear current_script.id
                log_w("loadScriptToExecute: List.json seems corrupted. Clearing current_script.id ('%s') to avoid getting stuck.", humanReadableScriptIdFromSpiffs.c_str());
                saveCurrentScriptId(""); // Clear it
            } else {
                log_w("loadScriptToExecute: List.json seems corrupted. No specific current_script.id was loaded to clear.");
            }

            // Trigger a fetch operation to try and rebuild the list
            log_w("loadScriptToExecute: Triggering a fetch operation to rebuild corrupted/missing list.json.");
            if (g_fetchRequestSemaphore != NULL) {
                xSemaphoreGive(g_fetchRequestSemaphore);
                log_i("loadScriptToExecute: Fetch request semaphore given due to corrupted/missing list.");
            } else {
                log_e("loadScriptToExecute: Fetch request semaphore is NULL, cannot trigger fetch for corrupted list.");
            }
        }
        return false; // Fallback to default script
    }

    // 6. Load script content using the determined fileId
    currentScriptId = finalHumanIdForExecution; // Set global currentScriptId for execution context
    log_i("loadScriptToExecute: Attempting to load content for humanId '%s' using fileId '%s'", currentScriptId.c_str(), determinedFileId.c_str());
    if (!loadScriptContent(determinedFileId.c_str(), currentScriptContent)) {
        log_e("loadScriptToExecute: Failed to load script content for fileId '%s' (humanId '%s'). Using default.", determinedFileId.c_str(), currentScriptId.c_str());
        currentScriptContent = ""; // Signal to use default_script
        return false; // Content load failed, fallback to default
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    log_i("loadScriptToExecute: Successfully set up script humanId '%s' (fileId '%s') for execution.", currentScriptId.c_str(), determinedFileId.c_str());
    return true;
}
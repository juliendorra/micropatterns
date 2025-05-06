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
volatile bool g_user_interrupt_signal_for_fetch_task = false; // Signal to fetch task to stop

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
    FETCH_INTERRUPTED_BY_USER,
    FETCH_NO_WIFI
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
            log_i("User action during fetch. Signaling fetch task to interrupt.");
            g_user_interrupt_signal_for_fetch_task = true;
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
            g_fetch_task_in_progress = true;
            g_user_interrupt_signal_for_fetch_task = false; // Clear interrupt signal for this new fetch run

            // Send "Fetching scripts..." message to UI task
            if (g_displayMessageQueue != NULL)
            {
                strncpy(msg_to_send.text, "Fetching scripts...", sizeof(msg_to_send.text) - 1);
                msg_to_send.text[sizeof(msg_to_send.text) - 1] = '\0';
                msg_to_send.y_offset = 50;
                msg_to_send.color = 15;
                msg_to_send.clear_canvas_first = false;
                msg_to_send.push_full_update = false; // DU4 for quick message
                xQueueSend(g_displayMessageQueue, &msg_to_send, pdMS_TO_TICKS(100));
            }

            FetchResultStatus fetch_status = perform_fetch_operations();

            // Prepare result message
            switch (fetch_status)
            {
            case FETCH_SUCCESS:
                strncpy(msg_to_send.text, "Fetch OK!", sizeof(msg_to_send.text) - 1);
                log_i("FetchTask: Completed successfully.");
                break;
            case FETCH_GENUINE_ERROR:
                strncpy(msg_to_send.text, "Fetch Failed!", sizeof(msg_to_send.text) - 1);
                log_e("FetchTask: Failed with genuine error.");
                break;
            case FETCH_INTERRUPTED_BY_USER:
                strncpy(msg_to_send.text, "Fetch Interrupted", sizeof(msg_to_send.text) - 1);
                log_i("FetchTask: Interrupted by user.");
                break;
            case FETCH_NO_WIFI:
                strncpy(msg_to_send.text, "Fetch: No WiFi", sizeof(msg_to_send.text) - 1);
                log_w("FetchTask: No WiFi connection.");
                break;
            }
            msg_to_send.text[sizeof(msg_to_send.text) - 1] = '\0';
            msg_to_send.y_offset = 100;
            msg_to_send.color = 15;
            msg_to_send.clear_canvas_first = false;
            msg_to_send.push_full_update = true; // GC16 for final status

            if (g_displayMessageQueue != NULL)
            {
                xQueueSend(g_displayMessageQueue, &msg_to_send, pdMS_TO_TICKS(100));
            }

            g_fetch_task_in_progress = false;
            // g_user_interrupt_signal_for_fetch_task is already false or handled.
            log_i("FetchTask: Operation finished, going back to wait for semaphore.");
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
    bool wifiConnected = connectToWiFi(); // connectToWiFi now handles its own cleanup on interrupt

    // Check interrupt flag *immediately* after connectToWiFi returns.
    // This handles cases where interrupt happened during connectToWiFi OR right after it succeeded.
    if (g_user_interrupt_signal_for_fetch_task) {
        log_i("perform_fetch_operations: Fetch interrupted by user (checked after connectToWiFi).");
        if (wifiConnected) { // If WiFi *did* connect but interrupt came just after
            disconnectWiFi(); // Ensure WiFi is off
        }
        // Note: if connectToWiFi was interrupted, it already turned off WiFi.
        return FETCH_INTERRUPTED_BY_USER;
    }

    if (!wifiConnected) { // WiFi failed for reasons other than user interrupt (e.g. timeout)
        log_e("perform_fetch_operations: WiFi connection failed (not user interrupted).");
        // connectToWiFi already turned off WiFi module on timeout or other internal failure.
        return FETCH_NO_WIFI;
    }

    // If we reach here, WiFi is connected and no interrupt was pending immediately after connection.

    WiFiClientSecure httpsClient;
    httpsClient.setCACert(rootCACertificate);
    HTTPClient http;
    bool overall_fetch_successful = true;

    // 1. Fetch Script List
    String listUrl = String(API_BASE_URL) + "/api/scripts";
    log_i("perform_fetch_operations: Fetching script list from: %s", listUrl.c_str());

    if (g_user_interrupt_signal_for_fetch_task)
    {
        disconnectWiFi();
        return FETCH_INTERRUPTED_BY_USER;
    }

    if (!http.begin(httpsClient, listUrl))
    {
        log_e("perform_fetch_operations: HTTPClient begin failed for list URL!");
        disconnectWiFi();
        return FETCH_GENUINE_ERROR;
    }

    int httpCode = http.GET();
    vTaskDelay(pdMS_TO_TICKS(10)); // Small yield after blocking call

    if (g_user_interrupt_signal_for_fetch_task)
    {
        http.end();
        disconnectWiFi();
        return FETCH_INTERRUPTED_BY_USER;
    }

    if (httpCode == HTTP_CODE_OK)
    {
        String payload = http.getString();
        if (g_user_interrupt_signal_for_fetch_task)
        {
            http.end();
            disconnectWiFi();
            return FETCH_INTERRUPTED_BY_USER;
        }
        log_d("perform_fetch_operations: Script list payload: %s", payload.c_str());

        if (!saveScriptList(payload.c_str()))
        {
            log_e("perform_fetch_operations: Failed to save script list JSON to SPIFFS.");
            overall_fetch_successful = false; // Mark as partial failure
        }

        if (g_user_interrupt_signal_for_fetch_task)
        {
            http.end();
            disconnectWiFi();
            return FETCH_INTERRUPTED_BY_USER;
        }

        if (overall_fetch_successful)
        { // Proceed only if list saving was okay
            DynamicJsonDocument listDoc(4096);
            DeserializationError error = deserializeJson(listDoc, payload);
            if (g_user_interrupt_signal_for_fetch_task)
            {
                http.end();
                disconnectWiFi();
                return FETCH_INTERRUPTED_BY_USER;
            }

            if (error)
            {
                log_e("perform_fetch_operations: Failed to parse script list JSON: %s", error.c_str());
                overall_fetch_successful = false;
            }
            else if (!listDoc.is<JsonArray>())
            {
                log_e("perform_fetch_operations: Script list JSON is not an array.");
                overall_fetch_successful = false;
            }
            else
            {
                JsonArray scriptList = listDoc.as<JsonArray>();
                log_i("perform_fetch_operations: Found %d scripts in list. Fetching content...", scriptList.size());

                for (JsonObject scriptInfo : scriptList)
                {
                    if (g_user_interrupt_signal_for_fetch_task)
                    {
                        overall_fetch_successful = false;
                        break;
                    }

                    const char *scriptIdJson = scriptInfo["id"];
                    if (!scriptIdJson)
                    {
                        log_w("perform_fetch_operations: Skipping script with missing ID in list.");
                        continue;
                    }

                    String scriptUrl = String(API_BASE_URL) + "/api/scripts/" + scriptIdJson;
                    log_d("perform_fetch_operations: Fetching script content from: %s", scriptUrl.c_str());

                    http.end(); // End previous connection
                    if (g_user_interrupt_signal_for_fetch_task)
                    {
                        overall_fetch_successful = false;
                        break;
                    }

                    if (!http.begin(httpsClient, scriptUrl))
                    {
                        log_e("perform_fetch_operations: HTTPClient begin failed for script URL: %s", scriptUrl.c_str());
                        overall_fetch_successful = false;
                        continue; // Skip this script
                    }

                    int scriptHttpCode = http.GET();
                    vTaskDelay(pdMS_TO_TICKS(10)); // Small yield

                    if (g_user_interrupt_signal_for_fetch_task)
                    {
                        http.end();
                        overall_fetch_successful = false;
                        break;
                    }

                    if (scriptHttpCode == HTTP_CODE_OK)
                    {
                        String scriptPayload = http.getString();
                        if (g_user_interrupt_signal_for_fetch_task)
                        {
                            http.end();
                            overall_fetch_successful = false;
                            break;
                        }

                        DynamicJsonDocument scriptDoc(8192);
                        DeserializationError scriptError = deserializeJson(scriptDoc, scriptPayload);
                        if (g_user_interrupt_signal_for_fetch_task)
                        {
                            http.end();
                            overall_fetch_successful = false;
                            break;
                        }

                        if (scriptError)
                        {
                            log_e("perform_fetch_operations: Failed to parse script content JSON for ID %s: %s", scriptIdJson, scriptError.c_str());
                            overall_fetch_successful = false;
                        }
                        else
                        {
                            const char *scriptContentFetched = scriptDoc["content"];
                            if (scriptContentFetched)
                            {
                                if (!saveScriptContent(scriptIdJson, scriptContentFetched))
                                {
                                    log_e("perform_fetch_operations: Failed to save script content for ID %s.", scriptIdJson);
                                    overall_fetch_successful = false;
                                }
                                vTaskDelay(pdMS_TO_TICKS(5)); // Small yield after SPIFFS write
                            }
                            else
                            {
                                log_e("perform_fetch_operations: Missing 'content' field for ID %s.", scriptIdJson);
                                overall_fetch_successful = false;
                            }
                        }
                    }
                    else
                    {
                        log_e("perform_fetch_operations: HTTP error fetching script ID %s: %d", scriptIdJson, scriptHttpCode);
                        overall_fetch_successful = false;
                    }
                    http.end();
                    vTaskDelay(pdMS_TO_TICKS(50)); // Delay between script fetches
                } // End loop through scripts
            }
        }
    }
    else
    {
        log_e("perform_fetch_operations: HTTP error fetching script list: %d", httpCode);
        overall_fetch_successful = false;
    }

    http.end();
    disconnectWiFi();

    if (g_user_interrupt_signal_for_fetch_task)
    {
        return FETCH_INTERRUPTED_BY_USER;
    }
    if (overall_fetch_successful)
    {
        return FETCH_SUCCESS;
    }

    return FETCH_GENUINE_ERROR; // If not success and not interrupted, must be a genuine error
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
    bool idLoadedFromSpiffs = loadCurrentScriptId(currentScriptId);
    vTaskDelay(pdMS_TO_TICKS(10)); // Yield

    if (!idLoadedFromSpiffs || currentScriptId.length() == 0)
    {
        log_w("No current script ID found in SPIFFS or it was empty. Attempting to use first script from list.");

        DynamicJsonDocument listDoc(4096);
        if (!loadScriptList(listDoc))
        {
            log_e("Failed to load script list from SPIFFS. Cannot determine a script ID to run.");
            currentScriptId = "";
            currentScriptContent = "";
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // Yield

        if (!listDoc.is<JsonArray>() || listDoc.as<JsonArray>().size() == 0)
        {
            log_e("Script list is not an array or is empty. Cannot determine a script ID to run.");
            currentScriptId = "";
            currentScriptContent = "";
            return false;
        }

        JsonArray scriptList = listDoc.as<JsonArray>();
        JsonObject firstScript = scriptList[0];
        const char *firstId = firstScript["id"];

        if (firstId && strlen(firstId) > 0)
        {
            currentScriptId = firstId;
            log_i("Using first script from list as current: %s", currentScriptId.c_str());
            if (!saveCurrentScriptId(currentScriptId.c_str()))
            {
                log_w("Failed to save the first script ID ('%s') as current to SPIFFS.", currentScriptId.c_str());
            }
            vTaskDelay(pdMS_TO_TICKS(10)); // Yield
        }
        else
        {
            log_e("First script in list has no valid ID. Cannot determine a script ID to run.");
            currentScriptId = "";
            currentScriptContent = "";
            return false;
        }
    }

    if (currentScriptId.length() == 0)
    {
        log_e("FATAL: currentScriptId is empty after attempting all loading strategies.");
        currentScriptContent = "";
        return false;
    }

    log_i("Attempting to load content for script ID: %s", currentScriptId.c_str());
    if (!loadScriptContent(currentScriptId.c_str(), currentScriptContent))
    {
        log_e("Failed to load script content for ID: '%s'.", currentScriptId.c_str());
        currentScriptContent = "";
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10)); // Yield after loading script content

    log_i("Successfully set up script ID '%s' for execution.", currentScriptId.c_str());
    return true;
}
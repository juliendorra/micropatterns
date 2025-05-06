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

// Function Prototypes
bool fetchAndStoreScripts();
bool selectNextScript(bool moveUp);
bool loadScriptToExecute();
void displayMessage(const String &msg, int y_offset = 50, uint16_t color = 15); // Black=15, White=0
void displayParseErrors();
void handleWakeupAndScriptExecution();

// Interrupt handling for wakeup pin is defined in global_setting.cpp

void setup()
{
    // Initialize M5Paper hardware, RTC, SPIFFS, etc.
    SysInit_Start();

    log_i("MicroPatterns M5Paper - Wakeup Cycle %d", executionCounter);

    // Create canvas AFTER M5.EPD.begin() inside SysInit_Start()
    canvas.createCanvas(540, 960);
    if (!canvas.frameBuffer())
    {
        log_e("Failed to create canvas framebuffer!");
        displayMessage("Canvas Error!");
        while (1)
            delay(1000); // Halt
    }
    log_i("Canvas created: %d x %d", canvas.width(), canvas.height());
    // Don't clear here, clear happens in runtime or if displaying messages
}

void loop()
{
    handleWakeupAndScriptExecution();
    goToLightSleep();
}

void displayParseErrors() {
    const auto& errors = parser.getErrors();
    canvas.fillCanvas(0); // White background
    displayMessage("Parse Error: " + currentScriptId, 50, 15);
    int y_pos = 100;
    for (const String& err : errors) {
        log_e("  %s", err.c_str());
        displayMessage(err, y_pos, 15);
        y_pos += 30;
        if (y_pos > canvas.height() - 50) break;
    }
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void handleWakeupAndScriptExecution() {
    bool scriptChangeRequest = false;
    bool moveUpDirection = false;
    bool fetchFromServerAfterExecution = false;

    // 1. Determine Wakeup Reason & Intent
    if (wakeup_pin != 0) {
        log_i("Wakeup caused by GPIO %d", wakeup_pin);
        if (wakeup_pin == BUTTON_UP_PIN) {
            log_i("Button UP (GPIO %d) detected.", wakeup_pin);
            scriptChangeRequest = true;
            moveUpDirection = true;
            fetchFromServerAfterExecution = true;
        } else if (wakeup_pin == BUTTON_DOWN_PIN) {
            log_i("Button DOWN (GPIO %d) detected.", wakeup_pin);
            scriptChangeRequest = true;
            moveUpDirection = false;
            fetchFromServerAfterExecution = true;
        } else if (wakeup_pin == BUTTON_PUSH_PIN) {
            log_i("Button PUSH (GPIO %d) detected.", wakeup_pin);
            // For PUSH, just re-run current script. No fetch by default.
            // If fetch is desired for PUSH, set fetchFromServerAfterExecution = true;
        }
    } else {
        log_i("Wakeup caused by timer or unknown source.");
    }

    // 2. Handle Script Selection if Requested (for UP/DOWN)
    if (scriptChangeRequest) {
        if (!selectNextScript(moveUpDirection)) {
            log_e("Failed to select next script. Will attempt to run current/default.");
            // loadScriptToExecute() below will handle loading the (potentially unchanged) currentScriptId or default.
        }
        // selectNextScript updates currentScriptId and displays a "Selected: ..." message.
    }

    // 3. Load Script Content (current, newly selected, or default)
    if (!loadScriptToExecute()) { // Loads currentScriptId (which might have been updated by selectNextScript) and currentScriptContent
        log_w("Failed to load script from SPIFFS. Using default script.");
        currentScriptContent = default_script;
        currentScriptId = "default"; // Ensure currentScriptId reflects the default script
    }

    // 4. Parse and Prepare Runtime
    log_i("Preparing to execute script ID: %s", currentScriptId.c_str());
    parser.reset();
    if (!parser.parse(currentScriptContent)) {
        log_e("Script parsing failed for ID: %s", currentScriptId.c_str());
        displayParseErrors(); // Display parse errors on screen
        if (runtime) {
            delete runtime;
            runtime = nullptr;
        }
    } else {
        log_i("Script '%s' parsed successfully.", currentScriptId.c_str());
        if (runtime) {
            delete runtime;
        }
        runtime = new MicroPatternsRuntime(&canvas, parser.getAssets());
        runtime->setCommands(&parser.getCommands());
        runtime->setDeclaredVariables(&parser.getDeclaredVariables());
    }

    // 5. Execute Script (if runtime is valid)
    if (runtime) {
        M5.RTC.getTime(&time_struct);
        executionCounter++; // Increment counter

        log_i("Executing script '%s' - Cycle: %d, Time: %02d:%02d:%02d",
              currentScriptId.c_str(), executionCounter, time_struct.hour, time_struct.min, time_struct.sec);

        runtime->setTime(time_struct.hour, time_struct.min, time_struct.sec);
        runtime->setCounter(executionCounter);
        runtime->execute(); // This includes drawing and pushing canvas
        log_i("Finished execution for cycle #%d", executionCounter);
    } else {
        log_e("Runtime not initialized, skipping execution. Check for parse errors.");
        // Error message (if any from parsing) should have already been shown by displayParseErrors().
    }

    // 6. Fetch from Server if Requested (e.g., after UP/DOWN interaction and execution)
    if (fetchFromServerAfterExecution) {
        displayMessage("Fetching scripts...", 50, 15); // Display message on screen
        canvas.pushCanvas(0, 0, UPDATE_MODE_DU4);      // Faster update for message
        yield();
        if (!fetchAndStoreScripts()) {
            displayMessage("Fetch Failed!", 100, 15);
        } else {
            displayMessage("Fetch OK!", 100, 15);
        }
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16); // Update display with fetch result
        yield();
        delay(1000);                               // Show message briefly
    }
}


// --- Helper Functions ---

void displayMessage(const String &msg, int y_offset, uint16_t color)
{
    // canvas must be created before calling this
    if (!canvas.frameBuffer())
        return;
    // canvas.fillCanvas(0); // Optional: Clear background? Be careful if drawing over existing content
    canvas.setTextSize(3);         // Adjust size as needed
    canvas.setTextDatum(TC_DATUM); // Top Center alignment
    canvas.setTextColor(color);
    canvas.drawString(msg, canvas.width() / 2, y_offset);
    log_i("Displaying message: %s", msg.c_str());
}

// Fetches script list and content from API, stores in SPIFFS
bool fetchAndStoreScripts()
{
    if (!connectToWiFi())
    {
        log_e("Cannot fetch scripts, WiFi connection failed.");
        return false;
    }

    WiFiClientSecure httpsClient;
    // Set the root CA certificate for the secure client
    // rootCACertificate is defined in global_setting.cpp and extern'd in global_setting.h
    httpsClient.setCACert(rootCACertificate);

    HTTPClient http;
    bool success = true;

    // 1. Fetch Script List
    String listUrl = String(API_BASE_URL) + "/api/scripts";
    log_i("Fetching script list from: %s", listUrl.c_str());

    // Use the configured WiFiClientSecure for HTTPClient
    if (!http.begin(httpsClient, listUrl)) {
        log_e("HTTPClient begin failed for list URL!");
        disconnectWiFi();
        return false;
    }

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK)
    {
        String payload = http.getString();
        log_d("Script list payload: %s", payload.c_str());

        // Save the raw list JSON to SPIFFS
        if (!saveScriptList(payload.c_str()))
        {
            log_e("Failed to save script list JSON to SPIFFS.");
            success = false;
        }
        else
        {
            // Parse the list to fetch individual scripts
            DynamicJsonDocument listDoc(4096); // Adjust size as needed
            DeserializationError error = deserializeJson(listDoc, payload);
            if (error)
            {
                log_e("Failed to parse script list JSON: %s", error.c_str());
                success = false;
            }
            else if (!listDoc.is<JsonArray>())
            {
                log_e("Script list JSON is not an array.");
                success = false;
            }
            else
            {
                JsonArray scriptList = listDoc.as<JsonArray>();
                log_i("Found %d scripts in list. Fetching content...", scriptList.size());

                // 2. Fetch Each Script Content
                for (JsonObject scriptInfo : scriptList)
                {
                    const char *scriptIdJson = scriptInfo["id"]; // Renamed to avoid conflict
                    if (!scriptIdJson)
                    {
                        log_w("Skipping script with missing ID in list.");
                        continue;
                    }

                    String scriptUrl = String(API_BASE_URL) + "/api/scripts/" + scriptIdJson;
                    log_d("Fetching script content from: %s", scriptUrl.c_str());
                    http.end(); // End previous connection before starting new one
                    // Use the configured WiFiClientSecure for HTTPClient
                    if (!http.begin(httpsClient, scriptUrl)) {
                        log_e("HTTPClient begin failed for script URL: %s", scriptUrl.c_str());
                        // Optionally skip this script and continue with others
                        continue;
                    }
                    int scriptHttpCode = http.GET();
                    yield(); // Yield after blocking network call (GET)

                    if (scriptHttpCode == HTTP_CODE_OK)
                    {
                        String scriptPayload = http.getString();
                        yield(); // Yield after getting string
                        // Parse the script JSON to get the "content" field
                        DynamicJsonDocument scriptDoc(8192); // Larger doc for script content
                        DeserializationError scriptError = deserializeJson(scriptDoc, scriptPayload);
                        yield(); // Yield after JSON parsing

                        if (scriptError)
                        {
                            log_e("Failed to parse script content JSON for ID %s: %s", scriptIdJson, scriptError.c_str());
                            // Optionally save raw payload? Or mark as failed?
                        }
                        else
                        {
                            const char *scriptContentFetched = scriptDoc["content"]; // Renamed
                            if (scriptContentFetched)
                            {
                                if (!saveScriptContent(scriptIdJson, scriptContentFetched))
                                {
                                    log_e("Failed to save script content for ID %s to SPIFFS.", scriptIdJson);
                                    // Consider this a partial failure?
                                }
                                yield(); // Yield after saving content to SPIFFS
                            }
                            else
                            {
                                log_e("Missing 'content' field in script JSON for ID %s.", scriptIdJson);
                            }
                        }
                    }
                    else
                    {
                        log_e("HTTP error fetching script ID %s: %d - %s", scriptIdJson, scriptHttpCode, http.errorToString(scriptHttpCode).c_str());
                        // Mark as failed?
                    }
                    http.end(); // End connection for this script
                    delay(50);  // Small delay between requests
                } // End loop through scripts
            }
        }
    }
    else
    {
        log_e("HTTP error fetching script list: %d - %s", httpCode, http.errorToString(httpCode).c_str());
        success = false;
    }

    http.end();
    disconnectWiFi();
    return success;
}

// Selects the next script ID based on the direction (up/down)
bool selectNextScript(bool moveUp)
{
    DynamicJsonDocument listDoc(4096); // Document to hold the list

    // Call loadScriptList to populate listDoc
    if (!loadScriptList(listDoc))
    {
        log_e("Cannot select next script, failed to load script list from SPIFFS.");
        return false;
    }
    // Check if the loaded document contains a valid array and it's not empty
    if (!listDoc.is<JsonArray>() || listDoc.as<JsonArray>().size() == 0) {
        log_e("Cannot select next script, list is empty or not an array after loading.");
        return false;
    }

    JsonArray scriptList = listDoc.as<JsonArray>(); // Get the array view from our document
    yield(); // Yield after loading script list from SPIFFS

    String loadedCurrentId;
    loadCurrentScriptId(loadedCurrentId); // Load the last used ID

    int currentIndex = -1;
    for (int i = 0; i < scriptList.size(); i++)
    {
        JsonObject scriptInfo = scriptList[i];
        const char *idJson = scriptInfo["id"]; // Renamed
        if (idJson && loadedCurrentId == idJson)
        {
            currentIndex = i;
            break;
        }
    }

    int nextIndex = 0; // Default to first script
    if (scriptList.size() > 0)
    {
        if (currentIndex != -1)
        {
            // Calculate next index with wrap-around
            int delta = moveUp ? -1 : 1;
            nextIndex = (currentIndex + delta + scriptList.size()) % scriptList.size();
        }
        else
        {
            // If current ID wasn't found or loaded, default based on direction
            nextIndex = moveUp ? scriptList.size() - 1 : 0;
        }
        JsonObject nextScriptInfo = scriptList[nextIndex];
        const char *nextId = nextScriptInfo["id"];
        const char *nextName = nextScriptInfo["name"]; // Get name for logging/display

        if (nextId)
        {
            log_i("Selected script index: %d, ID: %s, Name: %s", nextIndex, nextId, nextName ? nextName : "N/A");
            if (saveCurrentScriptId(nextId))
            {
                yield(); // Yield after saving current script ID to SPIFFS
                // Display message on screen (will be overwritten by script execution or fetch messages soon)
                canvas.fillCanvas(0); // Clear for message
                displayMessage(String("Selected: ") + (nextName ? nextName : nextId), 150, 15);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
                yield(); // Yield after pushing canvas
                delay(1500); // Show message
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
    // 1. Attempt to load currentScriptId from SPIFFS.
    //    The global currentScriptId will be updated by loadCurrentScriptId.
    bool idLoadedFromSpiffs = loadCurrentScriptId(currentScriptId);

    if (!idLoadedFromSpiffs || currentScriptId.length() == 0) {
        log_w("No current script ID found in SPIFFS or it was empty. Attempting to use first script from list.");
        yield();

        DynamicJsonDocument listDoc(4096);
        if (!loadScriptList(listDoc)) {
            log_e("Failed to load script list from SPIFFS. Cannot determine a script ID to run.");
            currentScriptId = ""; // Ensure currentScriptId is cleared
            currentScriptContent = "";
            return false; // Cannot proceed without a list to pick from
        }

        if (!listDoc.is<JsonArray>() || listDoc.as<JsonArray>().size() == 0) {
            log_e("Script list is not an array or is empty. Cannot determine a script ID to run.");
            currentScriptId = ""; // Ensure currentScriptId is cleared
            currentScriptContent = "";
            return false; // Cannot pick from an invalid or empty list
        }

        JsonArray scriptList = listDoc.as<JsonArray>();
        // scriptList.size() > 0 is guaranteed by the check above
        JsonObject firstScript = scriptList[0];
        const char *firstId = firstScript["id"];

        if (firstId && strlen(firstId) > 0) { // Check if firstId is valid and not empty
            currentScriptId = firstId;
            log_i("Using first script from list as current: %s", currentScriptId.c_str());
            if (!saveCurrentScriptId(currentScriptId.c_str())) {
                log_w("Failed to save the first script ID ('%s') as current to SPIFFS.", currentScriptId.c_str());
                // Continue anyway, currentScriptId is set in memory.
            }
            yield();
        } else {
            log_e("First script in list has no valid ID. Cannot determine a script ID to run.");
            currentScriptId = ""; // Ensure currentScriptId is cleared
            currentScriptContent = "";
            return false;
        }
    }

    // At this point, currentScriptId should be populated (either from SPIFFS or the first from the list).
    // If it's still somehow empty, it's an error.
    if (currentScriptId.length() == 0) {
        log_e("FATAL: currentScriptId is empty after attempting all loading strategies.");
        currentScriptContent = "";
        return false;
    }

    // 2. Load content for the determined currentScriptId.
    log_i("Attempting to load content for script ID: %s", currentScriptId.c_str());
    if (!loadScriptContent(currentScriptId.c_str(), currentScriptContent)) {
        log_e("Failed to load script content for ID: '%s'.", currentScriptId.c_str());
        currentScriptContent = ""; // Clear content on failure
        return false;
    }
    yield(); // Yield after loading script content from SPIFFS

    log_i("Successfully set up script ID '%s' for execution.", currentScriptId.c_str());
    return true; // Successfully loaded ID and Content
}
#include "script_manager.h"
#include "esp32-hal-log.h"
#include <set>
#include <vector>
#include <set>
#include <vector>

const char* ScriptManager::LIST_JSON_PATH = "/scripts/list.json";
const char* ScriptManager::CONTENT_DIR_PATH = "/scripts/content";
const char* ScriptManager::CURRENT_SCRIPT_ID_PATH = "/current_script.id";
const char* ScriptManager::SCRIPT_STATES_PATH = "/scripts/script_states.json";

// Default script content (shortened for brevity, use actual default script)
const char* ScriptManager::DEFAULT_SCRIPT_CONTENT = R"(
DEFINE PATTERN NAME="default" WIDTH=1 HEIGHT=1 DATA="1"
COLOR NAME=BLACK
FILL NAME=SOLID
FILL_RECT X=10 Y=10 WIDTH=100 HEIGHT=50
COLOR NAME=WHITE
DRAW NAME="default" X=0 Y=0
)";
const char* ScriptManager::DEFAULT_SCRIPT_ID = "default_fallback_script";


ScriptManager::ScriptManager() {
    _spiffsMutex = xSemaphoreCreateMutex();
    if (_spiffsMutex == NULL) {
        log_e("ScriptManager: Failed to create SPIFFS mutex!");
    }
}

ScriptManager::~ScriptManager() {
    if (_spiffsMutex != NULL) {
        vSemaphoreDelete(_spiffsMutex);
    }
}

bool ScriptManager::initialize() {
    if (xSemaphoreTake(_spiffsMutex, portMAX_DELAY) == pdTRUE) {
        bool success = initializeSPIFFS();
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("ScriptManager::initialize failed to take mutex.");
    return false;
}

bool ScriptManager::initializeSPIFFS() {
    if (!SPIFFS.begin(true)) { // `true` = format SPIFFS if mount fails
        log_e("SPIFFS Mount Failed even after formatting attempt!");
        return false;
    }
    log_i("SPIFFS Mounted successfully.");

    // Create directories if they don't exist
    if (!SPIFFS.exists("/scripts")) {
        if (!SPIFFS.mkdir("/scripts")) {
             log_e("Failed to create /scripts directory!");
             return false;
        }
        log_i("Created /scripts directory");
    }
    if (!SPIFFS.exists(CONTENT_DIR_PATH)) {
        if (!SPIFFS.mkdir(CONTENT_DIR_PATH)) {
            log_e("Failed to create %s directory!", CONTENT_DIR_PATH);
            return false;
        }
        log_i("Created %s directory", CONTENT_DIR_PATH);
    }
    return true;
}

// Implementation is now using JsonDocument instead of DynamicJsonDocument

bool ScriptManager::saveScriptList(const JsonDocument& listDoc) {
    if (!listDoc.is<JsonArray>()) {
        log_e("saveScriptList: Provided document is not a JSON array.");
        return false;
    }
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        File file = SPIFFS.open(LIST_JSON_PATH, FILE_WRITE);
        if (!file) {
            log_e("saveScriptList: Failed to open %s for writing", LIST_JSON_PATH);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        String outputJson;
        serializeJson(listDoc, outputJson);
        bool success = false;
        if (file.print(outputJson)) {
            log_i("Script list saved successfully to %s. (%d entries)", LIST_JSON_PATH, (int)listDoc.as<JsonArrayConst>().size());
            success = true;
        } else {
            log_e("saveScriptList: Failed to write script list to %s.", LIST_JSON_PATH);
        }
        file.close();
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("ScriptManager::saveScriptList failed to take mutex.");
    return false;
}

bool ScriptManager::loadScriptContent(const String& fileId, String& outContent) {
    if (fileId.isEmpty()) {
        log_e("loadScriptContent: fileId is empty.");
        return false;
    }
    String path = String(CONTENT_DIR_PATH) + "/" + fileId;
    
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (!SPIFFS.exists(path.c_str())) {
            log_w("loadScriptContent: Path does not exist: %s", path.c_str());
            outContent = "";
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        File file = SPIFFS.open(path.c_str(), FILE_READ);
        if (!file || file.isDirectory()) {
            log_w("loadScriptContent: Failed to open %s or it's a directory.", path.c_str());
            if(file) file.close();
            outContent = "";
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        outContent = file.readString();
        size_t fileSize = file.size(); // Get size before closing
        file.close();
        xSemaphoreGive(_spiffsMutex);

        if (outContent.length() > 0) {
            log_i("Script content for fileId '%s' loaded successfully (read %d bytes from %s).", fileId.c_str(), outContent.length(), path.c_str());
            return true;
        } else {
            log_w("Script content file for fileId '%s' (path: %s) is empty or readString() failed (content length: %d, reported file size: %u).", fileId.c_str(), path.c_str(), outContent.length(), fileSize);
            return false;
        }
    }
    log_e("ScriptManager::loadScriptContent failed to take mutex for fileId %s", fileId.c_str());
    return false;
}

bool ScriptManager::saveScriptContent(const String& fileId, const String& content) {
    if (fileId.isEmpty()) {
        log_e("saveScriptContent: fileId is empty.");
        return false;
    }
    String path = String(CONTENT_DIR_PATH) + "/" + fileId;

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        File file = SPIFFS.open(path.c_str(), FILE_WRITE);
        if (!file) {
            log_e("Failed to open %s for writing (fileId: %s)", path.c_str(), fileId.c_str());
            xSemaphoreGive(_spiffsMutex);
            return false;
        }
        bool success = false;
        if (file.print(content)) {
            log_i("Script content for fileId '%s' saved successfully to %s.", fileId.c_str(), path.c_str());
            success = true;
        } else {
            log_e("Failed to write script content for fileId '%s' to %s.", fileId.c_str(), path.c_str());
        }
        file.close();
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("ScriptManager::saveScriptContent failed to take mutex for fileId %s", fileId.c_str());
    return false;
}

bool ScriptManager::getCurrentScriptId(String& outHumanId) {
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        File file = SPIFFS.open(CURRENT_SCRIPT_ID_PATH, FILE_READ);
        if (!file || file.isDirectory()) {
            log_w("Failed to open %s for reading or it's a directory.", CURRENT_SCRIPT_ID_PATH);
            if(file) file.close();
            outHumanId = "";
            xSemaphoreGive(_spiffsMutex);
            return false;
        }
        outHumanId = file.readString();
        file.close();
        xSemaphoreGive(_spiffsMutex);

        if (outHumanId.length() > 0) {
            log_i("Current script humanId '%s' loaded from %s.", outHumanId.c_str(), CURRENT_SCRIPT_ID_PATH);
            return true;
        } else {
            log_w("%s is empty or read failed.", CURRENT_SCRIPT_ID_PATH);
            return false;
        }
    }
    log_e("ScriptManager::getCurrentScriptId failed to take mutex.");
    return false;
}

bool ScriptManager::saveCurrentScriptId(const String& humanId) {
    if (humanId.isEmpty()) {
        log_e("saveCurrentScriptId: humanId is empty.");
        return false;
    }
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        File file = SPIFFS.open(CURRENT_SCRIPT_ID_PATH, FILE_WRITE);
        if (!file) {
            log_e("Failed to open %s for writing", CURRENT_SCRIPT_ID_PATH);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }
        bool success = false;
        if (file.print(humanId)) {
            log_i("Current script humanId '%s' saved to %s.", humanId.c_str(), CURRENT_SCRIPT_ID_PATH);
            success = true;
        } else {
            log_e("Failed to write current script humanId '%s' to %s.", humanId.c_str(), CURRENT_SCRIPT_ID_PATH);
        }
        file.close();
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("ScriptManager::saveCurrentScriptId failed to take mutex for humanId %s", humanId.c_str());
    return false;
}

bool ScriptManager::loadScriptExecutionState(const String& humanId, ScriptExecState& outState) {
    if (humanId.isEmpty()) {
        log_e("loadScriptExecutionState: humanId is null or empty.");
        outState.state_loaded = false;
        return false;
    }
    outState = ScriptExecState(); // Reset to default

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        File file = SPIFFS.open(SCRIPT_STATES_PATH, FILE_READ);
        if (!file || file.isDirectory() || file.size() == 0) {
            if (file) file.close();
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            return false;
        }

        JsonDocument statesDoc;
        DeserializationError error = deserializeJson(statesDoc, file);
        file.close();
        

        if (error) {
            log_e("loadScriptExecutionState: Failed to parse %s: %s.", SCRIPT_STATES_PATH, error.c_str());
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            return false;
        }
        if (!statesDoc.is<JsonObject>()) {
            log_e("loadScriptExecutionState: %s content is not a JSON object.", SCRIPT_STATES_PATH);
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            return false;
        }

        JsonObject root = statesDoc.as<JsonObject>();

        JsonVariantConst scriptVariant = root[humanId.c_str()];

        if (scriptVariant.isNull()) { // Check if the key exists and is not null
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            // log_d("Script ID '%s' not found in states file.", humanId.c_str());
            return false; // Script ID not found in the states object
        }

        // Ensure the value for humanId is an object
        if (!scriptVariant.is<JsonObjectConst>()) { // Check if it can be viewed as JsonObjectConst
            log_e("loadScriptExecutionState: State for script ID '%s' is not a JSON object.", humanId.c_str());
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            return false;
        }

        JsonObjectConst scriptStateObj = scriptVariant.as<JsonObjectConst>(); // Use JsonObjectConst for reading
        if (!scriptStateObj["counter"].is<int>() || !scriptStateObj["hour"].is<int>() ||
            !scriptStateObj["minute"].is<int>() || !scriptStateObj["second"].is<int>()) {
            log_e("loadScriptExecutionState: Incomplete state for script ID '%s' in %s.", humanId.c_str(), SCRIPT_STATES_PATH);
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            return false;
        }

        outState.counter = scriptStateObj["counter"].as<int>();
        outState.hour = scriptStateObj["hour"].as<int>();
        outState.minute = scriptStateObj["minute"].as<int>();
        outState.second = scriptStateObj["second"].as<int>();
        outState.state_loaded = true;
        
        xSemaphoreGive(_spiffsMutex);
        log_i("Script execution state loaded for ID '%s': Counter=%d, Time=%02d:%02d:%02d",
              humanId.c_str(), outState.counter, outState.hour, outState.minute, outState.second);
        return true;
    }
    // Mutex was not taken or already given in a path above.
    // If mutex was taken and an early return happened, it should have been released.
    // If we reach here, it means xSemaphoreTake failed initially or was released and logic continued.
    // The original code had a path where mutex was given then another log_i and return false.
    // Ensuring mutex is always given if taken.
    // The original code had a bug here: if xSemaphoreTake failed, it would fall through and log "not found".
    // Corrected: if xSemaphoreTake fails, it should log and return.
    // If it succeeds, all paths must give it back.

    // This part of the original code is reached if xSemaphoreTake fails.
    // log_e("ScriptManager::loadScriptExecutionState failed to take mutex for humanId %s", humanId.c_str());
    // The original log_i("Script execution state not found...") was misleading if mutex failed.
    // The function should have already returned if mutex was taken.
    // If mutex was not taken, this is the correct path.
    log_e("ScriptManager::loadScriptExecutionState failed to take mutex for humanId %s", humanId.c_str());
    outState.state_loaded = false; // Ensure state_loaded is false if mutex fails
    return false;
}

bool ScriptManager::loadScriptList(JsonDocument& outListDoc) {
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        File file = SPIFFS.open(LIST_JSON_PATH, FILE_READ);
        if (!file || file.isDirectory() || file.size() == 0) {
            if (file) file.close();
            log_w("Script list file %s not found or empty.", LIST_JSON_PATH);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        DeserializationError error = deserializeJson(outListDoc, file);
        file.close();

        if (error) {
            log_e("Failed to parse script list JSON: %s", error.c_str());
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        if (!outListDoc.is<JsonArray>()) {
            log_e("Script list JSON is not an array.");
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        log_i("Script list loaded successfully (%zu entries).", outListDoc.as<JsonArray>().size());
        xSemaphoreGive(_spiffsMutex);
        return true;
    }
    log_e("ScriptManager::loadScriptList failed to take mutex.");
    return false;
}

bool ScriptManager::saveScriptExecutionState(const String& humanId, const ScriptExecState& state) {
     // Create a local copy of humanId to ensure its stability
    String localHumanId = humanId;

    if (localHumanId.isEmpty()) {
        log_e("saveScriptExecutionState: Attempted to save state for an empty humanId. Aborting.");
        return false;
    }

    if (localHumanId.isEmpty()) { // This check is redundant due to the one above, but kept from original for structure.
        log_e("saveScriptExecutionState: localHumanId (from humanId) is null or empty.");
        return false;
    }

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        JsonDocument statesDoc;

        File file = SPIFFS.open(SCRIPT_STATES_PATH, FILE_READ);
        if (file && file.size() > 0 && !file.isDirectory()) {
            DeserializationError error = deserializeJson(statesDoc, file);
            if (error) {
                log_w("saveScriptExecutionState: Failed to parse existing %s: %s. Will overwrite.", SCRIPT_STATES_PATH, error.c_str());
                statesDoc.clear();
            }
        }
        if(file) file.close();

        if (!statesDoc.is<JsonObject>()) {
            statesDoc.to<JsonObject>();
        }
        JsonObject root = statesDoc.as<JsonObject>();

        // Create or get the object for this script ID using to<JsonObject>()
        // This will create the object if it doesn't exist, or return it if it does.
        // If the key exists but is not an object, it will be replaced.
        JsonObject scriptStateObj = root[localHumanId.c_str()].to<JsonObject>();

        scriptStateObj["counter"] = state.counter;
        scriptStateObj["hour"] = state.hour;
        scriptStateObj["minute"] = state.minute;
        scriptStateObj["second"] = state.second;

        file = SPIFFS.open(SCRIPT_STATES_PATH, FILE_WRITE);
        if (!file) {
            log_e("saveScriptExecutionState: Failed to open %s for writing.", SCRIPT_STATES_PATH);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        bool success = false;
        if (serializeJson(statesDoc, file) == 0) {
            log_e("saveScriptExecutionState: Failed to write to %s.", SCRIPT_STATES_PATH);
        } else {
            success = true;
        }
        file.close();
        xSemaphoreGive(_spiffsMutex);

        if(success) log_i("Script state for ID '%s' (C:%d, T:%02d:%02d:%02d) saved to %s.", localHumanId.c_str(), state.counter, state.hour, state.minute, state.second, SCRIPT_STATES_PATH);
        return success;
    }
    log_e("ScriptManager::saveScriptExecutionState failed to take mutex for humanId %s", localHumanId.c_str());
    return false;
}


bool ScriptManager::selectNextScript(bool moveUp, String& outSelectedHumanId, String& outSelectedName) {
    JsonDocument listDoc;
    if (!loadScriptList(listDoc)) { // loadScriptList handles mutex
        log_e("Cannot select next script, failed to load script list.");
        return false;
    }
    if (!listDoc.is<JsonArray>() || listDoc.as<JsonArray>().size() == 0) {
        log_w("Cannot select next script, list is empty or not an array.");
        outSelectedHumanId = DEFAULT_SCRIPT_ID; // Fallback
        outSelectedName = "Default";
        saveCurrentScriptId(DEFAULT_SCRIPT_ID); // Attempt to save default
        return false; // Indicate that selection from list failed
    }

    JsonArray scriptList = listDoc.as<JsonArray>();
    String currentHumanId;
    getCurrentScriptId(currentHumanId); // getCurrentScriptId handles mutex

    int currentIndex = -1;
    for (int i = 0; i < scriptList.size(); i++) {
        JsonObject scriptInfo = scriptList[i];
        const char* idJson = scriptInfo["id"];
        if (idJson && currentHumanId == idJson) {
            currentIndex = i;
            break;
        }
    }

    int nextIndex = 0;
    if (scriptList.size() > 0) {
        if (currentIndex != -1) {
            int delta = moveUp ? -1 : 1;
            nextIndex = (currentIndex + delta + scriptList.size()) % scriptList.size();
        } else {
            nextIndex = moveUp ? scriptList.size() - 1 : 0; // Default to last or first
        }
        JsonObject nextScriptInfo = scriptList[nextIndex];
        const char* nextId = nextScriptInfo["id"];
        const char* nextName = nextScriptInfo["name"];

        if (nextId) {
            outSelectedHumanId = nextId;
            outSelectedName = nextName ? nextName : nextId;
            log_i("Selected script index: %d, ID: %s, Name: %s", nextIndex, outSelectedHumanId.c_str(), outSelectedName.c_str());
            if (saveCurrentScriptId(outSelectedHumanId)) { // saveCurrentScriptId handles mutex
                return true;
            } else {
                log_e("Failed to save the new current script ID: %s", outSelectedHumanId.c_str());
                return false;
            }
        } else {
            log_e("Script at index %d has no ID.", nextIndex);
        }
    }
    // Fallback if something went wrong or list was empty after all
    outSelectedHumanId = DEFAULT_SCRIPT_ID;
    outSelectedName = "Default";
    saveCurrentScriptId(DEFAULT_SCRIPT_ID);
    return false;
}

bool ScriptManager::getScriptForExecution(String& outHumanId, String& outFileId, String& outContent, ScriptExecState& outInitialState) {
    String humanIdToLoad;
    bool idFound = getCurrentScriptId(humanIdToLoad); // Mutex handled

    JsonDocument listDoc;
    bool listLoaded = loadScriptList(listDoc); // Mutex handled
    JsonArrayConst scriptList; // Default empty JsonArrayConst
    if (listLoaded && listDoc.is<JsonArray>()) { // Check if listDoc holds a JsonArray
        scriptList = listDoc.as<JsonArrayConst>(); // Get a const view
    }

    String fileIdToLoadContent = "";

    if (idFound && humanIdToLoad.isEmpty()){ // If ID was "found" but is empty, treat as not found
        log_w("Current script ID loaded from file but was empty. Will try first script or default.");
        idFound = false;
    }

    if (idFound && !humanIdToLoad.isEmpty()) { // Try current script ID first
        bool foundInList = false;
        if (listLoaded && !scriptList.isNull() && scriptList.size() > 0) { // Check scriptList is valid
            for (JsonVariantConst item : scriptList) { // Iterate JsonArrayConst
                if (item.is<JsonObjectConst>()) {
                    JsonObjectConst scriptInfo = item.as<JsonObjectConst>();
                    if (scriptInfo["id"].is<const char*>()) { // Check "id" exists and is string-like
                        if (humanIdToLoad == scriptInfo["id"].as<String>()) {
                            if (scriptInfo["fileId"].is<const char*>()) { // Check "fileId" exists and is string-like
                               fileIdToLoadContent = scriptInfo["fileId"].as<String>();
                            } else {
                                log_w("Script '%s' in list is missing 'fileId' or it's not a string. Using humanId as fileId.", scriptInfo["id"].as<const char*>());
                                fileIdToLoadContent = humanIdToLoad; // Fallback if fileId is missing
                            }
                            foundInList = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!foundInList) {
            log_w("Current script ID '%s' not found in list.json or list empty/invalid. Attempting first script.", humanIdToLoad.c_str());
            idFound = false; // Force trying the first script
            humanIdToLoad = ""; // Clear to ensure it tries first script logic
            fileIdToLoadContent = ""; // Clear as well
        }
    }
    
    // If no current ID, or it was invalid (not found in list or list empty), or fileId not found from current ID
    if (!idFound || humanIdToLoad.isEmpty() || fileIdToLoadContent.isEmpty()) {
        if (listLoaded && !scriptList.isNull() && scriptList.size() > 0) {
            JsonVariantConst item = scriptList[0]; // Get first item
            if (item.is<JsonObjectConst>()) {
                JsonObjectConst firstScript = item.as<JsonObjectConst>();
                if (firstScript["id"].is<const char*>()) {
                    humanIdToLoad = firstScript["id"].as<String>();
                    if (firstScript["fileId"].is<const char*>()) {
                        fileIdToLoadContent = firstScript["fileId"].as<String>();
                    } else {
                        log_w("First script '%s' in list is missing 'fileId'. Using humanId as fileId.", humanIdToLoad.c_str());
                        fileIdToLoadContent = humanIdToLoad; // Fallback
                    }
                    log_i("Using first script from list: ID '%s', fileId '%s'", humanIdToLoad.c_str(), fileIdToLoadContent.c_str());
                    saveCurrentScriptId(humanIdToLoad);
                } else {
                    log_e("First script in list is malformed (missing id or it's not a string).");
                    humanIdToLoad = "";
                    fileIdToLoadContent = "";
                }
            } else {
                log_e("First item in script list is not an object.");
                humanIdToLoad = "";
                fileIdToLoadContent = "";
            }
        } else {
            log_w("No scripts in list.json or list invalid. Using default script.");
            outHumanId = DEFAULT_SCRIPT_ID;
            outFileId = DEFAULT_SCRIPT_ID; // Use default ID as fileId for default script
            outContent = DEFAULT_SCRIPT_CONTENT;
            outInitialState = ScriptExecState();
            outInitialState.state_loaded = false;
            return true;
        }
    }

    if (fileIdToLoadContent.isEmpty() || humanIdToLoad.isEmpty()) {
        log_e("Could not determine a valid script to load (current and first script failed). Using default.");
        outHumanId = DEFAULT_SCRIPT_ID;
        outFileId = DEFAULT_SCRIPT_ID;
        outContent = DEFAULT_SCRIPT_CONTENT;
        outInitialState = ScriptExecState();
        outInitialState.state_loaded = false;
        return true;
    }

    if (loadScriptContent(fileIdToLoadContent, outContent)) { // Mutex handled
        outHumanId = humanIdToLoad;
        outFileId = fileIdToLoadContent;
        if (!loadScriptExecutionState(outHumanId, outInitialState)) { // Mutex handled
            log_i("No prior execution state for script '%s'. Starting fresh.", outHumanId.c_str());
            outInitialState = ScriptExecState();
            outInitialState.state_loaded = false;
        }
        return true;
    } else {
        log_e("Failed to load content for script ID '%s' (fileId '%s'). Using default script.", humanIdToLoad.c_str(), fileIdToLoadContent.c_str());
        outHumanId = DEFAULT_SCRIPT_ID;
        outFileId = DEFAULT_SCRIPT_ID;
        outContent = DEFAULT_SCRIPT_CONTENT;
        outInitialState = ScriptExecState();
        outInitialState.state_loaded = false;
        return true;
    }
}

void ScriptManager::clearAllScriptData() {
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        log_w("Clearing all script data (list.json, current_script.id, script_states.json and content files).");

        SPIFFS.remove(LIST_JSON_PATH);
        SPIFFS.remove(CURRENT_SCRIPT_ID_PATH);
        SPIFFS.remove(SCRIPT_STATES_PATH);

        File root = SPIFFS.open(CONTENT_DIR_PATH);
        if (root) {
            if (root.isDirectory()) {
                File entry = root.openNextFile();
                while (entry) {
                    if (!entry.isDirectory()) {
                        String pathComponent = entry.name();
                        String fullPathToDelete = String(CONTENT_DIR_PATH) + "/" + pathComponent;
                         // Check if pathComponent already starts with /scripts/content
                        if (pathComponent.startsWith(CONTENT_DIR_PATH)) {
                            fullPathToDelete = pathComponent;
                        } else if (pathComponent.startsWith("/")) {
                             // It's an absolute path but not the full one, likely just entry.name()
                             // This case needs care. Assuming entry.name() is just the filename.
                             fullPathToDelete = String(CONTENT_DIR_PATH) + "/" + pathComponent;
                        }


                        log_i("Deleting content file: %s", fullPathToDelete.c_str());
                        SPIFFS.remove(fullPathToDelete.c_str());
                    }
                    entry.close(); // Close file handle
                    entry = root.openNextFile();
                }
            }
            root.close(); // Close directory handle
        } else {
            log_w("Could not open %s directory for clearing.", CONTENT_DIR_PATH);
        }
        // Attempt to remove the content directory itself if it's empty
        SPIFFS.rmdir(CONTENT_DIR_PATH);
        // Recreate directory structure
        initializeSPIFFS(); // This will recreate dirs if they were removed or ensure they exist

        xSemaphoreGive(_spiffsMutex);
        log_i("Script data clearing completed.");
    } else {
        log_e("ScriptManager::clearAllScriptData failed to take mutex.");
    }
}

void ScriptManager::cleanupOrphanedStates(const JsonArrayConst& validScriptList) {
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        log_i("Cleaning up orphaned script execution states...");
        JsonDocument currentStatesDoc;
        File statesFile = SPIFFS.open(SCRIPT_STATES_PATH, FILE_READ);
        bool statesLoaded = false;

        if (!statesFile || statesFile.isDirectory() || statesFile.size() == 0) {
            if(statesFile) statesFile.close();
            log_i("%s not found or empty. No states to clean.", SCRIPT_STATES_PATH);
        } else {
            DeserializationError error = deserializeJson(currentStatesDoc, statesFile);
            statesFile.close();
            if (error) {
                log_e("Failed to parse %s for cleanup: %s. Skipping cleanup.", SCRIPT_STATES_PATH, error.c_str());
            } else if (!currentStatesDoc.is<JsonObject>()) {
                log_e("%s content not a JSON object. Skipping cleanup.", SCRIPT_STATES_PATH);
            } else {
                statesLoaded = true;
            }
        }

            if (statesLoaded) {
                std::set<String> validHumanIds;
                for (JsonVariantConst item : validScriptList) {
                    if (!item.is<JsonObjectConst>()) {
                        log_w("Cleanup: Item in validScriptList is not an object, skipping.");
                        continue;
                    }
                    JsonObjectConst item_obj = item.as<JsonObjectConst>(); // Corrected
                    const char* humanId = item_obj["id"].as<const char*>();
                    if (humanId) validHumanIds.insert(String(humanId));
                }
                
                JsonObject statesRoot = currentStatesDoc.as<JsonObject>();
                bool statesModified = false;
                std::vector<String> keysToRemove;
                
                // Collect keys to remove first
                for (auto kv : statesRoot) {
                    String scriptIdKey = kv.key().c_str();
                    if (validHumanIds.find(scriptIdKey) == validHumanIds.end()) {
                        keysToRemove.push_back(scriptIdKey);
                    }
                }

            if (!keysToRemove.empty()) {
                statesModified = true;
                log_i("Removing states for %d orphaned script IDs:", keysToRemove.size());
                for (const String& key : keysToRemove) {
                    log_i("  - Removing state for: %s", key.c_str());
                    statesRoot.remove(key);
                }
                
                statesFile = SPIFFS.open(SCRIPT_STATES_PATH, FILE_WRITE);
                if (!statesFile) {
                    log_e("Failed to open %s for writing cleaned states.", SCRIPT_STATES_PATH);
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
        xSemaphoreGive(_spiffsMutex);
    } else {
         log_e("ScriptManager::cleanupOrphanedStates failed to take mutex.");
    }
}

void ScriptManager::cleanupOrphanedContent(const JsonArrayConst& validScriptList) {
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        log_i("Cleaning up orphaned script content files...");
        std::set<String> validFileIds;
        for (JsonVariantConst item : validScriptList) {
            if (!item.is<JsonObjectConst>()) {
                log_w("Cleanup Content: Item in validScriptList is not an object, skipping.");
                continue;
            }
            JsonObjectConst item_obj = item.as<JsonObjectConst>();
            const char* fileId = item_obj["fileId"].as<const char*>();
            if (fileId) validFileIds.insert(String(fileId));
        }

        File root = SPIFFS.open(CONTENT_DIR_PATH);
        if (root && root.isDirectory()) {
            File entry = root.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String entryName = entry.name(); // This should be just the filename e.g. "s0", "s1"
                    // Construct fileId from entryName. If entryName includes path, extract filename part.
                    String fileIdFromPath = entryName.substring(entryName.lastIndexOf('/') + 1);

                    if (validFileIds.find(fileIdFromPath) == validFileIds.end()) {
                        String fullPathToRemove = String(CONTENT_DIR_PATH) + "/" + fileIdFromPath;
                        log_i("Removing orphaned script content: %s (fileId: %s)", fullPathToRemove.c_str(), fileIdFromPath.c_str());
                        if (!SPIFFS.remove(fullPathToRemove.c_str())) {
                            log_e("Failed to remove %s", fullPathToRemove.c_str());
                        }
                    }
                }
                entry.close();
                entry = root.openNextFile();
            }
            root.close();
        } else {
            if(root) root.close();
            log_w("Could not open %s for cleanup or not a directory.", CONTENT_DIR_PATH);
        }
        xSemaphoreGive(_spiffsMutex);
    } else {
        log_e("ScriptManager::cleanupOrphanedContent failed to take mutex.");
    }
}
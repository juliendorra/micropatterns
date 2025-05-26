#include "script_manager.h"
#include "esp32-hal-log.h"
#include <set>
#include <vector>
#include <map>            // Added for std::map
#include "esp_task_wdt.h" // Added for esp_task_wdt_reset
#include <sys/stat.h>     // Added for struct stat and stat()

const char *ScriptManager::LIST_JSON_PATH = "/scripts/list.json";
const char *ScriptManager::CONTENT_DIR_PATH = "/scripts/content";
const char *ScriptManager::CURRENT_SCRIPT_ID_PATH = "/current_script.id";
const char *ScriptManager::SCRIPT_STATES_PATH = "/scripts/script_states.json";

// Default script content with clear visual indication it's the fallback script
const char *ScriptManager::DEFAULT_SCRIPT_CONTENT = R"(
DEFINE PATTERN NAME="artdeco" WIDTH=20 HEIGHT=20 DATA="0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000001000000000000010000001000000000001000000001000000000100000000001000000010000000000001000001000000000000001000000000000010000000000000001000010000000000000100000010000000000010000000010000000001000000000010000000100000000000010000010000000000000000000000000000000000000000000000"

VAR $center_x
VAR $center_y
VAR $secondplus
VAR $rotation
VAR $size

# fill background
COLOR NAME=BLACK
FILL NAME=SOLID
FILL_RECT WIDTH=$WIDTH HEIGHT=$HEIGHT X=0 Y=0

LET $center_x = $WIDTH / 2
LET $center_y = $HEIGHT / 2

TRANSLATE DX=$center_x DY=$center_y

LET $secondplus = 3 + $SECOND * $counter % 15
LET $rotation = 360 * 89 / $secondplus
ROTATE DEGREES=$rotation

LET $size = $width / 40

FILL NAME="artdeco"
COLOR NAME=BLACK

REPEAT COUNT=$secondplus

ROTATE DEGREES=$rotation

VAR $radius = $INDEX * 10 % 50
VAR $Xposition= 0
VAR $Yposition= $INDEX

FILL_CIRCLE RADIUS=$INDEX X=$Xposition Y=$Yposition

IF $INDEX % 2 == 0 THEN
COLOR NAME=WHITE
SCALE FACTOR=$size
ELSE
COLOR NAME=BLACK
SCALE FACTOR=$size
ENDIF

DRAW name="artdeco" x=$Xposition y=$Yposition

ENDREPEAT
)";
const char *ScriptManager::DEFAULT_SCRIPT_ID = "default_fallback_script";

ScriptManager::ScriptManager()
{
    _spiffsMutex = xSemaphoreCreateMutex();
    if (_spiffsMutex == NULL)
    {
        log_e("ScriptManager: Failed to create LittleFS mutex!");
    }
}

ScriptManager::~ScriptManager()
{
    if (_spiffsMutex != NULL)
    {
        vSemaphoreDelete(_spiffsMutex);
    }
}

// Generate a short fileId for storage purposes (format: "s" + number)
// This is the private _nolock version, assumes mutex is held.
String ScriptManager::_generateShortFileId_nolock(const String &humanId)
{
    // If _nextFileIdCounter isn't initialized, determine the next available ID number
    if (_nextFileIdCounter == 0)
    {
        _nextFileIdCounter = _getHighestFileIdNumber_nolock() + 1;
    }

    // Generate the new short fileId
    String shortId = "s" + String(_nextFileIdCounter++);
    log_i("_generateShortFileId_nolock: Generated new short fileId '%s' for humanId '%s'", shortId.c_str(), humanId.c_str());
    return shortId;
}

// Public version of generateShortFileId, handles mutex.
String ScriptManager::generateShortFileId(const String &humanId)
{
    String shortId = "";
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        shortId = _generateShortFileId_nolock(humanId);
        xSemaphoreGive(_spiffsMutex);
    }
    else
    {
        log_e("generateShortFileId: Failed to take mutex for humanId %s", humanId.c_str());
        // Fallback or error handling - returning empty or a default might be options
        // For now, returns empty, caller should check.
    }
    return shortId;
}

// Analyze existing fileIds to find the highest current number
// Assumes _spiffsMutex is already held by the caller.
int ScriptManager::_getHighestFileIdNumber_nolock()
{
    int highest = -1; // Initialize to -1. If no 's' files, next is s0 (or s1 if counter starts at 1).

    if (LittleFS.exists(CONTENT_DIR_PATH))
    {
        File root = LittleFS.open(CONTENT_DIR_PATH);
        if (root && root.isDirectory())
        {
            File entry = root.openNextFile();
            while (entry)
            {
                if (!entry.isDirectory())
                {
                    String filename = entry.name();
                    // Extract just the filename part (e.g., "s123" from "/scripts/content/s123")
                    int lastSlash = filename.lastIndexOf('/');
                    if (lastSlash >= 0) {
                        filename = filename.substring(lastSlash + 1);
                    }

                    if (filename.startsWith("s") && filename.length() > 1)
                    {
                        String numPart = filename.substring(1);
                        // Safely convert to int; toInt() returns 0 on failure/non-numeric.
                        long numVal = strtol(numPart.c_str(), NULL, 10);
                        if (numVal > highest)
                        {
                            highest = numVal;
                        }
                    }
                }
                entry.close();
                entry = root.openNextFile();
            }
            root.close();
        }
        else
        {
            if (root) root.close(); // Close if opened but not a directory
            log_w("_getHighestFileIdNumber_nolock: %s is not a directory or failed to open.", CONTENT_DIR_PATH);
        }
    }
    else
    {
        log_w("_getHighestFileIdNumber_nolock: Content directory %s does not exist.", CONTENT_DIR_PATH);
    }

    log_i("_getHighestFileIdNumber_nolock: Highest existing fileId number: %d", highest);
    return highest;
}

// Internal helper: Assumes _spiffsMutex is already held.
void ScriptManager::ensureUniqueFileIds_nolock(JsonDocument &listDoc)
{
    if (!listDoc.is<JsonArray>())
    {
        log_e("ensureUniqueFileIds_nolock: Document is not a JSON array");
        return;
    }

    JsonArray scriptList = listDoc.as<JsonArray>();
    std::map<String, bool> usedFileIds; // Tracks fileIds used within this list processing
    std::map<String, String> contentFileIdMap; // Maps humanId to fileId with existing content
    bool listModified = false;

    // First, scan content directory to identify which fileIds have actual content files
    if (LittleFS.exists(CONTENT_DIR_PATH))
    {
        File root = LittleFS.open(CONTENT_DIR_PATH);
        if (root && root.isDirectory())
        {
            File entry = root.openNextFile();
            while (entry)
            {
                if (!entry.isDirectory())
                {
                    String filename = entry.name();
                    // Extract just the filename part
                    int lastSlash = filename.lastIndexOf('/');
                    if (lastSlash >= 0)
                    {
                        filename = filename.substring(lastSlash + 1);
                    }

                    // Track content files with 's' pattern (potential fileIds)
                    if (filename.startsWith("s") && filename.length() > 1)
                    {
                        log_d("ensureUniqueFileIds_nolock: Found content file: %s", filename.c_str());
                    }
                }
                entry.close();
                entry = root.openNextFile();
            }
            root.close();
        }
    }

    // First pass: Find humanId-to-fileId mappings that already have content files
    for (JsonObject scriptInfo : scriptList)
    {
        if (!scriptInfo["id"].is<const char*>() || !scriptInfo["fileId"].is<const char*>())
        {
            continue; // Skip entries without valid ID fields
        }

        String humanId = scriptInfo["id"].as<String>();
        String fileId = scriptInfo["fileId"].as<String>();

        if (!fileId.isEmpty() && fileId != "null" && fileId.startsWith("s"))
        {
            // Check if this fileId has corresponding content file
            String contentPath = String(CONTENT_DIR_PATH) + "/" + fileId;
            if (LittleFS.exists(contentPath))
            {
                log_i("ensureUniqueFileIds_nolock: Script '%s' has existing content file with fileId '%s'",
                     humanId.c_str(), fileId.c_str());
                contentFileIdMap[humanId] = fileId;
                usedFileIds[fileId] = true;
            }
        }
    }

    // Reset _nextFileIdCounter based on current highest known ID before generating new ones
    // Call the _nolock version as we already hold the mutex.
    _nextFileIdCounter = _getHighestFileIdNumber_nolock() + 1;

    // Second pass: Process each script while preserving existing fileIds that have content
    for (JsonObject scriptInfo : scriptList)
    {
        if (!scriptInfo["id"].is<const char*>())
        {
            log_w("ensureUniqueFileIds_nolock: Script missing valid 'id' field");
            continue;
        }

        String humanId = scriptInfo["id"].as<String>();
        String fileId;

        // If this humanId has a fileId with content, use that fileId
        if (contentFileIdMap.find(humanId) != contentFileIdMap.end())
        {
            fileId = contentFileIdMap[humanId];
            
            // Update if necessary (shouldn't be, but just in case)
            if (!scriptInfo["fileId"].is<const char*>() ||
                scriptInfo["fileId"].as<String>() != fileId)
            {
                scriptInfo["fileId"] = fileId;
                listModified = true;
                log_i("ensureUniqueFileIds_nolock: Restored content-matched fileId '%s' for '%s'",
                     fileId.c_str(), humanId.c_str());
            }
        }
        else
        {
            // No existing content file for this humanId, check if it has a valid fileId
            if (scriptInfo["fileId"].is<const char*>())
            {
                fileId = scriptInfo["fileId"].as<String>();
                if (fileId.isEmpty() || fileId == "null" || !fileId.startsWith("s"))
                {
                    // Invalid fileId, generate a new one using _nolock version
                    fileId = _generateShortFileId_nolock(humanId);
                    scriptInfo["fileId"] = fileId;
                    listModified = true;
                    log_i("ensureUniqueFileIds_nolock: Replaced invalid fileId for '%s' with '%s'",
                         humanId.c_str(), fileId.c_str());
                }
            }
            else
            {
                // Missing fileId, generate a new one using _nolock version
                fileId = _generateShortFileId_nolock(humanId);
                scriptInfo["fileId"] = fileId;
                listModified = true;
                log_i("ensureUniqueFileIds_nolock: Added missing fileId '%s' for '%s'",
                     fileId.c_str(), humanId.c_str());
            }

            // Check for duplicates within current list processing
            // (only for scripts that don't have existing content files)
            while (usedFileIds.count(fileId) && usedFileIds[fileId] == true)
            {
                log_w("ensureUniqueFileIds_nolock: fileId '%s' for '%s' is a duplicate. Regenerating.",
                     fileId.c_str(), humanId.c_str());
                fileId = _generateShortFileId_nolock(humanId); // Use _nolock version
                scriptInfo["fileId"] = fileId;
                listModified = true;
            }
        }
        
        usedFileIds[fileId] = true; // Mark this fileId as used
    }

    if (listModified)
    {
        log_i("ensureUniqueFileIds_nolock: Saving updated script list with proper fileIds");
        bool saveSuccess = saveScriptList_nolock(listDoc);
        log_i("ensureUniqueFileIds_nolock: Save result: %s", saveSuccess ? "success" : "failed");
    }
}


bool ScriptManager::initialize()
{
    if (xSemaphoreTake(_spiffsMutex, portMAX_DELAY) == pdTRUE)
    {
        bool success = initializeLittleFS(); // Changed initializeSPIFFS to initializeLittleFS
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("ScriptManager::initialize failed to take mutex.");
    return false;
}

bool ScriptManager::initializeLittleFS() // Renamed from initializeSPIFFS
{
    if (!LittleFS.begin(true)) // Changed SPIFFS to LittleFS
    { // `true` = format LittleFS if mount fails
        log_e("LittleFS Mount Failed even after formatting attempt!"); // Changed SPIFFS to LittleFS
        return false;
    }
    log_i("LittleFS Mounted successfully."); // Changed SPIFFS to LittleFS

    // Create directories if they don't exist
    if (!LittleFS.exists("/scripts")) // Changed SPIFFS to LittleFS
    {
        log_i("Directory /scripts does not exist. Creating...");
        if (!LittleFS.mkdir("/scripts")) // Changed SPIFFS to LittleFS
        {
            log_e("Failed to create /scripts directory!");
            return false;
        }
        log_i("Successfully created /scripts directory");
    } else {
        log_i("Directory /scripts already exists.");
    }

    if (!LittleFS.exists(CONTENT_DIR_PATH)) // Changed SPIFFS to LittleFS
    {
        log_i("Directory %s does not exist. Creating...", CONTENT_DIR_PATH);
        if (!LittleFS.mkdir(CONTENT_DIR_PATH)) // Changed SPIFFS to LittleFS
        {
            log_e("Failed to create %s directory!", CONTENT_DIR_PATH);
            return false;
        }
        log_i("Successfully created %s directory", CONTENT_DIR_PATH);
    } else {
        log_i("Directory %s already exists.", CONTENT_DIR_PATH);
    }
    return true;
}

// Public version of saveScriptList
bool ScriptManager::saveScriptList(JsonDocument &listDoc)
{
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        // Before saving, ensure fileIds are unique and correct
        ensureUniqueFileIds_nolock(listDoc); // This is _nolock, mutex is held

        bool success = saveScriptList_nolock(listDoc);
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("saveScriptList: Failed to take mutex after 1000ms");
    return false;
}

// Implementation is now using JsonDocument instead of DynamicJsonDocument

// Internal helper: Assumes _spiffsMutex is already held.
bool ScriptManager::saveScriptList_nolock(JsonDocument &listDoc)
{
    // Validations from the original public saveScriptList
    if (listDoc.isNull()) {
        log_e("saveScriptList_nolock: Document is null/empty");
        return false;
    }
    if (!listDoc.is<JsonArray>()) {
        log_e("saveScriptList_nolock: Document is not a JSON array. Type: %s", listDoc.is<JsonObject>() ? "object" : "unknown");
        return false;
    }
    size_t arraySize = listDoc.as<JsonArray>().size();
    if (arraySize == 0) {
        log_w("saveScriptList_nolock: Saving an empty array");
    }

    // Ensure directory exists
    if (!LittleFS.exists("/scripts")) { // Changed SPIFFS to LittleFS
        log_w("saveScriptList_nolock: /scripts directory does not exist, creating...");
        if (!LittleFS.mkdir("/scripts")) { // Changed SPIFFS to LittleFS
            log_e("saveScriptList_nolock: Failed to create /scripts directory");
            return false;
        }
    }

    File file = LittleFS.open(LIST_JSON_PATH, FILE_WRITE); // Changed SPIFFS to LittleFS
    if (!file) {
        log_e("saveScriptList_nolock: Failed to open %s for writing. LittleFS.open() returned null", LIST_JSON_PATH); // Changed SPIFFS to LittleFS
        return false;
    }

    bool success = false;
    log_d("saveScriptList_nolock: Beginning serialization of %u elements to %s", arraySize, LIST_JSON_PATH);
    esp_task_wdt_reset(); // Reset watchdog before serialization

    size_t bytesWritten = serializeJson(listDoc, file);
    log_i("saveScriptList_nolock: serializeJson wrote %u bytes.", bytesWritten);

    if (bytesWritten > 0) {
        log_i("saveScriptList_nolock: Saved list with %u entries (%u bytes) to %s",
              arraySize, bytesWritten, LIST_JSON_PATH);
        success = true;
    } else {
        log_e("saveScriptList_nolock: Failed to write to %s (serializeJson returned 0)", LIST_JSON_PATH);
    }
    file.close();
    return success;
}

// _nolock version of loadScriptList
bool ScriptManager::loadScriptList_nolock(JsonDocument &outListDoc)
{
    // Clear the output document first to ensure we start fresh
    outListDoc.clear();
    log_i("loadScriptList_nolock: Attempting to load script list from %s", LIST_JSON_PATH);

    if (!LittleFS.exists(LIST_JSON_PATH)) // Changed SPIFFS to LittleFS
    {
        log_w("loadScriptList_nolock: Script list file %s does not exist", LIST_JSON_PATH);
        return false;
    }

    log_d("loadScriptList_nolock: LittleFS space - Total: %u bytes, Used: %u bytes, Free: %u bytes", // Changed SPIFFS to LittleFS
          LittleFS.totalBytes(), LittleFS.usedBytes(), LittleFS.totalBytes() - LittleFS.usedBytes()); // Changed SPIFFS to LittleFS

    File file = LittleFS.open(LIST_JSON_PATH, FILE_READ); // Changed SPIFFS to LittleFS
    if (!file)
    {
        log_e("loadScriptList_nolock: Failed to open %s (file pointer is null)", LIST_JSON_PATH);
        return false;
    }
    if (file.isDirectory())
    {
        log_e("loadScriptList_nolock: %s is a directory, not a file", LIST_JSON_PATH);
        file.close();
        return false;
    }
    size_t fileSize = file.size();
    if (fileSize == 0)
    {
        log_w("loadScriptList_nolock: File %s exists but is empty (0 bytes)", LIST_JSON_PATH);
        file.close();
        return false;
    }
    log_d("loadScriptList_nolock: File %s opened, size: %u bytes", LIST_JSON_PATH, fileSize);
    esp_task_wdt_reset();
    DeserializationError error = deserializeJson(outListDoc, file);
    file.close();

    if (error)
    {
        log_e("loadScriptList_nolock: JSON parsing error: %s", error.c_str());
        if (fileSize < 200)
        {
            File readFile = LittleFS.open(LIST_JSON_PATH, FILE_READ); // Changed SPIFFS to LittleFS
            if (readFile)
            {
                String content = readFile.readString();
                log_e("loadScriptList_nolock: File content: %s", content.c_str());
                readFile.close();
            }
        }
        return false;
    }
    if (!outListDoc.is<JsonArray>())
    {
        log_e("loadScriptList_nolock: Parsed JSON is not an array. Type: %s",
              outListDoc.is<JsonObject>() ? "object" : "unknown");
        return false;
    }
    size_t numEntries = outListDoc.as<JsonArray>().size();
    log_i("loadScriptList_nolock: Successfully loaded script list with %u entries", numEntries);
    ensureUniqueFileIds_nolock(outListDoc); // This is already _nolock
    return true;
}


bool ScriptManager::loadScriptContent(const String &fileId, String &outContent)
{
// Reset output content first
outContent = "";

if (fileId.isEmpty()) { // Check before taking mutex
    log_e("loadScriptContent: fileId is empty");
    return false;
}

if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
{
    bool success = loadScriptContent_nolock(fileId, outContent); // Call the _nolock version
    xSemaphoreGive(_spiffsMutex);
    return success;
}
log_e("loadScriptContent: Failed to take mutex for fileId %s after 1000ms", fileId.c_str());
return false; // outContent is already ""
}

// _nolock version of loadScriptContent
bool ScriptManager::loadScriptContent_nolock(const String &fileId, String &outContent) {
    outContent = ""; // Reset output

    if (fileId.isEmpty()) {
        log_e("loadScriptContent_nolock: fileId is empty");
        return false;
    }

    if (fileId == DEFAULT_SCRIPT_ID) {
        log_i("loadScriptContent_nolock: Using built-in DEFAULT_SCRIPT_CONTENT for '%s'", fileId.c_str());
        outContent = DEFAULT_SCRIPT_CONTENT;
        return true;
    }

    String actualFileId = fileId;
    JsonDocument listDoc; // Use default allocator

    if (!fileId.startsWith("s")) { // If fileId is likely a humanId
        if (loadScriptList_nolock(listDoc)) { // Use _nolock
            bool idFound = false;
            for (JsonObject scriptInfo : listDoc.as<JsonArray>()) {
                if (scriptInfo["id"].as<String>() == fileId) {
                    if (!scriptInfo["fileId"].isNull() && scriptInfo["fileId"].is<const char*>()) {
                        String storedFileId = scriptInfo["fileId"].as<String>();
                        if (!storedFileId.isEmpty() && storedFileId != "null" && storedFileId.startsWith("s")) {
                            actualFileId = storedFileId;
                            idFound = true;
                            log_i("loadScriptContent_nolock: Mapped humanId '%s' to fileId '%s'", fileId.c_str(), actualFileId.c_str());
                            break;
                        }
                    }
                }
            }
            if (!idFound) log_w("loadScriptContent_nolock: Could not map humanId '%s' to a short fileId from list.", fileId.c_str());
        } else {
            log_w("loadScriptContent_nolock: Failed to load script list for humanId to fileId mapping.");
        }
    }

    String path = String(CONTENT_DIR_PATH) + "/" + actualFileId;
    log_i("loadScriptContent_nolock: Attempting to load script content from %s", path.c_str());

    if (!LittleFS.exists(path.c_str())) { // Changed SPIFFS to LittleFS
        log_w("loadScriptContent_nolock: Path does not exist: %s (for actualFileId: %s, original fileId: %s)", path.c_str(), actualFileId.c_str(), fileId.c_str());
        // Recovery attempt
        bool recovered = false;
        String humanIdForRecovery = (!actualFileId.startsWith("s")) ? actualFileId : "";
        if (actualFileId.startsWith("s") && humanIdForRecovery.isEmpty()) { // If actualFileId was 's'-style, find its humanId
            // listDoc might already be loaded if we came from the humanId mapping path.
            // If not, load it again.
            if (listDoc.isNull() || !listDoc.is<JsonArray>()) { // Check if listDoc is already populated
                 loadScriptList_nolock(listDoc); // Use _nolock
            }
            if (listDoc.is<JsonArray>()) {
                for (JsonObjectConst scriptItem : listDoc.as<JsonArray>()) {
                    if (!scriptItem["fileId"].isNull() && scriptItem["fileId"].as<String>() == actualFileId) {
                        if(!scriptItem["id"].isNull()) humanIdForRecovery = scriptItem["id"].as<String>();
                        break;
                    }
                }
            }
        }

        if (humanIdForRecovery.isEmpty()) {
            log_w("loadScriptContent_nolock: Could not determine humanId for actualFileId '%s'. Cannot recover.", actualFileId.c_str());
        } else {
            log_i("loadScriptContent_nolock: Initial path failed. Attempting recovery for humanId '%s'.", humanIdForRecovery.c_str());
            // listDoc should be loaded here if it wasn't before
            if (listDoc.isNull() || !listDoc.is<JsonArray>()) {
                 loadScriptList_nolock(listDoc); // Use _nolock
            }
            if (listDoc.is<JsonArray>()) {
                for (JsonObjectConst scriptItemConst : listDoc.as<JsonArray>()) {
                    if (!scriptItemConst["id"].isNull() && scriptItemConst["id"].as<String>() == humanIdForRecovery) {
                        if (!scriptItemConst["fileId"].isNull()) {
                            String fileIdFromList = scriptItemConst["fileId"].as<String>();
                            if (!fileIdFromList.isEmpty() && fileIdFromList.startsWith("s")) {
                                String pathFromList = String(CONTENT_DIR_PATH) + "/" + fileIdFromList;
                                log_i("loadScriptContent_nolock: For humanId '%s', list.json points to fileId '%s'. Checking path: %s", humanIdForRecovery.c_str(), fileIdFromList.c_str(), pathFromList.c_str());
                                if (LittleFS.exists(pathFromList.c_str())) { // Changed SPIFFS to LittleFS
                                    File recoveryFile = LittleFS.open(pathFromList.c_str(), FILE_READ); // Changed SPIFFS to LittleFS
                                    if (recoveryFile && !recoveryFile.isDirectory() && recoveryFile.size() > 0) {
                                        outContent = recoveryFile.readString();
                                        log_i("loadScriptContent_nolock: Recovery successful. Loaded %u bytes for humanId '%s' from '%s'.", outContent.length(), humanIdForRecovery.c_str(), pathFromList.c_str());
                                        recovered = true;
                                    } else { log_w("loadScriptContent_nolock: Recovery file '%s' exists but is empty/unreadable.", pathFromList.c_str()); }
                                    if (recoveryFile) recoveryFile.close();
                                } else { log_w("loadScriptContent_nolock: Path '%s' (from list.json for humanId '%s') also does not exist.", pathFromList.c_str(), humanIdForRecovery.c_str()); }
                            } else { log_w("loadScriptContent_nolock: fileId for humanId '%s' in list.json is invalid: '%s'", humanIdForRecovery.c_str(), fileIdFromList.c_str()); }
                        } else { log_w("loadScriptContent_nolock: humanId '%s' in list.json has no fileId field.", humanIdForRecovery.c_str()); }
                        break;
                    }
                }
            } else { log_w("loadScriptContent_nolock: Failed to load list.json for recovery attempt."); }
        }
        if (!recovered) {
            log_w("loadScriptContent_nolock: Recovery failed for actualFileId '%s'.", actualFileId.c_str());
            return false;
        }
        return true; // Recovered content
    }

    log_d("loadScriptContent_nolock: Opening file %s for reading", path.c_str());
    File file = LittleFS.open(path.c_str(), FILE_READ); // Changed SPIFFS to LittleFS
    if (!file) { log_e("loadScriptContent_nolock: Failed to open file (null pointer returned)"); return false; }
    if (file.isDirectory()) { log_e("loadScriptContent_nolock: Path is a directory, not a file"); file.close(); return false; }
    size_t fileSize = file.size();
    if (fileSize == 0) { log_w("loadScriptContent_nolock: File is empty (0 bytes)"); file.close(); return false; }
    log_d("loadScriptContent_nolock: File opened successfully, size: %u bytes", fileSize);
    esp_task_wdt_reset();
    if (fileSize > 5000) log_w("loadScriptContent_nolock: Large file detected (%u bytes)", fileSize);
    outContent = file.readString();
    size_t readLength = outContent.length();
    file.close();
    if (readLength == 0) { log_e("loadScriptContent_nolock: readString() returned empty string despite file size %u", fileSize); return false; }
    if (readLength != fileSize) log_w("loadScriptContent_nolock: Read %u bytes but file size is %u bytes", readLength, fileSize);
    log_i("loadScriptContent_nolock: Successfully loaded %u bytes for fileId '%s'", readLength, actualFileId.c_str()); // Use actualFileId for logging
    return true;
}

// _nolock version of saveScriptContent
bool ScriptManager::saveScriptContent_nolock(const String &fileId, const String &content) {
    if (fileId.isEmpty()) {
        log_e("saveScriptContent_nolock: fileId is empty");
        return false;
    }
    if (content.isEmpty()) {
        log_e("saveScriptContent_nolock: content is empty for fileId '%s'", fileId.c_str());
        return false;
    }

    String actualFileId = fileId;
    JsonDocument listDoc; // Use default allocator

    if (!fileId.startsWith("s") && fileId != DEFAULT_SCRIPT_ID) {
        if (loadScriptList_nolock(listDoc)) { // Use _nolock
            bool idFound = false;
            for (JsonObject scriptInfo : listDoc.as<JsonArray>()) {
                if (scriptInfo["id"].as<String>() == fileId) {
                    if (!scriptInfo["fileId"].isNull() && scriptInfo["fileId"].is<const char*>()) {
                        String storedFileId = scriptInfo["fileId"].as<String>();
                        if (!storedFileId.isEmpty() && storedFileId != "null" && storedFileId.startsWith("s")) {
                            actualFileId = storedFileId;
                            idFound = true;
                            log_i("saveScriptContent_nolock: Mapped humanId '%s' to fileId '%s'", fileId.c_str(), actualFileId.c_str());
                            break;
                        }
                    }
                }
            }
            if (!idFound) {
                actualFileId = _generateShortFileId_nolock(fileId); // Use _nolock
                log_w("saveScriptContent_nolock: Generated new fileId '%s' for humanId '%s'", actualFileId.c_str(), fileId.c_str());
                // Note: The listDoc is not updated here with the new fileId. This should be handled by ensureUniqueFileIds or similar.
            }
        } else {
             log_w("saveScriptContent_nolock: Failed to load script list for humanId to fileId mapping. Generating new fileId.");
             actualFileId = _generateShortFileId_nolock(fileId); // Use _nolock
        }
    }

    String path = String(CONTENT_DIR_PATH) + "/" + actualFileId;
    log_i("saveScriptContent_nolock: Attempting to save %u bytes to %s", content.length(), path.c_str());

    if (content.length() > 10000) {
        log_w("saveScriptContent_nolock: Large content detected (%u bytes)", content.length());
    }

    if (!LittleFS.exists(CONTENT_DIR_PATH)) { // Changed SPIFFS to LittleFS
        log_w("saveScriptContent_nolock: Directory %s does not exist, creating...", CONTENT_DIR_PATH);
        if (!LittleFS.mkdir(CONTENT_DIR_PATH)) { // Changed SPIFFS to LittleFS
            log_e("saveScriptContent_nolock: Failed to create directory %s", CONTENT_DIR_PATH);
            return false;
        }
    }

    size_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes(); // Changed SPIFFS to LittleFS
    if (content.length() + 100 > freeSpace) {
        log_e("saveScriptContent_nolock: Not enough space. Content: %u bytes, Free: %u bytes", content.length(), freeSpace);
        return false;
    }

    if (LittleFS.exists(path.c_str())) { // Changed SPIFFS to LittleFS
        log_d("saveScriptContent_nolock: Deleting existing file before writing new content");
        LittleFS.remove(path.c_str()); // Changed SPIFFS to LittleFS
    }

    log_d("saveScriptContent_nolock: Opening file for writing");
    File file = LittleFS.open(path.c_str(), FILE_WRITE); // Changed SPIFFS to LittleFS
    if (!file) {
        log_e("saveScriptContent_nolock: Failed to open %s for writing", path.c_str());
        return false;
    }

    esp_task_wdt_reset();
    size_t bytesWritten = file.print(content);
    esp_task_wdt_reset();
    bool success = (bytesWritten == content.length());

    if (success) log_i("saveScriptContent_nolock: Successfully wrote %u bytes to file", bytesWritten);
    else log_e("saveScriptContent_nolock: Write incomplete. Wrote %u of %u bytes", bytesWritten, content.length());
    file.close();

    if (success) { // Verification
        File verifyFile = LittleFS.open(path.c_str(), FILE_READ); // Changed SPIFFS to LittleFS
        if (verifyFile) {
            if (verifyFile.size() != content.length()) log_w("saveScriptContent_nolock: Verification size mismatch");
            verifyFile.close();
        } else { log_e("saveScriptContent_nolock: Failed to open file for verification"); }
    }
    return success;
}

bool ScriptManager::saveScriptContent(const String &fileId, const String &content)
{
    if (fileId.isEmpty()) { // Check before taking mutex
        log_e("saveScriptContent: fileId is empty");
        return false;
    }
    if (content.isEmpty()) { // Check before taking mutex
        log_e("saveScriptContent: content is empty for fileId '%s'", fileId.c_str());
        return false;
    }

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        bool success_local = saveScriptContent_nolock(fileId, content);
        xSemaphoreGive(_spiffsMutex);
        return success_local;
    }
    log_e("saveScriptContent: Failed to take mutex for fileId %s after 1000ms", fileId.c_str());
    return false;
}

bool ScriptManager::loadScriptExecutionState(const String &humanId, ScriptExecState &outState)
{
    if (humanId.isEmpty())
    {
        log_e("loadScriptExecutionState: humanId is null or empty.");
        outState.state_loaded = false;
        return false;
    }
    outState = ScriptExecState(); // Reset to default

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        File file = LittleFS.open(SCRIPT_STATES_PATH, FILE_READ); // Changed SPIFFS to LittleFS
        if (!file || file.isDirectory() || file.size() == 0)
        {
            if (file)
                file.close();
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            return false;
        }
    
        JsonDocument statesDoc; // Use default allocator
        DeserializationError error = deserializeJson(statesDoc, file);
        file.close();

        if (error)
        {
            log_e("loadScriptExecutionState: Failed to parse %s: %s.", SCRIPT_STATES_PATH, error.c_str());
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            return false;
        }
        if (!statesDoc.is<JsonObject>())
        {
            log_e("loadScriptExecutionState: %s content is not a JSON object.", SCRIPT_STATES_PATH);
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            return false;
        }

        JsonObject root = statesDoc.as<JsonObject>();

        JsonVariantConst scriptVariant = root[humanId.c_str()];

        if (scriptVariant.isNull())
        { // Check if the key exists and is not null
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            // log_d("Script ID '%s' not found in states file.", humanId.c_str());
            return false; // Script ID not found in the states object
        }

        // Ensure the value for humanId is an object
        if (!scriptVariant.is<JsonObjectConst>())
        { // Check if it can be viewed as JsonObjectConst
            log_e("loadScriptExecutionState: State for script ID '%s' is not a JSON object.", humanId.c_str());
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            return false;
        }

        JsonObjectConst scriptStateObj = scriptVariant.as<JsonObjectConst>(); // Use JsonObjectConst for reading
        if (!scriptStateObj["counter"].is<int>() || !scriptStateObj["hour"].is<int>() ||
            !scriptStateObj["minute"].is<int>() || !scriptStateObj["second"].is<int>())
        {
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

bool ScriptManager::loadScriptList(JsonDocument &outListDoc)
{
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        bool success = loadScriptList_nolock(outListDoc);
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("loadScriptList: Failed to take mutex after 1000ms");
    return false;
}

// _nolock version of getCurrentScriptId
bool ScriptManager::getCurrentScriptId_nolock(String &outHumanId)
{
    File file = LittleFS.open(CURRENT_SCRIPT_ID_PATH, FILE_READ); // Changed SPIFFS to LittleFS
    if (!file || file.isDirectory())
    {
        log_w("getCurrentScriptId_nolock: Failed to open %s for reading or it's a directory.", CURRENT_SCRIPT_ID_PATH);
        if (file) file.close();
        outHumanId = "";
        return false;
    }
    outHumanId = file.readString();
    file.close();

    if (outHumanId.length() > 0)
    {
        log_i("getCurrentScriptId_nolock: Current script humanId '%s' loaded from %s.", outHumanId.c_str(), CURRENT_SCRIPT_ID_PATH);
        return true;
    }
    else
    {
        log_w("getCurrentScriptId_nolock: %s is empty or read failed.", CURRENT_SCRIPT_ID_PATH);
        return false;
    }
}

bool ScriptManager::getCurrentScriptId(String &outHumanId)
{
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        bool success = getCurrentScriptId_nolock(outHumanId);
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("ScriptManager::getCurrentScriptId failed to take mutex.");
    return false;
}

// _nolock version of saveCurrentScriptId
bool ScriptManager::saveCurrentScriptId_nolock(const String &humanId)
{
    if (humanId.isEmpty())
    {
        log_e("saveCurrentScriptId_nolock: humanId is empty.");
        return false;
    }
    File file = LittleFS.open(CURRENT_SCRIPT_ID_PATH, FILE_WRITE); // Changed SPIFFS to LittleFS
    if (!file)
    {
        log_e("saveCurrentScriptId_nolock: Failed to open %s for writing", CURRENT_SCRIPT_ID_PATH);
        return false;
    }
    bool success = false;
    if (file.print(humanId))
    {
        log_i("saveCurrentScriptId_nolock: Current script humanId '%s' saved to %s.", humanId.c_str(), CURRENT_SCRIPT_ID_PATH);
        success = true;
    }
    else
    {
        log_e("saveCurrentScriptId_nolock: Failed to write current script humanId '%s' to %s.", humanId.c_str(), CURRENT_SCRIPT_ID_PATH);
    }
    file.close();
    return success;
}

bool ScriptManager::saveCurrentScriptId(const String &humanId)
{
    if (humanId.isEmpty()) { // Check before taking mutex
        log_e("saveCurrentScriptId: humanId is empty.");
        return false;
    }
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        bool success = saveCurrentScriptId_nolock(humanId);
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("ScriptManager::saveCurrentScriptId failed to take mutex for humanId %s", humanId.c_str());
    return false;
}

// _nolock version of loadScriptExecutionState
bool ScriptManager::loadScriptExecutionState_nolock(const String &humanId, ScriptExecState &outState)
{
    if (humanId.isEmpty())
    {
        log_e("loadScriptExecutionState_nolock: humanId is null or empty.");
        outState.state_loaded = false;
        return false;
    }
    outState = ScriptExecState(); // Reset to default

    File file = LittleFS.open(SCRIPT_STATES_PATH, FILE_READ); // Changed SPIFFS to LittleFS
    if (!file || file.isDirectory() || file.size() == 0)
    {
        if (file) file.close();
        outState.state_loaded = false;
        return false;
    }

    JsonDocument statesDoc; // Use default allocator
    DeserializationError error = deserializeJson(statesDoc, file);
    file.close();

    if (error)
    {
        log_e("loadScriptExecutionState_nolock: Failed to parse %s: %s.", SCRIPT_STATES_PATH, error.c_str());
        outState.state_loaded = false;
        return false;
    }
    if (!statesDoc.is<JsonObject>())
    {
        log_e("loadScriptExecutionState_nolock: %s content is not a JSON object.", SCRIPT_STATES_PATH);
        outState.state_loaded = false;
        return false;
    }

    JsonObject root = statesDoc.as<JsonObject>();
    JsonVariantConst scriptVariant = root[humanId.c_str()];

    if (scriptVariant.isNull())
    {
        outState.state_loaded = false;
        return false;
    }
    if (!scriptVariant.is<JsonObjectConst>())
    {
        log_e("loadScriptExecutionState_nolock: State for script ID '%s' is not a JSON object.", humanId.c_str());
        outState.state_loaded = false;
        return false;
    }

    JsonObjectConst scriptStateObj = scriptVariant.as<JsonObjectConst>();
    if (!scriptStateObj["counter"].is<int>() || !scriptStateObj["hour"].is<int>() ||
        !scriptStateObj["minute"].is<int>() || !scriptStateObj["second"].is<int>())
    {
        log_e("loadScriptExecutionState_nolock: Incomplete state for script ID '%s' in %s.", humanId.c_str(), SCRIPT_STATES_PATH);
        outState.state_loaded = false;
        return false;
    }

    outState.counter = scriptStateObj["counter"].as<int>();
    outState.hour = scriptStateObj["hour"].as<int>();
    outState.minute = scriptStateObj["minute"].as<int>();
    outState.second = scriptStateObj["second"].as<int>();
    outState.state_loaded = true;

    log_i("loadScriptExecutionState_nolock: Script execution state loaded for ID '%s': Counter=%d, Time=%02d:%02d:%02d",
          humanId.c_str(), outState.counter, outState.hour, outState.minute, outState.second);
    return true;
}

bool ScriptManager::saveScriptExecutionState(const String &humanId, const ScriptExecState &state)
{
    // Create a local copy of humanId to ensure its stability
    String localHumanId = humanId;

    if (localHumanId.isEmpty())
    {
        log_e("saveScriptExecutionState: Attempted to save state for an empty humanId. Aborting.");
        return false;
    }

    // Redundant check, already covered above.
    // if (localHumanId.isEmpty())
    // {
    //     log_e("saveScriptExecutionState: localHumanId (from humanId) is null or empty.");
    //     return false;
    // }

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        bool success = false; // Declare success variable
        JsonDocument statesDoc; // Use default allocator

        File file = LittleFS.open(SCRIPT_STATES_PATH, FILE_READ); // Changed SPIFFS to LittleFS
        if (file && file.size() > 0 && !file.isDirectory())
        {
            DeserializationError error = deserializeJson(statesDoc, file);
            if (error)
            {
                log_w("saveScriptExecutionState: Failed to parse existing %s: %s. Will overwrite.", SCRIPT_STATES_PATH, error.c_str());
                statesDoc.clear(); // Clear to ensure it's a fresh object if parsing failed
            }
        }
        if (file) file.close(); // Ensure file is closed

        // Ensure statesDoc is an object, even if file didn't exist or was unparsable
        if (!statesDoc.is<JsonObject>()) {
            statesDoc.to<JsonObject>();
        }
        JsonObject root = statesDoc.as<JsonObject>();
        
        JsonObject scriptStateObj = root[localHumanId.c_str()].to<JsonObject>();
        scriptStateObj["counter"] = state.counter;
        scriptStateObj["hour"] = state.hour;
        scriptStateObj["minute"] = state.minute;
        scriptStateObj["second"] = state.second;

        file = LittleFS.open(SCRIPT_STATES_PATH, FILE_WRITE); // Changed SPIFFS to LittleFS
        if (!file)
        {
            log_e("saveScriptExecutionState: Failed to open %s for writing.", SCRIPT_STATES_PATH);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }
    
        // Serialize the modified statesDoc to the file
        size_t bytesWritten = serializeJson(statesDoc, file);
        if (bytesWritten == 0) {
            log_e("saveScriptExecutionState: Failed to write to %s (serializeJson returned 0).", SCRIPT_STATES_PATH);
        } else {
            log_d("saveScriptExecutionState: Wrote %u bytes to %s", bytesWritten, SCRIPT_STATES_PATH);
            success = true;
        }
        file.close(); // Close file after writing

        xSemaphoreGive(_spiffsMutex);

        if (success) {
            log_i("Script state for ID '%s' (C:%d, T:%02d:%02d:%02d) saved to %s.", localHumanId.c_str(), state.counter, state.hour, state.minute, state.second, SCRIPT_STATES_PATH);
        }
        return success;
    }
    else // This else corresponds to if (xSemaphoreTake...
    {
        log_e("ScriptManager::saveScriptExecutionState failed to take mutex for humanId %s", localHumanId.c_str());
        return false;
    }
}

bool ScriptManager::selectNextScript(bool moveUp, String &outSelectedHumanId, String &outSelectedName)
{
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) { // Increased timeout for multiple operations
        JsonDocument listDoc; // Use default allocator
        if (!loadScriptList_nolock(listDoc))
        {
            log_e("selectNextScript: Cannot select next script, failed to load script list.");
            xSemaphoreGive(_spiffsMutex);
            return false;
        }
        if (!listDoc.is<JsonArray>() || listDoc.as<JsonArray>().size() == 0)
        {
            log_w("selectNextScript: Cannot select next script, list is empty or not an array.");
            outSelectedHumanId = DEFAULT_SCRIPT_ID;
            outSelectedName = "Default";
            saveCurrentScriptId_nolock(DEFAULT_SCRIPT_ID);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        JsonArray scriptList = listDoc.as<JsonArray>();
        String currentHumanId;
        getCurrentScriptId_nolock(currentHumanId);

        int currentIndex = -1;
        for (int i = 0; i < scriptList.size(); i++)
        {
            JsonObject scriptInfo = scriptList[i];
            const char *idJson = scriptInfo["id"];
            if (idJson && currentHumanId == idJson)
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
                outSelectedHumanId = nextId;
                outSelectedName = nextName ? nextName : nextId;
                log_i("selectNextScript: Selected script index: %d, ID: %s, Name: %s", nextIndex, outSelectedHumanId.c_str(), outSelectedName.c_str());
                if (saveCurrentScriptId_nolock(outSelectedHumanId))
                {
                    xSemaphoreGive(_spiffsMutex);
                    return true;
                }
                else
                {
                    log_e("selectNextScript: Failed to save the new current script ID: %s", outSelectedHumanId.c_str());
                }
            }
            else
            {
                log_e("selectNextScript: Script at index %d has no ID.", nextIndex);
            }
        }
        // Fallback
        outSelectedHumanId = DEFAULT_SCRIPT_ID;
        outSelectedName = "Default";
        saveCurrentScriptId_nolock(DEFAULT_SCRIPT_ID);
        xSemaphoreGive(_spiffsMutex);
        return false;
    }
    log_e("selectNextScript: Failed to take mutex.");
    return false;
}

bool ScriptManager::getScriptForExecution(String &outHumanId, String &outFileId, ScriptExecState &outInitialState)
{
    log_i("getScriptForExecution: Starting script selection process");
    outHumanId = "";
    outFileId = "";
    outInitialState = ScriptExecState();

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(2000)) == pdTRUE) { // Increased timeout
        String humanIdToLoad;
        bool idFound = getCurrentScriptId_nolock(humanIdToLoad);
        log_d("getScriptForExecution: getCurrentScriptId_nolock returned: found=%s, id='%s'",
              idFound ? "true" : "false", idFound ? humanIdToLoad.c_str() : "null");

        JsonDocument listDoc; // Use default allocator
        bool listLoaded = loadScriptList_nolock(listDoc);

        if (!listLoaded) log_w("getScriptForExecution: Failed to load script list, will try to use cached current script or default");
        else log_d("getScriptForExecution: Script list loaded successfully");

        JsonArray scriptList;
        if (listLoaded && listDoc.is<JsonArray>()) {
            scriptList = listDoc.as<JsonArray>();
            log_d("getScriptForExecution: Script list contains %u items", scriptList.size());
        } else if (listLoaded) {
            log_e("getScriptForExecution: Loaded document is not a JsonArray");
        }

        String fileIdToLoadContent = "";
        if (idFound && humanIdToLoad.isEmpty()) {
            log_w("getScriptForExecution: Current script ID loaded but was empty. Will try first script or default.");
            idFound = false;
        }

        if (idFound && !humanIdToLoad.isEmpty()) {
            log_i("getScriptForExecution: Looking for script '%s' in the list", humanIdToLoad.c_str());
            bool foundInList = false;
            if (listLoaded && !scriptList.isNull() && scriptList.size() > 0) {
                int index = 0;
                for (JsonVariant item : scriptList) {
                    if (item.is<JsonObject>()) {
                        JsonObject scriptInfo = item.as<JsonObject>();
                        if (!scriptInfo["id"].isNull() && scriptInfo["id"].is<const char*>()) {
                            String scriptId = scriptInfo["id"].as<String>();
                            if (humanIdToLoad == scriptId) {
                                log_i("getScriptForExecution: Found matching script at index %d", index);
                                if (!scriptInfo["fileId"].isNull() && scriptInfo["fileId"].is<const char*>()) {
                                    fileIdToLoadContent = scriptInfo["fileId"].as<String>();
                                    if (fileIdToLoadContent.isEmpty() || fileIdToLoadContent == "null" || !fileIdToLoadContent.startsWith("s")) {
                                        log_w("getScriptForExecution: Script '%s' has invalid fileId '%s', generating new one", scriptId.c_str(), fileIdToLoadContent.c_str());
                                        fileIdToLoadContent = _generateShortFileId_nolock(scriptId);
                                        scriptInfo["fileId"] = fileIdToLoadContent;
                                        saveScriptList_nolock(listDoc); // Use _nolock
                                    }
                                } else {
                                    log_w("getScriptForExecution: Script '%s' missing 'fileId'. Generating new one.", scriptId.c_str());
                                    fileIdToLoadContent = _generateShortFileId_nolock(scriptId);
                                    scriptInfo["fileId"] = fileIdToLoadContent;
                                    saveScriptList_nolock(listDoc); // Use _nolock
                                }
                                foundInList = true;
                                break;
                            }
                        } else { log_w("getScriptForExecution: Script at index %d has no valid 'id'", index); }
                    } else { log_w("getScriptForExecution: Item at index %d not an object", index); }
                    index++;
                }
            }
            if (!foundInList) {
                log_w("getScriptForExecution: Script ID '%s' not found in list. Will try first script.", humanIdToLoad.c_str());
                idFound = false; humanIdToLoad = ""; fileIdToLoadContent = "";
            }
        }

        if (!idFound || humanIdToLoad.isEmpty() || fileIdToLoadContent.isEmpty()) {
            log_i("getScriptForExecution: Attempting to use the first script in the list");
            if (listLoaded && !scriptList.isNull() && scriptList.size() > 0) {
                JsonVariant firstItem = scriptList[0];
                if (firstItem.is<JsonObject>()) {
                    JsonObject firstScript = firstItem.as<JsonObject>();
                    if (!firstScript["id"].isNull() && firstScript["id"].is<const char*>()) {
                        humanIdToLoad = firstScript["id"].as<String>();
                        if (!firstScript["fileId"].isNull() && firstScript["fileId"].is<const char*>()) {
                            fileIdToLoadContent = firstScript["fileId"].as<String>();
                            if (fileIdToLoadContent.isEmpty() || fileIdToLoadContent == "null" || !fileIdToLoadContent.startsWith("s")) {
                                log_w("getScriptForExecution: First script '%s' invalid fileId '%s', generating.", humanIdToLoad.c_str(), fileIdToLoadContent.c_str());
                                fileIdToLoadContent = _generateShortFileId_nolock(humanIdToLoad);
                                firstScript["fileId"] = fileIdToLoadContent;
                                saveScriptList_nolock(listDoc); // Use _nolock
                            }
                        } else {
                            log_w("getScriptForExecution: First script '%s' missing 'fileId'. Generating.", humanIdToLoad.c_str());
                            fileIdToLoadContent = _generateShortFileId_nolock(humanIdToLoad);
                            firstScript["fileId"] = fileIdToLoadContent;
                            saveScriptList_nolock(listDoc); // Use _nolock
                        }
                        log_i("getScriptForExecution: Using first script: ID '%s', fileId '%s'", humanIdToLoad.c_str(), fileIdToLoadContent.c_str());
                        if (!saveCurrentScriptId_nolock(humanIdToLoad)) log_w("getScriptForExecution: Failed to save new current script ID");
                    } else { log_e("getScriptForExecution: First script has no valid 'id'"); humanIdToLoad = ""; fileIdToLoadContent = ""; }
                } else { log_e("getScriptForExecution: First item not an object"); humanIdToLoad = ""; fileIdToLoadContent = ""; }
            } else {
                log_w("getScriptForExecution: No valid scripts in list. Using built-in default script.");
                outHumanId = DEFAULT_SCRIPT_ID; outFileId = DEFAULT_SCRIPT_ID; outInitialState.state_loaded = false;
                xSemaphoreGive(_spiffsMutex);
                return true;
            }
        }

        if (fileIdToLoadContent.isEmpty() || humanIdToLoad.isEmpty()) {
            log_i("getScriptForExecution: Failed to determine valid script. Using built-in default.");
            outHumanId = DEFAULT_SCRIPT_ID; outFileId = DEFAULT_SCRIPT_ID; outInitialState.state_loaded = false;
            xSemaphoreGive(_spiffsMutex);
            return true;
        }

        if (fileIdToLoadContent == DEFAULT_SCRIPT_ID) {
            log_i("getScriptForExecution: Identified as default script ID. RenderTask will use built-in content.");
            outHumanId = DEFAULT_SCRIPT_ID; outFileId = DEFAULT_SCRIPT_ID; outInitialState.state_loaded = false;
            xSemaphoreGive(_spiffsMutex);
            return true;
        }

        outHumanId = humanIdToLoad; outFileId = fileIdToLoadContent;
        bool stateLoaded = loadScriptExecutionState_nolock(outHumanId, outInitialState);
        if (!stateLoaded) {
            log_i("getScriptForExecution: No saved state for script '%s'. Using defaults.", outHumanId.c_str());
            outInitialState = ScriptExecState(); outInitialState.state_loaded = false;
        } else {
            log_i("getScriptForExecution: Loaded execution state for script '%s'", outHumanId.c_str());
        }
        log_i("getScriptForExecution: Successfully determined script '%s' (fileId '%s') for execution.", outHumanId.c_str(), outFileId.c_str());
        xSemaphoreGive(_spiffsMutex);
        return true;
    }
    log_e("getScriptForExecution: Failed to take mutex.");
    outHumanId = DEFAULT_SCRIPT_ID; outFileId = DEFAULT_SCRIPT_ID; outInitialState.state_loaded = false; // Fallback
    return false;
}

void ScriptManager::clearAllScriptData()
{
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        log_w("Clearing all script data (list.json, current_script.id, script_states.json and content files).");

        LittleFS.remove(LIST_JSON_PATH); // Changed SPIFFS to LittleFS
        LittleFS.remove(CURRENT_SCRIPT_ID_PATH); // Changed SPIFFS to LittleFS
        LittleFS.remove(SCRIPT_STATES_PATH); // Changed SPIFFS to LittleFS

        File root = LittleFS.open(CONTENT_DIR_PATH); // Changed SPIFFS to LittleFS
        if (root)
        {
            if (root.isDirectory())
            {
                File entry = root.openNextFile();
                while (entry)
                {
                    esp_task_wdt_reset();
                    if (!entry.isDirectory())
                    {
                        String baseName = entry.name();
                        String fullPath = String(CONTENT_DIR_PATH) + "/" + baseName;
                        log_i("Deleting content file: %s (derived from base: %s)", fullPath.c_str(), baseName.c_str());
                        if (!LittleFS.remove(fullPath.c_str())) { // Changed SPIFFS to LittleFS
                            log_e("Failed to remove %s", fullPath.c_str());
                        }
                    }
                    entry.close();
                    entry = root.openNextFile();
                }
            }
            root.close();
        }
        else
        {
            log_w("Could not open %s directory for clearing.", CONTENT_DIR_PATH);
        }
        
        LittleFS.rmdir(CONTENT_DIR_PATH); // Changed SPIFFS to LittleFS
        initializeLittleFS(); // Changed initializeSPIFFS to initializeLittleFS

        xSemaphoreGive(_spiffsMutex);
        log_i("Script data clearing completed.");
    }
    else
    {
        log_e("ScriptManager::clearAllScriptData failed to take mutex.");
    }
}

void ScriptManager::cleanupOrphanedStates(const JsonArrayConst &validScriptList)
{
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        log_i("Cleaning up orphaned script execution states...");
        JsonDocument currentStatesDoc; // Use default allocator
        File statesFile = LittleFS.open(SCRIPT_STATES_PATH, FILE_READ); // Changed SPIFFS to LittleFS
        bool statesLoaded = false;

        if (!statesFile || statesFile.isDirectory() || statesFile.size() == 0)
        {
            if (statesFile)
                statesFile.close();
            log_i("%s not found or empty. No states to clean.", SCRIPT_STATES_PATH);
        }
        else
        {
            DeserializationError error = deserializeJson(currentStatesDoc, statesFile);
            statesFile.close();
            if (error)
            {
                log_e("Failed to parse %s for cleanup: %s. Skipping cleanup.", SCRIPT_STATES_PATH, error.c_str());
            }
            else if (!currentStatesDoc.is<JsonObject>())
            {
                log_e("%s content not a JSON object. Skipping cleanup.", SCRIPT_STATES_PATH);
            }
            else
            {
                statesLoaded = true;
            }
        }

        if (statesLoaded)
        {
            std::set<String> validHumanIds;
            for (JsonVariantConst item : validScriptList)
            {
                if (!item.is<JsonObjectConst>())
                {
                    log_w("Cleanup: Item in validScriptList is not an object, skipping.");
                    continue;
                }
                JsonObjectConst item_obj = item.as<JsonObjectConst>();
                const char *humanId = item_obj["id"].as<const char *>();
                if (humanId)
                    validHumanIds.insert(String(humanId));
            }

            JsonObject statesRoot = currentStatesDoc.as<JsonObject>();
            bool statesModified = false;
            std::vector<String> keysToRemove;

            for (auto kv : statesRoot)
            {
                String scriptIdKey = kv.key().c_str();
                if (validHumanIds.find(scriptIdKey) == validHumanIds.end())
                {
                    keysToRemove.push_back(scriptIdKey);
                }
            }

            if (!keysToRemove.empty())
            {
                statesModified = true;
                log_i("Removing states for %d orphaned script IDs:", keysToRemove.size());
                for (const String &key : keysToRemove)
                {
                    log_i("  - Removing state for: %s", key.c_str());
                    statesRoot.remove(key);
                }

                statesFile = LittleFS.open(SCRIPT_STATES_PATH, FILE_WRITE); // Changed SPIFFS to LittleFS
                if (!statesFile)
                {
                    log_e("Failed to open %s for writing cleaned states.", SCRIPT_STATES_PATH);
                }
                else
                {
                    esp_task_wdt_reset();
                    if (serializeJson(currentStatesDoc, statesFile) > 0)
                    {
                        log_i("Successfully saved cleaned-up script states to %s.", SCRIPT_STATES_PATH);
                    }
                    else
                    {
                        log_e("Failed to write updated states to %s.", SCRIPT_STATES_PATH);
                    }
                    statesFile.close();
                }
            }
            else
            {
                log_i("No orphaned script states found to remove.");
            }
        }
        xSemaphoreGive(_spiffsMutex);
    }
    else
    {
        log_e("ScriptManager::cleanupOrphanedStates failed to take mutex.");
    }
}

void ScriptManager::cleanupOrphanedContent(const JsonArrayConst &validScriptList)
{
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        log_i("Cleaning up orphaned script content files...");
        std::set<String> validFileIds;
        for (JsonVariantConst item : validScriptList)
        {
            if (!item.is<JsonObjectConst>())
            {
                log_w("Cleanup Content: Item in validScriptList is not an object, skipping.");
                continue;
            }
            JsonObjectConst item_obj = item.as<JsonObjectConst>();

            String humanId;
            if (!item_obj["id"].isNull() && item_obj["id"].is<const char *>())
            {
                humanId = item_obj["id"].as<String>();
            }
            else
            {
                log_w("Cleanup Content: Item missing 'id' field or it's not a string, skipping.");
                continue;
            }

            String fileId;
            if (!item_obj["fileId"].isNull() && item_obj["fileId"].is<const char *>())
            {
                fileId = item_obj["fileId"].as<String>();
                if (fileId.isEmpty() || fileId == "null")
                {
                    log_d("Cleanup Content: Script '%s' has empty or 'null' fileId, using humanId instead", humanId.c_str());
                    fileId = humanId; // This case should not happen if ensureUniqueFileIds is working
                }
            }
            else
            {
                log_d("Cleanup Content: Script '%s' missing 'fileId' field or it's not a string. Using humanId.", humanId.c_str());
                fileId = humanId; // This case should not happen if ensureUniqueFileIds is working
            }
            // Only add valid 's' prefixed fileIds or the default script ID
            if (fileId.startsWith("s") || fileId == DEFAULT_SCRIPT_ID) {
                 validFileIds.insert(fileId);
            } else {
                log_w("Cleanup Content: Script '%s' has an invalid fileId '%s' that is not 's'-prefixed or default. Skipping for cleanup consideration.", humanId.c_str(), fileId.c_str());
            }
        }

        File root = LittleFS.open(CONTENT_DIR_PATH); // Changed SPIFFS to LittleFS
        if (root && root.isDirectory())
        {
            File entry = root.openNextFile();
            while (entry)
            {
                esp_task_wdt_reset();
                if (!entry.isDirectory())
                {
                    String entryName = entry.name();
                    String fileIdFromPath = entryName.substring(entryName.lastIndexOf('/') + 1);

                    if (validFileIds.find(fileIdFromPath) == validFileIds.end())
                    {
                        String fullPathToRemove = String(CONTENT_DIR_PATH) + "/" + fileIdFromPath;
                        log_i("Removing orphaned script content: %s (fileId: %s)", fullPathToRemove.c_str(), fileIdFromPath.c_str());
                        if (!LittleFS.remove(fullPathToRemove.c_str())) // Changed SPIFFS to LittleFS
                        {
                            log_e("Failed to remove %s", fullPathToRemove.c_str());
                        }
                    }
                }
                entry.close();
                entry = root.openNextFile();
            }
            root.close();
        }
        else
        {
            if (root)
                root.close();
            log_w("Could not open %s for cleanup or not a directory.", CONTENT_DIR_PATH);
        }
        xSemaphoreGive(_spiffsMutex);
    }
    else
    {
        log_e("ScriptManager::cleanupOrphanedContent failed to take mutex.");
    }
}

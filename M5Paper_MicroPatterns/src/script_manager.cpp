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
        log_e("ScriptManager: Failed to create SPIFFS mutex!");
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
String ScriptManager::generateShortFileId(const String &humanId)
{
    // If _nextFileIdCounter isn't initialized, determine the next available ID number
    if (_nextFileIdCounter == 0)
    {
        _nextFileIdCounter = getHighestFileIdNumber() + 1;
    }

    // First check if content already exists for this humanId
    // This requires accessing the script list which we can't do here
    // without risking circular dependencies or mutex deadlocks.
    // Instead, the caller (ensureUniqueFileIds_nolock) should handle
    // preserving existing content file mappings.

    // Generate the new short fileId
    String shortId = "s" + String(_nextFileIdCounter++);
    log_i("Generated new short fileId '%s' for humanId '%s'", shortId.c_str(), humanId.c_str());
    return shortId;
}

// Analyze existing fileIds to find the highest current number
int ScriptManager::getHighestFileIdNumber()
{
    int highest = 0;
    std::map<String, bool> contentFileExists; // Track which fileIds have content files

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        // Check file system first to find existing content files
        if (SPIFFS.exists(CONTENT_DIR_PATH))
        {
            File root = SPIFFS.open(CONTENT_DIR_PATH);
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

                        // Check if filename matches our "s" + number pattern
                        if (filename.startsWith("s") && filename.length() > 1)
                        {
                            String numPart = filename.substring(1);
                            if (numPart.toInt() > 0 || numPart == "0")
                            {
                                int fileNum = numPart.toInt();
                                if (fileNum > highest)
                                {
                                    highest = fileNum;
                                }
                                // Track that this fileId has content
                                contentFileExists[filename] = true;
                                log_d("getHighestFileIdNumber: Found content file: %s", filename.c_str());
                            }
                        }
                    }
                    entry.close();
                    entry = root.openNextFile();
                }
                root.close();
            }
        }

        // Also check script list for any fileIds
        JsonDocument listDoc;
        if (loadScriptList(listDoc))
        {
            if (listDoc.is<JsonArray>())
            {
                JsonArray scriptList = listDoc.as<JsonArray>();
                for (JsonObject scriptInfo : scriptList)
                {
                    if (!scriptInfo["fileId"].isNull() && scriptInfo["fileId"].is<const char *>())
                    {
                        String fileId = scriptInfo["fileId"].as<String>();
                        if (fileId.startsWith("s") && fileId.length() > 1)
                        {
                            String numPart = fileId.substring(1);
                            if (numPart.toInt() > 0 || numPart == "0")
                            {
                                int fileNum = numPart.toInt();
                                if (fileNum > highest)
                                {
                                    highest = fileNum;
                                }
                                
                                // Store whether this fileId in the list has content
                                if (contentFileExists.find(fileId) != contentFileExists.end()) {
                                    log_d("getHighestFileIdNumber: fileId %s in list has matching content file", fileId.c_str());
                                } else {
                                    log_d("getHighestFileIdNumber: fileId %s in list has NO matching content file", fileId.c_str());
                                }
                            }
                        }
                    }
                }
            }
        }

        xSemaphoreGive(_spiffsMutex);
    }

    log_i("Highest existing fileId number: %d", highest);
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
    if (SPIFFS.exists(CONTENT_DIR_PATH))
    {
        File root = SPIFFS.open(CONTENT_DIR_PATH);
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
            if (SPIFFS.exists(contentPath))
            {
                log_i("ensureUniqueFileIds_nolock: Script '%s' has existing content file with fileId '%s'",
                     humanId.c_str(), fileId.c_str());
                contentFileIdMap[humanId] = fileId;
                usedFileIds[fileId] = true;
            }
        }
    }

    // Reset _nextFileIdCounter based on current highest known ID before generating new ones
    _nextFileIdCounter = getHighestFileIdNumber() + 1;

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
                    // Invalid fileId, generate a new one
                    fileId = generateShortFileId(humanId);
                    scriptInfo["fileId"] = fileId;
                    listModified = true;
                    log_i("ensureUniqueFileIds_nolock: Replaced invalid fileId for '%s' with '%s'",
                         humanId.c_str(), fileId.c_str());
                }
            }
            else
            {
                // Missing fileId, generate a new one
                fileId = generateShortFileId(humanId);
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
                fileId = generateShortFileId(humanId);
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
        bool success = initializeSPIFFS();
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("ScriptManager::initialize failed to take mutex.");
    return false;
}

bool ScriptManager::initializeSPIFFS()
{
    if (!SPIFFS.begin(true))
    { // `true` = format SPIFFS if mount fails
        log_e("SPIFFS Mount Failed even after formatting attempt!");
        return false;
    }
    log_i("SPIFFS Mounted successfully.");

    // Create directories if they don't exist
    if (!SPIFFS.exists("/scripts"))
    {
        if (!SPIFFS.mkdir("/scripts"))
        {
            log_e("Failed to create /scripts directory!");
            return false;
        }
        log_i("Created /scripts directory");
    }
    if (!SPIFFS.exists(CONTENT_DIR_PATH))
    {
        if (!SPIFFS.mkdir(CONTENT_DIR_PATH))
        {
            log_e("Failed to create %s directory!", CONTENT_DIR_PATH);
            return false;
        }
        log_i("Created %s directory", CONTENT_DIR_PATH);
    }
    return true;
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
    if (!SPIFFS.exists("/scripts")) {
        log_w("saveScriptList_nolock: /scripts directory does not exist, creating...");
        if (!SPIFFS.mkdir("/scripts")) {
            log_e("saveScriptList_nolock: Failed to create /scripts directory");
            return false;
        }
    }

    File file = SPIFFS.open(LIST_JSON_PATH, FILE_WRITE);
    if (!file) {
        log_e("saveScriptList_nolock: Failed to open %s for writing. SPIFFS.open() returned null", LIST_JSON_PATH);
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


bool ScriptManager::saveScriptList(JsonDocument &listDoc)
{
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(3000)) == pdTRUE) { // Increased timeout
        bool success = saveScriptList_nolock(listDoc);
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("ScriptManager::saveScriptList failed to take mutex after 3000ms");
    return false;
}

bool ScriptManager::loadScriptContent(const String &fileId, String &outContent)
{
    // Reset output content
    outContent = "";
    JsonDocument listDoc; // Declare listDoc at the beginning of the function

    if (fileId.isEmpty())
    {
        log_e("loadScriptContent: fileId is empty");
        return false;
    }

    // Special handling for default script ID - use built-in content directly
    if (fileId == DEFAULT_SCRIPT_ID)
    {
        log_i("loadScriptContent: Using built-in DEFAULT_SCRIPT_CONTENT for '%s'", fileId.c_str());
        outContent = DEFAULT_SCRIPT_CONTENT;
        return true;
    }

    // Use the fileId as-is if it starts with 's' (indicating it's already a short fileId)
    // Otherwise, it might be a humanId - check if we need to convert it
    String actualFileId = fileId;
    if (!fileId.startsWith("s") && fileId != DEFAULT_SCRIPT_ID)
    {
        // Look up the correct fileId in the script list
        // JsonDocument listDoc; // Moved to the top of the function
        if (loadScriptList(listDoc) && listDoc.is<JsonArray>()) // loadScriptList now handles ensureUniqueFileIds_nolock
        {
            bool idFound = false;
            for (JsonObject scriptInfo : listDoc.as<JsonArray>())
            {
                if (scriptInfo["id"].as<String>() == fileId)
                {
                    // Found matching humanId, check for valid fileId
                    if (!scriptInfo["fileId"].isNull() && scriptInfo["fileId"].is<const char *>())
                    {
                        String storedFileId = scriptInfo["fileId"].as<String>();
                        if (!storedFileId.isEmpty() && storedFileId != "null" && storedFileId.startsWith("s"))
                        {
                            actualFileId = storedFileId;
                            idFound = true;
                            log_i("loadScriptContent: Mapped humanId '%s' to fileId '%s'", fileId.c_str(), actualFileId.c_str());
                            break;
                        }
                    }
                }
            }

            if (!idFound)
            {
                log_w("loadScriptContent: Could not map humanId '%s' to a short fileId from list.", fileId.c_str());
            }
        }
    }

    String path = String(CONTENT_DIR_PATH) + "/" + actualFileId;
    log_i("loadScriptContent: Attempting to load script content from %s", path.c_str());

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        // Check if the exact path exists first
        if (!SPIFFS.exists(path.c_str()))
        {
            log_w("loadScriptContent: Path does not exist: %s (for actualFileId: %s, original fileId: %s)", path.c_str(), actualFileId.c_str(), fileId.c_str());
            bool recovered = false;

            // Determine the humanId we are trying to load content for.
            String humanIdForRecovery = "";
            if (!actualFileId.startsWith("s")) { // If actualFileId was a humanId initially (meaning fileId was humanId)
                humanIdForRecovery = actualFileId;
            } else { // actualFileId is 's'-style. Find its humanId from listDoc.
                     // listDoc was passed by the caller (getScriptForExecution) or loaded if called directly.
                     // For this recovery, we need a fresh load of listDoc to ensure we have the latest mappings.
                JsonDocument currentListDoc;
                if (loadScriptList(currentListDoc) && currentListDoc.is<JsonArray>()) { // loadScriptList handles mutex internally
                    for (JsonObjectConst scriptItem : currentListDoc.as<JsonArray>()) {
                        if (!scriptItem["fileId"].isNull() && scriptItem["fileId"].as<String>() == actualFileId) {
                            if(!scriptItem["id"].isNull()) {
                                humanIdForRecovery = scriptItem["id"].as<String>();
                                break;
                            }
                        }
                    }
                }
            }

            if (humanIdForRecovery.isEmpty()) {
                log_w("loadScriptContent: Could not determine humanId for actualFileId '%s'. Cannot recover.", actualFileId.c_str());
            } else {
                log_i("loadScriptContent: Initial path failed for actualFileId '%s'. Attempting recovery for humanId '%s'.", actualFileId.c_str(), humanIdForRecovery.c_str());
                // Re-load listDoc to ensure we have the latest version after any ensureUniqueFileIds_nolock calls.
                JsonDocument freshListDoc;
                if (loadScriptList(freshListDoc) && freshListDoc.is<JsonArray>()) { // loadScriptList handles mutex internally
                    for (JsonObjectConst scriptItemConst : freshListDoc.as<JsonArray>()) { // Iterate with JsonObjectConst
                        if (!scriptItemConst["id"].isNull() && scriptItemConst["id"].as<String>() == humanIdForRecovery) {
                            if (!scriptItemConst["fileId"].isNull()) {
                                String fileIdFromList = scriptItemConst["fileId"].as<String>();
                                if (!fileIdFromList.isEmpty() && fileIdFromList.startsWith("s")) {
                                    String pathFromList = String(CONTENT_DIR_PATH) + "/" + fileIdFromList;
                                    log_i("loadScriptContent: For humanId '%s', list.json points to fileId '%s'. Checking path: %s", humanIdForRecovery.c_str(), fileIdFromList.c_str(), pathFromList.c_str());
                                    if (SPIFFS.exists(pathFromList.c_str())) {
                                        File recoveryFile = SPIFFS.open(pathFromList.c_str(), FILE_READ);
                                        if (recoveryFile && !recoveryFile.isDirectory() && recoveryFile.size() > 0) {
                                            outContent = recoveryFile.readString();
                                            log_i("loadScriptContent: Recovery successful. Loaded %u bytes for humanId '%s' from '%s'.", outContent.length(), humanIdForRecovery.c_str(), pathFromList.c_str());
                                            recovered = true;
                                        } else {
                                            log_w("loadScriptContent: Recovery file '%s' exists but is empty or unreadable.", pathFromList.c_str());
                                        }
                                        if (recoveryFile) recoveryFile.close();
                                    } else {
                                        log_w("loadScriptContent: Path '%s' (from list.json for humanId '%s') also does not exist.", pathFromList.c_str(), humanIdForRecovery.c_str());
                                    }
                                } else {
                                     log_w("loadScriptContent: fileId for humanId '%s' in list.json is invalid or empty: '%s'", humanIdForRecovery.c_str(), fileIdFromList.c_str());
                                }
                            } else {
                                log_w("loadScriptContent: humanId '%s' in list.json has no fileId field.", humanIdForRecovery.c_str());
                            }
                            break; // Found the humanId in the list
                        }
                    }
                } else {
                    log_w("loadScriptContent: Failed to load list.json for recovery attempt.");
                }
            }

            if (!recovered) {
                log_w("loadScriptContent: Recovery failed for actualFileId '%s'.", actualFileId.c_str());
                xSemaphoreGive(_spiffsMutex);
                return false;
            }
            // If recovered, outContent is populated. Proceed to return true at the end.
            // The 'path' variable is not updated here, but outContent is directly set.
            // The original file opening logic below will be skipped if recovered.
            // To use the original logic, we'd update 'path' and 'actualFileId' and not return early.
            // For simplicity, if recovered, we return true now.
            xSemaphoreGive(_spiffsMutex);
            return true;
        }

        // This part executes if the original path existed, or if recovery updated 'path' and we didn't return early.
        // Given the recovery logic now returns true directly, this part is only for the initial successful SPIFFS.exists(path.c_str())
        log_d("loadScriptContent: Opening file %s for reading", path.c_str());
        File file = SPIFFS.open(path.c_str(), FILE_READ);

        if (!file)
        {
            log_e("loadScriptContent: Failed to open file (null pointer returned)");
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        if (file.isDirectory())
        {
            log_e("loadScriptContent: Path is a directory, not a file");
            file.close();
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        // Get file size
        size_t fileSize = file.size();

        if (fileSize == 0)
        {
            log_w("loadScriptContent: File is empty (0 bytes)");
            file.close();
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        log_d("loadScriptContent: File opened successfully, size: %u bytes", fileSize);

        // Reset watchdog before potentially time-consuming read
        esp_task_wdt_reset();

        // Read file content - consider streaming or chunking for very large files
        if (fileSize > 5000)
        {
            log_w("loadScriptContent: Large file detected (%u bytes) - this may cause memory issues", fileSize);
        }

        // Attempt to read the entire file
        outContent = file.readString();
        size_t readLength = outContent.length();
        file.close();

        // Validate read operation
        if (readLength == 0)
        {
            log_e("loadScriptContent: readString() returned empty string despite file size %u", fileSize);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        if (readLength != fileSize)
        {
            log_w("loadScriptContent: Read %u bytes but file size is %u bytes", readLength, fileSize);
            // Continue anyway, we got some content
        }

        xSemaphoreGive(_spiffsMutex);
        log_i("loadScriptContent: Successfully loaded %u bytes for fileId '%s'", readLength, fileId.c_str());
        return true;
    }

    log_e("loadScriptContent: Failed to take mutex for fileId %s after 1000ms", fileId.c_str());
    return false;
}

bool ScriptManager::saveScriptContent(const String &fileId, const String &content)
{
    if (fileId.isEmpty())
    {
        log_e("saveScriptContent: fileId is empty");
        return false;
    }

    if (content.isEmpty())
    {
        log_e("saveScriptContent: content is empty for fileId '%s'", fileId.c_str());
        return false;
    }

    // Ensure we're using a short fileId for storage
    String actualFileId = fileId;
    if (!fileId.startsWith("s") && fileId != DEFAULT_SCRIPT_ID)
    {
        // Look up the correct fileId in the script list
        JsonDocument listDoc;
        if (loadScriptList(listDoc) && listDoc.is<JsonArray>())
        {
            bool idFound = false;
            for (JsonObject scriptInfo : listDoc.as<JsonArray>())
            {
                if (scriptInfo["id"].as<String>() == fileId)
                {
                    // Found matching humanId, check for valid fileId
                    if (!scriptInfo["fileId"].isNull() && scriptInfo["fileId"].is<const char *>())
                    {
                        String storedFileId = scriptInfo["fileId"].as<String>();
                        if (!storedFileId.isEmpty() && storedFileId != "null" && storedFileId.startsWith("s"))
                        {
                            actualFileId = storedFileId;
                            idFound = true;
                            log_i("saveScriptContent: Mapped humanId '%s' to fileId '%s'", fileId.c_str(), actualFileId.c_str());
                            break;
                        }
                    }
                }
            }

            if (!idFound)
            {
                // If we still don't have a valid fileId, generate one
                if (fileId != DEFAULT_SCRIPT_ID)
                {
                    actualFileId = generateShortFileId(fileId);
                    log_w("saveScriptContent: Generated new fileId '%s' for humanId '%s'", actualFileId.c_str(), fileId.c_str());

                    // TODO: We should update the script list, but that's handled elsewhere
                }
            }
        }
    }

    String path = String(CONTENT_DIR_PATH) + "/" + actualFileId;
    log_i("saveScriptContent: Attempting to save %u bytes to %s", content.length(), path.c_str());

    // Check if content is reasonably sized
    if (content.length() > 10000)
    {
        log_w("saveScriptContent: Large content detected (%u bytes) - this may impact memory", content.length());
    }

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    { // Increased timeout
        // Ensure directory exists
        if (!SPIFFS.exists(CONTENT_DIR_PATH))
        {
            log_w("saveScriptContent: Directory %s does not exist, creating...", CONTENT_DIR_PATH);
            if (!SPIFFS.mkdir(CONTENT_DIR_PATH))
            {
                log_e("saveScriptContent: Failed to create directory %s", CONTENT_DIR_PATH);
                xSemaphoreGive(_spiffsMutex);
                return false;
            }
        }

        // Check available space
        size_t freeSpace = SPIFFS.totalBytes() - SPIFFS.usedBytes();
        if (content.length() + 100 > freeSpace)
        { // Add buffer for filesystem overhead
            log_e("saveScriptContent: Not enough space. Content: %u bytes, Free: %u bytes",
                  content.length(), freeSpace);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        // Delete any existing file first (optional, but can help with fragmentation)
        if (SPIFFS.exists(path.c_str()))
        {
            log_d("saveScriptContent: Deleting existing file before writing new content");
            SPIFFS.remove(path.c_str());
        }

        log_d("saveScriptContent: Opening file for writing");
        File file = SPIFFS.open(path.c_str(), FILE_WRITE);
        if (!file)
        {
            log_e("saveScriptContent: Failed to open %s for writing", path.c_str());
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        // Reset watchdog before potentially lengthy operation
        esp_task_wdt_reset();

        // Write content
        size_t bytesWritten = file.print(content); // print() returns bytes written
        esp_task_wdt_reset(); // Reset WDT after potentially lengthy write
        bool success = (bytesWritten == content.length());

        if (success)
        {
            log_i("saveScriptContent: Successfully wrote %u bytes to file", bytesWritten);
        }
        else
        {
            log_e("saveScriptContent: Write incomplete. Wrote %u of %u bytes",
                  bytesWritten, content.length());
        }

        file.close();

        // Verify file was written correctly
        if (success)
        {
            File verifyFile = SPIFFS.open(path.c_str(), FILE_READ);
            if (verifyFile)
            {
                size_t fileSize = verifyFile.size();
                if (fileSize != content.length())
                {
                    log_w("saveScriptContent: Verification shows file size (%u) != content length (%u)",
                          fileSize, content.length());
                    // Don't fail just due to size mismatch - file system overhead might affect this
                }
                verifyFile.close();
            }
            else
            {
                log_e("saveScriptContent: Failed to open file for verification");
                // Still consider it a success since we wrote the bytes
            }
        }

        xSemaphoreGive(_spiffsMutex);
        return success;
    }

    log_e("saveScriptContent: Failed to take mutex for fileId %s after 1000ms", fileId.c_str());
    return false;
}

bool ScriptManager::getCurrentScriptId(String &outHumanId)
{
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        File file = SPIFFS.open(CURRENT_SCRIPT_ID_PATH, FILE_READ);
        if (!file || file.isDirectory())
        {
            log_w("Failed to open %s for reading or it's a directory.", CURRENT_SCRIPT_ID_PATH);
            if (file)
                file.close();
            outHumanId = "";
            xSemaphoreGive(_spiffsMutex);
            return false;
        }
        outHumanId = file.readString();
        file.close();
        xSemaphoreGive(_spiffsMutex);

        if (outHumanId.length() > 0)
        {
            log_i("Current script humanId '%s' loaded from %s.", outHumanId.c_str(), CURRENT_SCRIPT_ID_PATH);
            return true;
        }
        else
        {
            log_w("%s is empty or read failed.", CURRENT_SCRIPT_ID_PATH);
            return false;
        }
    }
    log_e("ScriptManager::getCurrentScriptId failed to take mutex.");
    return false;
}

bool ScriptManager::saveCurrentScriptId(const String &humanId)
{
    if (humanId.isEmpty())
    {
        log_e("saveCurrentScriptId: humanId is empty.");
        return false;
    }
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        File file = SPIFFS.open(CURRENT_SCRIPT_ID_PATH, FILE_WRITE);
        if (!file)
        {
            log_e("Failed to open %s for writing", CURRENT_SCRIPT_ID_PATH);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }
        bool success = false;
        if (file.print(humanId))
        {
            log_i("Current script humanId '%s' saved to %s.", humanId.c_str(), CURRENT_SCRIPT_ID_PATH);
            success = true;
        }
        else
        {
            log_e("Failed to write current script humanId '%s' to %s.", humanId.c_str(), CURRENT_SCRIPT_ID_PATH);
        }
        file.close();
        xSemaphoreGive(_spiffsMutex);
        return success;
    }
    log_e("ScriptManager::saveCurrentScriptId failed to take mutex for humanId %s", humanId.c_str());
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
        File file = SPIFFS.open(SCRIPT_STATES_PATH, FILE_READ);
        if (!file || file.isDirectory() || file.size() == 0)
        {
            if (file)
                file.close();
            xSemaphoreGive(_spiffsMutex);
            outState.state_loaded = false;
            return false;
        }

        JsonDocument statesDoc; // Use JsonDocument
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
    // Clear the output document first to ensure we start fresh
    outListDoc.clear();

    log_i("loadScriptList: Attempting to load script list from %s", LIST_JSON_PATH);

    // memoryUsage() is deprecated for JsonDocument and returns 0.
    // log_d("loadScriptList: Document memory usage: %u bytes", outListDoc.memoryUsage());

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    { // Increased timeout
        // First check if the file exists without opening it
        if (!SPIFFS.exists(LIST_JSON_PATH))
        {
            log_w("loadScriptList: Script list file %s does not exist", LIST_JSON_PATH);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        // Check SPIFFS status
        log_d("loadScriptList: SPIFFS space - Total: %u bytes, Used: %u bytes, Free: %u bytes",
              SPIFFS.totalBytes(), SPIFFS.usedBytes(), SPIFFS.totalBytes() - SPIFFS.usedBytes());

        File file = SPIFFS.open(LIST_JSON_PATH, FILE_READ);

        if (!file)
        {
            log_e("loadScriptList: Failed to open %s (file pointer is null)", LIST_JSON_PATH);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        if (file.isDirectory())
        {
            log_e("loadScriptList: %s is a directory, not a file", LIST_JSON_PATH);
            file.close();
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        size_t fileSize = file.size();
        if (fileSize == 0)
        {
            log_w("loadScriptList: File %s exists but is empty (0 bytes)", LIST_JSON_PATH);
            file.close();
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        log_d("loadScriptList: File %s opened, size: %u bytes", LIST_JSON_PATH, fileSize);

        // Reset watchdog before potentially time-consuming operation
        esp_task_wdt_reset();

        // Use ArduinoJson's stream parsing
        DeserializationError error = deserializeJson(outListDoc, file);
        file.close();

        if (error)
        {
            log_e("loadScriptList: JSON parsing error: %s", error.c_str());

            // Try to log the file content for debugging if it's small enough
            if (fileSize < 200)
            {
                File readFile = SPIFFS.open(LIST_JSON_PATH, FILE_READ);
                if (readFile)
                {
                    String content = readFile.readString();
                    log_e("loadScriptList: File content: %s", content.c_str());
                    readFile.close();
                }
            }

            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        // Validate document structure
        if (!outListDoc.is<JsonArray>())
        {
            log_e("loadScriptList: Parsed JSON is not an array. Type: %s",
                  outListDoc.is<JsonObject>() ? "object" : "unknown");
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        size_t numEntries = outListDoc.as<JsonArray>().size();
        log_i("loadScriptList: Successfully loaded script list with %u entries", numEntries);

        // Ensure all scripts have valid, unique fileIds before returning
        // This might also save the list if modifications were made.
        ensureUniqueFileIds_nolock(outListDoc);

        xSemaphoreGive(_spiffsMutex);
        return true;
    }

    log_e("loadScriptList: Failed to take mutex after 1000ms");
    return false;
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

    if (localHumanId.isEmpty())
    { // This check is redundant due to the one above, but kept from original for structure.
        log_e("saveScriptExecutionState: localHumanId (from humanId) is null or empty.");
        return false;
    }

    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        JsonDocument statesDoc; // Use JsonDocument

        File file = SPIFFS.open(SCRIPT_STATES_PATH, FILE_READ);
        if (file && file.size() > 0 && !file.isDirectory())
        {
            DeserializationError error = deserializeJson(statesDoc, file);
            if (error)
            {
                log_w("saveScriptExecutionState: Failed to parse existing %s: %s. Will overwrite.", SCRIPT_STATES_PATH, error.c_str());
                statesDoc.clear();
            }
        }
        if (file)
            file.close();

        if (!statesDoc.is<JsonObject>())
        {
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
        if (!file)
        {
            log_e("saveScriptExecutionState: Failed to open %s for writing.", SCRIPT_STATES_PATH);
            xSemaphoreGive(_spiffsMutex);
            return false;
        }

        bool success = false;
        if (serializeJson(statesDoc, file) == 0)
        {
            log_e("saveScriptExecutionState: Failed to write to %s.", SCRIPT_STATES_PATH);
        }
        else
        {
            success = true;
        }
        file.close();
        xSemaphoreGive(_spiffsMutex);

        if (success)
            log_i("Script state for ID '%s' (C:%d, T:%02d:%02d:%02d) saved to %s.", localHumanId.c_str(), state.counter, state.hour, state.minute, state.second, SCRIPT_STATES_PATH);
        return success;
    }
    log_e("ScriptManager::saveScriptExecutionState failed to take mutex for humanId %s", localHumanId.c_str());
    return false;
}

bool ScriptManager::selectNextScript(bool moveUp, String &outSelectedHumanId, String &outSelectedName)
{
    JsonDocument listDoc; // Use JsonDocument
    if (!loadScriptList(listDoc))
    { // loadScriptList handles mutex
        log_e("Cannot select next script, failed to load script list.");
        return false;
    }
    if (!listDoc.is<JsonArray>() || listDoc.as<JsonArray>().size() == 0)
    {
        log_w("Cannot select next script, list is empty or not an array.");
        outSelectedHumanId = DEFAULT_SCRIPT_ID; // Fallback
        outSelectedName = "Default";
        saveCurrentScriptId(DEFAULT_SCRIPT_ID); // Attempt to save default
        return false;                           // Indicate that selection from list failed
    }

    JsonArray scriptList = listDoc.as<JsonArray>();
    String currentHumanId;
    getCurrentScriptId(currentHumanId); // getCurrentScriptId handles mutex

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
            nextIndex = moveUp ? scriptList.size() - 1 : 0; // Default to last or first
        }
        JsonObject nextScriptInfo = scriptList[nextIndex];
        const char *nextId = nextScriptInfo["id"];
        const char *nextName = nextScriptInfo["name"];

        if (nextId)
        {
            outSelectedHumanId = nextId;
            outSelectedName = nextName ? nextName : nextId;
            log_i("Selected script index: %d, ID: %s, Name: %s", nextIndex, outSelectedHumanId.c_str(), outSelectedName.c_str());
            if (saveCurrentScriptId(outSelectedHumanId))
            { // saveCurrentScriptId handles mutex
                return true;
            }
            else
            {
                log_e("Failed to save the new current script ID: %s", outSelectedHumanId.c_str());
                return false;
            }
        }
        else
        {
            log_e("Script at index %d has no ID.", nextIndex);
        }
    }
    // Fallback if something went wrong or list was empty after all
    outSelectedHumanId = DEFAULT_SCRIPT_ID;
    outSelectedName = "Default";
    saveCurrentScriptId(DEFAULT_SCRIPT_ID);
    return false;
}

bool ScriptManager::getScriptForExecution(String &outHumanId, String &outFileId, String &outContent, ScriptExecState &outInitialState)
{
    log_i("getScriptForExecution: Starting script selection process");

    // Initialize outputs with empty/default values
    outHumanId = "";
    outFileId = "";
    outContent = "";
    outInitialState = ScriptExecState();

    // Step 1: Try to load the current script ID
    String humanIdToLoad;
    bool idFound = getCurrentScriptId(humanIdToLoad); // Mutex handled
    log_d("getScriptForExecution: getCurrentScriptId returned: found=%s, id='%s'",
          idFound ? "true" : "false", idFound ? humanIdToLoad.c_str() : "null");

    // Step 2: Load the script list
    JsonDocument listDoc;                      // Use JsonDocument
    bool listLoaded = loadScriptList(listDoc); // Mutex handled

    if (!listLoaded)
    {
        log_w("getScriptForExecution: Failed to load script list, will try to use cached current script or default");
    }
    else
    {
        log_d("getScriptForExecution: Script list loaded successfully");
    }

    // Get script list as a mutable array view if possible, as we might modify it
    JsonArray scriptList; // Default empty JsonArray
    if (listLoaded && listDoc.is<JsonArray>())
    {
        scriptList = listDoc.as<JsonArray>(); // Get a mutable view
        log_d("getScriptForExecution: Script list contains %u items", scriptList.size());
    }
    else if (listLoaded)
    {
        log_e("getScriptForExecution: Loaded document is not a JsonArray");
    }

    String fileIdToLoadContent = "";

    // Step 3: Handle case where we found an ID but it's empty
    if (idFound && humanIdToLoad.isEmpty())
    {
        log_w("getScriptForExecution: Current script ID loaded but was empty. Will try first script or default.");
        idFound = false;
    }

    // Step 4: If we have a valid current script ID, try to find its details in the list
    if (idFound && !humanIdToLoad.isEmpty())
    {
        log_i("getScriptForExecution: Looking for script '%s' in the list", humanIdToLoad.c_str());
        bool foundInList = false;

        if (listLoaded && !scriptList.isNull() && scriptList.size() > 0)
        {
            int index = 0;
            for (JsonVariant item : scriptList)
            { // Iterate with mutable JsonVariant
                if (item.is<JsonObject>())
                {                                                  // Check if it can be viewed as JsonObject
                    JsonObject scriptInfo = item.as<JsonObject>(); // Get mutable JsonObject

                    // Check if the item has an "id" field
                    if (!scriptInfo["id"].isNull() && scriptInfo["id"].is<const char *>())
                    {
                        String scriptId = scriptInfo["id"].as<String>();

                        if (humanIdToLoad == scriptId)
                        {
                            log_i("getScriptForExecution: Found matching script at index %d", index);

                            // Check for fileId field - ensure we have a valid short fileId
                            if (!scriptInfo["fileId"].isNull() && scriptInfo["fileId"].is<const char *>())
                            {
                                fileIdToLoadContent = scriptInfo["fileId"].as<String>();
                                // Check if fileId is empty, "null", or doesn't follow our format
                                if (fileIdToLoadContent.isEmpty() || fileIdToLoadContent == "null" || !fileIdToLoadContent.startsWith("s"))
                                {
                                    log_w("getScriptForExecution: Script '%s' has invalid fileId '%s', generating new one",
                                          scriptId.c_str(), fileIdToLoadContent.c_str());

                                    fileIdToLoadContent = generateShortFileId(scriptId);

                                    // Update the fileId in the list document so it persists
                                    scriptInfo["fileId"] = fileIdToLoadContent; // This is now valid

                                    // Flag that we need to save the updated list
                                    bool saveResult = saveScriptList(listDoc);
                                    log_i("getScriptForExecution: Updated script list with new fileId '%s' for '%s'. Save result: %s",
                                          fileIdToLoadContent.c_str(), scriptId.c_str(), saveResult ? "success" : "failed");
                                }
                            }
                            else
                            {
                                log_w("getScriptForExecution: Script '%s' missing 'fileId' field or it's not a string. Generating new one.", scriptId.c_str());

                                fileIdToLoadContent = generateShortFileId(scriptId);

                                // Add the fileId to the script info
                                scriptInfo["fileId"] = fileIdToLoadContent; // This is now valid

                                // Save the updated list
                                bool saveResult = saveScriptList(listDoc);
                                log_i("getScriptForExecution: Added new fileId '%s' to script '%s'. Save result: %s",
                                      fileIdToLoadContent.c_str(), scriptId.c_str(), saveResult ? "success" : "failed");
                            }
                            foundInList = true;
                            break;
                        }
                    }
                    else
                    {
                        log_w("getScriptForExecution: Script at index %d has no valid 'id' field", index);
                    }
                }
                else
                {
                    log_w("getScriptForExecution: Item at index %d is not a JSON object", index);
                }
                index++;
            }
        }

        if (!foundInList)
        {
            log_w("getScriptForExecution: Script ID '%s' not found in list. Will try first script.", humanIdToLoad.c_str());
            idFound = false;
            humanIdToLoad = "";
            fileIdToLoadContent = "";
        }
    }

    // Step 5: If no current ID or it wasn't found in the list, try using the first script from the list
    if (!idFound || humanIdToLoad.isEmpty() || fileIdToLoadContent.isEmpty())
    {
        log_i("getScriptForExecution: Attempting to use the first script in the list");

        if (listLoaded && !scriptList.isNull() && scriptList.size() > 0)
        {
            JsonVariant firstItem = scriptList[0]; // Get mutable JsonVariant

            if (firstItem.is<JsonObject>())
            {                                                        // Check if it can be viewed as JsonObject
                JsonObject firstScript = firstItem.as<JsonObject>(); // Get mutable JsonObject

                // Check if first item has valid id
                if (!firstScript["id"].isNull() && firstScript["id"].is<const char *>())
                {
                    humanIdToLoad = firstScript["id"].as<String>();

                    // Check for fileId - standardize handling of null/empty/literal "null" values
                    if (!firstScript["fileId"].isNull() && firstScript["fileId"].is<const char *>())
                    {
                        fileIdToLoadContent = firstScript["fileId"].as<String>();
                        // Check if fileId is empty or the literal string "null"
                        if (fileIdToLoadContent.isEmpty() || fileIdToLoadContent == "null" || !fileIdToLoadContent.startsWith("s"))
                        {
                            log_w("getScriptForExecution: First script '%s' has invalid or empty/null fileId '%s', generating new one", humanIdToLoad.c_str(), fileIdToLoadContent.c_str());
                            fileIdToLoadContent = generateShortFileId(humanIdToLoad);
                            firstScript["fileId"] = fileIdToLoadContent; // Update in list
                            saveScriptList(listDoc);                     // Save updated list
                        }
                    }
                    else
                    {
                        log_w("getScriptForExecution: First script '%s' missing 'fileId' field or it's not a string. Generating new one.", humanIdToLoad.c_str());
                        fileIdToLoadContent = generateShortFileId(humanIdToLoad);
                        firstScript["fileId"] = fileIdToLoadContent; // Update in list
                        saveScriptList(listDoc);                     // Save updated list
                    }

                    log_i("getScriptForExecution: Using first script: ID '%s', fileId '%s'",
                          humanIdToLoad.c_str(), fileIdToLoadContent.c_str());

                    // Save this as the new current script
                    if (!saveCurrentScriptId(humanIdToLoad))
                    {
                        log_w("getScriptForExecution: Failed to save new current script ID");
                    }
                }
                else
                {
                    log_e("getScriptForExecution: First script has no valid 'id' field");
                    humanIdToLoad = "";
                    fileIdToLoadContent = "";
                }
            }
            else
            {
                log_e("getScriptForExecution: First item in list is not a JSON object");
                humanIdToLoad = "";
                fileIdToLoadContent = "";
            }
        }
        else
        {
            // No valid list or it's empty - use default script
            log_w("getScriptForExecution: No valid scripts in list. Using built-in default script.");
            outHumanId = DEFAULT_SCRIPT_ID;
            outFileId = DEFAULT_SCRIPT_ID;
            outContent = DEFAULT_SCRIPT_CONTENT;
            outInitialState.state_loaded = false;
            log_i("getScriptForExecution: Returning built-in default script");
            return true;
        }
    }

    // Step 6: Final validation before trying to load content
    if (fileIdToLoadContent.isEmpty() || humanIdToLoad.isEmpty())
    {
        log_i("getScriptForExecution: Failed to determine valid script. Using built-in default.");
        outHumanId = DEFAULT_SCRIPT_ID;
        outFileId = DEFAULT_SCRIPT_ID;
        outContent = DEFAULT_SCRIPT_CONTENT;
        outInitialState.state_loaded = false;
        return true;
    }

    // Step 7: Try to load the actual script content
    log_i("getScriptForExecution: Loading content for fileId '%s'", fileIdToLoadContent.c_str());

    // Check if it's the default script ID, if so use built-in content directly
    if (fileIdToLoadContent == DEFAULT_SCRIPT_ID)
    {
        log_i("getScriptForExecution: Using built-in default script content directly");
        outHumanId = DEFAULT_SCRIPT_ID;
        outFileId = DEFAULT_SCRIPT_ID;
        outContent = DEFAULT_SCRIPT_CONTENT;
        outInitialState.state_loaded = false;
        return true;
    }

    if (loadScriptContent(fileIdToLoadContent, outContent))
    {
        outHumanId = humanIdToLoad;
        outFileId = fileIdToLoadContent;

        // Try to load execution state
        bool stateLoaded = loadScriptExecutionState(outHumanId, outInitialState);
        if (!stateLoaded)
        {
            log_i("getScriptForExecution: No saved state for script '%s'. Using defaults.", outHumanId.c_str());
            outInitialState = ScriptExecState();
            outInitialState.state_loaded = false;
        }
        else
        {
            log_i("getScriptForExecution: Loaded execution state for script '%s'", outHumanId.c_str());
        }

        log_i("getScriptForExecution: Successfully loaded script '%s' (content length: %u bytes)",
              outHumanId.c_str(), outContent.length());
        return true;
    }
    else
    {
        // Failed to load content - fall back to default
        log_i("getScriptForExecution: Failed to load content for script '%s' (fileId '%s'). Using default.",
              humanIdToLoad.c_str(), fileIdToLoadContent.c_str());
        outHumanId = DEFAULT_SCRIPT_ID;
        outFileId = DEFAULT_SCRIPT_ID;
        outContent = DEFAULT_SCRIPT_CONTENT;
        outInitialState.state_loaded = false;
        return true;
    }
}

void ScriptManager::clearAllScriptData()
{
    if (xSemaphoreTake(_spiffsMutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        log_w("Clearing all script data (list.json, current_script.id, script_states.json and content files).");

        SPIFFS.remove(LIST_JSON_PATH);
        SPIFFS.remove(CURRENT_SCRIPT_ID_PATH);
        SPIFFS.remove(SCRIPT_STATES_PATH);

        File root = SPIFFS.open(CONTENT_DIR_PATH);
        if (root)
        {
            if (root.isDirectory())
            {
                File entry = root.openNextFile();
                while (entry)
                {
                    esp_task_wdt_reset(); // Reset WDT during file iteration
                    if (!entry.isDirectory())
                    {
                        String baseName = entry.name(); // This seems to return basename e.g. "s1"
                        // entry.path() might be more reliable if available and returns full path.
                        // For now, construct path based on observed behavior of entry.name().
                        String fullPath = String(CONTENT_DIR_PATH) + "/" + baseName;
                        log_i("Deleting content file: %s (derived from base: %s)", fullPath.c_str(), baseName.c_str());
                        if (!SPIFFS.remove(fullPath.c_str())) {
                            log_e("Failed to remove %s", fullPath.c_str());
                        }
                    }
                    entry.close(); // Close file handle
                    entry = root.openNextFile();
                }
            }
            root.close(); // Close directory handle
        }
        else
        {
            log_w("Could not open %s directory for clearing.", CONTENT_DIR_PATH);
        }
        // Attempt to remove the content directory itself if it's empty
        // This might fail if not empty, which is fine.
        SPIFFS.rmdir(CONTENT_DIR_PATH);
        // Recreate directory structure
        initializeSPIFFS(); // This will recreate dirs if they were removed or ensure they exist

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
        JsonDocument currentStatesDoc; // Use JsonDocument
        File statesFile = SPIFFS.open(SCRIPT_STATES_PATH, FILE_READ);
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
                JsonObjectConst item_obj = item.as<JsonObjectConst>(); // Corrected
                const char *humanId = item_obj["id"].as<const char *>();
                if (humanId)
                    validHumanIds.insert(String(humanId));
            }

            JsonObject statesRoot = currentStatesDoc.as<JsonObject>();
            bool statesModified = false;
            std::vector<String> keysToRemove;

            // Collect keys to remove first
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

                statesFile = SPIFFS.open(SCRIPT_STATES_PATH, FILE_WRITE);
                if (!statesFile)
                {
                    log_e("Failed to open %s for writing cleaned states.", SCRIPT_STATES_PATH);
                }
                else
                {
                    esp_task_wdt_reset(); // Reset WDT before writing states file
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

            // Get humanId first as fallback
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

            // Get fileId with standardized null/empty handling
            String fileId;
            if (!item_obj["fileId"].isNull() && item_obj["fileId"].is<const char *>())
            {
                fileId = item_obj["fileId"].as<String>();
                // Check if fileId is empty or the literal string "null"
                if (fileId.isEmpty() || fileId == "null")
                {
                    log_d("Cleanup Content: Script '%s' has empty or 'null' fileId, using humanId instead", humanId.c_str());
                    fileId = humanId;
                }
            }
            else
            {
                log_d("Cleanup Content: Script '%s' missing 'fileId' field or it's not a string. Using humanId.", humanId.c_str());
                fileId = humanId;
            }

            validFileIds.insert(fileId);
        }

        File root = SPIFFS.open(CONTENT_DIR_PATH);
        if (root && root.isDirectory())
        {
            File entry = root.openNextFile();
            while (entry)
            {
                esp_task_wdt_reset(); // Reset WDT during file iteration
                if (!entry.isDirectory())
                {
                    String entryName = entry.name(); // This should be just the filename e.g. "s0", "s1"
                    // Construct fileId from entryName. If entryName includes path, extract filename part.
                    String fileIdFromPath = entryName.substring(entryName.lastIndexOf('/') + 1);

                    if (validFileIds.find(fileIdFromPath) == validFileIds.end())
                    {
                        String fullPathToRemove = String(CONTENT_DIR_PATH) + "/" + fileIdFromPath;
                        log_i("Removing orphaned script content: %s (fileId: %s)", fullPathToRemove.c_str(), fileIdFromPath.c_str());
                        if (!SPIFFS.remove(fullPathToRemove.c_str()))
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
#ifndef SCRIPT_MANAGER_H
#define SCRIPT_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include "event_defs.h" // For ScriptExecState
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" // For mutex

// Define constants for JSON document capacities
const size_t JSON_DOC_CAPACITY_SCRIPT_LIST = 1024; // For list.json, observed size 378 bytes
const size_t JSON_DOC_CAPACITY_SCRIPT_STATES = 2048; // For script_states.json, can grow with more scripts

class ScriptManager
{
public:
    ScriptManager();
    ~ScriptManager();
    bool initialize(); // General initialization for ScriptManager

    // Script List Management
    // Loads script list into the provided JsonDocument. Returns true on success.
    bool loadScriptList(JsonDocument &outListDoc);
    // Saves the script list from the provided JsonDocument. Returns true on success.
    bool saveScriptList(JsonDocument &listDoc); // Changed to non-const reference

    // Script Content Management
    bool loadScriptContent(const String &fileId, String &outContent);
    bool saveScriptContent(const String &fileId, const String &content);

    // Current Script ID Management
    bool getCurrentScriptId(String &outHumanId); // Gets human-readable ID
    bool saveCurrentScriptId(const String &humanId);

    // Script Execution State Management
    bool loadScriptExecutionState(const String &humanId, ScriptExecState &outState);
    bool saveScriptExecutionState(const String &humanId, const ScriptExecState &state);

    // Script Selection Logic
    // Selects next/prev script, saves it as current, returns its humanId and name.
    bool selectNextScript(bool moveUp, String &outSelectedHumanId, String &outSelectedName);

    // Get Script for Execution
    // Tries to load current script. If not found, tries first script. If none, uses default.
    // Returns humanId, fileId (for content loading), and initialState. Content is loaded by RenderTask.
    bool getScriptForExecution(String &outHumanId, String &outFileId, ScriptExecState &outInitialState);

    // FileId generation and management
    String generateShortFileId(const String& humanId); // Public method, handles mutex

    // Maintenance
    void clearAllScriptData();                                          // Deletes all script files and list.json
    void cleanupOrphanedStates(const JsonArrayConst &validScriptList);  // Removes states for non-existent scripts
    void cleanupOrphanedContent(const JsonArrayConst &validScriptList); // Removes content files not in list

private:
    SemaphoreHandle_t _spiffsMutex; // Mutex to protect SPIFFS operations

    // SPIFFS paths
    static const char *LIST_JSON_PATH;
    static const char *CONTENT_DIR_PATH;
    static const char *CURRENT_SCRIPT_ID_PATH;
    static const char *SCRIPT_STATES_PATH;

    // Default script content
public: // Made DEFAULT_SCRIPT_ID and DEFAULT_SCRIPT_CONTENT public
    static const char *DEFAULT_SCRIPT_CONTENT;
    static const char *DEFAULT_SCRIPT_ID;
private:

    // FileId generation and management
    /**
     * Ensures all scripts in the list have valid, unique fileIds, with special handling to
     * preserve any existing fileIds that have content files.
     *
     * Internal version that assumes the mutex is already held.
     */
    void ensureUniqueFileIds_nolock(JsonDocument& listDoc);

    // Counter for generating new sequential fileIds
    int _nextFileIdCounter = 0;

    bool initializeSPIFFS(); // Internal SPIFFS mount and directory check

    // Internal helper methods that assume mutex is already taken
    bool saveScriptList_nolock(JsonDocument &listDoc);
    int _getHighestFileIdNumber_nolock(); // Private helper, assumes mutex is held
    String _generateShortFileId_nolock(const String& humanId); // Private helper, assumes mutex is held
    
    bool loadScriptList_nolock(JsonDocument &outListDoc);
    bool getCurrentScriptId_nolock(String &outHumanId);
    bool saveCurrentScriptId_nolock(const String &humanId);
    bool loadScriptExecutionState_nolock(const String &humanId, ScriptExecState &outState);
    bool saveScriptContent_nolock(const String &fileId, const String &content);
    bool loadScriptContent_nolock(const String &fileId, String &outContent);
};

#endif // SCRIPT_MANAGER_H
#ifndef SCRIPT_MANAGER_H
#define SCRIPT_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include "event_defs.h" // For ScriptExecState
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" // For mutex

class ScriptManager
{
public:
    ScriptManager();
    ~ScriptManager();

    bool initialize(); // Initializes SPIFFS and creates directories

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
    // Returns humanId, fileId (for content loading), and content.
    bool getScriptForExecution(String &outHumanId, String &outFileId, String &outContent, ScriptExecState &outInitialState);

    // FileId generation and management
    String generateShortFileId(const String& humanId);

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
    static const char *DEFAULT_SCRIPT_CONTENT;
    static const char *DEFAULT_SCRIPT_ID;

    // FileId generation and management (generateShortFileId moved to public)
    void ensureUniqueFileIds(JsonDocument& listDoc);
    int getHighestFileIdNumber();
    int _nextFileIdCounter = 0;

    bool initializeSPIFFS(); // Internal SPIFFS mount and directory check
};

#endif // SCRIPT_MANAGER_H
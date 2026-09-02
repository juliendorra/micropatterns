#ifndef SCRIPT_MANAGER_H
#define SCRIPT_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include "event_defs.h" // For ScriptExecState
#include "mp_program.h"
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

    // False when the filesystem could not be mounted. Mounting NEVER formats:
    // the scripts on this partition may be the only copy that exists (see
    // docs/incidents/2026-08-27-m5paper-script-loss.md). formatStorage() is
    // the explicit, destructive way out, taken by the sync gesture because a
    // sync is what refills the partition.
    bool storageAvailable() const { return _storageOk; }
    bool formatStorage();

    // --- Compiled program cache (see mp_program.h) -------------------------
    // Returns the program for a script, from /scripts/compiled/<fileId>.mpc
    // when that file is a current-format program compiled from the source
    // that is on flash right now (header check against a streamed CRC of the
    // content file), otherwise by compiling the source and storing the result
    // for next time. A render therefore parses at most once per source
    // change, and normally never.
    bool loadProgram(const String &fileId, MpProgram &out, String *error = nullptr);
    // Compiles and stores if the stored program is missing or stale. Called
    // for every script at the end of a sync, after WiFi is down, so renders
    // find a fresh program waiting. `force` recompiles regardless.
    bool compileAndStoreProgram(const String &fileId, bool force = false, String *error = nullptr);
    // Parses source text into a program with no storage involved (the
    // built-in default script, host tools).
    static bool compileSource(const String &source, MpProgram &out, String *error = nullptr);
    // Removes compiled programs whose script is no longer listed.
    void cleanupOrphanedPrograms(const JsonArrayConst &validScriptList);
    // True when a stored program exists that matches the source on flash, i.e.
    // loadProgram() will not have to parse. Lets a caller that is about to
    // render decide whether a compile is about to happen.
    bool programIsFresh(const String &fileId);
    // Compiles every listed script whose program is missing or stale (or all
    // of them, with `force`). Boot calls this once so that no render ever has
    // to compile lazily -- e.g. right after a firmware update, when the stored
    // programs carry an older format version. Returns the number compiled.
    int compileAllPrograms(bool force = false);

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
    static const char *COMPILED_DIR_PATH;
    bool _storageOk = false;

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

    // In-memory copy of list.json.
    //
    // Every read used to go to flash and then re-validate every entry's fileId
    // by stat-ing its content file. On the M5Paper that made a single
    // next-script step cost ~900ms of SPIFFS work -- measured, and far more
    // than the ~250ms panel update it was blamed on. The list only changes when
    // a sync writes it, so it is cached and the cache is dropped on write.
    JsonDocument _listCache;
    bool _listCacheValid = false;
    bool getCurrentScriptId_nolock(String &outHumanId);
    bool saveCurrentScriptId_nolock(const String &humanId);
    bool loadScriptExecutionState_nolock(const String &humanId, ScriptExecState &outState);
    bool saveScriptContent_nolock(const String &fileId, const String &content);
    bool loadScriptContent_nolock(const String &fileId, String &outContent);

    // Writes to `<path>.tmp`, verifies the size, then removes `path` and
    // renames the temp file over it. The old contents survive until the
    // rename, so a reset mid-write costs the write, never the file. (SPIFFS
    // itself is not power-fail-safe; this narrows the window from the whole
    // write to two metadata operations. LittleFS would close it.)
    bool writeFileAtomic_nolock(const String &path, const uint8_t *data, size_t len);
    bool ensureDirectories_nolock();
    String compiledPath(const String &fileId) const;
    // Streams the content file through the CRC without loading it.
    bool sourceFingerprint_nolock(const String &fileId, uint32_t &outLen, uint32_t &outCrc);
    bool loadStoredProgram_nolock(const String &fileId, uint32_t srcLen, uint32_t srcCrc, MpProgram &out);
    bool compileAndStoreProgram_nolock(const String &fileId, bool force, MpProgram *outProgram, String *error);
};

#endif // SCRIPT_MANAGER_H
#ifndef EVENT_DEFS_H
#define EVENT_DEFS_H

#include <Arduino.h>
#include <ArduinoJson.h> // For script list if passed in messages

// --- Max String Lengths for Queue Items ---
#define MAX_SCRIPT_ID_LEN 64
#define MAX_ERROR_MSG_LEN 256
#define MAX_SCRIPT_CONTENT_LEN 2048 // Adjust if scripts can be larger
#define MAX_FETCH_MSG_LEN 128


// --- Script Execution State ---
struct ScriptExecState {
    int counter = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    bool state_loaded = false; // Indicates if this state was loaded or is default
};

// --- Input Task Events ---
enum class InputEventType {
    NONE,
    NEXT_SCRIPT,
    PREVIOUS_SCRIPT,
    CONFIRM_ACTION, // E.g., PUSH button
    // Add more specific events if needed
};

struct InputEvent {
    InputEventType type;
    // uint8_t raw_gpio; // Optional: if MainControlTask needs the raw pin
};


// --- Render Task Communication ---
// Internal String-based version
struct RenderJobData {
    String script_id; // This is human_id
    String file_id;   // This is the fileId for loading content
    String script_content;
    ScriptExecState initial_state;
};

// char[]-based version for queue
struct RenderJobQueueItem {
    char human_id[MAX_SCRIPT_ID_LEN];
    char file_id[MAX_SCRIPT_ID_LEN];
    ScriptExecState initial_state;

    void fromRenderJobData(const RenderJobData& rjd) {
        strncpy(human_id, rjd.script_id.c_str(), MAX_SCRIPT_ID_LEN - 1);
        human_id[MAX_SCRIPT_ID_LEN - 1] = '\0';
        strncpy(file_id, rjd.file_id.c_str(), MAX_SCRIPT_ID_LEN - 1);
        file_id[MAX_SCRIPT_ID_LEN - 1] = '\0';
        // script_content is not copied
        initial_state = rjd.initial_state;
    }

    // This method might need adjustment or be used differently,
    // as script_content is no longer directly in RenderJobQueueItem.
    // RenderTask will load content separately.
    RenderJobData toRenderJobData() const {
        RenderJobData rjd;
        rjd.script_id = String(human_id);
        rjd.file_id = String(file_id);
        // rjd.script_content would be loaded by RenderTask
        rjd.initial_state = initial_state;
        return rjd;
    }
};

// Internal String-based version
struct RenderResultData {
    bool success;
    bool interrupted;
    String error_message;
    String script_id;
    ScriptExecState final_state;
};

// char[]-based version for queue
struct RenderResultQueueItem {
    bool success;
    bool interrupted;
    char error_message[MAX_ERROR_MSG_LEN];
    char script_id[MAX_SCRIPT_ID_LEN];
    ScriptExecState final_state;

    void fromRenderResultData(const RenderResultData& rrd) {
        success = rrd.success;
        interrupted = rrd.interrupted;
        strncpy(script_id, rrd.script_id.c_str(), MAX_SCRIPT_ID_LEN - 1);
        script_id[MAX_SCRIPT_ID_LEN - 1] = '\0';
        strncpy(error_message, rrd.error_message.c_str(), MAX_ERROR_MSG_LEN - 1);
        error_message[MAX_ERROR_MSG_LEN - 1] = '\0';
        final_state = rrd.final_state;
    }

    RenderResultData toRenderResultData() const {
        RenderResultData rrd;
        rrd.success = success;
        rrd.interrupted = interrupted;
        rrd.script_id = String(script_id);
        rrd.error_message = String(error_message);
        rrd.final_state = final_state;
        return rrd;
    }
};


// --- Fetch Task Communication ---
enum class FetchResultStatus {
    SUCCESS,
    GENUINE_ERROR,
    INTERRUPTED_BY_USER,
    NO_WIFI,
    RESTART_REQUESTED
};

// RenderJob already defined, use it if FetchJob is identical or create new.
// For now, FetchJob is simple.
struct FetchJob { // This is simple enough, no Strings, can remain as is.
    bool full_refresh;
};

// Internal String-based version
struct FetchResultData {
    FetchResultStatus status;
    String message;
    bool new_scripts_available;
};

// char[]-based version for queue
struct FetchResultQueueItem {
    FetchResultStatus status;
    char message[MAX_FETCH_MSG_LEN];
    bool new_scripts_available;

    void fromFetchResultData(const FetchResultData& frd) {
        status = frd.status;
        strncpy(message, frd.message.c_str(), MAX_FETCH_MSG_LEN - 1);
        message[MAX_FETCH_MSG_LEN - 1] = '\0';
        new_scripts_available = frd.new_scripts_available;
    }

    FetchResultData toFetchResultData() const {
        FetchResultData frd;
        frd.status = status;
        frd.message = String(message);
        frd.new_scripts_available = new_scripts_available;
        return frd;
    }
};


// --- General Application State (for MainControlTask) ---
enum class AppState {
    IDLE,
    MENU_DISPLAY,
    RENDERING_SCRIPT,
    FETCHING_DATA,
    SHOWING_MESSAGE,
    SLEEPING
    // Add more states as needed
};

#endif // EVENT_DEFS_H
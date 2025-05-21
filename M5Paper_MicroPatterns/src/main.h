#ifndef MAIN_H
#define MAIN_H

#include <M5EPD.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h" // For event flags
#include <ArduinoJson.h>           // For StaticJsonDocument

#include "systeminit.h" // For SysInit_EarlyHardware
#include "event_defs.h" // For all event and queue payload definitions

// Manager Class Headers
#include "system_manager.h"
#include "input_manager.h"
#include "display_manager.h"
#include "script_manager.h"
#include "network_manager.h"
#include "render_controller.h"

// --- Task Configuration ---
#define MAIN_CONTROL_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define INPUT_TASK_PRIORITY (tskIDLE_PRIORITY + 3) // Higher for responsiveness
#define RENDER_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define FETCH_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

#define MAIN_CONTROL_TASK_STACK_SIZE (4096) // Words
#define INPUT_TASK_STACK_SIZE (2048)
#define RENDER_TASK_STACK_SIZE (8192) // Rendering can be heavy
#define FETCH_TASK_STACK_SIZE (8192)  // WiFi/HTTPS needs stack

// --- Task Handles (defined in main.cpp) ---
extern TaskHandle_t g_mainControlTaskHandle;
extern TaskHandle_t g_inputTaskHandle;
extern TaskHandle_t g_renderTaskHandle;
extern TaskHandle_t g_fetchTaskHandle;

// --- Queue Handles (defined in main.cpp) ---
extern QueueHandle_t g_inputEventQueue;    // From InputManager to MainControlTask
extern QueueHandle_t g_renderCommandQueue; // From MainControlTask to RenderTask
extern QueueHandle_t g_renderStatusQueue;  // From RenderTask to MainControlTask
extern QueueHandle_t g_fetchCommandQueue;  // From MainControlTask to FetchTask
extern QueueHandle_t g_fetchStatusQueue;   // From FetchTask to MainControlTask

// --- Event Group Handles (defined in main.cpp) ---
extern EventGroupHandle_t g_appEventGroup; // General application events/flags
// Example event bits for g_appEventGroup (define specific bits as needed)
// extern const EventBits_t WIFI_CONNECTED_BIT;
// extern const EventBits_t FETCH_INTERRUPT_REQUESTED_BIT;
extern EventGroupHandle_t g_renderTaskEventFlags; // For render task specific flags
extern const EventBits_t RENDER_INTERRUPT_BIT;    // Bit to signal render interrupt

// --- Task Function Prototypes ---
void MainControlTask_Function(void *pvParameters);
void InputTask_Function(void *pvParameters);
void RenderTask_Function(void *pvParameters);
void FetchTask_Function(void *pvParameters);

// Global Manager Instances (defined in main.cpp)
// These are pointers because their constructors might need FreeRTOS objects
// that are created in setup(). Or, they can be global objects if constructors are simple.
// For now, let's make them global objects, constructed before setup tasks.
// Ensure their constructors don't do things requiring FreeRTOS scheduler running,
// or move instantiation into setup().
// Plan implies they are initialized in setup(), so pointers are better.
extern SystemManager *g_systemManager;
extern InputManager *g_inputManager;
extern DisplayManager *g_displayManager;
extern ScriptManager *g_scriptManager;
extern NetworkManager *g_networkManager;
// extern RenderController *g_renderController; // RenderController is instantiated by RenderTask

// Note: Original FetchResultStatus enum moved to event_defs.h
// Note: RTC_DATA_ATTR variables like g_full_refresh_intended are now managed by SystemManager.
// Note: Functions like shouldPerformFetch, clearAllScriptDataFromSPIFFS, selectNextScript,
// loadScriptToExecute, handleWakeupAndScriptExecution are refactored into managers or tasks.

#endif // MAIN_H
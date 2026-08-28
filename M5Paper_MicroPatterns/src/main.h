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
#include "script_manager.h" // Also brings in JSON_DOC_CAPACITY_SCRIPT_LIST, JSON_DOC_CAPACITY_SCRIPT_STATES
#include "network_manager.h"
#include "render_controller.h"

// --- Task Configuration ---
#define MAIN_CONTROL_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define INPUT_TASK_PRIORITY (tskIDLE_PRIORITY + 3) // Higher for responsiveness
#define RENDER_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define FETCH_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

// BYTES, not words -- xTaskCreate() on ESP-IDF takes a byte count, and the
// original "// Words" comment here was wrong by a factor of four.
//
// This was 4096 and survived on under 160 bytes of margin. Extracting
// FetchTask's body into script_sync.cpp changed inlining in this translation
// unit, MainControlTask_Function's frame grew 1072 -> 1232 bytes, and the task
// then tripped its stack canary on every boot -- a crash loop caused by a
// refactor that never touched the task. The task runs ArduinoJson parsing and
// SPIFFS calls, so 4KB was never a defensible budget; RenderTask and FetchTask
// have had 8KB all along. High-water mark is logged at startup so the margin is
// observable instead of being a cliff.
#define MAIN_CONTROL_TASK_STACK_SIZE (8192)
#define INPUT_TASK_STACK_SIZE (2048)
#define RENDER_TASK_STACK_SIZE (8192) // Rendering can be heavy
#define FETCH_TASK_STACK_SIZE (8192)  // WiFi/HTTPS needs stack

// --- Task Handles (defined in main.cpp) ---
extern TaskHandle_t g_mainControlTaskHandle;
extern TaskHandle_t g_inputTaskHandle;
extern TaskHandle_t g_serialConsoleTaskHandle;
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

// Mid-render interrupt flag, read from the rasterizer's INNERMOST loops.
//
// RenderController::checkInterrupt() used to answer this question by calling
// xEventGroupGetBits(), which takes a critical section, on every unlatched call.
// The rasterizer polls that callback from its scanline loops in fillRect /
// fillCircle / drawAsset, so a render paid for tens of thousands of FreeRTOS
// critical sections. Measured on the host harness (tools/host_harness) against
// the real rasterizer: a lock-shaped check costs +1% to +3.3% of rasterization
// time versus a plain flag load, and a plain flag load is free versus no check
// at all (+-0.6%, inside noise). It was far worse (+95% to +154%) while the
// drawing layer still polled per pixel, which is where the change originated.
//
// So: MainControlTask sets BOTH this plain flag and RENDER_INTERRUPT_BIT. The
// event bit stays because RenderTask's post-render handshake still reads it;
// this flag is what the hot path polls. Plain `volatile bool` is sufficient --
// it is a single-writer/single-reader boolean latch, never a read-modify-write.
extern volatile bool g_renderInterruptRequested;

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
extern MPNetworkManager *g_networkManager;
// extern RenderController *g_renderController; // RenderController is instantiated by RenderTask

// Note: Original FetchResultStatus enum moved to event_defs.h
// Note: RTC_DATA_ATTR variables like g_full_refresh_intended are now managed by SystemManager.
// Note: Functions like shouldPerformFetch, clearAllScriptDataFromSPIFFS, selectNextScript,
// loadScriptToExecute, handleWakeupAndScriptExecution are refactored into managers or tasks.

#endif // MAIN_H
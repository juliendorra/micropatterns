#include "main.h"
#include "defines/watchy_defines.h" // Watchy specific pins

// Watchy Hardware & HAL
#include <GxEPD2_BW.h>
#include <SmallRTC.h>
#include "WatchyDrawingHAL.h"
#include "MicroPatternsDrawing.h"

// Managers (forward declare or include actual headers once adapted)
#include "SystemManager.h"
#include "InputManager.h"
#include "ScriptManager.h"
#include "RenderController.h"
// #include "NetworkManager.h" // Optional

#include "esp_system.h" // For esp_chip_info_t
#include "esp_idf_version.h" // For ESP-IDF version
#include "nvs_flash.h" // For NVS initialization

// --- Watchy Hardware Instances ---
// For Watchy V1/V2 with GDEH0154D67 1.54" E-Paper display (200x200)
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> watchyDisplay(GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
SmallRTC watchyRTC;

// --- HAL and Drawing Instances ---
WatchyDrawingHAL* g_watchyDrawingHAL = nullptr;
MicroPatternsDrawing* g_microPatternsDrawer = nullptr;

// --- Manager Instances ---
SystemManager* g_systemManager = nullptr;
InputManager* g_inputManager = nullptr;
ScriptManager* g_scriptManager = nullptr;
RenderController* g_renderController = nullptr; // RenderController might be instantiated by RenderTask
// NetworkManager* g_networkManager = nullptr; // Optional

// --- Global Handles (definitions) ---
TaskHandle_t g_mainControlTaskHandle = NULL;
TaskHandle_t g_inputTaskHandle = NULL;
TaskHandle_t g_renderTaskHandle = NULL;
TaskHandle_t g_fetchTaskHandle = NULL; // Optional

QueueHandle_t g_inputEventQueue = NULL;
QueueHandle_t g_renderCommandQueue = NULL;
QueueHandle_t g_renderStatusQueue = NULL;
QueueHandle_t g_fetchCommandQueue = NULL; // Optional
QueueHandle_t g_fetchStatusQueue = NULL;  // Optional

EventGroupHandle_t g_renderTaskEventFlags = NULL;


// --- Early Hardware Initialization (called from setup) ---
static void earlyHardwareInit() {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Watchy specific: Power on peripherals if necessary (e.g. from deep sleep)
    // watchyRTC.begin(); // Ensure RTC is powered if needed before further use
    // Often, the display.init() and rtc.init() handle their own power needs.
}


void setup() {
    Serial.begin(115200);
    // Wait a moment for serial to establish
    // vTaskDelay(pdMS_TO_TICKS(1000)); 
    // Log IDF version (optional)
    // log_i("ESP-IDF Version: %s", esp_get_idf_version());


    // 1. Initialize Hardware
    earlyHardwareInit(); // NVS etc.

    watchyDisplay.init(0); // 0 for default baud, init GFX too
    watchyDisplay.setRotation(0); // Default Watchy rotation

    Wire.begin(RTC_SDA, RTC_SCL);
    watchyRTC.init();
    watchyRTC.setRegularInterrupt(0); // Disable SmallRTC's own regular interrupt if we manage sleep via MainControlTask

    // 2. Instantiate HAL and Core Drawing Logic
    g_watchyDrawingHAL = new WatchyDrawingHAL(watchyDisplay.gfx());
    g_watchyDrawingHAL->initDisplay(); // Initializes HAL, clears screen, etc.
    g_microPatternsDrawer = new MicroPatternsDrawing(g_watchyDrawingHAL);
    
    // Optional: Display a boot message
    g_watchyDrawingHAL->setTextColor(HAL_COLOR_BLACK);
    g_watchyDrawingHAL->setTextSize(1);
    g_watchyDrawingHAL->setCursor(5, 5);
    g_watchyDrawingHAL->print("Booting MicroPatterns...");
    g_watchyDrawingHAL->updateDisplay();
    vTaskDelay(pdMS_TO_TICKS(500));


    // 3. Create Queues and Event Groups
    g_inputEventQueue = xQueueCreate(10, sizeof(InputEvent));
    g_renderCommandQueue = xQueueCreate(1, sizeof(RenderJobQueueItem));
    g_renderStatusQueue = xQueueCreate(1, sizeof(RenderResultQueueItem));
    // g_fetchCommandQueue = xQueueCreate(1, sizeof(FetchJob)); // Optional
    // g_fetchStatusQueue = xQueueCreate(1, sizeof(FetchResultQueueItem)); // Optional
    g_renderTaskEventFlags = xEventGroupCreate();

    if (!g_inputEventQueue || !g_renderCommandQueue || !g_renderStatusQueue || !g_renderTaskEventFlags /* || !g_fetchCommandQueue || !g_fetchStatusQueue */ ) {
        // log_e("FATAL: Failed to create FreeRTOS objects. Halting.");
        g_watchyDrawingHAL->clearScreen();
        g_watchyDrawingHAL->setCursor(5,20); g_watchyDrawingHAL->print("RTOS Queue Fail!");
        g_watchyDrawingHAL->updateDisplay(true);
        while(1) vTaskDelay(portMAX_DELAY);
    }

    // 4. Initialize Managers (Order can be important)
    // These will be adapted/copied from M5Paper version.
    // For now, assume they have default constructors or simple init.
    g_systemManager = new SystemManager(&watchyRTC); // Pass RTC if SystemManager needs it
    if (g_systemManager) g_systemManager->initialize();
    
    g_scriptManager = new ScriptManager();
    if (g_scriptManager) g_scriptManager->initialize(); // Initializes LittleFS

    // RenderController might be instantiated per-task or globally.
    // For now, assume RenderTask will create its own, or pass g_microPatternsDrawer
    // g_renderController = new RenderController(*g_microPatternsDrawer);


    g_inputManager = new InputManager(g_inputEventQueue); // Pass queue to InputManager
    if (g_inputManager) g_inputManager->initialize(); // Sets up GPIOs

    // g_networkManager = new NetworkManager(g_systemManager); // Optional

    // 5. Create Tasks
    xTaskCreatePinnedToCore(MainControlTask_Function, "MainCtrl", MAIN_CONTROL_TASK_STACK_SIZE, NULL, MAIN_CONTROL_TASK_PRIORITY, &g_mainControlTaskHandle, 1);
    xTaskCreatePinnedToCore(InputTask_Function, "Input", INPUT_TASK_STACK_SIZE, NULL, INPUT_TASK_PRIORITY, &g_inputTaskHandle, 1);
    xTaskCreatePinnedToCore(RenderTask_Function, "Render", RENDER_TASK_STACK_SIZE, NULL, RENDER_TASK_PRIORITY, &g_renderTaskHandle, 0);
    // xTaskCreatePinnedToCore(FetchTask_Function, "Fetch", FETCH_TASK_STACK_SIZE, NULL, FETCH_TASK_PRIORITY, &g_fetchTaskHandle, 0); // Optional

    if (!g_mainControlTaskHandle || !g_inputTaskHandle || !g_renderTaskHandle /* || !g_fetchTaskHandle */) {
        // log_e("FATAL: Failed to create one or more tasks. Halting.");
         g_watchyDrawingHAL->clearScreen();
         g_watchyDrawingHAL->setCursor(5,35); g_watchyDrawingHAL->print("RTOS Task Fail!");
         g_watchyDrawingHAL->updateDisplay(true);
        while(1) vTaskDelay(portMAX_DELAY);
    }
    
    // log_i("Setup complete. Tasks created.");
    g_watchyDrawingHAL->setCursor(5,50); g_watchyDrawingHAL->print("Setup OK!");
    g_watchyDrawingHAL->updateDisplay(true);

    // Delete this setup task
    vTaskDelete(NULL);
}

void loop() {
    // This task is deleted in setup(). FreeRTOS tasks handle the main logic.
    vTaskDelay(portMAX_DELAY);
}

// --- Task Function Stubs (to be filled in later steps) ---
void MainControlTask_Function(void *pvParameters) {
    // log_i("MainControlTask started.");
    // esp_task_wdt_init(30, true); // Enable panic WDT
    // esp_task_wdt_add(NULL);
    for (;;) {
        // esp_task_wdt_reset();
        // Handle events from input queue, render status queue, fetch status queue
        // Manage application state, trigger rendering, manage sleep
        vTaskDelay(pdMS_TO_TICKS(1000));
         // log_d("MainControlTask alive");
    }
}

void InputTask_Function(void *pvParameters) {
    // log_i("InputTask started.");
    // esp_task_wdt_init(30, true);
    // esp_task_wdt_add(NULL);
    // This task will be entirely replaced by the adapted InputManager's taskFunction
    if (g_inputManager) {
        g_inputManager->taskFunction(); // This should be a blocking loop
    }
    for (;;) { // Fallback loop if taskFunction returns
        // esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
        // log_d("InputTask (stub) alive");
    }
}

void RenderTask_Function(void *pvParameters) {
    // log_i("RenderTask started.");
    // esp_task_wdt_init(60, true); // Longer timeout for rendering
    // esp_task_wdt_add(NULL);
    
    // RenderController could be local to this task
    // RenderController renderCtrl(*g_microPatternsDrawer); 

    for (;;) {
        // esp_task_wdt_reset();
        // Wait for render command from g_renderCommandQueue
        // Execute rendering using RenderController
        // Send status to g_renderStatusQueue
        vTaskDelay(pdMS_TO_TICKS(1000));
        // log_d("RenderTask alive");
    }
}

void FetchTask_Function(void *pvParameters) { // Optional
    // log_i("FetchTask started.");
    // esp_task_wdt_init(120, true); // Long timeout for network
    // esp_task_wdt_add(NULL);
    for (;;) {
        // esp_task_wdt_reset();
        // Wait for fetch command
        // Execute network operations
        // Send status
        vTaskDelay(pdMS_TO_TICKS(1000));
        // log_d("FetchTask alive");
    }
}

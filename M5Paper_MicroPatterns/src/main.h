#ifndef MAIN_H
#define MAIN_H

#include <M5EPD.h>
#include "systeminit.h"
#include "micropatterns_parser.h"
#include "micropatterns_runtime.h"
#include "micropatterns_drawing.h"
#include "global_setting.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <set>
#include <vector> // Added for state cleanup logic

// Enum for fetch operation results
enum FetchResultStatus
{
    FETCH_SUCCESS,
    FETCH_GENUINE_ERROR,
    FETCH_INTERRUPTED_BY_USER, // Hard interrupt
    FETCH_NO_WIFI,
    FETCH_RESTART_REQUESTED // Graceful restart
};

// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// Function declarations
void setup();
void loop();
void displayMessage(const String &msg, int y_offset = 50, uint16_t color = 15);
void displayParseErrors();
void handleWakeupAndScriptExecution(uint8_t raw_gpio_from_isr); // Modified signature
void fetchTaskFunction(void *pvParameters);
FetchResultStatus perform_fetch_operations(bool isFullRefresh); // Modified to accept isFullRefresh
bool selectNextScript(bool moveUp);
bool loadScriptToExecute();
bool shouldPerformFetch(const char* caller);  // Helper to check if fetch should be performed based on time criteria
void clearAllScriptDataFromSPIFFS();      // New helper function

// RTC Data Attribute for full refresh intent
extern RTC_DATA_ATTR bool g_full_refresh_intended;

#endif // MAIN_H
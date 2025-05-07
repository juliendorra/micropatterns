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
void handleWakeupAndScriptExecution();
void fetchTaskFunction(void *pvParameters);
bool selectNextScript(bool moveUp);
bool loadScriptToExecute();

#endif // MAIN_H
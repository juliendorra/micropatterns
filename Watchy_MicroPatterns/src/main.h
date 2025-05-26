#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "event_defs.h" // Should exist from M5Paper

// --- Task Stack Sizes & Priorities (example values, adjust as needed) ---
#define MAIN_CONTROL_TASK_STACK_SIZE 4096
#define INPUT_TASK_STACK_SIZE        3072
#define RENDER_TASK_STACK_SIZE       8192 // Rendering can be stack intensive
#define FETCH_TASK_STACK_SIZE        4096

#define MAIN_CONTROL_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define INPUT_TASK_PRIORITY        (tskIDLE_PRIORITY + 3) // Higher for responsiveness
#define RENDER_TASK_PRIORITY       (tskIDLE_PRIORITY + 1)
#define FETCH_TASK_PRIORITY        (tskIDLE_PRIORITY + 1)

// --- Global Handles (declared here, defined in main.cpp) ---
// Task Handles
extern TaskHandle_t g_mainControlTaskHandle;
extern TaskHandle_t g_inputTaskHandle;
extern TaskHandle_t g_renderTaskHandle;
extern TaskHandle_t g_fetchTaskHandle; // Optional

// Queue Handles
extern QueueHandle_t g_inputEventQueue;    // For InputEvent
extern QueueHandle_t g_renderCommandQueue; // For RenderJobQueueItem
extern QueueHandle_t g_renderStatusQueue;  // For RenderResultQueueItem
extern QueueHandle_t g_fetchCommandQueue;  // For FetchJob (Optional)
extern QueueHandle_t g_fetchStatusQueue;   // For FetchResultQueueItem (Optional)

// Event Group Handles
extern EventGroupHandle_t g_renderTaskEventFlags; // For render interrupts

// Forward declarations for task functions
void MainControlTask_Function(void *pvParameters);
void InputTask_Function(void *pvParameters);
void RenderTask_Function(void *pvParameters);
void FetchTask_Function(void *pvParameters); // Optional

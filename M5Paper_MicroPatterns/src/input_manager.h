#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "event_defs.h" // For InputEvent, InputEventType

// Define button GPIOs (consistent with original global_setting.h)
#define BUTTON_UP_PIN GPIO_NUM_37
#define BUTTON_DOWN_PIN GPIO_NUM_39
#define BUTTON_PUSH_PIN GPIO_NUM_38

class InputManager
{
public:
    InputManager(QueueHandle_t inputEventQueue);
    bool initialize();
    void taskFunction(); // Public for xTaskCreate, or make static and pass 'this'

private:
    QueueHandle_t _inputEventQueue_to_main_ctrl; // Queue to send logical events to MainControlTask
    QueueHandle_t _rawInputQueue_internal;       // Internal queue for ISR to post raw pin events

    volatile uint32_t _lastButtonTime[GPIO_NUM_MAX]; // Tracks last interrupt time per button
    static const uint32_t DEBOUNCE_TIME_MS = 200;    // Debounce time

    // ISR handler - must be static or global
    static void IRAM_ATTR button_isr_handler(void *arg);

    // Task-context processing of raw inputs
    void processRawInput(uint8_t gpio_num);
};

#endif // INPUT_MANAGER_H
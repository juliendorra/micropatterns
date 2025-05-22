#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "event_defs.h" // For InputEvent, InputEventType
#include "driver/gpio.h" // Required for gpio_num_t

// Define button GPIOs (consistent with original global_setting.h)
#define BUTTON_UP_PIN GPIO_NUM_37
#define BUTTON_DOWN_PIN GPIO_NUM_39
#define BUTTON_PUSH_PIN GPIO_NUM_38

// Enum to track the processing state of each pin
enum class PinProcessingState {
    IDLE_ISR_ENABLED,             // Waiting for an interrupt, ISR is active
    ISR_TRIGGERED_ISR_DISABLED,   // ISR has fired and disabled itself, task will now debounce
    CONFIRMED_PRESS_ISR_DISABLED  // Debounce confirmed a press, ISR remains disabled, polling for release
};

class InputManager
{
public:
    InputManager(QueueHandle_t inputEventQueue);
    bool initialize();
    void taskFunction(); // Public for xTaskCreate, or make static and pass 'this'

private:
    QueueHandle_t _inputEventQueue_to_main_ctrl; // Queue to send logical events to MainControlTask
    QueueHandle_t _rawInputQueue_internal;       // Internal queue for ISR to post raw pin events

    volatile uint32_t _lastSentEventTime[GPIO_NUM_MAX]; // Tracks time of last sent logical event
    volatile PinProcessingState _pin_processing_state[GPIO_NUM_MAX]; // Tracks current state of each pin

    static const uint32_t ISR_EVENT_DEBOUNCE_DELAY_MS = 50;  // Physical debounce delay after ISR
    static const uint32_t LOGICAL_EVENT_MIN_INTERVAL_MS = 200; // Min interval between logical events for the same pin

    // ISR handler - must be static or global
    static void IRAM_ATTR button_isr_handler(void *arg);

    // Task-context processing of raw inputs (removed, logic integrated into taskFunction)
    // void processRawInput(uint8_t gpio_num);
};

#endif // INPUT_MANAGER_H
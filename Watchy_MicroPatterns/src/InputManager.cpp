#include "input_manager.h"
#include "esp32-hal-log.h"
// driver/gpio.h is included via input_manager.h
#include "esp_task_wdt.h" // For esp_task_wdt_reset
#include "main.h"         // For g_displayManager and ActivityIndicatorType (via display_manager.h)

// Declare the global queue handle that the ISR will use.
// This handle is defined in main.cpp and initialized by InputManager's constructor.
extern QueueHandle_t g_im_raw_queue_ref;

InputManager::InputManager(QueueHandle_t inputEventQueue_to_main_ctrl_param)
    : _inputEventQueue_to_main_ctrl(inputEventQueue_to_main_ctrl_param) {
    for (int i = 0; i < GPIO_NUM_MAX; ++i) {
        _lastSentEventTime[i] = 0;
        _pin_processing_state[i] = PinProcessingState::IDLE_ISR_ENABLED; // Initialize all to idle
    }
    _rawInputQueue_internal = xQueueCreate(10, sizeof(uint8_t)); // Queue for 10 raw pin numbers
    if (_rawInputQueue_internal == NULL) {
        log_e("InputManager: Failed to create _rawInputQueue_internal!");
    } else {
        if (g_im_raw_queue_ref == NULL) {
            g_im_raw_queue_ref = _rawInputQueue_internal;
            log_i("InputManager: g_im_raw_queue_ref set by constructor.");
        } else {
            log_w("InputManager: g_im_raw_queue_ref was already set. Current _rawInputQueue_internal: %p, g_im_raw_queue_ref: %p", _rawInputQueue_internal, g_im_raw_queue_ref);
        }
    }
}

bool InputManager::initialize() {
    log_i("InputManager initializing...");

    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.pin_bit_mask = (1ULL << BUTTON_UP_PIN) | (1ULL << BUTTON_DOWN_PIN) | (1ULL << BUTTON_PUSH_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        log_e("InputManager: GPIO config failed: %s", esp_err_to_name(err));
        return false;
    }

    // ISR service should be installed by SysInit_EarlyHardware.

    err = gpio_isr_handler_add(BUTTON_UP_PIN, button_isr_handler, (void*)BUTTON_UP_PIN);
    if (err != ESP_OK) log_e("Failed to add ISR for BUTTON_UP_PIN: %s", esp_err_to_name(err));
    else gpio_intr_enable(BUTTON_UP_PIN); // Explicitly enable after adding

    err = gpio_isr_handler_add(BUTTON_DOWN_PIN, button_isr_handler, (void*)BUTTON_DOWN_PIN);
    if (err != ESP_OK) log_e("Failed to add ISR for BUTTON_DOWN_PIN: %s", esp_err_to_name(err));
    else gpio_intr_enable(BUTTON_DOWN_PIN); // Explicitly enable after adding
    
    err = gpio_isr_handler_add(BUTTON_PUSH_PIN, button_isr_handler, (void*)BUTTON_PUSH_PIN);
    if (err != ESP_OK) log_e("Failed to add ISR for BUTTON_PUSH_PIN: %s", esp_err_to_name(err));
    else gpio_intr_enable(BUTTON_PUSH_PIN); // Explicitly enable after adding

    // Initialize pin states correctly after ISR setup
    _pin_processing_state[BUTTON_UP_PIN] = PinProcessingState::IDLE_ISR_ENABLED;
    _pin_processing_state[BUTTON_DOWN_PIN] = PinProcessingState::IDLE_ISR_ENABLED;
    _pin_processing_state[BUTTON_PUSH_PIN] = PinProcessingState::IDLE_ISR_ENABLED;

    log_i("InputManager initialized successfully. ISRs enabled for managed pins.");
    return true;
}

void IRAM_ATTR InputManager::button_isr_handler(void* arg) {
    uint32_t gpio_num_uint32 = (uint32_t)arg;
    // Immediately disable the interrupt for this pin to prevent storm
    gpio_intr_disable(static_cast<gpio_num_t>(gpio_num_uint32));
    
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    extern QueueHandle_t g_im_raw_queue_ref; // Use the global reference
    
    if (g_im_raw_queue_ref != NULL) {
        // Send the gpio_num (as uint8_t if it fits, or handle type conversion)
        // Assuming gpio_num_uint32 fits in uint8_t for this queue.
        uint8_t gpio_num_u8 = (uint8_t)gpio_num_uint32;
        xQueueSendFromISR(g_im_raw_queue_ref, &gpio_num_u8, &xHigherPriorityTaskWoken);
    }
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}


void InputManager::taskFunction() {
    log_i("InputManager Task started. Waiting for raw inputs from _rawInputQueue_internal.");
    const TickType_t queueReceiveTimeout = pdMS_TO_TICKS(50); // Poll for releases every 50ms

    for (;;) {
        esp_task_wdt_reset();
        uint8_t raw_gpio_num; // GPIO number received from ISR queue

        if (xQueueReceive(_rawInputQueue_internal, &raw_gpio_num, queueReceiveTimeout) == pdTRUE) {
            // An ISR was triggered, and it disabled its own interrupt.
            // Mark state as ISR_TRIGGERED_ISR_DISABLED (or similar, to indicate task is processing it)
            // This state change is implicit: we received it, so ISR is disabled.
            log_d("InputTask: Received raw event from ISR for GPIO %d. ISR is now disabled for this pin.", raw_gpio_num);
            _pin_processing_state[raw_gpio_num] = PinProcessingState::ISR_TRIGGERED_ISR_DISABLED;

            // Perform physical debounce delay
            vTaskDelay(pdMS_TO_TICKS(ISR_EVENT_DEBOUNCE_DELAY_MS));

            bool pin_is_low_after_debounce = (digitalRead(raw_gpio_num) == LOW);
            uint32_t current_time = millis();

            if (pin_is_low_after_debounce) {
                // Pin is still LOW: Confirmed press
                log_d("InputTask: GPIO %d confirmed LOW after %ums debounce.", raw_gpio_num, ISR_EVENT_DEBOUNCE_DELAY_MS);
                _pin_processing_state[raw_gpio_num] = PinProcessingState::CONFIRMED_PRESS_ISR_DISABLED;

                // Draw activity indicator immediately with the correct type
                ActivityIndicatorType indicatorType = ACTIVITY_PUSH; // Default for safety
                if (raw_gpio_num == BUTTON_UP_PIN) {
                    indicatorType = ACTIVITY_UP;
                } else if (raw_gpio_num == BUTTON_DOWN_PIN) {
                    indicatorType = ACTIVITY_DOWN;
                } else if (raw_gpio_num == BUTTON_PUSH_PIN) {
                    indicatorType = ACTIVITY_PUSH;
                }

                if (g_displayManager) {
                    g_displayManager->drawActivityIndicator(indicatorType);
                } else {
                    log_e("InputTask: g_displayManager is NULL, cannot draw activity indicator.");
                }

                // Check if enough time has passed since the last logical event for this pin
                if ((current_time - _lastSentEventTime[raw_gpio_num]) >= LOGICAL_EVENT_MIN_INTERVAL_MS || current_time < _lastSentEventTime[raw_gpio_num] /* overflow */) {
                    InputEvent event;
                    event.type = InputEventType::NONE;
                    if (raw_gpio_num == BUTTON_UP_PIN) event.type = InputEventType::PREVIOUS_SCRIPT;
                    else if (raw_gpio_num == BUTTON_DOWN_PIN) event.type = InputEventType::NEXT_SCRIPT;
                    else if (raw_gpio_num == BUTTON_PUSH_PIN) event.type = InputEventType::CONFIRM_ACTION;

                    if (event.type != InputEventType::NONE) {
                        if (_inputEventQueue_to_main_ctrl != NULL) {
                            if (xQueueSend(_inputEventQueue_to_main_ctrl, &event, pdMS_TO_TICKS(10)) == pdTRUE) {
                                log_i("InputTask: Sent logical event %d for GPIO %d.", (int)event.type, raw_gpio_num);
                                _lastSentEventTime[raw_gpio_num] = current_time; // Update time only if event sent
                            } else {
                                log_e("InputTask: Failed to send logical event to _inputEventQueue_to_main_ctrl for GPIO %d.", raw_gpio_num);
                                // If send fails, ISR is still disabled. Pin state is CONFIRMED_PRESS_ISR_DISABLED.
                                // It will be polled for release.
                            }
                        } else {
                            log_e("InputTask: _inputEventQueue_to_main_ctrl is NULL!");
                        }
                    }
                } else {
                    log_d("InputTask: GPIO %d press confirmed, but logical event rate-limited. LastSent: %u, Curr: %u", raw_gpio_num, _lastSentEventTime[raw_gpio_num], current_time);
                }
                // Regardless of logical event sending, ISR for raw_gpio_num remains disabled.
                // State is CONFIRMED_PRESS_ISR_DISABLED, will be polled for release.
            } else {
                // Pin is HIGH: Noise or bounce resolved
                log_d("InputTask: GPIO %d was noise (HIGH after %ums debounce). Re-enabling ISR.", raw_gpio_num, ISR_EVENT_DEBOUNCE_DELAY_MS);
                gpio_intr_enable(static_cast<gpio_num_t>(raw_gpio_num));
                _pin_processing_state[raw_gpio_num] = PinProcessingState::IDLE_ISR_ENABLED;
            }
        } else {
            // Queue receive timed out. Poll for releases of any buttons in CONFIRMED_PRESS_ISR_DISABLED state.
            const uint8_t managed_pins[] = {BUTTON_UP_PIN, BUTTON_DOWN_PIN, BUTTON_PUSH_PIN};
            for (uint8_t pin_to_check : managed_pins) {
                if (_pin_processing_state[pin_to_check] == PinProcessingState::CONFIRMED_PRESS_ISR_DISABLED) {
                    if (digitalRead(pin_to_check) == HIGH) { // Button released
                        log_i("InputTask: GPIO %d detected HIGH on poll (released). Re-enabling ISR.", pin_to_check);
                        gpio_intr_enable(static_cast<gpio_num_t>(pin_to_check));
                        _pin_processing_state[pin_to_check] = PinProcessingState::IDLE_ISR_ENABLED;
                        // Optionally, send a "button released" logical event here if needed by MainControlTask
                    }
                }
            }
        }
    }
}

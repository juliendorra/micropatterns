#include "input_manager.h"
#include "esp32-hal-log.h"
#include "driver/gpio.h"

// Declare the global queue handle that the ISR will use.
// This handle is defined in main.cpp and initialized by InputManager's constructor.
extern QueueHandle_t g_im_raw_queue_ref;

InputManager::InputManager(QueueHandle_t inputEventQueue_to_main_ctrl_param)
    : _inputEventQueue_to_main_ctrl(inputEventQueue_to_main_ctrl_param) {
    for (int i = 0; i < GPIO_NUM_MAX; ++i) {
        _lastButtonTime[i] = 0;
    }
    _rawInputQueue_internal = xQueueCreate(10, sizeof(uint8_t)); // Queue for 10 raw pin numbers
    if (_rawInputQueue_internal == NULL) {
        log_e("InputManager: Failed to create _rawInputQueue_internal!");
    } else {
        // Set the global queue reference for the ISR if it hasn't been set yet.
        // This ensures the ISR uses the queue created by this InputManager instance.
        if (g_im_raw_queue_ref == NULL) {
            g_im_raw_queue_ref = _rawInputQueue_internal;
            log_i("InputManager: g_im_raw_queue_ref set by constructor.");
        } else {
            // This case should ideally not happen if InputManager is a singleton.
            log_w("InputManager: g_im_raw_queue_ref was already set. Current _rawInputQueue_internal: %p, g_im_raw_queue_ref: %p", _rawInputQueue_internal, g_im_raw_queue_ref);
        }
    }
}

bool InputManager::initialize() {
    log_i("InputManager initializing...");

    // Configure GPIOs
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_NEGEDGE; // Interrupt on falling edge (button press)
    io_conf.pin_bit_mask = (1ULL << BUTTON_UP_PIN) | (1ULL << BUTTON_DOWN_PIN) | (1ULL << BUTTON_PUSH_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        log_e("InputManager: GPIO config failed: %s", esp_err_to_name(err));
        return false;
    }

    // Install ISR service if not already installed (usually done once globally)
    // Assuming it's installed by SystemInit or main setup. If not, add:
    // if (gpio_install_isr_service(0) != ESP_OK) {
    //     log_e("InputManager: Failed to install ISR service.");
    //     // return false; // This might fail if already installed. Check error code.
    // }


    // Add ISR handlers for each button
    // Pass the GPIO pin number as the argument to the static ISR handler
    err = gpio_isr_handler_add(BUTTON_UP_PIN, button_isr_handler, (void*)BUTTON_UP_PIN);
    if (err != ESP_OK) log_e("Failed to add ISR for BUTTON_UP_PIN: %s", esp_err_to_name(err));

    err = gpio_isr_handler_add(BUTTON_DOWN_PIN, button_isr_handler, (void*)BUTTON_DOWN_PIN);
    if (err != ESP_OK) log_e("Failed to add ISR for BUTTON_DOWN_PIN: %s", esp_err_to_name(err));
    
    err = gpio_isr_handler_add(BUTTON_PUSH_PIN, button_isr_handler, (void*)BUTTON_PUSH_PIN);
    if (err != ESP_OK) log_e("Failed to add ISR for BUTTON_PUSH_PIN: %s", esp_err_to_name(err));

    log_i("InputManager initialized successfully.");
    return true;
}

void IRAM_ATTR InputManager::button_isr_handler(void* arg) {
    // The arg is the GPIO pin number that triggered the interrupt
    uint32_t gpio_num = (uint32_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Use the global queue reference declared in main.cpp
    extern QueueHandle_t g_im_raw_queue_ref;
    
    if (g_im_raw_queue_ref != NULL) {
        xQueueSendFromISR(g_im_raw_queue_ref, &gpio_num, &xHigherPriorityTaskWoken);
    }
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}


void InputManager::taskFunction() {
    // The global g_im_raw_queue_ref is set by the constructor.
    // This task reads from its own instance member _rawInputQueue_internal.

    uint8_t raw_gpio_num;
    log_i("InputManager Task started. Waiting for raw inputs from _rawInputQueue_internal.");

    for (;;) {
        if (xQueueReceive(_rawInputQueue_internal, &raw_gpio_num, portMAX_DELAY) == pdTRUE) {
            // Check if this pin is one we manage (it should be if ISR is set up correctly)
            if (raw_gpio_num != BUTTON_UP_PIN && raw_gpio_num != BUTTON_DOWN_PIN && raw_gpio_num != BUTTON_PUSH_PIN) {
                log_w("InputTask: Received ISR event for unmanaged GPIO %d", raw_gpio_num);
                continue;
            }

            uint32_t current_time = millis();
            // Debounce logic:
            // Check if enough time has passed since the last accepted press for THIS button.
            if ((current_time - _lastButtonTime[raw_gpio_num]) >= DEBOUNCE_TIME_MS || current_time < _lastButtonTime[raw_gpio_num] /* handle millis overflow */ ) {
                
                // Additional check: verify the button is still pressed (helps with noise)
                if (digitalRead(raw_gpio_num) == LOW) {
                    _lastButtonTime[raw_gpio_num] = current_time;
                    log_i("InputTask: Debounced input from GPIO %d", raw_gpio_num);

                    InputEvent event;
                    event.type = InputEventType::NONE;

                    if (raw_gpio_num == BUTTON_UP_PIN) {
                        event.type = InputEventType::PREVIOUS_SCRIPT; // Or map to generic UP
                    } else if (raw_gpio_num == BUTTON_DOWN_PIN) {
                        event.type = InputEventType::NEXT_SCRIPT; // Or map to generic DOWN
                    } else if (raw_gpio_num == BUTTON_PUSH_PIN) {
                        event.type = InputEventType::CONFIRM_ACTION; // Or map to generic SELECT/CONFIRM
                    }

                    if (event.type != InputEventType::NONE) {
                        if (_inputEventQueue_to_main_ctrl != NULL) {
                            if (xQueueSend(_inputEventQueue_to_main_ctrl, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
                                log_e("InputTask: Failed to send logical event to _inputEventQueue_to_main_ctrl.");
                            } else {
                                log_i("InputTask: Sent logical event %d", (int)event.type);
                            }
                        } else {
                            log_e("InputTask: _inputEventQueue_to_main_ctrl is NULL!");
                        }
                    }
                } else {
                    log_d("InputTask: GPIO %d was not LOW on re-check, likely noise.", raw_gpio_num);
                }
            } else {
                log_d("InputTask: GPIO %d event debounced out (time). Last: %u, Curr: %u, Diff: %u", raw_gpio_num, _lastButtonTime[raw_gpio_num], current_time, current_time - _lastButtonTime[raw_gpio_num]);
            }
        }
    }
}
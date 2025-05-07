#include "systeminit.h"
#include "global_setting.h" // For LoadSetting, initializeSPIFFS, getNTPTime
#include <rtc.h>            // For RTC_TimeTypeDef

// WiFi/NTP constants and functions moved to global_setting.cpp/h

void SysInit_Start(void)
{
    pinMode(M5EPD_MAIN_PWR_PIN, OUTPUT);
    M5.enableMainPower();

    bool ret = false;
    Serial.begin(115200);
    Serial.flush();
    delay(50);
    Serial.print("M5EPD initializing...");

    // Initialize watchdog early
    esp_task_wdt_init(30, false); // 30 second timeout, don't panic on timeout
    esp_task_wdt_add(NULL);       // Add current task to watchdog

    // Initialize GPIOs for buttons early
    pinMode(BUTTON_UP_PIN, INPUT_PULLUP);   // GPIO 37 (UP)
    pinMode(BUTTON_DOWN_PIN, INPUT_PULLUP); // GPIO 39 (DOWN)
    pinMode(BUTTON_PUSH_PIN, INPUT_PULLUP); // GPIO 38 (PUSH)

    // Power Pins
    pinMode(M5EPD_EXT_PWR_EN_PIN, OUTPUT);
    pinMode(M5EPD_EPD_PWR_EN_PIN, OUTPUT);
    // Other Key Pins (if needed, though we use 37,38,39)
    // pinMode(M5EPD_KEY_RIGHT_PIN, INPUT);
    // pinMode(M5EPD_KEY_LEFT_PIN, INPUT);
    delay(100);

    M5.enableEXTPower();
    M5.enableEPDPower();
    delay(1000); // Wait for power stabilization

    M5.BatteryADCBegin();
    log_i("Battery Voltage: %d mV", M5.getBatteryVoltage());

    M5.RTC.begin();

    // Initialize SPIFFS *before* potentially needing it for NTP fallback time?
    if (!initializeSPIFFS())
    {
        log_e("FATAL: SPIFFS Initialization failed!");
        // Handle error appropriately, maybe halt or display error
        while (1)
            delay(1000);
    }

    // Get time via NTP (uses WiFi functions from global_setting)
    getNTPTime();

    // Initialize EPD
    M5.EPD.begin(M5EPD_SCK_PIN, M5EPD_MOSI_PIN, M5EPD_MISO_PIN, M5EPD_CS_PIN,
                 M5EPD_BUSY_PIN);
    M5.EPD.SetRotation(M5EPD_Driver::ROTATE_90);
    // M5.EPD.Clear(true); // Avoid full clear on startup unless necessary

    // Initialize Touch Panel
    if (M5.TP.begin(21, 22, 36) != ESP_OK)
    {
        log_e("Touch pad initialization failed.");
    }
    else
    {
        M5.TP.SetRotation(GT911::ROTATE_90);
    }
    taskYIELD();

    // Load NVS settings (like timezone)
    LoadSetting();

    // Setup Interrupt Service Routine
    gpio_install_isr_service(0);

    // Configure interrupts for each button to trigger on falling edge (press)
    gpio_set_intr_type(BUTTON_UP_PIN, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(BUTTON_UP_PIN, button_isr, (void *)BUTTON_UP_PIN);

    gpio_set_intr_type(BUTTON_DOWN_PIN, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(BUTTON_DOWN_PIN, button_isr, (void *)BUTTON_DOWN_PIN);

    gpio_set_intr_type(BUTTON_PUSH_PIN, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(BUTTON_PUSH_PIN, button_isr, (void *)BUTTON_PUSH_PIN);

    log_i("System Initialization done");
    Serial.println("OK");

    delay(500); // Keep a small delay
}
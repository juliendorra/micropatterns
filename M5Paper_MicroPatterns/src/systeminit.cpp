#include "systeminit.h"
#include "global_setting.h" // Keep for LoadSetting
#include <WiFi.h>           // Keep for potential future use

// Removed _initcanvas, xQueue_Info, WaitForUser, Screen_Test, SysInit_Loading, SysInit_UpdateInfo

void SysInit_Start(void)
{
    bool ret = false;
    Serial.begin(115200);
    Serial.flush();
    delay(50);
    Serial.print("M5EPD initializing...");

    pinMode(M5EPD_EXT_PWR_EN_PIN, OUTPUT);
    pinMode(M5EPD_EPD_PWR_EN_PIN, OUTPUT);
    pinMode(M5EPD_KEY_RIGHT_PIN, INPUT);
    pinMode(M5EPD_KEY_PUSH_PIN, INPUT);
    pinMode(M5EPD_KEY_LEFT_PIN, INPUT);
    delay(100);

    M5.enableEXTPower();
    // M5.disableEPDPower(); // Keep commented out
    // delay(500);
    M5.enableEPDPower();
    delay(1000);

    M5.BatteryADCBegin();

    log_e("Battery Voltage: %d", M5.getBatteryVoltage());

    M5.EPD.begin(M5EPD_SCK_PIN, M5EPD_MOSI_PIN, M5EPD_MISO_PIN, M5EPD_CS_PIN,
                 M5EPD_BUSY_PIN);

    // Don't do a full clear (true) which causes black blanking
    // Either use false (buffer clear only) or remove entirely
    // M5.EPD.Clear(false);  // Only clear buffer, not the physical display
    M5.EPD.SetRotation(M5EPD_Driver::ROTATE_90);
    M5.TP.SetRotation(GT911::ROTATE_90);

    // SysInit_UpdateInfo("Initializing SD card..."); // Removed loading info update
    ret = SD.begin(4, *M5.EPD.GetSPI(), 20000000);
    if (ret == false)
    {
        // SetInitStatus(0, 0); // Removed - part of UI
        log_e("Failed to initialize SD card.");
        // SysInit_UpdateInfo("[ERROR] Failed to initialize SD card."); // Removed loading info update
        // WaitForUser(); // Removed
    }
    else
    {
        // is_factory_test = SD.exists("/__factory_test_flag__"); // Removed - factory test logic not needed now
    }

    // SysInit_UpdateInfo("Initializing Touch pad..."); // Removed loading info update
    if (M5.TP.begin(21, 22, 36) != ESP_OK)
    {
        // SetInitStatus(1, 0); // Removed - part of UI
        log_e("Touch pad initialization failed.");
        // SysInit_UpdateInfo("[ERROR] Failed to initialize Touch pad."); // Removed loading info update
        // WaitForUser(); // Removed
    }
    taskYIELD();

    M5.BatteryADCBegin();
    LoadSetting(); // Keep loading basic settings like timezone if needed

    log_d("System Initialization done");

    Serial.println("OK");

    delay(500); // Keep a small delay
}
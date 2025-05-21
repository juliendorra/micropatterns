#include "systeminit.h"
#include "esp32-hal-log.h"
#include "nvs_flash.h" // For nvs_flash_init, nvs_flash_erase
#include "nvs.h" // For NVS error codes
// No longer includes global_setting.h for most things.
// Specific managers will handle their own initializations.

// This function handles essential pre-M5.begin() initializations.
// M5.begin() will handle Serial, Power, RTC, EPD.
void SysInit_EarlyHardware(void)
{
    // Initialize NVS (Non-Volatile Storage)
    // This is crucial for SystemManager and other components that might use NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    log_i("SysInit_EarlyHardware: NVS flash initialized.");
    
    // Install global ISR service once. InputManager will add handlers to it.
    // It's safe to call multiple times; it only installs if not already done.
    esp_err_t isr_service_err = gpio_install_isr_service(0); // ESP_INTR_FLAG_IRAM or 0
    if (isr_service_err == ESP_OK) {
        log_i("SysInit_EarlyHardware: GPIO ISR service installed.");
    } else if (isr_service_err == ESP_ERR_NOT_FOUND || isr_service_err == ESP_ERR_NO_MEM) {
        log_e("SysInit_EarlyHardware: Failed to install GPIO ISR service: %s", esp_err_to_name(isr_service_err));
        // This is a critical failure for button inputs.
    } else {
        // ESP_ERR_INVALID_STATE means already installed, which is fine.
        if (isr_service_err != ESP_ERR_INVALID_STATE) {
            log_w("SysInit_EarlyHardware: GPIO ISR service status: %s", esp_err_to_name(isr_service_err));
        } else {
            log_i("SysInit_EarlyHardware: GPIO ISR service already installed.");
        }
    }

    log_i("SysInit_EarlyHardware: Minimal pre-M5.begin() setup completed.");
}
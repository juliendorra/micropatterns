#include "global_setting.h"
// Removed: #include "./resources/ImageResource.h"
#include "esp32-hal-log.h"
#include <WiFi.h> // Keep for potential future use (e.g., time sync)

#define DEFAULT_TIMEZONE 0 // Default timezone offset

// Removed: SemaphoreHandle_t _xSemaphore_LoadingAnime = NULL;
// Removed: static uint8_t _loading_anime_eixt_flag    = false;
esp_err_t __espret__;
#define NVS_CHECK(x)            \
    __espret__ = x;             \
    if (__espret__ != ESP_OK) { \
        nvs_close(nvs_arg);     \
        log_e("Check Err");     \
        return __espret__;      \
    }

// Removed: const uint8_t *kIMGLoading[16] = { ... };

int8_t global_timezone = DEFAULT_TIMEZONE;

int8_t GetTimeZone(void) {
    return global_timezone;
}

void SetTimeZone(int8_t time_zone) {
    global_timezone = time_zone;
}

esp_err_t LoadSetting(void) {
    nvs_handle nvs_arg;
    esp_err_t err = nvs_open("Setting", NVS_READONLY, &nvs_arg);
    if (err != ESP_OK) {
        log_e("NVS Open failed");
        return err;
    }
    nvs_get_i8(nvs_arg, "timezone", &global_timezone);
    nvs_close(nvs_arg);
    return ESP_OK;
}

esp_err_t SaveSetting(void) {
    nvs_handle nvs_arg;
    esp_err_t err = nvs_open("Setting", NVS_READWRITE, &nvs_arg);
     if (err != ESP_OK) {
        log_e("NVS Open failed");
        return err;
    }
    NVS_CHECK(nvs_set_i8(nvs_arg, "timezone", global_timezone));
    NVS_CHECK(nvs_commit(nvs_arg));
    nvs_close(nvs_arg);
    return ESP_OK;
}

// Removed: GetLoadingIMG_32x32 function
// Removed: __LoadingAnime_32x32 task function
// Removed: LoadingAnime_32x32_Start function
// Removed: LoadingAnime_32x32_Stop function

void Shutdown() {
    log_d("Shutdown requested.");
    // Optional: Display a shutdown message
    M5EPD_Canvas shutdownCanvas(&M5.EPD);
    // Use hardcoded dimensions 540x960 for M5Paper
    shutdownCanvas.createCanvas(540, 960);
    shutdownCanvas.fillCanvas(0); // White background
    shutdownCanvas.setTextSize(3); // Use a reasonable size
    shutdownCanvas.setTextColor(15); // Black text
    shutdownCanvas.setTextDatum(CC_DATUM);
    shutdownCanvas.drawString("Shutting down...", shutdownCanvas.width() / 2, shutdownCanvas.height() / 2);
    shutdownCanvas.pushCanvas(0, 0, UPDATE_MODE_GC16); // Full update
    shutdownCanvas.deleteCanvas();

    SaveSetting(); // Save any final settings if needed
    delay(1000); // Wait for display update

    M5.disableEPDPower();
    M5.disableEXTPower();
    M5.disableMainPower();
    esp_deep_sleep_start();
    while (1); // Should not reach here
}
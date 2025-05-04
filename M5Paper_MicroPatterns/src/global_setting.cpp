#include "global_setting.h"
#include <M5EPD.h>
#include <esp_sleep.h> // For esp_sleep_enable_ext0_wakeup, esp_deep_sleep_start
#include "esp32-hal-log.h"
#include <WiFi.h> // Keep for potential future use (e.g., time sync)
#include <rtc.h>  // For RTC_TimeTypeDef

#define DEFAULT_TIMEZONE 0 // Default timezone offset

#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP 1877        /* Time ESP32 will go to sleep (in seconds) */

// Removed: SemaphoreHandle_t _xSemaphore_LoadingAnime = NULL;
// Removed: static uint8_t _loading_anime_eixt_flag    = false;
esp_err_t __espret__;
#define NVS_CHECK(x)          \
    __espret__ = x;           \
    if (__espret__ != ESP_OK) \
    {                         \
        nvs_close(nvs_arg);   \
        log_e("Check Err");   \
        return __espret__;    \
    }

// Removed: const uint8_t *kIMGLoading[16] = { ... };

int8_t global_timezone = DEFAULT_TIMEZONE;

int8_t GetTimeZone(void)
{
    return global_timezone;
}

void SetTimeZone(int8_t time_zone)
{
    global_timezone = time_zone;
}

esp_err_t LoadSetting(void)
{
    nvs_handle nvs_arg;
    esp_err_t err = nvs_open("Setting", NVS_READONLY, &nvs_arg);
    if (err != ESP_OK)
    {
        log_e("NVS Open failed");
        return err;
    }
    nvs_get_i8(nvs_arg, "timezone", &global_timezone);
    nvs_close(nvs_arg);
    return ESP_OK;
}

esp_err_t SaveSetting(void)
{
    nvs_handle nvs_arg;
    esp_err_t err = nvs_open("Setting", NVS_READWRITE, &nvs_arg);
    if (err != ESP_OK)
    {
        log_e("NVS Open failed");
        return err;
    }
    NVS_CHECK(nvs_set_i8(nvs_arg, "timezone", global_timezone));
    NVS_CHECK(nvs_commit(nvs_arg));
    nvs_close(nvs_arg);
    return ESP_OK;
}

void Shutdown()
{
    log_d("Shutdown requested.");

    // https://github.com/espressif/arduino-esp32/issues/4903
    // WiFi.setSleep(WIFI_PS_NONE);

    SaveSetting(); // Save any final settings if needed

    // delay(1000); // Wait for display update

    // Configure wake-up timer (5 seconds)
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    log_i("Setup ESP32 to wake up after %d seconds", TIME_TO_SLEEP);

    esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, LOW);

    // Get current time from RTC
    // RTC_Time timeStruct;
    // M5.RTC.getTime(&timeStruct);

    // Create rtc_time_t structure required by shutdown()
    // rtc_time_t rtcTimeStruct;
    // rtcTimeStruct.hour = timeStruct.hour;
    // rtcTimeStruct.min = timeStruct.min;
    // rtcTimeStruct.sec = timeStruct.sec + TIME_TO_SLEEP;

    // log_i("Going to sleep at %02d:%02d:%02d", rtcTimeStruct.hour, rtcTimeStruct.min, rtcTimeStruct.sec);

    // by default, the ESP32 automatically powers down the peripherals that are not needed with the wake up source you define.
    // M5.disableEPDPower();
    // M5.disableEXTPower();
    // M5.disableMainPower();

    // Pass the rtc_time_t structure to shutdown()
    // shutdown(TIME_TO_SLEEP);

    // This code will only execute if shutdown fails
    // log_e("Shutdown failed, falling back to esp_deep_sleep_start");

    gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
    gpio_deep_sleep_hold_en();

    esp_light_sleep_start();
}
#include "system_manager.h"
#include "network_manager.h" // For NetworkManager reference in syncTimeWithNTP
#include <WiFi.h>          // For WiFi status check, should be through NetworkManager
#include "time.h"          // For NTP time struct
#include "esp32-hal-log.h"
#include "esp_task_wdt.h"  // For esp_task_wdt_reset()

// NVS Keys
const char* SystemManager::NVS_NAMESPACE = "sys_mgr";
const char* SystemManager::NVS_KEY_TIMEZONE = "timezone";
const char* SystemManager::NVS_KEY_FRESH_START_COUNT = "fresh_start_cnt";
const char* SystemManager::NVS_KEY_FULL_REFRESH_INTENT = "full_ref_int";
const char* SystemManager::NVS_KEY_LAST_FETCH_YEAR = "lf_year";
const char* SystemManager::NVS_KEY_LAST_FETCH_MONTH = "lf_month";
const char* SystemManager::NVS_KEY_LAST_FETCH_DAY = "lf_day";
const char* SystemManager::NVS_KEY_LAST_FETCH_HOUR = "lf_hour";
const char* SystemManager::NVS_KEY_LAST_FETCH_MINUTE = "lf_min";

const char* SystemManager::NTP_SERVER_DEFAULT = "pool.ntp.org";
const uint32_t SystemManager::DEFAULT_SLEEP_DURATION_S = 77;


SystemManager::SystemManager() :
    _timezone(1), // Default timezone
    _freshStartCounter(0),
    _fullRefreshIntended(false),
    _lastFetchYear(-1),
    _lastFetchMonth(-1),
    _lastFetchDay(-1),
    _lastFetchHour(-1),
    _lastFetchMinute(-1)
     {}

bool SystemManager::initialize() {
    log_i("SystemManager initializing...");
    // M5.enableMainPower() is usually called very early in main setup or SysInit_Start
    // M5.RTC.begin() is also called early.

    if (!loadSettings()) {
        log_w("Failed to load system settings from NVS. Using defaults and attempting to save.");
        // Attempt to save defaults if loading failed, to initialize NVS
        if (!saveSettings()) {
            log_e("FATAL: Failed to save initial system settings to NVS.");
            return false; // Critical failure
        }
    }
    log_i("SystemManager initialized. Timezone: %d, FreshStartCounter: %d, FullRefreshIntended: %s", _timezone, _freshStartCounter, _fullRefreshIntended ? "true" : "false");
    return true;
}

bool SystemManager::openNVS(nvs_open_mode_t open_mode) {
    esp_err_t err = nvs_open(NVS_NAMESPACE, open_mode, &_nvsHandle);
    if (err != ESP_OK) {
        log_e("NVS open failed for namespace '%s', mode %d: %s", NVS_NAMESPACE, open_mode, esp_err_to_name(err));
        return false;
    }
    return true;
}

void SystemManager::closeNVS() {
    if (_nvsHandle != 0) {
        nvs_close(_nvsHandle);
        _nvsHandle = 0;
    }
}

bool SystemManager::loadSettings() {
    if (!openNVS(NVS_READONLY)) return false;

    esp_err_t err;
    err = nvs_get_i8(_nvsHandle, NVS_KEY_TIMEZONE, &_timezone);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) log_w("NVS: Failed to get timezone: %s", esp_err_to_name(err));
    
    err = nvs_get_i32(_nvsHandle, NVS_KEY_FRESH_START_COUNT, &_freshStartCounter);
     if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) log_w("NVS: Failed to get fresh_start_cnt: %s", esp_err_to_name(err));

    uint8_t fullRefreshIntended_u8 = 0;
    err = nvs_get_u8(_nvsHandle, NVS_KEY_FULL_REFRESH_INTENT, &fullRefreshIntended_u8);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) log_w("NVS: Failed to get full_refresh_int: %s", esp_err_to_name(err));
    else _fullRefreshIntended = (fullRefreshIntended_u8 == 1);

    err = nvs_get_i32(_nvsHandle, NVS_KEY_LAST_FETCH_YEAR, &_lastFetchYear);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) log_w("NVS: Failed to get lf_year: %s", esp_err_to_name(err));
    err = nvs_get_i32(_nvsHandle, NVS_KEY_LAST_FETCH_MONTH, &_lastFetchMonth);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) log_w("NVS: Failed to get lf_month: %s", esp_err_to_name(err));
    err = nvs_get_i32(_nvsHandle, NVS_KEY_LAST_FETCH_DAY, &_lastFetchDay);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) log_w("NVS: Failed to get lf_day: %s", esp_err_to_name(err));
    err = nvs_get_i32(_nvsHandle, NVS_KEY_LAST_FETCH_HOUR, &_lastFetchHour);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) log_w("NVS: Failed to get lf_hour: %s", esp_err_to_name(err));
    err = nvs_get_i32(_nvsHandle, NVS_KEY_LAST_FETCH_MINUTE, &_lastFetchMinute);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) log_w("NVS: Failed to get lf_min: %s", esp_err_to_name(err));

    closeNVS();
    // ESP_ERR_NVS_NOT_FOUND is okay on first boot, defaults will be used.
    return true;
}

bool SystemManager::saveSettings() {
    if (!openNVS(NVS_READWRITE)) return false;

    esp_err_t err;
    err = nvs_set_i8(_nvsHandle, NVS_KEY_TIMEZONE, _timezone);
    if (err != ESP_OK) { log_e("NVS: Failed to set timezone: %s", esp_err_to_name(err)); closeNVS(); return false; }
    
    err = nvs_set_i32(_nvsHandle, NVS_KEY_FRESH_START_COUNT, _freshStartCounter);
    if (err != ESP_OK) { log_e("NVS: Failed to set fresh_start_cnt: %s", esp_err_to_name(err)); closeNVS(); return false; }

    uint8_t fullRefreshIntended_u8 = _fullRefreshIntended ? 1 : 0;
    err = nvs_set_u8(_nvsHandle, NVS_KEY_FULL_REFRESH_INTENT, fullRefreshIntended_u8);
    if (err != ESP_OK) { log_e("NVS: Failed to set full_refresh_int: %s", esp_err_to_name(err)); closeNVS(); return false; }

    err = nvs_set_i32(_nvsHandle, NVS_KEY_LAST_FETCH_YEAR, _lastFetchYear);
    if (err != ESP_OK) { log_e("NVS: Failed to set lf_year: %s", esp_err_to_name(err)); closeNVS(); return false; }
    err = nvs_set_i32(_nvsHandle, NVS_KEY_LAST_FETCH_MONTH, _lastFetchMonth);
    if (err != ESP_OK) { log_e("NVS: Failed to set lf_month: %s", esp_err_to_name(err)); closeNVS(); return false; }
    err = nvs_set_i32(_nvsHandle, NVS_KEY_LAST_FETCH_DAY, _lastFetchDay);
    if (err != ESP_OK) { log_e("NVS: Failed to set lf_day: %s", esp_err_to_name(err)); closeNVS(); return false; }
    err = nvs_set_i32(_nvsHandle, NVS_KEY_LAST_FETCH_HOUR, _lastFetchHour);
    if (err != ESP_OK) { log_e("NVS: Failed to set lf_hour: %s", esp_err_to_name(err)); closeNVS(); return false; }
    err = nvs_set_i32(_nvsHandle, NVS_KEY_LAST_FETCH_MINUTE, _lastFetchMinute);
    if (err != ESP_OK) { log_e("NVS: Failed to set lf_min: %s", esp_err_to_name(err)); closeNVS(); return false; }

    err = nvs_commit(_nvsHandle);
    if (err != ESP_OK) { log_e("NVS: Failed to commit settings: %s", esp_err_to_name(err)); closeNVS(); return false; }

    closeNVS();
    log_i("System settings saved to NVS.");
    return true;
}

bool SystemManager::syncTimeWithNTP(NetworkManager& netMgr) {
    if (!netMgr.isConnected()) { // Use NetworkManager's status check
        log_i("NTP Sync: WiFi not connected. Attempting to connect...");
        if (!netMgr.connectWiFi(pdMS_TO_TICKS(15000))) { // 15s timeout for NTP sync
            log_e("NTP Sync: WiFi connection failed. Cannot sync time.");
            // netMgr.disconnectWiFi(); // Ensure WiFi is off if connection failed
            return false;
        }
    }

    const long gmtOffset_sec = _timezone * 3600;
    const int daylightOffset_sec = 0; // Adjust if needed

    log_i("Configuring time with GMT offset %ld sec, DST offset %d sec, NTP server %s", gmtOffset_sec, daylightOffset_sec, NTP_SERVER_DEFAULT);
    configTime(gmtOffset_sec, daylightOffset_sec, NTP_SERVER_DEFAULT);

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10000)) { // 10s timeout for getLocalTime
        log_e("Failed to obtain NTP time after connection.");
        // netMgr.disconnectWiFi(); // Disconnect if time sync failed
        return false;
    }

    printLocalTimeAndSetRTC(timeinfo);
    // netMgr.disconnectWiFi(); // Disconnect WiFi after successful time sync
    return true;
}

void SystemManager::printLocalTimeAndSetRTC(struct tm &timeinfo) {
    log_i("NTP time obtained: %s", asctime(&timeinfo));

    RTC_Time rtcTime;
    rtcTime.hour = timeinfo.tm_hour;
    rtcTime.min = timeinfo.tm_min;
    rtcTime.sec = timeinfo.tm_sec;
    M5.RTC.setTime(&rtcTime);

    RTC_Date rtcDate;
    rtcDate.day = timeinfo.tm_mday;
    rtcDate.mon = timeinfo.tm_mon + 1;      // tm_mon is 0-11
    rtcDate.year = timeinfo.tm_year + 1900; // tm_year is years since 1900
    M5.RTC.setDate(&rtcDate);

    log_i("RTC set to: %d-%02d-%02d %02d:%02d:%02d", rtcDate.year, rtcDate.mon, rtcDate.day, rtcTime.hour, rtcTime.min, rtcTime.sec);
}


RTC_Time SystemManager::getTime() {
    RTC_Time t;
    M5.RTC.getTime(&t);
    return t;
}

RTC_Date SystemManager::getDate() {
    RTC_Date d;
    M5.RTC.getDate(&d);
    return d;
}

void SystemManager::configureWakeupSources() {
    // 1. Timer
    esp_sleep_enable_timer_wakeup(DEFAULT_SLEEP_DURATION_S * 1000000ULL);
    log_i("Setup ESP32 to wake up after %d seconds.", DEFAULT_SLEEP_DURATION_S);

    // 2. GPIOs (Buttons UP, DOWN, PUSH) - Wake on LOW level
    // Pins are defined in global_settings.h or will be in input_manager.h
    // For now, assuming direct use of defines like BUTTON_UP_PIN
    // These should ideally come from InputManager or be passed in.
    // Using fixed values for now as per original global_setting.h
    gpio_wakeup_enable(GPIO_NUM_37, GPIO_INTR_LOW_LEVEL); // BUTTON_UP_PIN
    gpio_wakeup_enable(GPIO_NUM_39, GPIO_INTR_LOW_LEVEL); // BUTTON_DOWN_PIN
    gpio_wakeup_enable(GPIO_NUM_38, GPIO_INTR_LOW_LEVEL); // BUTTON_PUSH_PIN
    esp_sleep_enable_gpio_wakeup();
    log_i("Setup ESP32 to wake up on LOW level for GPIOs 37, 39, 38.");
}

void SystemManager::goToLightSleep(TickType_t sleepDurationSec, WakeupCallback onWakeup) {
    log_i("Preparing for light sleep...");
    esp_task_wdt_reset();
    
    if (!saveSettings()) { // Save any final settings
        log_w("Failed to save settings before sleep.");
    }
    esp_task_wdt_reset();

    configureWakeupSources(); // Configure wake-up sources

    // If sleepDurationSec was passed as 0 or some other indicator for default, use DEFAULT_SLEEP_DURATION_S
    // For now, assume sleepDurationSec is the intended duration or use DEFAULT_SLEEP_DURATION_S if it's a specific value (e.g. 0)
    // The call from main.cpp uses a hardcoded 77, which matches DEFAULT_SLEEP_DURATION_S.
    // To be more explicit, main.cpp could pass SystemManager::DEFAULT_SLEEP_DURATION_S.
    // For this change, we assume sleepDurationSec is the value to use.
    // If the intent is to always use the default from this function if called with a specific value:
    // TickType_t actualSleepDurationSec = (sleepDurationSec == 77) ? DEFAULT_SLEEP_DURATION_S : sleepDurationSec;
    // log_i("Entering light sleep for %lu seconds...", actualSleepDurationSec);
    // For now, just log the passed value.
    log_i("Entering light sleep for %lu seconds...", sleepDurationSec);


    // M5.disableEPDPower(); // Optional: Power down EPD
    // M5.disableEXTPower(); // Optional: Power down external components

    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_light_sleep_start();
    
    // --- Woke up ---
    esp_task_wdt_reset();
    log_i("Woke up from light sleep. Cause: %d", esp_sleep_get_wakeup_cause());

    disableWakeupSources(); // Disable GPIO wakeups after waking

    // M5.enableEPDPower(); // Re-enable power if disabled
    // M5.enableEXTPower();
    // vTaskDelay(pdMS_TO_TICKS(100)); // Allow power to stabilize

    if (onWakeup) {
        onWakeup();
    }
}

esp_sleep_wakeup_cause_t SystemManager::getWakeupCause() {
    return esp_sleep_get_wakeup_cause();
}

void SystemManager::disableWakeupSources() {
    gpio_wakeup_disable(GPIO_NUM_37);
    gpio_wakeup_disable(GPIO_NUM_39);
    gpio_wakeup_disable(GPIO_NUM_38);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    log_i("Disabled GPIO wakeup sources.");
}


// --- Getters and Setters for NVS-backed properties ---
int8_t SystemManager::getTimezone() const { return _timezone; }
void SystemManager::setTimezone(int8_t tz) { _timezone = tz; /* Consider immediate save or flag for saving */ }

int SystemManager::getFreshStartCounter() const { return _freshStartCounter; }
void SystemManager::incrementFreshStartCounter() { _freshStartCounter++; }
void SystemManager::resetFreshStartCounter() { _freshStartCounter = 0; }

bool SystemManager::isFullRefreshIntended() const { return _fullRefreshIntended; }
void SystemManager::setFullRefreshIntended(bool intended) { _fullRefreshIntended = intended; }

void SystemManager::getLastFetchTimestamp(int& year, int& month, int& day, int& hour, int& minute) {
    year = _lastFetchYear;
    month = _lastFetchMonth;
    day = _lastFetchDay;
    hour = _lastFetchHour;
    minute = _lastFetchMinute;
}

void SystemManager::updateLastFetchTimestamp() {
    RTC_Time rtcTime = getTime();
    RTC_Date rtcDate = getDate();
    _lastFetchYear = rtcDate.year;
    _lastFetchMonth = rtcDate.mon;
    _lastFetchDay = rtcDate.day;
    _lastFetchHour = rtcTime.hour;
    _lastFetchMinute = rtcTime.min;
    log_i("Updated last fetch timestamp in SystemManager: %d-%02d-%02d %02d:%02d", _lastFetchYear, _lastFetchMonth, _lastFetchDay, _lastFetchHour, _lastFetchMinute);
    // Consider if saveSettings() should be called here or batched.
}
#include "system_manager.h"
#include "network_manager.h" // For NetworkManager reference in syncTimeWithNTP
#include <WiFi.h>          // For WiFi status check, should be through NetworkManager
#include "time.h"          // For NTP time struct
#include "esp32-hal-log.h"
#include "esp_task_wdt.h"  // For esp_task_wdt_reset()
#include "global_setting.h" // For button pin definitions

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

    // RTC_Time rtcTime; // M5Paper type
    // rtcTime.hour = timeinfo.tm_hour;
    // rtcTime.min = timeinfo.tm_min;
    // rtcTime.sec = timeinfo.tm_sec;
    // M5.RTC.setTime(&rtcTime); // M5Paper call

    // RTC_Date rtcDate; // M5Paper type
    // rtcDate.day = timeinfo.tm_mday;
    // rtcDate.mon = timeinfo.tm_mon + 1;      // tm_mon is 0-11
    // rtcDate.year = timeinfo.tm_year + 1900; // tm_year is years since 1900
    // M5.RTC.setDate(&rtcDate); // M5Paper call

    // Watchy RTC setting
    Watchy::RTC.setDateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                            timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    log_i("RTC set to: %d-%02d-%02d %02d:%02d:%02d", 
          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
}

::RTC_Date SystemManager::getDateTime() { // Changed from getTime/getDate
    return Watchy::RTC.getRtcDateTime();
}

void SystemManager::configureWakeupSources(bool forDeepSleep) {
    if (forDeepSleep) {
        // For Watchy deep sleep, RTC alarm is primary.
        // Button wakeups are usually configured by Watchy::deepSleep itself if it uses ESP_EXT1_WAKEUP.
        // Or, if Watchy uses a simpler RTC + timer deep sleep, buttons might not wake it directly from deep sleep
        // without additional configuration not typically managed here.
        // For now, this method will focus on timer based wakeup for light sleep or simple deep sleep.
        // Watchy::setWakeupAlarm() is the proper way to set RTC alarm for Watchy.
        log_i("Deep sleep wakeup configuration is primarily handled by Watchy::deepSleep or Watchy::setWakeupAlarm.");
        // Example: esp_sleep_enable_ext0_wakeup(RTC_IO_T4, 0); // Example for one button on RTC IO pin
    } else {
        // Timer wakeup (e.g., for light sleep or periodic deep sleep if Watchy::deepSleep isn't used)
        // esp_sleep_enable_timer_wakeup(DEFAULT_SLEEP_DURATION_S * 1000000ULL);
        // log_i("Setup ESP32 to wake up via timer after %d seconds (if used for light/simple deep sleep).", DEFAULT_SLEEP_DURATION_S);

        // GPIO wakeups for light sleep (if Watchy framework doesn't handle button wake from light sleep)
        // Using placeholder pins from global_setting.h. These should align with Watchy's actual pins.
        // Note: Watchy handles button wakeups from deep sleep usually via RTC EXT1 or by restarting and checking pins.
        // This direct GPIO configuration is more for generic ESP32 light sleep.
        uint64_t mask = 0;
        mask |= (1ULL << BUTTON_UP_PIN);
        mask |= (1ULL << BUTTON_DOWN_PIN);
        mask |= (1ULL << BUTTON_MENU_PIN); // BUTTON_PUSH_PIN maps to MENU
        mask |= (1ULL << BUTTON_BACK_PIN); 
        esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH); // Or ALL_LOW depending on Watchy button circuit. Watchy buttons are active HIGH.
        log_i("Configured EXT1 wakeup for Watchy buttons (pins: UP=%d, DOWN=%d, MENU=%d, BACK=%d) - ACTIVE HIGH.", BUTTON_UP_PIN, BUTTON_DOWN_PIN, BUTTON_MENU_PIN, BUTTON_BACK_PIN);
    }
}

void SystemManager::goToSleep(TickType_t sleepDurationSec) { // Was goToLightSleep, removed WakeupCallback
    log_i("SystemManager::goToSleep called. Preparing for Watchy deep sleep...");
    esp_task_wdt_reset();
    
    if (!saveSettings()) { 
        log_w("Failed to save settings before sleep.");
    }
    esp_task_wdt_reset();

    // Watchy's deep sleep mechanism:
    // 1. Set RTC alarm for timed wakeup (if sleepDurationSec > 0)
    // 2. Configure button pins for wakeup (Watchy::deepSleep does this)
    // 3. Enter deep sleep

    if (sleepDurationSec > 0) {
        // Example: Set RTC alarm
        // Watchy::RTC.setAlarm(Watchy::RTC.rtc_time.hour, Watchy::RTC.rtc_time.minute + (sleepDurationSec / 60), ...); 
        // This is complex. For now, let Watchy's default behavior in main loop handle timed sleep if any,
        // or just use a simple button-wake deep sleep.
        log_i("Timed deep sleep for %lu seconds requested. Watchy's main loop should handle RTC alarm.", sleepDurationSec);
        // For a simple timed deep sleep without full Watchy class integration for RTC alarm:
        // esp_sleep_enable_timer_wakeup(sleepDurationSec * 1000000ULL);
    } else {
        log_i("Deep sleep until button press requested.");
        // No timer wakeup, rely on button configured by Watchy::deepSleep or configureWakeupSources
    }
    
    // Call Watchy's own deep sleep method which handles RTC alarm for next minute wakeup by default,
    // and EXT1 wakeup for buttons.
    // If sleepDurationSec is used, Watchy::setWakeupAlarm would be called before this.
    // For now, let's assume we want button wakeup.
    // configureWakeupSources(true); // true for deep sleep specific config if any (like EXT0)
                                 // Watchy::deepSleep usually configures EXT1.

    // Watchy::display.powerDown(); // Handled by Watchy::deepSleep or if display object is destroyed.
    
    log_i("Entering deep sleep via Watchy::deepSleep()...");
    Watchy::deepSleep(); // This function configures wake sources (RTC, buttons) and calls esp_deep_sleep_start()
    
    // Code below this point should not be reached if esp_deep_sleep_start() is called.
    // On wakeup, the ESP32 resets and starts from setup().
}


esp_sleep_wakeup_cause_t SystemManager::getWakeupCause() {
    return esp_sleep_get_wakeup_cause();
}

void SystemManager::disableWakeupSources() {
    // This might not be strictly necessary if Watchy's deepSleep and wakeup handling
    // reconfigures pins appropriately. However, for light sleep or non-Watchy deep sleep, it's good.
    // Using placeholder pins from global_setting.h
    gpio_wakeup_disable(static_cast<gpio_num_t>(BUTTON_UP_PIN));
    gpio_wakeup_disable(static_cast<gpio_num_t>(BUTTON_DOWN_PIN));
    gpio_wakeup_disable(static_cast<gpio_num_t>(BUTTON_MENU_PIN)); // Was BUTTON_PUSH_PIN
    gpio_wakeup_disable(static_cast<gpio_num_t>(BUTTON_BACK_PIN)); 
    // esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO); // Deprecated, use esp_sleep_disable_ext0_wakeup or ext1
    // If ext1 was used:
    // No direct "disable ext1" function, but reconfiguring pins or sleep modes handles it.
    // For now, just disabling individual GPIOs from wakeup path.
    log_i("Disabled individual GPIO wakeup configurations (if any were set manually).");
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
    ::RTC_Date rtcDateTime = getDateTime(); // Use the new combined getter
    _lastFetchYear = rtcDateTime.Year;
    _lastFetchMonth = rtcDateTime.Month;
    _lastFetchDay = rtcDateTime.Day;
    _lastFetchHour = rtcDateTime.Hour;
    _lastFetchMinute = rtcDateTime.Minute;
    log_i("Updated last fetch timestamp in SystemManager: %d-%02d-%02d %02d:%02d", _lastFetchYear, _lastFetchMonth, _lastFetchDay, _lastFetchHour, _lastFetchMinute);
    _lastFetchDay = rtcDate.day;
    _lastFetchHour = rtcTime.hour;
    _lastFetchMinute = rtcTime.min;
    log_i("Updated last fetch timestamp in SystemManager: %d-%02d-%02d %02d:%02d", _lastFetchYear, _lastFetchMonth, _lastFetchDay, _lastFetchHour, _lastFetchMinute);
    // Consider if saveSettings() should be called here or batched.
}

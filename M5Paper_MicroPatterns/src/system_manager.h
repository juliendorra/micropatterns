#ifndef SYSTEM_MANAGER_H
#define SYSTEM_MANAGER_H

#include <M5EPD.h>
#include <nvs.h>
#include <esp_sleep.h>
#include "event_defs.h" // For ScriptExecState if needed, or other system-wide defs

// Forward declaration
class NetworkManager; // If SystemManager needs to interact with NetworkManager (e.g. for WiFi credentials)

typedef void (*WakeupCallback)();

class SystemManager
{
public:
    SystemManager();
    bool initialize(); // General system initialization

    // RTC and Time
    bool syncTimeWithNTP(NetworkManager &netMgr); // Pass NetworkManager for WiFi
    RTC_Time getTime();
    RTC_Date getDate();
    void printLocalTimeAndSetRTC(struct tm &timeinfo); // Helper for NTP

    // Power Management
    void goToLightSleep(TickType_t sleepDurationSec, WakeupCallback onWakeup = nullptr); // Specify duration
    esp_sleep_wakeup_cause_t getWakeupCause();
    void configureWakeupSources(); // Configures GPIO and Timer wakeups
    void disableWakeupSources();   // Disables GPIO wakeups after waking

    // NVS Settings
    bool loadSettings(); // Loads timezone, fetch state, etc.
    bool saveSettings(); // Saves timezone, fetch state, etc.

    int8_t getTimezone() const;
    void setTimezone(int8_t tz);

    int getFreshStartCounter() const;
    void incrementFreshStartCounter();
    void resetFreshStartCounter();

    bool isFullRefreshIntended() const;
    void setFullRefreshIntended(bool intended);

    // Fetch timing related (persisted in NVS)
    void getLastFetchTimestamp(int &year, int &month, int &day, int &hour, int &minute);
    void updateLastFetchTimestamp();

private:
    // NVS handle management
    nvs_handle_t _nvsHandle;
    bool openNVS(nvs_open_mode_t open_mode);
    void closeNVS();

    // Settings managed by SystemManager (persisted in NVS)
    int8_t _timezone;
    int _freshStartCounter;
    bool _fullRefreshIntended;
    int _lastFetchYear;
    int _lastFetchMonth;
    int _lastFetchDay;
    int _lastFetchHour;
    int _lastFetchMinute;

    // Constants
    static const char *NVS_NAMESPACE;
    static const char *NVS_KEY_TIMEZONE;
    static const char *NVS_KEY_FRESH_START_COUNT;
    static const char *NVS_KEY_FULL_REFRESH_INTENT; // Shortened to "full_ref_int" in .cpp
    static const char *NVS_KEY_LAST_FETCH_YEAR;
    static const char *NVS_KEY_LAST_FETCH_MONTH;
    static const char *NVS_KEY_LAST_FETCH_DAY;
    static const char *NVS_KEY_LAST_FETCH_HOUR;
    static const char *NVS_KEY_LAST_FETCH_MINUTE;

    static const char *NTP_SERVER_DEFAULT;

public:
    // Constants made public for external access
    static const uint32_t DEFAULT_SLEEP_DURATION_S;
};

#endif // SYSTEM_MANAGER_H
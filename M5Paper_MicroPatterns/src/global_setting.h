#ifndef _GLOBAL_SETTING_H_
#define _GLOBAL_SETTING_H_

#include <Arduino.h> // For basic types if needed

// This file is significantly reduced. Most settings and functions
// have been moved to their respective manager classes (SystemManager,
// InputManager, ScriptManager, NetworkManager, DisplayManager).

// --- Potentially remaining global constants (if any truly global and not fitting elsewhere) ---
// Example:
// extern const char* SOME_TRULY_GLOBAL_CONSTANT;

// --- Forward declarations for any remaining global utility functions (if any) ---
// Most utility functions should now be part of a manager class.

// GPIO Pin definitions (can also be in a dedicated pins.h or within InputManager)
// These are used by InputManager and SystemManager (for wakeup sources)
// If InputManager is the sole user, they can be private to it.
// If SystemManager also needs them for esp_sleep_enable_gpio_wakeup, they need to be accessible.
// For now, keep them accessible if multiple modules need them.
#define BUTTON_UP_PIN GPIO_NUM_37
#define BUTTON_DOWN_PIN GPIO_NUM_39
#define BUTTON_PUSH_PIN GPIO_NUM_38

// Note: Most extern variables like wakeup_pin, g_last_button_time, g_fetchRequestSemaphore,
// g_user_interrupt_signal_for_fetch_task, g_wakeup_handled, global_timezone, SCRIPT_STATES_PATH,
// WIFI_SSID, WIFI_PASSWORD, API_BASE_URL, rootCACertificate, NTP_SERVER
// have been moved into their respective manager classes or replaced by new mechanisms (queues, flags).

// Functions like connectToWiFi, getNTPTime, SPIFFS functions, LoadSetting, SaveSetting, goToLightSleep
// are now methods of their respective manager classes.

#endif // _GLOBAL_SETTING_H_
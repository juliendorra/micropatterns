#ifndef _GLOBAL_SETTING_H_
#define _GLOBAL_SETTING_H_

#include <Watchy.h> // Include Watchy.h to get access to BTN_UP, BTN_DOWN etc.

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

// Use Watchy library defined button pins directly.
// These are typically defined in Watchy.h or board_config.h included by Watchy.h
// Example: #define BTN_UP 32 (actual value depends on Watchy version/board config)
// We will map the old names to the Watchy names.

#define BUTTON_UP_PIN BTN_UP       // Maps to Watchy's UP button pin
#define BUTTON_DOWN_PIN BTN_DOWN     // Maps to Watchy's DOWN button pin
#define BUTTON_MENU_PIN BTN_MENU     // Maps to Watchy's MENU button pin
#define BUTTON_BACK_PIN BTN_BACK     // Maps to Watchy's BACK button pin

// Mapping M5Paper concepts to Watchy placeholders:
#define BUTTON_PUSH_PIN BUTTON_MENU_PIN // M5Paper PUSH maps to Watchy MENU (or BTN_SELECT if available)

// Note: Most extern variables like wakeup_pin, g_last_button_time, g_fetchRequestSemaphore,
// g_user_interrupt_signal_for_fetch_task, g_wakeup_handled, global_timezone, SCRIPT_STATES_PATH,
// WIFI_SSID, WIFI_PASSWORD, API_BASE_URL, rootCACertificate, NTP_SERVER
// have been moved into their respective manager classes or replaced by new mechanisms (queues, flags).

// Functions like connectToWiFi, getNTPTime, SPIFFS functions, LoadSetting, SaveSetting, goToLightSleep
// are now methods of their respective manager classes.

#endif // _GLOBAL_SETTING_H_

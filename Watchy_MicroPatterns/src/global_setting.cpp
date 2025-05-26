#include "global_setting.h"
#include "esp32-hal-log.h" // For logging if any init remains

// This file is significantly reduced. Most settings and functions
// have been moved to their respective manager classes.

// --- Definitions for any remaining global constants ---
// Example:
// const char* SOME_TRULY_GLOBAL_CONSTANT = "value";

// All functions previously here (button_isr, WiFi functions, SPIFFS functions,
// NVS settings, sleep functions) have been moved to:
// - InputManager (button handling, ISR logic)
// - NetworkManager (WiFi, HTTP, NTP constants)
// - ScriptManager (SPIFFS operations, script state)
// - SystemManager (NVS settings, sleep logic, RTC/NTP coordination)

// If this file becomes empty, it can be removed from the build,
// or kept as a placeholder for future truly global settings.
// For now, it's kept minimal.

// Note: The original `button_isr` is now part of `InputManager`'s logic,
// likely with a static ISR trampoline or a global ISR that posts to InputManager's queue.
// The `wakeup_pin` and related debounce logic are also within `InputManager`.
// WiFi credentials, API URLs, NTP server, and CA cert are now typically consts within `NetworkManager.cpp`
// or loaded from configuration by `NetworkManager`/`SystemManager`.
// SPIFFS paths are consts within `ScriptManager.cpp`.
// Timezone and other NVS settings are handled by `SystemManager`.
// Sleep logic is in `SystemManager`.

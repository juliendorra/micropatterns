#ifndef _GLOBAL_SETTING_H_
#define _GLOBAL_SETTING_H_

#include <M5EPD.h>
#include <nvs.h>
#include <esp_task_wdt.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ArduinoJson.h> // Include ArduinoJson

// --- Constants ---
// WiFi Credentials
extern const char* WIFI_SSID;
extern const char* WIFI_PASSWORD;
// NTP Server
extern const char* NTP_SERVER;
// API Endpoint
extern const char* API_BASE_URL;
// Root CA Certificate for HTTPS
extern const char* rootCACertificate;
// GPIO Pins
#define BUTTON_UP_PIN GPIO_NUM_37
#define BUTTON_DOWN_PIN GPIO_NUM_39
#define BUTTON_PUSH_PIN GPIO_NUM_38 // Push button on the side

// Shared variable for wakeup pin tracking
extern volatile uint8_t wakeup_pin;
// ISR for button interrupts
void IRAM_ATTR button_isr(void* arg);

// Basic settings placeholder - can be expanded later
// Example: Timezone might still be relevant
extern int8_t global_timezone;

int8_t GetTimeZone(void);
void SetTimeZone(int8_t time_zone);

// Placeholder for loading/saving settings if needed
esp_err_t LoadSetting(void);
esp_err_t SaveSetting(void);

// --- WiFi Functions ---
bool connectToWiFi();
void disconnectWiFi();
void getNTPTime(); // Keep declaration if used elsewhere, definition moved

// --- SPIFFS Functions ---
bool initializeSPIFFS();
bool saveScriptList(const char* jsonContent);
bool loadScriptList(JsonArray& listArray); // Use JsonArray for loading
bool saveScriptContent(const char* id, const char* content);
bool loadScriptContent(const char* id, String& content);
bool saveCurrentScriptId(const char* id);
bool loadCurrentScriptId(String& id);

// --- Sleep/Shutdown ---
void goToLightSleep(); // Renamed from Shutdown

#endif // _GLOBAL_SETTING_H_
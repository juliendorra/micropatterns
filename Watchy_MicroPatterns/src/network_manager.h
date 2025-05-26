#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "event_defs.h" // For FetchResultStatus

// Forward declaration
class SystemManager;

class NetworkManager
{
public:
    // Constructor can take SystemManager for WiFi credentials if they are dynamic,
    // or use hardcoded/NVS values directly.
    NetworkManager(SystemManager *sysMgr = nullptr); // Optional SystemManager

    // WiFi Management
    // Connects to WiFi. Timeout is in FreeRTOS ticks.
    // Returns true if connected, false otherwise (timeout, error, interrupt).
    // Checks interruptFlag periodically.
    bool connectWiFi(TickType_t timeout = pdMS_TO_TICKS(20000));
    void disconnectWiFi();
    bool isConnected();

    // API Operations
    // Fetches the script list. Parses into outListDoc.
    // Checks interruptFlag periodically.
    FetchResultStatus fetchScriptList(JsonDocument &outListDoc);
    // Fetches content for a single script. Parses into outContentJson (which should be a JsonObject).
    // Checks interruptFlag periodically.
    FetchResultStatus fetchScriptContent(const String &humanId, JsonDocument &outScriptDoc);

    // Interrupt mechanism for long operations
    void setInterruptFlag(volatile bool *flag); // Pointer to a flag that can be set externally

private:
    SystemManager *_sysMgr;               // Optional, for settings like timezone if needed by API
    volatile bool *_interruptRequestFlag; // External flag to signal interruption

    // WiFi credentials and API endpoint (can be constants or from config)
    static const char *WIFI_SSID_DEFAULT;
    static const char *WIFI_PASSWORD_DEFAULT;
    static const char *API_BASE_URL_DEFAULT;
    static const char *USER_ID_DEFAULT; // Added User ID
    static const char *ROOT_CA_CERT_DEFAULT;

    // Helper for HTTP requests
    int performHttpRequest(const String &url, String &payload, WiFiClientSecure &client, HTTPClient &http);
};

#endif // NETWORK_MANAGER_H

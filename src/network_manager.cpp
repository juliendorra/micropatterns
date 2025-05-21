#include "network_manager.h"
#include "system_manager.h" // If used for config
#include "esp32-hal-log.h"
#include "esp_task_wdt.h" // For esp_task_wdt_reset()

// Define constant for interrupt code
const int FETCH_INTERRUPTED_BY_USER = -9999;

// Define default credentials and API URL (can be overridden by NVS or SystemManager later)
const char* NetworkManager::WIFI_SSID_DEFAULT = "OpenWrt2.4";
const char* NetworkManager::WIFI_PASSWORD_DEFAULT = "hudohudo";
const char* NetworkManager::API_BASE_URL_DEFAULT = "https://micropatterns-api.deno.dev";

// ISRG Root X1 CA Certificate
const char* NetworkManager::ROOT_CA_CERT_DEFAULT = \
    "-----BEGIN CERTIFICATE-----\n" \
    "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\n" \
    "-----END CERTIFICATE-----\n";

NetworkManager::NetworkManager(SystemManager* sysMgr) : _sysMgr(sysMgr), _interruptRequestFlag(nullptr) {}

void NetworkManager::setInterruptFlag(volatile bool* flag) {
    _interruptRequestFlag = flag;
}

bool NetworkManager::connectWiFi(TickType_t timeout) {
    if (WiFi.status() == WL_CONNECTED) {
        log_i("WiFi already connected.");
        return true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false); // Disconnect if previously connected, don't erase config
    vTaskDelay(pdMS_TO_TICKS(100));

    log_i("Connecting to WiFi SSID: %s", WIFI_SSID_DEFAULT);
    WiFi.begin(WIFI_SSID_DEFAULT, WIFI_PASSWORD_DEFAULT);

    TickType_t startTimeTicks = xTaskGetTickCount();
    TickType_t last_dot_log_time_ticks = startTimeTicks;
    int retry_count = 0;
    const int max_retries = 1; // Total 2 attempts (initial + 1 retry)

    while (retry_count <= max_retries) {
        while (WiFi.status() != WL_CONNECTED) {
            if (_interruptRequestFlag && *_interruptRequestFlag) {
                log_i("connectWiFi: User interrupt detected. Aborting connection.");
                WiFi.disconnect(false);
                WiFi.mode(WIFI_OFF);
                return false;
            }

            vTaskDelay(pdMS_TO_TICKS(50));

            if ((xTaskGetTickCount() - last_dot_log_time_ticks) > pdMS_TO_TICKS(500)) {
                log_d(".");
                last_dot_log_time_ticks = xTaskGetTickCount();
            }

            if ((xTaskGetTickCount() - startTimeTicks) > timeout / (max_retries + 1) ) { // Timeout per attempt
                if (retry_count < max_retries) {
                    log_w("WiFi connection attempt %d timed out, retrying...", retry_count + 1);
                    WiFi.disconnect(false);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    WiFi.begin(WIFI_SSID_DEFAULT, WIFI_PASSWORD_DEFAULT);
                    startTimeTicks = xTaskGetTickCount(); // Reset timeout for next attempt
                    retry_count++;
                    break;
                } else {
                    log_e("WiFi connection failed after %d attempts!", max_retries + 1);
                    WiFi.disconnect(false);
                    WiFi.mode(WIFI_OFF);
                    return false;
                }
            }
            if (WiFi.status() == WL_CONNECTED) break;
        }
        if (WiFi.status() == WL_CONNECTED) break;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        log_i("WiFi connected! IP Address: %s", WiFi.localIP().toString().c_str());
        return true;
    }
    
    log_e("WiFi connection failed with unexpected state!");
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    return false;
}

void NetworkManager::disconnectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        log_i("Disconnecting WiFi.");
        WiFi.disconnect(true); // Disconnect and erase STA config
    }
    if (WiFi.getMode() != WIFI_OFF) {
         WiFi.mode(WIFI_OFF); // Turn off WiFi module to save power
         log_d("WiFi module turned OFF.");
    } else {
        log_d("WiFi already disconnected or module off.");
    }
}

bool NetworkManager::isConnected() {
    return (WiFi.status() == WL_CONNECTED);
}

int NetworkManager::performHttpRequest(const String& url, String& payload, WiFiClientSecure& client, HTTPClient& http) {
    if (_interruptRequestFlag && *_interruptRequestFlag) {
        log_i("HTTP request to %s interrupted before begin.", url.c_str());
        return FETCH_INTERRUPTED_BY_USER; // Special code for interruption
    }

    if (!http.begin(client, url)) {
        log_e("HTTPClient begin failed for URL: %s", url.c_str());
        return -1; // Generic error code for begin failure
    }
    esp_task_wdt_reset(); // Reset WDT before blocking GET

    int httpCode = http.GET();
    vTaskDelay(pdMS_TO_TICKS(10)); // Small yield
    esp_task_wdt_reset(); // Reset WDT after GET

    if (_interruptRequestFlag && *_interruptRequestFlag) {
        log_i("HTTP GET to %s interrupted.", url.c_str());
        http.end();
        return FETCH_INTERRUPTED_BY_USER;
    }

    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            payload = http.getString();
        } else {
            log_w("HTTP GET error for %s: %d (%s)", url.c_str(), httpCode, http.errorToString(httpCode).c_str());
        }
    } else {
        log_e("HTTP GET failed for %s: %s", url.c_str(), http.errorToString(httpCode).c_str());
    }
    http.end();
    return httpCode;
}


FetchResultStatus NetworkManager::fetchScriptList(JsonDocument& outListDoc) {
    if (!isConnected()) {
        log_w("fetchScriptList: Not connected to WiFi.");
        return FetchResultStatus::NO_WIFI;
    }

    WiFiClientSecure httpsClient;
    httpsClient.setCACert(ROOT_CA_CERT_DEFAULT);
    HTTPClient http;

    String listUrl = String(API_BASE_URL_DEFAULT) + "/api/device/scripts";
    log_i("Fetching script list from: %s", listUrl.c_str());
    String payload;

    int httpCode = performHttpRequest(listUrl, payload, httpsClient, http);

    if (httpCode == FETCH_INTERRUPTED_BY_USER) return FetchResultStatus::INTERRUPTED_BY_USER;
    if (httpCode < 0) return FetchResultStatus::GENUINE_ERROR; // Covers begin error or GET error

    if (httpCode == HTTP_CODE_OK) {
        DeserializationError error = deserializeJson(outListDoc, payload);
        if (error) {
            log_e("Failed to parse server script list JSON: %s", error.c_str());
            return FetchResultStatus::GENUINE_ERROR;
        }
        if (!outListDoc.is<JsonArray>()) {
            log_e("Server script list JSON is not an array.");
            outListDoc.clear(); // Ensure document is cleared if not an array
            return FetchResultStatus::GENUINE_ERROR;
        }
        log_i("Script list fetched and parsed successfully (%d scripts).", outListDoc.as<JsonArray>().size());
        return FetchResultStatus::SUCCESS;
    } else {
        log_e("HTTP error fetching script list: %d", httpCode);
        return FetchResultStatus::GENUINE_ERROR;
    }
}

FetchResultStatus NetworkManager::fetchScriptContent(const String& humanId, JsonDocument& outScriptDoc) {
    if (humanId.isEmpty()) {
        log_e("fetchScriptContent: humanId is empty.");
        return FetchResultStatus::GENUINE_ERROR;
    }
    if (!isConnected()) {
        log_w("fetchScriptContent: Not connected to WiFi.");
        return FetchResultStatus::NO_WIFI;
    }

    WiFiClientSecure httpsClient;
    httpsClient.setCACert(ROOT_CA_CERT_DEFAULT);
    HTTPClient http;

    String scriptUrl = String(API_BASE_URL_DEFAULT) + "/api/scripts/" + humanId;
    log_i("Fetching script content for '%s' from: %s", humanId.c_str(), scriptUrl.c_str());
    String payload;

    int httpCode = performHttpRequest(scriptUrl, payload, httpsClient, http);

    if (httpCode == FETCH_INTERRUPTED_BY_USER) return FetchResultStatus::INTERRUPTED_BY_USER;
    if (httpCode < 0) return FetchResultStatus::GENUINE_ERROR;

    if (httpCode == HTTP_CODE_OK) {
        DeserializationError error = deserializeJson(outScriptDoc, payload);
        if (error) {
            log_e("Failed to parse script content JSON for '%s': %s", humanId.c_str(), error.c_str());
            outScriptDoc.clear(); // Clear on parse error
            return FetchResultStatus::GENUINE_ERROR;
        }
        // Check if the parsed document is an object and contains the "content" key
        if (!outScriptDoc.is<JsonObject>() || !outScriptDoc.as<JsonObject>().containsKey("content")) {
            log_e("Script content JSON for '%s' is not an object or missing 'content' field.", humanId.c_str());
            outScriptDoc.clear(); // Clear if structure is not as expected
            return FetchResultStatus::GENUINE_ERROR;
        }
        log_i("Script content for '%s' fetched and parsed successfully.", humanId.c_str());
        return FetchResultStatus::SUCCESS;
    } else {
        log_e("HTTP error fetching script content for '%s': %d", humanId.c_str(), httpCode);
        return FetchResultStatus::GENUINE_ERROR;
    }
}
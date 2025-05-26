#include "network_manager.h"
#include "system_manager.h" // If used for config
#include "esp32-hal-log.h"
#include "esp_task_wdt.h" // For esp_task_wdt_reset()

// Define constant for interrupt code
const int FETCH_INTERRUPTED_BY_USER = -9999;

// Define default credentials and API URL (can be overridden by NVS or SystemManager later)
const char *NetworkManager::WIFI_SSID_DEFAULT = "OpenWrt2.4";
const char *NetworkManager::WIFI_PASSWORD_DEFAULT = "hudohudo";
const char *NetworkManager::API_BASE_URL_DEFAULT = "https://micropatterns-api.deno.dev";
const char *NetworkManager::USER_ID_DEFAULT = "kksh2hjtkb"; // Added User ID

// ISRG Root X1 CA Certificate
const char *NetworkManager::ROOT_CA_CERT_DEFAULT =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

NetworkManager::NetworkManager(SystemManager *sysMgr) : _sysMgr(sysMgr), _interruptRequestFlag(nullptr) {}

void NetworkManager::setInterruptFlag(volatile bool *flag)
{
    _interruptRequestFlag = flag;
}

bool NetworkManager::connectWiFi(TickType_t timeout)
{
    if (WiFi.status() == WL_CONNECTED)
    {
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

    while (retry_count <= max_retries)
    {
        while (WiFi.status() != WL_CONNECTED)
        {
            if (_interruptRequestFlag && *_interruptRequestFlag)
            {
                log_i("connectWiFi: User interrupt detected. Aborting connection.");
                WiFi.disconnect(false);
                WiFi.mode(WIFI_OFF);
                return false;
            }

            vTaskDelay(pdMS_TO_TICKS(50));

            if ((xTaskGetTickCount() - last_dot_log_time_ticks) > pdMS_TO_TICKS(500))
            {
                log_d(".");
                esp_task_wdt_reset(); // Reset WDT periodically during WiFi connection attempts
                last_dot_log_time_ticks = xTaskGetTickCount();
            }

            if ((xTaskGetTickCount() - startTimeTicks) > timeout / (max_retries + 1))
            { // Timeout per attempt
                if (retry_count < max_retries)
                {
                    log_w("WiFi connection attempt %d timed out, retrying...", retry_count + 1);
                    WiFi.disconnect(false);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    WiFi.begin(WIFI_SSID_DEFAULT, WIFI_PASSWORD_DEFAULT);
                    startTimeTicks = xTaskGetTickCount(); // Reset timeout for next attempt
                    retry_count++;
                    break;
                }
                else
                {
                    log_e("WiFi connection failed after %d attempts!", max_retries + 1);
                    WiFi.disconnect(false);
                    WiFi.mode(WIFI_OFF);
                    return false;
                }
            }
            if (WiFi.status() == WL_CONNECTED)
                break;
        }
        if (WiFi.status() == WL_CONNECTED)
            break;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        log_i("WiFi connected! IP Address: %s", WiFi.localIP().toString().c_str());
        return true;
    }

    log_e("WiFi connection failed with unexpected state!");
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    return false;
}

void NetworkManager::disconnectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        log_i("Disconnecting WiFi.");
        WiFi.disconnect(true); // Disconnect and erase STA config
    }
    if (WiFi.getMode() != WIFI_OFF)
    {
        WiFi.mode(WIFI_OFF); // Turn off WiFi module to save power
        log_d("WiFi module turned OFF.");
    }
    else
    {
        log_d("WiFi already disconnected or module off.");
    }
}

bool NetworkManager::isConnected()
{
    return (WiFi.status() == WL_CONNECTED);
}

int NetworkManager::performHttpRequest(const String &url, String &payload, WiFiClientSecure &client, HTTPClient &http)
{
    if (_interruptRequestFlag && *_interruptRequestFlag)
    {
        log_i("HTTP request to %s interrupted before begin.", url.c_str());
        return FETCH_INTERRUPTED_BY_USER; // Special code for interruption
    }

    if (!http.begin(client, url))
    {
        log_e("HTTPClient begin failed for URL: %s", url.c_str());
        return -1; // Generic error code for begin failure
    }
    esp_task_wdt_reset(); // Reset WDT before blocking GET

    int httpCode = http.GET();
    vTaskDelay(pdMS_TO_TICKS(10)); // Small yield
    esp_task_wdt_reset();          // Reset WDT after GET

    if (_interruptRequestFlag && *_interruptRequestFlag)
    {
        log_i("HTTP GET to %s interrupted.", url.c_str());
        http.end();
        return FETCH_INTERRUPTED_BY_USER;
    }

    if (httpCode > 0)
    {
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)
        {
            payload = http.getString();
        }
        else
        {
            log_w("HTTP GET error for %s: %d (%s)", url.c_str(), httpCode, http.errorToString(httpCode).c_str());
        }
    }
    else
    {
        log_e("HTTP GET failed for %s: %s", url.c_str(), http.errorToString(httpCode).c_str());
    }
    http.end();
    return httpCode;
}

FetchResultStatus NetworkManager::fetchScriptList(JsonDocument &outListDoc)
{
    // Clear output document first to ensure we start fresh
    outListDoc.clear();
    
    // memoryUsage() is deprecated for JsonDocument and returns 0.
    // log_d("fetchScriptList: Document memory usage: %u bytes", outListDoc.memoryUsage());
    
    if (!isConnected())
    {
        log_w("fetchScriptList: Not connected to WiFi");
        return FetchResultStatus::NO_WIFI;
    }

    WiFiClientSecure httpsClient;
    httpsClient.setCACert(ROOT_CA_CERT_DEFAULT);
    HTTPClient http;

    String listUrl = String(API_BASE_URL_DEFAULT) + "/api/device/scripts/" + String(USER_ID_DEFAULT);
    log_i("fetchScriptList: Fetching from %s", listUrl.c_str());

    // Check for interrupt before starting request
    if (_interruptRequestFlag && *_interruptRequestFlag) {
        log_i("fetchScriptList: Interrupted before HTTP request");
        return FetchResultStatus::INTERRUPTED_BY_USER;
    }

    // Begin HTTP connection
    if (!http.begin(httpsClient, listUrl)) {
        log_e("fetchScriptList: HTTPClient.begin failed for URL: %s", listUrl.c_str());
        return FetchResultStatus::GENUINE_ERROR;
    }
    
    // Set a reasonable timeout
    http.setTimeout(10000); // 10 seconds
    
    // Reset watchdog before HTTP operation
    esp_task_wdt_reset();
    log_d("fetchScriptList: Sending HTTP GET request");
    
    // Send the request
    int httpCode = http.GET();
    
    // Small yield to allow other tasks to run
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_task_wdt_reset();

    // Check for interrupt after request
    if (_interruptRequestFlag && *_interruptRequestFlag) {
        log_i("fetchScriptList: Interrupted after HTTP request");
        http.end();
        return FetchResultStatus::INTERRUPTED_BY_USER;
    }
    
    FetchResultStatus status = FetchResultStatus::GENUINE_ERROR; // Default to error

    if (httpCode > 0) {
        log_d("fetchScriptList: Received HTTP status code: %d", httpCode);
        
        if (httpCode == HTTP_CODE_OK) {
            // Get content length for diagnostics
            int contentLength = http.getSize();
            log_d("fetchScriptList: Content length: %d bytes", contentLength);
            
            if (contentLength <= 0) {
                log_w("fetchScriptList: Server returned empty response");
                http.end();
                return FetchResultStatus::GENUINE_ERROR;
            }
            
            // Heuristic check for very large content that might strain memory, memoryUsage() is 0 for JsonDocument.
            // Max typical list size might be around 8KB.
            if (contentLength > 8192) {
                log_w("fetchScriptList: Content length (%d) is large and might exceed typical document memory capacity (heuristic: 8192 bytes)",
                     contentLength);
                // Continue anyway, ArduinoJson might handle it with truncation or reallocation
            }
            
            // Reset watchdog before JSON parsing
            esp_task_wdt_reset();
            
            // Use ArduinoJson streaming API for efficient parsing
            log_d("fetchScriptList: Deserializing JSON from HTTP stream");
            DeserializationError error = deserializeJson(outListDoc, http.getStream());
            
            if (error) {
                log_e("fetchScriptList: JSON parse error: %s", error.c_str());
                
                // Try to log response for debugging small payloads
                if (contentLength < 500) {
                    // Re-request to get the payload as string for logging
                    String responseBody = http.getString();
                    log_e("fetchScriptList: Response body: %s", responseBody.c_str());
                }
                
                // Clear document on error
                outListDoc.clear();
            } else {
                log_d("fetchScriptList: JSON successfully parsed");
                
                // Validate document structure
                if (!outListDoc.is<JsonArray>()) {
                    log_e("fetchScriptList: Expected JSON array but got %s",
                         outListDoc.is<JsonObject>() ? "object" : "unknown type");
                    
                    // If it's an object, log some keys for debugging
                    if (outListDoc.is<JsonObject>()) {
                        JsonObject rootObj = outListDoc.as<JsonObject>();
                        String keys;
                        int i = 0;
                        for (JsonPair p : rootObj) {
                            if (i++ < 5) { // Log up to 5 keys
                                keys += String(p.key().c_str()) + " ";
                            }
                        }
                        log_e("fetchScriptList: Object contains keys: %s", keys.c_str());
                    }
                    
                    // Clear document if it's the wrong type
                    outListDoc.clear();
                } else {
                    // Success - valid array
                    JsonArray array = outListDoc.as<JsonArray>();
                    size_t items = array.size();
                    log_i("fetchScriptList: Successfully parsed JSON array with %u items", items);
                    log_d("fetchScriptList: Parsed doc type: isArray=%d, isObject=%d, isNull=%d, size=%d", outListDoc.is<JsonArray>(), outListDoc.is<JsonObject>(), outListDoc.isNull(), items);
                    
                    // Verify array elements for basic structure
                    bool allValid = true;
                    for (size_t i = 0; i < items && i < 10; i++) { // Check up to 10 items
                        JsonVariant item = array[i];
                        if (!item.is<JsonObject>()) {
                            log_w("fetchScriptList: Item %u is not a JSON object", i);
                            allValid = false;
                            continue;
                        }
                        
                        // Check for required fields
                        JsonObject obj = item.as<JsonObject>();
                        if (!obj["id"].is<const char*>()) {
                            log_w("fetchScriptList: Item %u missing required 'id' field", i);
                            allValid = false;
                        }
                    }
                    
                    if (allValid) {
                        log_i("fetchScriptList: All items have the required structure");
                        status = FetchResultStatus::SUCCESS;
                    } else {
                        log_w("fetchScriptList: Some items have invalid structure but proceeding anyway");
                        status = FetchResultStatus::SUCCESS; // Still consider it a success
                    }
                }
            }
        } else {
            log_w("fetchScriptList: HTTP error: %d (%s)",
                 httpCode, http.errorToString(httpCode).c_str());
                 
            // For some status codes, try to get more info from response
            if (httpCode >= 400 && httpCode < 500) {
                String errorResponse = http.getString();
                if (errorResponse.length() > 0 && errorResponse.length() < 500) {
                    log_e("fetchScriptList: Error response: %s", errorResponse.c_str());
                }
            }
        }
    } else {
        log_e("fetchScriptList: HTTP request failed: %s", http.errorToString(httpCode).c_str());
    }
    
    // Cleanup
    http.end();
    log_d("fetchScriptList: HTTP connection closed. Status: %d", (int)status);
    return status;
}

FetchResultStatus NetworkManager::fetchScriptContent(const String &humanId, JsonDocument &outScriptDoc)
{
    // Clear output document first
    outScriptDoc.clear();
    
    // memoryUsage() is deprecated for JsonDocument and returns 0.
    // log_d("fetchScriptContent: Document memory usage: %u bytes", outScriptDoc.memoryUsage());
    
    if (humanId.isEmpty())
    {
        log_e("fetchScriptContent: humanId is empty");
        return FetchResultStatus::GENUINE_ERROR;
    }
    
    if (!isConnected())
    {
        log_w("fetchScriptContent: Not connected to WiFi");
        return FetchResultStatus::NO_WIFI;
    }

    WiFiClientSecure httpsClient;
    httpsClient.setCACert(ROOT_CA_CERT_DEFAULT);
    HTTPClient http;

    String scriptUrl = String(API_BASE_URL_DEFAULT) + "/api/scripts/" + String(USER_ID_DEFAULT) + "/" + humanId;
    log_i("fetchScriptContent: Fetching script '%s' from %s", humanId.c_str(), scriptUrl.c_str());

    // Check for interrupt before request
    if (_interruptRequestFlag && *_interruptRequestFlag) {
        log_i("fetchScriptContent: Interrupted before HTTP request");
        return FetchResultStatus::INTERRUPTED_BY_USER;
    }

    // Begin HTTP connection
    if (!http.begin(httpsClient, scriptUrl)) {
        log_e("fetchScriptContent: HTTPClient.begin failed for URL: %s", scriptUrl.c_str());
        return FetchResultStatus::GENUINE_ERROR;
    }
    
    // Set a reasonable timeout
    http.setTimeout(15000); // 15 seconds - scripts might be large
    
    // Reset watchdog before HTTP operation
    esp_task_wdt_reset();
    log_d("fetchScriptContent: Sending HTTP GET request");
    
    // Send the request
    int httpCode = http.GET();
    
    // Small yield to allow other tasks to run
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_task_wdt_reset();

    // Check for interrupt after request
    if (_interruptRequestFlag && *_interruptRequestFlag) {
        log_i("fetchScriptContent: Interrupted after HTTP request");
        http.end();
        return FetchResultStatus::INTERRUPTED_BY_USER;
    }

    FetchResultStatus status = FetchResultStatus::GENUINE_ERROR; // Default to error

    if (httpCode > 0) {
        log_d("fetchScriptContent: Received HTTP status code: %d", httpCode);
        
        if (httpCode == HTTP_CODE_OK) {
            // Get content length for diagnostics
            int contentLength = http.getSize();
            log_d("fetchScriptContent: Content length: %d bytes", contentLength);
            
            if (contentLength <= 0) {
                log_w("fetchScriptContent: Server returned empty response");
                http.end();
                return FetchResultStatus::GENUINE_ERROR;
            }
            
            // Heuristic check for very large content. MAX_SCRIPT_CONTENT_LEN is 5600.
            // JsonDocument for script content is SCRIPT_CONTENT_JSON_CAPACITY (MAX_SCRIPT_CONTENT_LEN + 512)
            if (contentLength > (MAX_SCRIPT_CONTENT_LEN + 200)) { // If content is larger than expected max + buffer
                log_w("fetchScriptContent: Content length (%d) is large and might exceed typical document memory capacity (expected max script: %d)",
                     contentLength, MAX_SCRIPT_CONTENT_LEN);
                // Continue anyway, ArduinoJson might handle it with truncation or reallocation
            }
            
            // Reset watchdog before JSON parsing
            esp_task_wdt_reset();
            
            // Deserialize from stream for memory efficiency
            log_d("fetchScriptContent: Deserializing JSON response");
            DeserializationError error = deserializeJson(outScriptDoc, http.getStream());
            
            if (error) {
                log_e("fetchScriptContent: JSON parse error for '%s': %s",
                     humanId.c_str(), error.c_str());
                
                // Try to log response for debugging if it's small
                if (contentLength < 500) {
                    // Re-request to get the payload for logging
                    String responseBody = http.getString();
                    log_e("fetchScriptContent: Response body: %s", responseBody.c_str());
                }
                
                outScriptDoc.clear();
            } else {
                log_d("fetchScriptContent: JSON successfully parsed");
                
                // Validate response format
                if (!outScriptDoc.is<JsonObject>()) {
                    log_e("fetchScriptContent: Expected JSON object but got %s",
                         outScriptDoc.is<JsonArray>() ? "array" : "unknown type");
                    outScriptDoc.clear();
                } else {
                    // Check for 'content' field which should be a string
                    JsonObject scriptObj = outScriptDoc.as<JsonObject>();
                    
                    if (scriptObj["content"].isNull()) { // Use isNull for existence check
                        log_e("fetchScriptContent: Missing required 'content' field or it is null");
                        
                        // Log available fields for debugging
                        String fields;
                        for (JsonPair p : scriptObj) {
                            fields += String(p.key().c_str()) + " ";
                        }
                        log_e("fetchScriptContent: Available fields: %s", fields.c_str());
                        
                        outScriptDoc.clear();
                    } else if (!scriptObj["content"].is<const char*>()) {
                        log_e("fetchScriptContent: 'content' field is not a string. Type: %s",
                              scriptObj["content"].is<JsonObject>() ? "object" :
                              scriptObj["content"].is<JsonArray>() ? "array" :
                              scriptObj["content"].is<bool>() ? "bool" :
                              scriptObj["content"].is<int>() ? "number" : "unknown");
                        
                        outScriptDoc.clear();
                    } else {
                        // Success! We have a valid script content response
                        const char* contentStr = scriptObj["content"].as<const char*>();
                        size_t contentLen = contentStr ? strlen(contentStr) : 0;
                        
                        log_i("fetchScriptContent: Script '%s' fetched successfully. Content length: %u bytes",
                              humanId.c_str(), contentLen);
                        log_d("fetchScriptContent: Parsed doc type: isArray=%d, isObject=%d, isNull=%d", outScriptDoc.is<JsonArray>(), outScriptDoc.is<JsonObject>(), outScriptDoc.isNull());
                        
                        // Check for excessively small content that might indicate a problem
                        if (contentLen < 10) {
                            log_w("fetchScriptContent: Content is suspiciously short (%u bytes): %s",
                                 contentLen, contentStr);
                            // Continue anyway - might be a legitimately short script
                        }
                        
                        status = FetchResultStatus::SUCCESS;
                    }
                }
            }
        } else {
            log_w("fetchScriptContent: HTTP error: %d (%s)",
                 httpCode, http.errorToString(httpCode).c_str());
                 
            // For client errors, try to get more info from response
            if (httpCode >= 400 && httpCode < 500) {
                String errorResponse = http.getString();
                if (errorResponse.length() > 0 && errorResponse.length() < 500) {
                    log_e("fetchScriptContent: Error response: %s", errorResponse.c_str());
                }
            }
        }
    } else {
        log_e("fetchScriptContent: HTTP request failed: %s", http.errorToString(httpCode).c_str());
    }
    
    // Cleanup
    http.end();
    log_d("fetchScriptContent: HTTP connection closed. Status: %d", (int)status);
    return status;
}

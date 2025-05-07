#include "global_setting.h"
#include <M5EPD.h>
#include <esp_sleep.h> // For sleep functions
#include "esp32-hal-log.h"
#include <WiFi.h>
#include <rtc.h> // For RTC_TimeTypeDef
#include <SPIFFS.h>
#include "time.h" // For NTP time struct

// Define the shared wakeup_pin variable
volatile uint8_t wakeup_pin = 0;
volatile bool g_wakeup_handled = false; // Initialize the wakeup handled flag

// Last time a button interrupt was triggered
volatile uint32_t g_last_button_time = 0;
// Debounce time in milliseconds
const uint32_t DEBOUNCE_TIME_MS = 300;

// ISR for button interrupts
void IRAM_ATTR button_isr(void *arg) {
    uint32_t gpio_num_val = (uint32_t)arg;
    uint8_t expected_wakeup_pin_val = 0; // We only want to set wakeup_pin if it's currently 0
    uint8_t desired_wakeup_pin_val = (uint8_t)gpio_num_val;

    // Atomically set wakeup_pin to gpio_num_val ONLY if wakeup_pin is currently 0.
    // This prevents a rapid second interrupt from overwriting the first if it hasn't been processed yet
    // by the main loop, and ensures the ISR is very fast.
    // The 'false' means this is a "weak" compare-exchange, which is fine here.
    // __ATOMIC_SEQ_CST provides the strongest memory ordering guarantees.
    __atomic_compare_exchange_n(&wakeup_pin, &expected_wakeup_pin_val, desired_wakeup_pin_val, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// --- Constants ---
const char *WIFI_SSID = "OpenWrt2.4";   // Replace with your SSID
const char *WIFI_PASSWORD = "hudohudo"; // Replace with your Password

const char *rootCACertificate =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

const char *NTP_SERVER = "pool.ntp.org";
const char *API_BASE_URL = "https://micropatterns-api.deno.dev";

#define DEFAULT_TIMEZONE 1        // Default timezone offset
#define uS_TO_S_FACTOR 1000000ULL /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP_S 77        /* Time ESP32 will go to sleep (in seconds) - Reduced for testing */

esp_err_t __espret__;
#define NVS_CHECK(x)          \
    __espret__ = x;           \
    if (__espret__ != ESP_OK) \
    {                         \
        nvs_close(nvs_arg);   \
        log_e("Check Err");   \
        return __espret__;    \
    }

int8_t global_timezone = DEFAULT_TIMEZONE;

int8_t GetTimeZone(void)
{
    return global_timezone;
}

void SetTimeZone(int8_t time_zone)
{
    global_timezone = time_zone;
}

esp_err_t LoadSetting(void)
{
    nvs_handle nvs_arg;
    esp_err_t err = nvs_open("Setting", NVS_READONLY, &nvs_arg);
    if (err != ESP_OK)
    {
        log_e("NVS Open failed");
        return err;
    }
    nvs_get_i8(nvs_arg, "timezone", &global_timezone);
    nvs_close(nvs_arg);
    return ESP_OK;
}

esp_err_t SaveSetting(void)
{
    nvs_handle nvs_arg;
    esp_err_t err = nvs_open("Setting", NVS_READWRITE, &nvs_arg);
    if (err != ESP_OK)
    {
        log_e("NVS Open failed");
        return err;
    }
    NVS_CHECK(nvs_set_i8(nvs_arg, "timezone", global_timezone));
    NVS_CHECK(nvs_commit(nvs_arg));
    nvs_close(nvs_arg);
    return ESP_OK;
}

// --- WiFi Functions ---
bool connectToWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        log_i("WiFi already connected.");
        return true;
    }

    log_i("Connecting to WiFi SSID: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startTime = millis();
    unsigned long last_dot_log_time = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        // Check for user interrupt frequently
        if (g_user_interrupt_signal_for_fetch_task) {
            log_i("connectToWiFi: User interrupt detected. Aborting connection.");
            WiFi.disconnect(false); // Try to stop current operations
            WiFi.mode(WIFI_OFF);    // Turn off WiFi module
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // Yield for a short period (50ms)

        if (millis() - last_dot_log_time > 500) { // Log "." approx every 500ms
            log_d(".");
            last_dot_log_time = millis();
        }

        if (millis() - startTime > 15000) { // 15 second timeout
            log_e("WiFi connection timed out!");
            WiFi.disconnect(false); // Don't erase config
            WiFi.mode(WIFI_OFF);    // Turn off WiFi module
            return false;
        }
    }
    log_i("WiFi connected! IP Address: %s", WiFi.localIP().toString().c_str());
    return true;
}

void disconnectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        log_i("Disconnecting WiFi.");
        WiFi.disconnect(true); // Disconnect and erase config? Set to false if you want to keep config.
        WiFi.mode(WIFI_OFF);   // Turn off WiFi module to save power
    }
    else
    {
        log_d("WiFi already disconnected.");
        // Ensure WiFi module is off if not connected
        if (WiFi.getMode() != WIFI_OFF)
        {
            WiFi.mode(WIFI_OFF);
        }
    }
}

void printLocalTimeAndSetRTC()
{
    struct tm timeinfo;

    if (getLocalTime(&timeinfo) == false)
    {
        log_e("Failed to obtain NTP time");
        return;
    }

    log_i("NTP time obtained: %s", asctime(&timeinfo));

    RTC_Time time;
    time.hour = timeinfo.tm_hour;
    time.min = timeinfo.tm_min;
    time.sec = timeinfo.tm_sec;
    M5.RTC.setTime(&time);

    RTC_Date date;
    date.day = timeinfo.tm_mday;
    date.mon = timeinfo.tm_mon + 1;      // tm_mon is 0-11
    date.year = timeinfo.tm_year + 1900; // tm_year is years since 1900
    M5.RTC.setDate(&date);

    log_i("RTC set to: %d-%02d-%02d %02d:%02d:%02d", date.year, date.mon, date.day, time.hour, time.min, time.sec);
}

void getNTPTime()
{
    if (!connectToWiFi())
    {
        log_e("Cannot get NTP time, WiFi connection failed.");
        // Set a default time if NTP fails?
        // RTC_Time time; time.hour = 12; time.min = 0; time.sec = 0; M5.RTC.setTime(&time);
        // RTC_Date date; date.day = 1; date.mon = 1; date.year = 2024; M5.RTC.setDate(&date);
        return;
    }

    // Configure time: GMT offset, daylight offset, NTP server
    // Example: GMT+1 (CET), no daylight saving
    const long gmtOffset_sec = global_timezone * 3600;
    const int daylightOffset_sec = 0; // Adjust if needed

    log_i("Configuring time with GMT offset %ld sec, DST offset %d sec, NTP server %s", gmtOffset_sec, daylightOffset_sec, NTP_SERVER);
    configTime(gmtOffset_sec, daylightOffset_sec, NTP_SERVER);

    printLocalTimeAndSetRTC();

    disconnectWiFi(); // Disconnect after getting time
}

// --- SPIFFS Functions ---
bool initializeSPIFFS()
{
    log_i("Initializing SPIFFS...");
    if (!SPIFFS.begin(false))
    { // `false` = don't format if mount fails
        log_e("SPIFFS Mount Failed. Formatting...");
        if (!SPIFFS.begin(true))
        { // `true` = format SPIFFS if mount fails
            log_e("SPIFFS Formatting Failed!");
            return false;
        }
        log_i("SPIFFS Formatted successfully.");
    }
    else
    {
        log_i("SPIFFS Mounted successfully.");
        // Optional: List files
        File root = SPIFFS.open("/");
        File file = root.openNextFile();
        while (file)
        {
            log_d("  FILE: %s  SIZE: %d", file.name(), file.size());
            file = root.openNextFile();
        }
    }
    // Create directories if they don't exist
    if (!SPIFFS.exists("/scripts"))
    {
        SPIFFS.mkdir("/scripts");
        log_i("Created /scripts directory");
    }
    if (!SPIFFS.exists("/scripts/content"))
    {
        if (!SPIFFS.mkdir("/scripts/content")) {
            log_e("Failed to create /scripts/content directory!");
            return false;
        }
        log_i("Created /scripts/content directory");
    }
    return true;
}

// Saves the provided DynamicJsonDocument (expected to be an array of script metadata) to /scripts/list.json
bool saveScriptList(DynamicJsonDocument& docToSave)
{
    if (!docToSave.is<JsonArray>()) {
        log_e("saveScriptList (doc): Provided document is not a JSON array. Cannot save.");
        return false;
    }

    File file = SPIFFS.open("/scripts/list.json", FILE_WRITE);
    if (!file)
    {
        log_e("saveScriptList (doc): Failed to open /scripts/list.json for writing");
        return false;
    }

    String outputJson;
    serializeJson(docToSave, outputJson); // Serialize the provided document

    if (file.print(outputJson))
    {
        log_i("Script list (from doc) saved successfully to SPIFFS. (%d entries)", docToSave.as<JsonArray>().size());
        file.close();
        return true;
    }
    else
    {
        log_e("saveScriptList (doc): Failed to write script list to SPIFFS.");
        file.close();
        return false;
    }
}

// Loads the script list JSON string and parses it into the provided DynamicJsonDocument
bool loadScriptList(DynamicJsonDocument &doc) // Changed parameter
{
    File file = SPIFFS.open("/scripts/list.json", FILE_READ);
    if (!file || file.isDirectory())
    {
        log_w("Failed to open /scripts/list.json for reading or it's a directory.");
        return false;
    }

    // The caller provides the DynamicJsonDocument, so we use it directly.
    // Clear it first to ensure it's empty before deserialization.
    doc.clear();

    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error)
    {
        log_e("Failed to parse script list JSON: %s", error.c_str());
        doc.clear(); // Ensure doc is cleared on error
        return false;
    }

    if (!doc.is<JsonArray>())
    {
        log_e("Script list JSON is not an array.");
        doc.clear(); // Ensure doc is cleared if content is not an array
        return false;
    }

    // The document 'doc' is now populated. The caller will use doc.as<JsonArray>().
    log_i("Script list loaded successfully into Document from SPIFFS. Found %d scripts.", doc.as<JsonArray>().size());
    return true;
}

bool saveScriptContent(const char *fileId, const char *content)
{
    String path = "/scripts/content/" + String(fileId); // Use fileId, no .txt
    File file = SPIFFS.open(path.c_str(), FILE_WRITE);
    if (!file)
    {
        log_e("Failed to open %s for writing (using fileId: %s)", path.c_str(), fileId);
        return false;
    }
    if (file.print(content))
    {
        log_i("Script content for fileId '%s' saved successfully to %s.", fileId, path.c_str());
        file.close();
        return true;
    }
    else
    {
        log_e("Failed to write script content for fileId '%s' to %s.", fileId, path.c_str());
        file.close();
        return false;
    }
}

bool loadScriptContent(const char *fileId, String &content)
{
    String path = "/scripts/content/" + String(fileId); // Use fileId, no .txt
    log_d("loadScriptContent: Attempting to load path: %s (using fileId: %s, length: %d)", path.c_str(), fileId, path.length());

    if (!SPIFFS.exists(path.c_str())) {
        log_w("loadScriptContent: Path does not exist: %s", path.c_str());
        content = "";
        return false;
    }
    log_d("loadScriptContent: Path exists: %s", path.c_str());

    File file = SPIFFS.open(path.c_str(), FILE_READ);
    if (!file)
    {
        log_w("loadScriptContent: SPIFFS.open failed for path: %s. file.operator! is true.", path.c_str());
        content = "";
        return false;
    }
    
    log_d("loadScriptContent: SPIFFS.open succeeded for path: %s. File name from object: %s, Size: %u", path.c_str(), file.name(), file.size());

    if (file.isDirectory())
    {
        log_w("loadScriptContent: Path is a directory: %s", path.c_str());
        file.close();
        content = "";
        return false;
    }

    content = file.readString();
    size_t fileSizeBeforeRead = file.size(); // Get size before closing, though readString might affect reported size for some impls.
    file.close();

    if (content.length() > 0)
    {
        log_i("Script content for fileId '%s' loaded successfully (read %d bytes from path %s).", fileId, content.length(), path.c_str());
        return true;
    }
    else
    {
        log_w("Script content file for fileId '%s' (path: %s) is empty or readString() failed (content length: %d, reported file size: %u).", fileId, path.c_str(), content.length(), fileSizeBeforeRead);
        return false;
    }
}

bool saveCurrentScriptId(const char *id)
{
    File file = SPIFFS.open("/current_script.id", FILE_WRITE);
    if (!file)
    {
        log_e("Failed to open /current_script.id for writing");
        return false;
    }
    if (file.print(id))
    {
        log_i("Current script ID '%s' saved.", id);
        file.close();
        return true;
    }
    else
    {
        log_e("Failed to write current script ID '%s'.", id);
        file.close();
        return false;
    }
}

bool loadCurrentScriptId(String &id)
{
    File file = SPIFFS.open("/current_script.id", FILE_READ);
    if (!file || file.isDirectory())
    {
        log_w("Failed to open /current_script.id for reading or it's a directory.");
        id = ""; // Default to empty if file doesn't exist
        return false;
    }
    id = file.readString();
    file.close();
    if (id.length() > 0)
    {
        log_i("Current script ID '%s' loaded.", id.c_str());
        return true;
    }
    else
    {
        log_w("/current_script.id is empty or read failed.");
        return false;
    }
}

// --- Sleep/Shutdown ---
void goToLightSleep()
{
    log_i("Preparing for light sleep...");

    // Reset watchdog before potentially expensive operations
    esp_task_wdt_reset();
    
    SaveSetting(); // Save any final settings if needed
    
    // Reset watchdog after saving settings
    esp_task_wdt_reset();

    // Configure wake-up sources
    // 1. Timer
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_S * uS_TO_S_FACTOR);
    log_i("Setup ESP32 to wake up after %d seconds.", TIME_TO_SLEEP_S);

    // 2. GPIOs (Buttons UP, DOWN, PUSH) - Wake on LOW level using individual GPIO wakeup
    // Ensure pull-ups are enabled (done in SysInit_Start, but good practice)
    // Note: Pins 37, 38, 39 are RTC GPIOs, but we use the standard GPIO wakeup for light sleep.
    gpio_wakeup_enable(BUTTON_UP_PIN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(BUTTON_DOWN_PIN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(BUTTON_PUSH_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    log_i("Setup ESP32 to wake up on LOW level for GPIOs %d, %d, %d.", BUTTON_UP_PIN, BUTTON_DOWN_PIN, BUTTON_PUSH_PIN);

    // Reset wakeup pin and handled flag before sleep
    wakeup_pin = 0;
    g_wakeup_handled = false;

    // Optional: Hold GPIO states during sleep if needed (e.g., keep power pin high)
    // gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
    // gpio_deep_sleep_hold_en(); // Use deep sleep hold for light sleep too? Check docs. Seems ok.

    log_i("Entering light sleep...");
    // M5.disableEPDPower(); // Power down EPD before sleep? Test this.
    // M5.disableEXTPower();

    // Reset watchdog one last time before sleep
    esp_task_wdt_reset();
    
    // Use vTaskDelay instead of delay
    vTaskDelay(pdMS_TO_TICKS(10)); // Reduced delay before sleep

    esp_light_sleep_start();
    
    // Reset wakeup_handled flag after wake to ensure we process the next wakeup event
    g_wakeup_handled = false;
    
    // Reset watchdog immediately after waking up
    esp_task_wdt_reset();

    // After wakeup, check which pin triggered it
    if (wakeup_pin == BUTTON_UP_PIN)
    {
        log_i("Wakeup triggered by UP button");
    }
    else if (wakeup_pin == BUTTON_DOWN_PIN)
    {
        log_i("Wakeup triggered by DOWN button");
    }
    else if (wakeup_pin == BUTTON_PUSH_PIN)
    {
        log_i("Wakeup triggered by PUSH button");
    }
    else
    {
        log_i("Wakeup triggered by timer or unknown source");
    }

    // Disable GPIO wakeup sources after waking up
    gpio_wakeup_disable(BUTTON_UP_PIN);
    gpio_wakeup_disable(BUTTON_DOWN_PIN);
    gpio_wakeup_disable(BUTTON_PUSH_PIN);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    // Optional: Release GPIO hold if enabled
    // gpio_hold_dis((gpio_num_t)M5EPD_MAIN_PWR_PIN);
    // gpio_deep_sleep_hold_dis();

    // Re-enable power if disabled before sleep
    // M5.enableEPDPower();
    // M5.enableEXTPower();
    // delay(500); // Allow power to stabilize
}
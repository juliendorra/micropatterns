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

// ISR for button interrupts
void IRAM_ATTR button_isr(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    wakeup_pin = gpio_num;
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
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        log_d(".");
        if (millis() - startTime > 15000)
        { // 15 second timeout
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
        SPIFFS.mkdir("/scripts/content");
        log_i("Created /scripts/content directory");
    }
    return true;
}

bool saveScriptList(const char *jsonContent)
{
    File file = SPIFFS.open("/scripts/list.json", FILE_WRITE);
    if (!file)
    {
        log_e("Failed to open /scripts/list.json for writing");
        return false;
    }
    if (file.print(jsonContent))
    {
        log_i("Script list saved successfully to SPIFFS.");
        file.close();
        return true;
    }
    else
    {
        log_e("Failed to write script list to SPIFFS.");
        file.close();
        return false;
    }
}

// Loads the script list JSON string and parses it into the provided JsonArray
bool loadScriptList(JsonArray &listArray)
{
    File file = SPIFFS.open("/scripts/list.json", FILE_READ);
    if (!file || file.isDirectory())
    {
        log_w("Failed to open /scripts/list.json for reading or it's a directory.");
        return false;
    }

    // Allocate a temporary JsonDocument. Adjust size as needed.
    // Calculate size based on expected number of scripts * size per script entry + buffer
    // Example: 20 scripts * ({"id":"...", "name":"..."} ~ 100 chars) = 2000 + buffer
    DynamicJsonDocument doc(4096); // Increased size

    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error)
    {
        log_e("Failed to parse script list JSON: %s", error.c_str());
        return false;
    }

    if (!doc.is<JsonArray>())
    {
        log_e("Script list JSON is not an array.");
        return false;
    }

    // Clear the provided array before adding new elements
    listArray = doc.as<JsonArray>(); // Assign the parsed array

    if (listArray.isNull())
    {
        log_e("Failed to assign parsed JSON array.");
        return false;
    }

    log_i("Script list loaded successfully from SPIFFS. Found %d scripts.", listArray.size());
    return true;
}

bool saveScriptContent(const char *id, const char *content)
{
    String path = "/scripts/content/" + String(id) + ".txt";
    File file = SPIFFS.open(path.c_str(), FILE_WRITE);
    if (!file)
    {
        log_e("Failed to open %s for writing", path.c_str());
        return false;
    }
    if (file.print(content))
    {
        log_i("Script content for ID '%s' saved successfully.", id);
        file.close();
        return true;
    }
    else
    {
        log_e("Failed to write script content for ID '%s'.", id);
        file.close();
        return false;
    }
}

bool loadScriptContent(const char *id, String &content)
{
    String path = "/scripts/content/" + String(id) + ".txt";
    File file = SPIFFS.open(path.c_str(), FILE_READ);
    if (!file || file.isDirectory())
    {
        log_w("Failed to open %s for reading or it's a directory.", path.c_str());
        return false;
    }
    content = file.readString();
    file.close();
    if (content.length() > 0)
    {
        log_i("Script content for ID '%s' loaded successfully (%d bytes).", id, content.length());
        return true;
    }
    else
    {
        log_w("Script content file for ID '%s' is empty or read failed.", id);
        return false; // Consider empty file as failure? Or success? Let's say failure.
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

    SaveSetting(); // Save any final settings if needed

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

    // Reset wakeup pin before sleep
    wakeup_pin = 0;

    // Optional: Hold GPIO states during sleep if needed (e.g., keep power pin high)
    // gpio_hold_en((gpio_num_t)M5EPD_MAIN_PWR_PIN);
    // gpio_deep_sleep_hold_en(); // Use deep sleep hold for light sleep too? Check docs. Seems ok.

    log_i("Entering light sleep...");
    // M5.disableEPDPower(); // Power down EPD before sleep? Test this.
    // M5.disableEXTPower();

    delay(100); // Short delay before sleep

    esp_light_sleep_start();

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
    // esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    // Optional: Release GPIO hold if enabled
    // gpio_hold_dis((gpio_num_t)M5EPD_MAIN_PWR_PIN);
    // gpio_deep_sleep_hold_dis();

    // Re-enable power if disabled before sleep
    // M5.enableEPDPower();
    // M5.enableEXTPower();
    // delay(500); // Allow power to stabilize
}
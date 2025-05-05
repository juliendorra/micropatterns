#include "systeminit.h"
#include "global_setting.h" // Keep for LoadSetting
#include <rtc.h>            // For RTC_TimeTypeDef
#include <WiFi.h>
#include "time.h"

const char *ssid = "SSID";
const char *password = "PASSWORD";
const char *ntpServer = "pool.ntp.org";

void printLocalTimeAndSetRTC()
{
    struct tm timeinfo;

    if (getLocalTime(&timeinfo) == false)
    {
        Serial.println("Failed to obtain time");
        return;
    }

    log_d("We got local time");
    Serial.println(&timeinfo, "Local Time is: %A, %B %d %Y %H:%M:%S");

    RTC_Time time;
    time.hour = timeinfo.tm_hour;
    time.min = timeinfo.tm_min;
    time.sec = timeinfo.tm_sec;
    M5.RTC.setTime(&time);

    RTC_Date date;
    date.day = timeinfo.tm_mday;
    date.mon = timeinfo.tm_mon + 1;
    date.year = timeinfo.tm_year + 1900;
    M5.RTC.setDate(&date);
}

void getNTPTime()
{
    // Try to connect for 10 seconds
    uint32_t connect_timeout = millis() + 10000;

    log_d("Connecting to %s ", ssid);
    WiFi.begin(ssid, password);
    while ((WiFi.status() != WL_CONNECTED) && (millis() < connect_timeout))
    {
        delay(500);
        log_d(".");
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        // WiFi connection failed - set fantasy time and date

        log_d("WiFi connection failed - set fantasy time and date");

        RTC_Time time;
        time.hour = 11;
        time.min = 11;
        time.sec = 11;
        M5.RTC.setTime(&time);

        log_d("Fantasy Time is: %d : %d : %d", time.hour, time.min, time.sec);

        RTC_Date date;
        date.day = 11;
        date.mon = 11;
        date.year = 2011;
        M5.RTC.setDate(&date);

        return;
    }

    log_d("Connected");

    const long gmtOffset_sec = 3600;
    const int daylightOffset_sec = 3600;

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    printLocalTimeAndSetRTC();

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void SysInit_Start(void)
{
    bool ret = false;
    Serial.begin(115200);
    Serial.flush();
    delay(50);
    Serial.print("M5EPD initializing...");

    pinMode(M5EPD_EXT_PWR_EN_PIN, OUTPUT);
    pinMode(M5EPD_EPD_PWR_EN_PIN, OUTPUT);
    pinMode(M5EPD_KEY_RIGHT_PIN, INPUT);
    pinMode(M5EPD_KEY_PUSH_PIN, INPUT);
    pinMode(M5EPD_KEY_LEFT_PIN, INPUT);
    delay(100);

    M5.enableEXTPower();
    // M5.disableEPDPower(); // Keep commented out
    // delay(500);
    M5.enableEPDPower();
    delay(1000);

    M5.BatteryADCBegin();

    log_e("Battery Voltage: %d", M5.getBatteryVoltage());

    M5.RTC.begin();

    getNTPTime();

    M5.EPD.begin(M5EPD_SCK_PIN, M5EPD_MOSI_PIN, M5EPD_MISO_PIN, M5EPD_CS_PIN,
                 M5EPD_BUSY_PIN);

    // Don't do a full clear (true) which causes black blanking
    // Either use false (buffer clear only) or remove entirely
    // M5.EPD.Clear(false);  // Only clear buffer, not the physical display
    M5.EPD.SetRotation(M5EPD_Driver::ROTATE_90);
    M5.TP.SetRotation(GT911::ROTATE_90);

    // SysInit_UpdateInfo("Initializing SD card...");
    // ret = SD.begin(4, *M5.EPD.GetSPI(), 20000000);
    // if (ret == false)
    // {
    //     log_e("Failed to initialize SD card.");
    // }

    if (M5.TP.begin(21, 22, 36) != ESP_OK)
    {
        log_e("Touch pad initialization failed.");
    }
    taskYIELD();

    M5.BatteryADCBegin();
    LoadSetting(); // Keep loading basic settings like timezone if needed

    log_d("System Initialization done");

    Serial.println("OK");

    delay(500); // Keep a small delay
}
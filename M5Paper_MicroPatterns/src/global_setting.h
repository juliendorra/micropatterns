#ifndef _GLOBAL_SETTING_H_
#define _GLOBAL_SETTING_H_

#include <M5EPD.h>
#include <nvs.h>
#include <esp_task_wdt.h>

// Basic settings placeholder - can be expanded later
// Example: Timezone might still be relevant
extern int8_t global_timezone;

int8_t GetTimeZone(void);
void SetTimeZone(int8_t time_zone);

// Placeholder for loading/saving settings if needed
esp_err_t LoadSetting(void);
esp_err_t SaveSetting(void);

// Removed loading animation helpers
// Removed: const uint8_t* GetLoadingIMG_32x32(uint8_t id);
// Removed: void LoadingAnime_32x32_Start(uint16_t x, uint16_t y);
// Removed: void LoadingAnime_32x32_Stop();

// Keep shutdown helper
void Shutdown();

#endif // _GLOBAL_SETTING_H_
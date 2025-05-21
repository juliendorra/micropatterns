#ifndef __SYSTEMINIT_H
#define __SYSTEMINIT_H

#include <M5EPD.h> // M5EPD might still be needed if SysInit_Start does early M5 power enable

// SysInit_Start's responsibilities will be largely moved.
// - M5 power enable: Early in main's setup().
// - Serial.begin: Early in main's setup().
// - Watchdog init: MainControlTask or specific tasks.
// - GPIO init for buttons: InputManager.
// - Other power pins: SystemManager or main setup.
// - Battery ADC: SystemManager or main setup.
// - RTC.begin: SystemManager or main setup.
// - SPIFFS init: ScriptManager.
// - NTP time sync: SystemManager (using NetworkManager).
// - EPD init: DisplayManager.
// - Touch Panel init: Could be InputManager or a dedicated TouchManager if complex. For now, main setup.
// - NVS LoadSetting: SystemManager.
// - ISR service install & handler add: InputManager.

// The SysInit_Start function might become a smaller helper called from main's setup(),
// or its contents entirely integrated into setup() and manager initializations.
// For now, let's assume it might still do very early hardware power-up.

void SysInit_EarlyHardware(void); // For very basic power and serial

#endif //__SYSTEMINIT_H
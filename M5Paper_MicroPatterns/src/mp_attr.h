#pragma once
// MP_HOT marks the rasteriser's hot functions. With MP_HOT_IRAM defined it
// places them in IRAM on the ESP32, where there is no cache; the bench
// environments define it, the shipped firmware cannot (its IRAM is full: the
// Watchy firmware would overflow by ~20KB, the M5Paper by ~14KB).
//
// Why the bench needs it: code in flash goes through a small cache whose
// behaviour depends on where the linker put each function. Measured on the
// Watchy (2026-09-05): byte-identical code shifted by 8 bytes ran up to 35%
// faster or slower on real scripts, and a build with ~1KB more code in the
// rasteriser ran PIXEL 3x slower with no change in the work done. With the hot
// code in IRAM the same 8-byte shift moved the median by 0.0% (worst case 8%).
// So an A/B on the flash build compares placements; an A/B on the IRAM build
// compares algorithms. The shipped firmware still runs from flash and still
// has placement variance; this is the instrument, not the cure.
#if defined(MP_HOT_IRAM) && (defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32))
#include "esp_attr.h"
#define MP_HOT IRAM_ATTR
#else
#define MP_HOT
#endif

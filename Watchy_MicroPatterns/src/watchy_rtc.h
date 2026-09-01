#ifndef WATCHY_RTC_H
#define WATCHY_RTC_H

// The Watchy's external real-time clock, read so scripts get real
// $HOUR/$MINUTE/$SECOND instead of the zeros this firmware used to pass.
//
// WHY A DRIVER HERE AND NOT SmallRTC / Rtc_Pcf8563.
// docs/analysis/watchy-port-design.md named those as the intended dependency.
// SmallRTC is a Watchy-library component and drags that whole library (and its
// GxEPD2/display assumptions) into a build that already pins its own GxEPD2
// fork; Rtc_Pcf8563 is light but PCF8563-only, and this unit's chip has never
// been confirmed -- the hardware doc (section "Definitive RTC-chip check")
// records DS3231 on v1.0 and PCF8563 on v1.5/v2.0 per SQFMI, with the actual
// part on this watch still unverified. So this probes the bus and speaks to
// whichever chip answers, which is what SmallRTC does and the only part of it
// this firmware needs. Two register maps, about a hundred lines, no new deps.
//
// Both chips hold BCD time behind a register pointer and both carry a flag
// saying "my oscillator stopped, do not trust me" -- PCF8563's VL bit and
// DS3231's OSF bit. valid() reports that flag: it is what tells a fresh watch
// that it must go to NTP rather than draw a confident 00:00.

#include <Arduino.h>
#include <time.h>

namespace WatchyRTC {

enum Chip { CHIP_NONE = 0, CHIP_PCF8563, CHIP_DS3231 };

// Brings up I2C and probes for a chip. Safe to call more than once.
// Returns false when nothing answers, in which case every other call fails and
// the caller should carry on without a clock rather than block.
bool begin();

Chip chip();
const char* chipName();

// False when the chip says its time is not trustworthy (power was lost and the
// oscillator stopped), or when there is no chip at all.
bool valid();

// Local wall-clock time as the chip holds it. The chip stores whatever offset
// was written to it -- there is no timezone in hardware -- so this is local
// time exactly as long as set() was given local time.
bool now(int& hour, int& minute, int& second);

// Writes date and time, and clears the "untrustworthy" flag. tm_year/tm_mon
// follow the usual struct tm convention (years since 1900, months 0-11).
bool set(const struct tm& t);

}  // namespace WatchyRTC

#endif  // WATCHY_RTC_H

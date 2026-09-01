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
// oscillator stopped), when the test override below is in force, or when there
// is no chip at all.
bool valid();

// Local wall-clock time as the chip holds it. The chip stores whatever offset
// was written to it -- there is no timezone in hardware -- so this is local
// time exactly as long as set() was given local time.
bool now(int& hour, int& minute, int& second);

// Stages the cold-clock state for testing the boot-goes-to-NTP path, without
// opening the watch to disconnect the battery.
//
// It does NOT set the chip's own flag, and cannot: on the PCF8563 VL is
// CLEAR-ONLY in software. Writing 0x80 to the seconds register was tried on
// the device and the register read back 0x03 -- the seconds landed, bit 7 did
// not. Only the chip's low-voltage detector raises VL. (DS3231's OSF is
// writable, but making one chip behave one way and the other another is worse
// than one honest mechanism.)
//
// So the override is ours, in NVS, and valid() honours it. What that buys is a
// real end-to-end test of the decision AND the recovery: boot finds the clock
// untrusted, goes to NTP, and set() clears the override on success exactly as
// it clears the hardware flag -- so the next boot is normal again, repeatably.
//
// What it does NOT cover, and what the battery pull is still for: what the
// time registers actually read after a true power loss. They are undefined;
// this zeroes them, which is a guess at the pessimistic case.
bool invalidate();

// True while the NVS override above is in force, so the console can say
// "untrusted because we said so" rather than leaving it ambiguous against the
// hardware flag.
bool forcedUntrusted();

// The raw register byte carrying the trust flag -- PCF8563's VL_seconds (0x02)
// or DS3231's status (0x0F). Exposed because "the flag did not take" is
// otherwise indistinguishable from "the flag is not what decides", and the
// console is the only way to look at this chip. Returns false on a bad read.
bool statusByte(uint8_t& out);

// Writes date and time, and clears the "untrustworthy" flag. tm_year/tm_mon
// follow the usual struct tm convention (years since 1900, months 0-11).
bool set(const struct tm& t);

}  // namespace WatchyRTC

#endif  // WATCHY_RTC_H

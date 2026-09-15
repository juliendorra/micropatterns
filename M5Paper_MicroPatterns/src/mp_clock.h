#ifndef MP_CLOCK_H
#define MP_CLOCK_H

// NTP + timezone, shared by both firmwares.
//
// WHY THIS IS ONE MODULE. Setting the clock is three steps -- apply the
// timezone, ask SNTP, write the RTC -- and only the third differs between the
// devices (M5.RTC against WatchyRTC). Before this it was written twice, in
// SystemManager::syncTimeWithNTP() and the Watchy's syncTimeFromNTP(), and the
// two had already drifted: one read a timezone from NVS that nothing could
// write, the other had the offset as a `static const int` with a comment saying
// "changing zone means changing this line". So the two steps that are the same
// live here, and each firmware registers how to write its own RTC.
//
// WHAT "TIMEZONE" MEANS HERE. A POSIX TZ string, e.g.
//
//     CET-1CEST,M3.5.0,M10.5.0/3
//
// -- the standard offset, the daylight offset, and the two dates on which they
// swap. It is written over BLE by the editor, which derives it from the
// browser's zone (micropatterns_emulator/timezone.js), and stored by
// mp_provisioning. Note the POSIX sign convention: the offset is what you ADD
// TO LOCAL TIME TO GET UTC, so UTC+1 is spelled "-1".
//
// WHY RULES AND NOT AN OFFSET. Both devices keep LOCAL wall time in the RTC and
// only re-derive it at a sync, and the Watchy's RTC read returns no date at all,
// so no firmware here can notice that the March switch has passed. Daylight
// saving can therefore only ever be applied AT A SYNC -- which is why
// mp_sync_scripts() now resyncs the clock every time it has WiFi up, so a
// device corrects itself within one script sync of a switch instead of staying
// an hour out until someone reprovisions it.

#include <Arduino.h>
#include <time.h>

namespace MPClock {

// The TZ used when nothing has been provisioned: UTC+1, no daylight saving.
//
// Not a neutral choice, and deliberately not UTC -- it is what both firmwares
// already did (SystemManager's _timezone started at 1 and the Watchy hardcoded
// MP_TZ_OFFSET_HOURS = 1), so a device that never gets a timezone written keeps
// reading exactly as it did before this existed rather than jumping an hour.
static const char* DEFAULT_TZ = "STD-1";

static const char* NTP_SERVER = "pool.ntp.org";

// Writes a broken-down local time to the device's RTC. Returns false if the
// chip did not take it.
typedef bool (*RtcWriteFn)(const struct tm& local);

// Registered once at boot by each firmware. Without it syncNow() can still set
// the ESP32's own clock but will report that the RTC was not written.
void setRtcWriter(RtcWriteFn fn);

// Overrides DEFAULT_TZ with a fixed whole-hour offset east of UTC, used only
// when no TZ string has been provisioned.
//
// Exists for the M5Paper, which has carried an int8 `timezone` in its own NVS
// namespace since long before any of this. Nothing could ever write that key --
// there was no UI -- so in practice it is always 1, but silently ignoring a
// stored setting because we assume its value would be its own small bug.
void setFallbackOffsetHours(int hours);

// The TZ string in force: the provisioned one, or the fallback.
String activeTZ();

// True when the TZ in force came from the editor rather than the fallback.
bool provisioned();

// setenv("TZ") + tzset(), so localtime() and strftime() in this process agree
// with the stored zone. syncNow() does this itself; call it at boot if anything
// else formats a time.
void apply();

// Asks SNTP for the time and writes the RTC.
//
// REQUIRES WIFI TO ALREADY BE UP -- it neither connects nor disconnects, so it
// can be dropped into a path that has a connection open for another reason
// (mp_sync_scripts) without stealing it. Returns false on no reply within
// `timeoutMs`, in which case the RTC is left exactly as it was.
bool syncNow(uint32_t timeoutMs = 10000);

// The local time of the last successful syncNow(), for logging. Zeroed until
// one succeeds.
bool lastSync(struct tm& out);

} // namespace MPClock

#endif // MP_CLOCK_H

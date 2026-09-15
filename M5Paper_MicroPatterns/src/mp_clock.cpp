#include "mp_clock.h"

#include "mp_provisioning.h"
#include "esp32-hal-log.h"

namespace MPClock {
namespace {

RtcWriteFn g_writer = nullptr;
int  g_fallbackHours = 1;
bool g_haveLastSync = false;
struct tm g_lastSync = {};

// A whole-hour fallback as a POSIX string. Remember the inverted sign: east of
// UTC is written negative, so +1 becomes "STD-1".
String fallbackTZ()
{
    return String("STD") + (g_fallbackHours > 0 ? "-" : "+") + String(abs(g_fallbackHours));
}

} // namespace

void setRtcWriter(RtcWriteFn fn) { g_writer = fn; }

void setFallbackOffsetHours(int hours)
{
    if (hours < -14 || hours > 14) {
        log_w("Clock: fallback offset %d is not a real timezone; keeping %d", hours, g_fallbackHours);
        return;
    }
    g_fallbackHours = hours;
}

bool provisioned() { return MPProvisioning::posixTZ().length() > 0; }

String activeTZ()
{
    const String stored = MPProvisioning::posixTZ();
    return stored.length() ? stored : fallbackTZ();
}

void apply()
{
    const String tz = activeTZ();
    setenv("TZ", tz.c_str(), 1);
    tzset();
}

bool syncNow(uint32_t timeoutMs)
{
    const String tz = activeTZ();

    // configTzTime, NOT configTime(offset, dst, server).
    //
    // The offset form takes a fixed number of seconds and a fixed daylight
    // correction, which is why both firmwares were an hour out every summer:
    // there is nowhere in that call to say WHEN the correction applies, so both
    // passed 0 and gave up. Handing newlib the rules instead makes the switch
    // dates its problem, and it already knows how to do it.
    configTzTime(tz.c_str(), NTP_SERVER);

    struct tm local;
    // getLocalTime() polls until the system clock looks set, then applies TZ.
    if (!getLocalTime(&local, timeoutMs)) {
        log_w("Clock: no NTP reply within %ums; leaving the clock as it is", (unsigned)timeoutMs);
        return false;
    }

    g_lastSync = local;
    g_haveLastSync = true;

    if (!g_writer) {
        // The ESP32's own clock is now right, but it does not survive the deep
        // sleep either device spends most of its life in -- only the RTC chip
        // does. Say so rather than reporting success.
        log_e("Clock: NTP gave %02d:%02d:%02d but no RTC writer is registered",
              local.tm_hour, local.tm_min, local.tm_sec);
        return false;
    }
    if (!g_writer(local)) {
        log_e("Clock: NTP gave %02d:%02d:%02d but the RTC would not take it",
              local.tm_hour, local.tm_min, local.tm_sec);
        return false;
    }

    log_i("Clock: RTC set to %04d-%02d-%02d %02d:%02d:%02d (TZ=%s%s, dst=%s)",
          local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
          local.tm_hour, local.tm_min, local.tm_sec,
          tz.c_str(), provisioned() ? "" : ", fallback",
          local.tm_isdst > 0 ? "yes" : (local.tm_isdst == 0 ? "no" : "unknown"));
    return true;
}

bool lastSync(struct tm& out)
{
    if (!g_haveLastSync) return false;
    out = g_lastSync;
    return true;
}

} // namespace MPClock

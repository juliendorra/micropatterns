#include "watchy_rtc.h"

#include <Wire.h>
#include <Preferences.h>

// I2C pins, from InkWatchy's condition.h for Watchy 2.0 (SDA 21 / SCL 22, with
// the RTC's interrupt on 27 -- unused here: this firmware reads the clock, it
// does not take alarms from it).
#define RTC_SDA_PIN 21
#define RTC_SCL_PIN 22

// Bus addresses. These do not overlap, which is what makes the probe below a
// reliable identification rather than a guess.
#define ADDR_PCF8563 0x51
#define ADDR_DS3231  0x68

// PCF8563 register map (datasheet section 8.2).
#define PCF_CTRL1      0x00   // bit5 STOP
#define PCF_SECONDS    0x02   // bit7 VL: "voltage low, time is not reliable"
// DS3231/DS3232 register map.
#define DS_SECONDS     0x00
#define DS_STATUS      0x0F   // bit7 OSF: "oscillator has stopped"

namespace {

WatchyRTC::Chip g_chip = WatchyRTC::CHIP_NONE;
bool            g_begun = false;

// The test override lives in NVS rather than RAM so it survives the reboot it
// exists to test -- a RAM flag would be gone by the time boot read it.
const char* NVS_NS  = "mprtc";
const char* NVS_KEY = "untrusted";
bool g_forcedUntrusted = false;

void storeForced(bool v)
{
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putBool(NVS_KEY, v);
    p.end();
    g_forcedUntrusted = v;
}

uint8_t bcdToBin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
uint8_t binToBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool present(uint8_t addr)
{
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t len)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    // No stop condition: a repeated start is what keeps the register pointer
    // from being reset out from under the read on both of these chips.
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)addr, (int)len) != len) return false;
    for (uint8_t i = 0; i < len; ++i) buf[i] = Wire.read();
    return true;
}

bool writeRegs(uint8_t addr, uint8_t reg, const uint8_t* buf, uint8_t len)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    for (uint8_t i = 0; i < len; ++i) Wire.write(buf[i]);
    return Wire.endTransmission() == 0;
}

}  // namespace

namespace WatchyRTC {

bool begin()
{
    if (g_begun) return g_chip != CHIP_NONE;
    g_begun = true;

    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

    {
        Preferences p;
        if (p.begin(NVS_NS, true)) { g_forcedUntrusted = p.getBool(NVS_KEY, false); p.end(); }
    }

    if      (present(ADDR_PCF8563)) g_chip = CHIP_PCF8563;
    else if (present(ADDR_DS3231))  g_chip = CHIP_DS3231;
    else                            g_chip = CHIP_NONE;

    return g_chip != CHIP_NONE;
}

Chip chip() { return g_chip; }

const char* chipName()
{
    switch (g_chip) {
        case CHIP_PCF8563: return "PCF8563";
        case CHIP_DS3231:  return "DS3231/DS3232";
        default:           return "none";
    }
}

bool forcedUntrusted() { return g_forcedUntrusted; }

bool valid()
{
    if (g_forcedUntrusted) return false;

    uint8_t b = 0;
    switch (g_chip) {
        case CHIP_PCF8563:
            if (!readRegs(ADDR_PCF8563, PCF_SECONDS, &b, 1)) return false;
            return (b & 0x80) == 0;          // VL clear: integrity guaranteed
        case CHIP_DS3231:
            if (!readRegs(ADDR_DS3231, DS_STATUS, &b, 1)) return false;
            return (b & 0x80) == 0;          // OSF clear: oscillator never stopped
        default:
            return false;
    }
}

bool now(int& hour, int& minute, int& second)
{
    uint8_t r[7];
    switch (g_chip) {
        case CHIP_PCF8563:
            if (!readRegs(ADDR_PCF8563, PCF_SECONDS, r, 3)) return false;
            second = bcdToBin(r[0] & 0x7F);  // mask VL
            minute = bcdToBin(r[1] & 0x7F);
            hour   = bcdToBin(r[2] & 0x3F);
            break;
        case CHIP_DS3231:
            if (!readRegs(ADDR_DS3231, DS_SECONDS, r, 3)) return false;
            second = bcdToBin(r[0] & 0x7F);
            minute = bcdToBin(r[1] & 0x7F);
            // Bit6 selects 12-hour mode, where bit5 is then AM/PM. Nothing here
            // ever writes that mode, but a chip that came out of another
            // firmware may be left in it, and misreading it would put every
            // afternoon script twelve hours out.
            if (r[2] & 0x40) {
                hour = bcdToBin(r[2] & 0x1F) % 12;
                if (r[2] & 0x20) hour += 12;
            } else {
                hour = bcdToBin(r[2] & 0x3F);
            }
            break;
        default:
            return false;
    }
    // A chip that answers on the bus but returns nonsense BCD (a bad read, a
    // half-powered part) would otherwise reach scripts as, say, hour 93.
    return hour >= 0 && hour < 24 && minute >= 0 && minute < 60 &&
           second >= 0 && second < 60;
}

bool statusByte(uint8_t& out)
{
    switch (g_chip) {
        case CHIP_PCF8563: return readRegs(ADDR_PCF8563, PCF_SECONDS, &out, 1);
        case CHIP_DS3231:  return readRegs(ADDR_DS3231,  DS_STATUS,  &out, 1);
        default:           return false;
    }
}

bool invalidate()
{
    if (g_chip == CHIP_NONE) return false;

    storeForced(true);

    // Zero the visible time too, so the watch shows the midnight a cold chip
    // would rather than carrying on with the correct time behind an override.
    // The trust flag itself is untouched -- see the header for why it cannot be
    // set from here.
    uint8_t r[3] = { 0x00, 0x00, 0x00 };
    const uint8_t addr = (g_chip == CHIP_PCF8563) ? ADDR_PCF8563 : ADDR_DS3231;
    const uint8_t reg  = (g_chip == CHIP_PCF8563) ? PCF_SECONDS  : DS_SECONDS;
    return writeRegs(addr, reg, r, 3);
}

static bool writeClock(const struct tm& t)
{
    const int year = t.tm_year + 1900;

    if (g_chip == CHIP_PCF8563) {
        // Stop the counter while the registers are rewritten, per the
        // datasheet: a carry landing mid-write would corrupt the value being
        // written. Writing seconds also clears VL, which is how the chip is
        // told its time is trustworthy again.
        uint8_t stop = 0x20;
        if (!writeRegs(ADDR_PCF8563, PCF_CTRL1, &stop, 1)) return false;

        uint8_t r[7];
        r[0] = binToBcd((uint8_t)t.tm_sec);          // VL cleared by writing 0
        r[1] = binToBcd((uint8_t)t.tm_min);
        r[2] = binToBcd((uint8_t)t.tm_hour);
        r[3] = binToBcd((uint8_t)t.tm_mday);
        r[4] = (uint8_t)t.tm_wday;
        r[5] = binToBcd((uint8_t)(t.tm_mon + 1));
        // Century bit (bit7 of the months register) means 19xx on this chip;
        // leave it clear for 20xx and store the two-digit year.
        r[6] = binToBcd((uint8_t)(year % 100));
        if (!writeRegs(ADDR_PCF8563, PCF_SECONDS, r, 7)) return false;

        uint8_t run = 0x00;
        return writeRegs(ADDR_PCF8563, PCF_CTRL1, &run, 1);
    }

    if (g_chip == CHIP_DS3231) {
        uint8_t r[7];
        r[0] = binToBcd((uint8_t)t.tm_sec);
        r[1] = binToBcd((uint8_t)t.tm_min);
        r[2] = binToBcd((uint8_t)t.tm_hour);         // bit6 clear: 24-hour mode
        r[3] = (uint8_t)(t.tm_wday + 1);             // this chip counts 1-7
        r[4] = binToBcd((uint8_t)t.tm_mday);
        r[5] = binToBcd((uint8_t)(t.tm_mon + 1));    // bit7 century, left clear
        r[6] = binToBcd((uint8_t)(year % 100));
        if (!writeRegs(ADDR_DS3231, DS_SECONDS, r, 7)) return false;

        // OSF is sticky: it stays set after a power loss until software clears
        // it, so without this the clock would report itself untrustworthy
        // forever and every boot would go back to NTP.
        uint8_t status = 0;
        if (!readRegs(ADDR_DS3231, DS_STATUS, &status, 1)) return false;
        status &= (uint8_t)~0x80;
        return writeRegs(ADDR_DS3231, DS_STATUS, &status, 1);
    }

    return false;
}

bool set(const struct tm& t)
{
    if (!writeClock(t)) return false;

    // The time is trustworthy again, so the test override lifts exactly where
    // the hardware flag is cleared -- and only on success, or one failed NTP
    // reply would quietly end the test it was staged for.
    if (g_forcedUntrusted) storeForced(false);
    return true;
}

}  // namespace WatchyRTC

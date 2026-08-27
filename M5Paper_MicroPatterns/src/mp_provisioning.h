#ifndef MP_PROVISIONING_H
#define MP_PROVISIONING_H

// BLE provisioning: WiFi network list + secret user ID, written from the editor.
//
// SHARED between the M5Paper and Watchy firmwares, compiled from this one file
// by both -- the same arrangement as the renderer core. Two implementations of
// one protocol would drift.
//
// Why BLE for both rather than serial for one: serial does not work on the
// Watchy at all (the official InkWatchy image runs visibly while emitting zero
// bytes over 60s), and the M5Paper's USB bridge powers down when it light-
// sleeps after 3s. BLE also reaches Chrome on Android, which Web Serial does
// not. Measured cost on the constrained device: +16KB static RAM, +793KB flash.
// See docs/analysis/device-provisioning-design.md.
//
// Protocol -- newline-terminated JSON, chunked to fit the ~185 byte MTU:
//   {"cmd":"provision","userId":"...","networks":[{"ssid":"..","psk":".."}]}
//   {"cmd":"status"}   -> {"ok":true,"userId":"..","ssids":[..],"connected":".."}
//   {"cmd":"forget"}   -> wipes stored credentials
//
// status NEVER returns stored passwords.

#include <Arduino.h>

// Bumped when the wire format changes, so the editor can warn about a firmware
// it does not understand rather than failing obscurely.
#define MP_PROVISIONING_VERSION "1"

namespace MPProvisioning {

// Custom 128-bit UUIDs ("mprovision" in the first bytes) so the editor can find
// the device without relying on its advertised name.
static const char* SERVICE_UUID = "6d70726f-7669-7369-6f6e-000000000001";
static const char* CHAR_WRITE_UUID  = "6d70726f-7669-7369-6f6e-000000000002";
static const char* CHAR_NOTIFY_UUID = "6d70726f-7669-7369-6f6e-000000000003";

static const int MAX_NETWORKS = 5;

// Loads stored config from NVS. Does NOT start BLE -- call openWindow() for
// that, so the radio is only up when the user asked for it.
void begin();

// Starts advertising for `ms`, then stops on its own. Provisioning is a
// deliberate act: the window should be opened by a physical button, otherwise
// anyone in range could repoint the device at their own scripts.
void openWindow(uint32_t ms = 120000);
void closeWindow();
bool windowOpen();

// Call from the main loop; closes the window when it expires.
void tick();

// --- stored configuration -------------------------------------------------
bool    isProvisioned();          // true once a user ID has been written
String  userId();                 // empty if never provisioned
int     networkCount();
String  networkSSID(int index);
String  networkPSK(int index);

// Index of the network that last connected successfully, or -1.
// Try this one FIRST: walking a 5-network list in order costs a full
// association timeout each time, which on battery is the difference between a
// 2s and a 20s wake.
int  lastGoodIndex();
void setLastGood(int index);

} // namespace MPProvisioning

#endif // MP_PROVISIONING_H

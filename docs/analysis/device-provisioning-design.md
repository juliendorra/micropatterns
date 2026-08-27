# Provisioning devices from the editor (WiFi list + user ID)

Status: **design, not implemented.** Written 2026-08-27.

## Why this is a fix, not a feature

Credentials are compile-time constants in `network_manager.cpp`:

```cpp
const char *NetworkManager::WIFI_SSID_DEFAULT     = "OpenWrt2.4";
const char *NetworkManager::WIFI_PASSWORD_DEFAULT = "hudohudo";
const char *NetworkManager::USER_ID_DEFAULT       = "kksh2hjtkb";
```

`github.com/juliendorra/micropatterns` is **public**, and those values are in
git history across several commits. So:

- **The WiFi password is publicly readable and must be rotated.** Deleting the
  line does not help; the history holds it.
- **The user ID is publicly readable**, and it is identity *and* authentication
  for the script API — anyone can read and modify the scripts.
- One firmware build is bound to one network. A watch cannot move between home,
  work and a phone hotspot, which is exactly what a *watch* needs to do.

## The constraint that decides the architecture

**Serial does not work on the Watchy.** Established on 2026-08-27: the official
InkWatchy image runs visibly while emitting **zero bytes over 60 seconds**, and
no mechanism was ever found (see `watchy-port-attempt-log.md` §4.2d). Our own
firmware is equally silent. The motor and the panel are the only channels that
have ever reported anything from that device.

So a single serial-based provisioning path cannot cover both devices.

| | M5Paper | Watchy |
|---|---|---|
| USB bridge | CH9102 | CP2102N / CH9102 (varies by unit) |
| Serial in run mode | **works** — used all day | **dead** |
| BLE | yes (ESP32-D0WDQ6) | yes (ESP32-PICO-D4) |
| PSRAM | 4.5 MB | **none**, ~300 KB heap |

## Recommendation

**Web Serial for the M5Paper, Web Bluetooth for the Watchy, one shared UI in the
editor.**

Note this is **Web Serial**, not WebUSB. WebUSB cannot claim a CH9102/CP2102N —
those are USB-UART bridges owned by the OS driver, and the ESP32 classic has no
native USB of its own. Web Serial is the API that actually talks to them, and it
is what Chrome/Edge expose for exactly this case.

Both APIs require HTTPS and a user gesture. The editor is HTTPS and already
holds the user ID, so it is the natural host — and provisioning from the same
page that manages scripts means the ID is never typed twice.

### Browser reality, stated plainly

| API | Chrome/Edge desktop | Safari | Firefox | Android Chrome | iOS |
|---|---|---|---|---|---|
| Web Serial | yes | **no** | **no** | no | **no** |
| Web Bluetooth | yes | **no** | **no** | yes | **no** |

Neither works on iOS at all, and neither works in Safari or Firefox. This is a
Chrome/Edge feature. If provisioning must work from an iPhone, the fallback is a
**SoftAP captive portal** on the device — worth designing for, not building
first.

## Telling the user when their browser cannot do this

Feature-detect on load and **never render a button that cannot work**. The two
APIs fail on different platforms, so one generic "unsupported browser" message
is wrong in both directions — a Chrome user on Android has Web Bluetooth but not
Web Serial, and can therefore provision the Watchy but not the M5Paper.

```js
const canSerial    = "serial"    in navigator;   // Chrome/Edge desktop only
const canBluetooth = "bluetooth" in navigator;   // + Chrome Android
```

Rules:

1. **Detect per transport, not per page.** Show the M5Paper option only if
   `canSerial`, the Watchy option only if `canBluetooth`. If neither, replace
   the whole panel with the explanation rather than disabled controls.
2. **Say what to do, not what is missing.** "Connecting a device needs Chrome or
   Edge on a computer" beats "navigator.serial is undefined".
3. **Name iOS explicitly.** Neither API exists in ANY iOS browser — including
   Chrome on iOS, which is Safari underneath. A user who installs Chrome on
   their iPhone to fix this will fail again and blame the page. Detect iOS and
   say so directly.
4. **Distinguish the three failure modes**, because they look identical to a
   user and have different fixes:
   - browser cannot (wrong browser/platform) -> switch browser or use SoftAP
   - user dismissed the chooser (`NotFoundError`) -> retry, no drama
   - device not responding after a port/device was picked -> the device-side
     problem; for the M5Paper, note it sleeps after 3 s and the bridge powers
     down with it (`tools/device/SERIAL-TROUBLESHOOTING.md`)
5. **`navigator.bluetooth` can exist and still be unusable** — Bluetooth off at
   the OS level, or no permission. Present that as "turn on Bluetooth", not as
   an unsupported browser.
6. Both APIs require **HTTPS and a user gesture**. The deployed editor is HTTPS,
   but a `file://` copy will silently lack both — worth detecting, since editing
   locally is a normal thing to do here.

The honest summary for the UI: this works in **Chrome or Edge on a computer**,
Web Bluetooth also works in **Chrome on Android**, and **nothing works on
iPhone or iPad**. If that matrix is too narrow, the SoftAP captive portal is the
only route that reaches every device.

## Wire format

One schema, two transports, so the editor has a single code path:

```jsonc
// editor -> device
{ "cmd": "provision",
  "userId": "kksh2hjtkb",
  "networks": [                       // ordered, tried in sequence
    { "ssid": "home",  "psk": "..." },
    { "ssid": "work",  "psk": "..." },
    { "ssid": "phone", "psk": "..." }
  ] }
{ "cmd": "status" }                   // -> current SSID, RSSI, userId, version
{ "cmd": "forget" }                   // wipe stored credentials
```

- **Serial**: newline-delimited JSON at 115200, reusing the existing
  `MPCON|` console (`serial_console.cpp`) which already parses commands.
- **BLE**: one GATT service, a write characteristic for the command and a notify
  characteristic for the reply, chunked — MTU is ~185 bytes after negotiation
  and a three-network payload will exceed it.

## Storage

NVS on both, via `Preferences`. The M5Paper's `SystemManager` already owns NVS,
so it gains the keys; the Watchy needs a small settings module.

```
wifi.count      u8
wifi.0.ssid     str     wifi.0.psk   str
wifi.1.ssid     str     wifi.1.psk   str        (cap at 4-5)
wifi.last       u8      index that last connected -- try it FIRST
user.id         str
```

`wifi.last` matters more than it looks: trying four networks in order costs a
full association timeout each when you are at work and home is first in the
list. On a battery device that is the difference between a 2-second and a
20-second wake.

## Connection logic

Replace the single `WiFi.begin(SSID, PASS)` with: try `wifi.last`, then the rest
in order, short timeout each (~8 s), remember whichever succeeds. Keep the
existing `NO_WIFI` result so the fetch backoff added today still applies — a
device that cannot find any known network must not retry every wake.

## Security

- **The write path must require physical confirmation.** BLE is wireless, so
  without a gesture anyone nearby could rewrite the user ID and point the device
  at their own scripts. Require a button press to open a provisioning window of
  ~60 s, and show it on the panel.
- Never make credentials readable back: `status` returns the *current SSID*, not
  the stored PSKs.
- The user ID is a bearer token in a URL path. Provisioning is the moment to
  rotate it — issue a fresh one from the editor and write it to both the device
  and S3.

## Order of work

1. **Rotate the exposed WiFi password and user ID.** Independent of any code.
2. NVS storage + multi-network connect logic on the M5Paper, with the hardcoded
   values as a fallback until provisioning exists. Testable immediately.
3. Web Serial provisioning in the editor, against the M5Paper. Fastest path to
   something real, because serial there already works.
4. BLE provisioning for the Watchy. More work: BLE stack RAM on a device with
   ~300 KB heap and no PSRAM needs measuring before committing.
5. SoftAP captive-portal fallback, only if iOS provisioning is needed.

## Rejected

- **WebUSB.** Cannot claim a USB-UART bridge; ESP32 classic has no native USB.
- **Serial-only for both.** The Watchy cannot do it — see the constraint above.
- **WiFiProv / ESP-IDF unified provisioning.** Pulls in its own BLE protocol and
  phone apps, and does not fit "configure it from the editor you already have
  open". Worth revisiting if the custom BLE service proves fiddly.
- **Storing credentials in the script list on S3.** Would put the WiFi password
  behind a bearer token that is itself in a public repo.

# Provisioning devices from the editor (WiFi list + user ID)

Status: **design, not implemented.** Written 2026-08-27.

## Why this is a fix, not a feature

Credentials are compile-time constants in `network_manager.cpp`:

```cpp
const char *NetworkManager::WIFI_SSID_DEFAULT     = "<an SSID>";
const char *NetworkManager::WIFI_PASSWORD_DEFAULT = "<a plaintext password>";
const char *NetworkManager::USER_ID_DEFAULT       = "<the secret user ID>";
```

`github.com/juliendorra/micropatterns` is **public**, and the real values are in
git history. Exposed in `8e911e7` (2025-05-06), `6c1b5cd` (2025-05-21) and
`536e62b` (2025-05-26), all already pushed. The user ID additionally appears in
`8759166`. Values are redacted above so this document does not add another copy.
So:

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

## Recommendation: Web Bluetooth for BOTH devices

An earlier draft split this — Web Serial for the M5Paper, Web Bluetooth for the
Watchy — on the assumption that BLE might not fit the Watchy. **Measured, it
fits**, so aligning on one transport is better.

BLE footprint on the Watchy (ESP32-PICO-D4, no PSRAM), measured by building the
real firmware with a BLE server, service and characteristic:

| | without BLE | with BLE | delta |
|---|---|---|---|
| RAM (static) | 21,876 B (6.7%) | 38,068 B (11.6%) | **+16 KB** |
| Flash | 382,689 B (12.2%) | 1,175,581 B (37.4%) | **+793 KB** |

37% of flash and 12% of static RAM on the *constrained* device. The M5Paper has
4.5 MB of PSRAM and 6.5 MB of flash, so it is not a question there.

Aligning wins on every axis that matters here:

- **One protocol, one editor code path, one firmware module.** The same
  argument that made the renderer core shared: two implementations of the same
  idea drift, and this project already compiles one renderer for two devices.
- **No cable.** Provisioning a *watch* by USB is the wrong shape, and the
  M5Paper's bridge disappears when it sleeps after 3 s — a trap that has already
  cost hours (`tools/device/SERIAL-TROUBLESHOOTING.md`).
- **Wider reach.** Web Bluetooth also works in Chrome on Android; Web Serial is
  desktop-only. BLE-only is a *superset* of what serial would have covered.
- Serial provisioning would have needed the console extended anyway.

**Serial stays as a debug path on the M5Paper**, because `serial_console.cpp`
already exists and costs nothing to keep. It is a developer convenience, not the
supported route, and the Watchy cannot use it at all.

Note this is **Web Bluetooth**, and explicitly **not WebUSB**: WebUSB cannot
claim a CH9102/CP2102N — those are USB-UART bridges owned by the OS driver — and
the ESP32 classic has no native USB of its own.

### Caveat on the measurement

The +16 KB is **static** RAM. The BLE stack allocates more at runtime once
advertising and connected; that was not measured. On the Watchy that lands on a
~300 KB heap that also holds the display list, so measure free heap while
connected before trusting it. Mitigation if it bites: `BLEDevice::deinit()` once
provisioning completes, so the cost is only paid during the provisioning window
and not for the rest of the device's life.

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

## The panel as built: rules the editor UI has to keep

The panel lives in a `controls-group`, and in the default Memphis theme that
group paints its text directly onto a background pattern. That single fact
decides most of the styling:

- **Nothing that carries text may be partly transparent.** Dimming with
  `opacity` is what everything unavailable used to do, and over a patterned
  background the pattern reads straight through the glyphs. Disabled buttons are
  now solid (and `button.secondary:disabled` has to be spelled out, or the
  `.secondary` colour out-specifies the disabled one and only the opacity
  survives). The log paints its own background. An unticked section dims to
  0.7, not 0.45 — enough signal, still legible.
- **The `<details>` summary needs its own solid chip**, for the same reason:
  bold black on black squiggles is unreadable. Keep it `display: list-item`
  (`inline-block` drops the disclosure triangle), and keep it short — it is a
  dropdown label, not a description. The description belongs in the body.
- **Every theme must style `input[type="password"]`.** The themes styled
  `text`/`number`/`select` only, so the WiFi password field fell back to the
  browser default next to a themed SSID field in the same row.

Two naming/behaviour decisions worth not re-litigating:

- **The device picker is always filtered by the service UUID.** There was an
  "show all Bluetooth devices" checkbox that switched `requestDevice` to
  `acceptAllDevices`. It was removed: it listed every radio in range, which is
  confusing when the one thing you want is the device you just pressed a button
  on. The firmware advertises the service UUID (`adv->addServiceUUID`), so a
  device missing from the picker is not advertising — which is what the
  "no device chosen" message now says, instead of pointing at a checkbox.
- **`forget` is not "forget the ID".** It runs `prefs.clear()` on the whole
  `mpprov` namespace: user ID, every SSID and PSK, the count, and `w.last`. The
  button says "Erase WiFi + ID" so the label matches, and it still confirms
  first. Rotating a leaked ID on its own is the *other* path — tick "Write the
  user ID" and untick the networks.

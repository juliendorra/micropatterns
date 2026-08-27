# Talking to these devices over USB: failure modes and how to tell them apart

Every entry here cost real time on 2026-08-27. Several look identical at a
glance and have completely different causes, which is exactly why they cost so
much. Read the "how to tell" column before acting.

## Quick triage: the port is missing or won't open

| Symptom | Cause | Fix |
|---|---|---|
| No `/dev/cu.usbserial-*` at all, and `system_profiler` shows no bridge | **M5Paper is asleep** (see 1) | Press any button on the device |
| Port exists but opening it throws `termios.error: (22, 'Invalid argument')` or `device reports readiness to read but returned no data` | **macOS driver wedge** (see 2) | Physically unplug and replug the cable |
| Port opens, device silent | Depends on device — see 4 and 5 | |

**Check `system_profiler SPUSBDataType` before concluding anything.** If the
bridge is not in the USB tree, the Mac cannot see the device and no amount of
retrying in software will help. That single check distinguishes 1 from 2.

## 1. The M5Paper vanishes from USB when it sleeps — this is NORMAL

The firmware light-sleeps after `SLEEP_IDLE_THRESHOLD_MS` (**3 seconds**) of
idle, and **the CH9102 USB-serial bridge powers down with it**, removing
`/dev/cu.usbserial-*` entirely.

So a plugged-in, perfectly healthy M5Paper disappears from the Mac within
seconds of being left alone. It is not broken, not wedged, and not disconnected.

- **Wake it with a button press**, then act immediately; the window is short.
- Every successful flash is done right after a reset or a wake.
- `dump_scripts.sh` carries the same warning in its header.

**Do not diagnose this as a wedge.** It was misdiagnosed as one twice, and the
distinguishing evidence is free: a wedge leaves the bridge *present* in
`system_profiler` and the port *present* in `/dev`; sleep removes both.

## 2. The macOS CH9102 driver wedge — port present, unusable

Distinct from 1. The port node exists, but opening it fails:

```
termios.error: (22, 'Invalid argument')
serial.serialutil.SerialException: device reports readiness to read but
returned no data (device disconnected or multiple access on port?)
```

`lsof` shows nothing holding it. esptool fails the same way, so it is below
pyserial. **Only a physical replug clears it.** Seen twice in one day.

## 3. Opening the port RESETS the ESP32 — and it used to destroy data

`pyserial` asserts DTR on open, which resets the chip. Consequences:

- A command sent immediately after opening hits a **booting** device. Sync on a
  known banner (`serial console ready`) rather than guessing a settle delay.
- Every open increments the M5Paper's NVS fresh-start counter. Before the
  2026-08-27 fix, enough of those triggered a full refresh that **deleted every
  script on the device** — see `docs/incidents/2026-08-27-m5paper-script-loss.md`.
  Fixed, but back up first anyway: `tools/device/dump_scripts.sh`.

## 4. Baud rates above 115200 corrupt transfers on BOTH bridges

Not a maybe. Observed failures:

- M5Paper SPIFFS read at 921600: `Unable to verify flash chip connection`
- M5Paper SPIFFS read at 460800: `Invalid head of packet ... serial noise`
- Watchy `pio run -t upload` at 460800: `Failed to connect ... No serial data received`

All of them succeed at **115200**. Both `upload_speed` settings are pinned there
with comments. A 3.4 MB read takes ~324 s at that rate; that is the price.

## 5. The Watchy emits NOTHING over serial, even when running fine

**Silence proves nothing on the Watchy.** The official prebuilt InkWatchy image —
visibly running, updating its display — produced **zero bytes in a 60-second
capture** at 115200. No mechanism was ever established for why run-mode UART does
not reach the host while esptool's stub talks fine over the same pins.

Hours were lost reasoning from that silence. Do not repeat it. Use an instrument
that is known to work:

- `tools/device/buzz_watchy/` — motor-only firmware, a **positive control**.
  Buzzes → the device runs your code, so the fault is your firmware. Silent →
  the fault is the build/flash/boot path.
- `-DMP_STAGE_BUZZ=1` in the Watchy firmware — a FreeRTOS task repeats the
  furthest boot stage reached, forever, so it keeps reporting even while
  `setup()` is blocked.
- `-DMP_PANEL_PROBE=1` — diagonal stripes drawn right after `init()`, before any
  parser/runtime/renderer code can hang.

## 6. Identifying which device is on which port

Port names are not stable and the names do not tell you what is behind them.

| device | USB bridge | VID:PID | chip | MAC |
|---|---|---|---|---|
| M5Paper | CH9102F | `1a86:55d4` | ESP32-D0WDQ6-V3 | `30:c6:f7:1f:79:f8` |
| Watchy 2.0 | CP2102N | `10c4:ea60` | ESP32-PICO-D4 | `4c:75:25:a7:65:c0` |

```bash
system_profiler SPUSBDataType | grep -E 'CP2102N|CH9102|Product ID'   # which bridges
esptool.py --port /dev/cu.usbserial-XXX flash_id                       # which chip + MAC
```

A device was misidentified early on by trusting the port name and an assumption
about which was plugged in; the descriptors settle it in seconds.

## 7. esptool flags that change what you are measuring

- **`--after no_reset` leaves the chip in the ROM loader.** A later
  `--before no_reset` then "discovers" the chip in download mode — which is the
  state your own previous flag created, not a property of the device. This
  produced a completely wrong conclusion that was committed before being caught.
- `--before default_reset` succeeding proves the bridge, EN and GPIO0 all work.
- Read the ROM banner's **`rst:` cause**. `rst:0x10 (RTCWDT_RTC_RESET)` is a
  watchdog reset, not a pin reset — misreading that one line produced two wrong
  conclusions about reset-line polarity.
- Bootloader path is `tools/sdk/**esp32**/bin/bootloader_dio_40m.bin`. The
  `esp32c3` one fails with `Unexpected chip id in image` — esptool catches it.

## 8. Host-side scripting traps that look like device faults

Both of these produced "no output" that was nearly read as a dead device:

- **`timeout` does not exist on macOS.** Commands wrapped in it silently produce
  nothing.
- **zsh errors on unmatched globs.** `ls /dev/cu.usbserial-537A*` with no match
  aborts the enclosing script *before* `ls` runs, so `2>/dev/null` does not help.
  A watcher loop built this way died on its first iteration and then reported
  "never appeared". Use `find /dev -maxdepth 1 -name 'cu.usbserial-537A*'`.

> **The recurring lesson:** verify your instrument reads a known-good state
> before trusting it to report a bad one. Nearly every long detour on this
> hardware came from a tool returning nothing for reasons unrelated to the
> device under test.

# Incident: the M5Paper deleted every script on the device (2026-08-27)

**Severity:** total loss of user data that existed in exactly one place.
**Outcome:** fully recovered from a backup taken ~40 minutes earlier. Root cause fixed.

This is written as a history, not just a handoff. The dead ends are recorded on
purpose: several of them were plausible and cost real time, and the next person
down this path should not have to re-derive why they were wrong.

## 1. What was lost

Six scripts, stored ONLY on the device's SPIFFS partition. The repo tracked one
unrelated script. There was no server copy (see §4).

| fileId | id | name | bytes |
|---|---|---|---|
| s0 | `circuits` | Circuits | 8064 |
| s1 | `city-2-by-telohtrab` | City 2 by Telohtrab | 14850 |
| s2 | `city-by-telohtrab` | City by Telohtrab | 11852 |
| s3 | `eyes` | Eyes | 5350 |
| s4 | `reconnected` | Re/Connected | 1791 |
| s5 | `thunderstorms` | Thunderstorms | 1652 |

## 2. Timeline

- **14:35** — Backup taken with `tools/device/dump_scripts.sh` before any
  flashing. Read the SPIFFS partition (`0xc90000`, `0x370000`) and unpacked it.
  All six scripts present and valid. This backup turned out to be the only
  surviving copy.
- **~15:00** — Firmware flashed (serial console feature). Flash erase covered
  `0x10000`–`0x14bfff` only; SPIFFS at `0xc90000` was demonstrably untouched.
- **~15:10** — Device observed rendering `default_fallback_script`. Boot log
  showed `/scripts` did not exist and had to be recreated. SPIFFS was empty.
- **~15:50** — Root cause found (§3), fix applied, scripts restored from backup.
- **~16:00** — Verified: six scripts load, and a *failing* fetch no longer wipes.

## 3. Root cause

`FetchTask` performed a "full refresh" by deleting everything **before**
fetching the replacements:

```
if (job.full_refresh) {
    g_scriptManager->clearAllScriptData();   // <-- destroys all local scripts
}
resultData.status = g_networkManager->fetchScriptList(serverListDoc);  // <-- then fails
```

The fetch returned `404 Not Found (DEPLOYMENT_NOT_FOUND)`. So the delete
succeeded and the refill never happened. Any fetch failure — 404, dropped WiFi,
malformed list — would have had the same effect.

**What armed it:** `MainControlTask` increments a fresh-start counter in NVS on
every cold boot; at `== 1` or past `FRESH_START_THRESHOLD` it sets full-refresh
intent. Reflashing reboots the device, and *every `pyserial` port open also
resets the ESP32 via DTR*, so an afternoon of flashing and serial testing
incremented that counter repeatedly until it tripped. The user's own earlier
reboot did not trip it — the 14:35 backup still contained data.

**Attribution:** the destructive ordering was pre-existing, but the reboots that
tripped it came from this session's flashing and serial probing. Backing up
first is what made this recoverable.

## 4. Why the device could not self-heal

The API returns 404 because **Deno Deploy Classic was sunset on 2026-07-20** and
no longer serves deployments. Recovered verbatim from the device's own boot log:

```
fetchScriptList: Error response: 404: Not Found (DEPLOYMENT_NOT_FOUND)
... was sunset on July 20, 2026 and no longer serves deployments.
```

This is a server-side task (redeploy to the new platform) and is unrelated to
firmware. Until then the device is offline-only — which is now safe.

Separately, the device currently also fails to associate with WiFi at all
(`Reason: 203 - ASSOC_FAIL` against SSID `OpenWrt2.4`). Unrelated to this
incident and not investigated.

## 5. The fix

The pre-emptive wipe was **deleted, not reordered**. It was redundant as well as
dangerous:

- `saveScriptList()` replaces `list.json` wholesale.
- The content loop overwrites each content file.
- `cleanupOrphanedContent()` / `cleanupOrphanedStates()` already remove anything
  the server no longer lists — and they run only **after** a fully successful
  sync.

So deleting nothing up front reaches the same end state, and a failed fetch is
now a no-op instead of data loss.

**Verified on hardware:** the fetch still 404s, and the device keeps its six
scripts and re-renders the current one.

## 6. Restore procedure (worked, reusable)

```bash
mkspiffs_espressif32_arduino -c backups/<stamp>/files -b 4096 -p 256 -s 3604480 restore.bin
esptool.py --port <port> --baud 115200 write_flash 0xc90000 restore.bin
```

Note `--baud 115200`: see §7.

## 7. Dead ends and gotchas (recorded deliberately)

- **High baud rates do not work on these bridges.** The SPIFFS read failed at
  `921600` (`Unable to verify flash chip connection`) and again at `460800`
  (`Invalid head of packet ... serial noise`). Only `115200` completed — 324 s
  for 3.4 MB. `dump_scripts.sh` gained a `--baud` flag because of this. The
  Watchy's CP2102N behaved the same way: `pio run -t upload` failed at 460800
  while plain esptool at 115200 worked.
- **`_getHighestFileIdNumber_nolock: Content directory /scripts/content does not
  exist`** appears after a mkspiffs-based restore. SPIFFS has no real
  directories and the image contains no directory marker the firmware expects.
  Scripts load and render fine; this affects only new fileId generation. Left
  alone, but it is a real difference between a restored and a device-grown
  filesystem.
- **A wrong hypothesis worth remembering:** the wipe was initially suspected to
  be caused by the flash erase itself. It was not — the erase range
  (`0x10000`–`0x14bfff`) is nowhere near SPIFFS (`0xc90000`). Confirming the
  erase range from esptool's own output ruled this out in seconds and should be
  the first check next time.
- **`clearAllScriptData()` has exactly one caller.** `grep` for it before
  assuming there are other wipe paths; there are not.

## 8. Prevention

- Run `tools/device/dump_scripts.sh` before flashing an M5Paper. It is read-only
  (`esptool read_flash` only) and cheap.
- The backup at `tools/device/backups/2026-08-27-143524/` is committed to the
  repo because it was, for a period, the only copy of this user's work.

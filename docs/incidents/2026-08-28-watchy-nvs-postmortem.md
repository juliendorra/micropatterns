# Post-mortem: the Watchy NVS hunt, and how it went wrong

**Status: RESOLVED.** Root cause in section 8. The rest of this document is the
process post-mortem written while it was still unresolved, kept unchanged
because the wrong turns are the useful part.

**The reproducer is committed:** `tools/device/probes/nvs-probe/`. Two envs, one
source -- `arduino2` fails, `arduino3` passes. Run it before blaming NVS on
anything else, and run it again on any future platform bump.

Written at the project owner's request, after the observation that "instead of
following best practices you invented code that doesn't work."

## 1. The symptom

BLE provisioning stores WiFi credentials and a user ID in NVS. On the M5Paper it
works. On the Watchy:

```
open=ESP_OK  set=ESP_OK  commit=ESP_OK  get=ESP_ERR_NVS_NOT_FOUND
used=0  free=630  total=630   part=@0x9000 sz=0x5000
```

`nvs_set_u8` reports success and stores nothing. Enumerating the whole partition
finds zero entries.

## 2. What was ruled out (each of these cost a flash-and-ask cycle)

| Hypothesis | How it was killed |
|---|---|
| NVS never initialised on the Watchy | True and fixed -- the M5Paper inits NVS in `systeminit.cpp`, the Watchy never did. Not the cause. |
| The success reply was unverified | True and fixed -- it reported networks *parsed*, not stored. Not the cause. |
| Stale NVS region left by InkWatchy's layout | Erased the region and verified `0xFF`. No change. |
| BLE MTU truncation eating replies | True and fixed -- 180-byte chunks over a 23-byte default MTU. Not the cause. |
| Work done on the BLE callback task | True and fixed -- JSON, NVS and chunked notifies ran on the BLE stack's small-stack task. Not the cause. |
| Single-slot command queue dropping commands | True and fixed. Not the cause. |
| Flash physically unwritable at `0x9000` | Wrote a pattern with esptool, read it back byte-identical. |
| Flash block protection | `read_flash_status` -> `SR1 = 0x00` on both devices. No protection. |
| Flash mode QIO vs DIO | **Made it worse** -- see section 4. |
| Flash frequency 40MHz vs 80MHz | Fixed SERIAL (see section 5). Did **not** fix NVS. |

Six of those were real bugs worth fixing. **None of them was the bug.**

## 3. The one hard piece of evidence

Dumping the NVS partition after a failed write, at 40MHz:

```
bootdiag present at offset 72
k        present at offset 104
context: ... 6b 00 00 ... 2a ffffffff
```

The key `k` and the value `42` (`0x2a`) were physically in flash. But the 2-bit
entry-state bitmap read back as `0b01` -- an illegal state -- and `0b00`
(erased), instead of `0b10` (written). **NVS wrote the data and mangled the
metadata, so it skipped entries it had just written.**

At 80MHz the behaviour changed: nothing is written at all (`used=0`, zero
entries enumerated). Still unexplained.

Note the asymmetry that made this hard to see: **esptool writes are always
perfect**, because esptool drives the flash through its own stub. The app writes
through the IDF driver configured from the bootloader header. Only the app's
writes misbehave.

## 4. The worst mistake: inventing settings instead of copying a working one

InkWatchy runs on this exact watch. Its `sdkconfig.Watchy_2` contains:

```
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHMODE="dio"
CONFIG_ESPTOOLPY_FLASHFREQ="80m"
```

Those first two lines look contradictory. They are not: on ESP32 the bootloader
**header** is written as DIO and the bootloader upgrades to QIO at runtime after
reading the flash ID. I read `_QIO=y`, wrote `qio` into the header, flashed, and
**the board stopped executing entirely** -- no display, no buttons, no motor.

That is the mistake worth naming. The right move, from the very beginning, was
to **copy the known-good configuration wholesale** -- board, flash mode, flash
frequency, partition table -- from the firmware that demonstrably runs on this
hardware, and only then change one thing at a time. Instead the Watchy
environment was assembled from `esp32dev` defaults and my own assumptions, and
every one of those assumptions had to be discovered the hard way.

Related: I dismissed the flash-frequency difference early with "DIO/40 is more
conservative, therefore safer." That reasoning is wrong. A mismatch is wrong in
*either* direction, and reads working proves nothing about writes.

## 5. What the flash frequency DID fix: serial

Raising the header from 40MHz to 80MHz made **serial output work on the Watchy
for the first time**. Before that, the device emitted nothing -- which caused an
entire earlier investigation (see `../analysis/watchy-port-attempt-log.md`) to
conclude that serial was dead on this hardware and to build motor- and
panel-based telemetry instead.

That conclusion was wrong, and the cost was enormous: hours of debugging a
device through instruments that had to be invented, when the actual fix was one
line of build configuration copied from the reference firmware.

InkWatchy also does the obvious thing we did not: after `Serial.begin()` it can
`delay(7500)` (`WAIT_FOR_MONITOR`) so a monitor can attach before the boot
output is produced, because **opening the port resets the ESP32**. Every capture
we ran had the wrong shape: attach, reset the device, miss the output.

## 6. What should have been done, in order

1. **Copy the working reference configuration** (InkWatchy's board/flash/
   partition settings) before writing a line of firmware.
2. **Get serial working first**, and prove it with a known-good positive
   control, rather than building substitute instruments.
3. **Change one variable at a time**, and verify each before layering the next.
   Six real bugs were fixed during this hunt; because they were fixed in a
   batch with the device in an unknown state, none of them could be attributed.
4. **Prefer the platform's own mechanisms** over hand-rolled ones. A custom BLE
   GATT protocol with JSON, manual chunking, a reassembly buffer and a
   hand-written command queue is a lot of invented surface for "store two
   strings"; ESP-IDF ships provisioning for exactly this, and every one of those
   pieces produced at least one bug of its own.

## 7. Where it stands

- The Watchy runs, renders, and takes button input, on DIO/80MHz.
- Serial works. **Use it.**
- BLE provisioning transfers correctly and reports failure honestly.
- NVS writes still do not persist. The device therefore cannot be provisioned.
- The M5Paper is unaffected and provisions normally.

Next thing to try, in the spirit of section 6: build the Watchy firmware using
InkWatchy's exact `platformio.ini` foundation -- its platform version, framework
packages, and partition table -- rather than `esp32dev` defaults, and see
whether NVS behaves. That is copying what works instead of reasoning about what
should work.


---

# 8. RESOLVED: NVS is broken on Arduino 2.0.4 / IDF 4.4 for this chip

Found by the bisect that section 6 said to do, on a firmware containing nothing
but NVS calls -- no display, no BLE, none of this project's code:

| platform | result |
|---|---|
| `espressif32@5.1.0` (Arduino 2.0.4 / IDF 4.4) | `set` OK, `commit` OK, **`get` -> `ESP_ERR_NVS_NOT_FOUND`**, 0 entries |
| pioarduino (Arduino 3.1 / IDF 5.3) | `set`/`commit`/`get` all OK, value read back, survives reopen |

Same chip, same board, same partition table, same flash settings. **Nothing in
this project was ever going to fix it.**

Verified on the device end to end afterwards:

```
provision -> 2 network(s) stored, none left blank
status    -> 2 network(s) stored: OpenWrt2.4 > TP-Link_5GHz_5C53F3
diag      -> rawGet: ESP_OK  rawValue: 42  entries used: 93
```

## What the migration then exposed

Moving platforms was correct but not free. Two regressions appeared immediately,
both now fixed:

1. **Rendering 490ms -> 1197ms.** IDF 5.3 logs an error for every
   `esp_task_wdt_reset()` called from a task that is not subscribed to the
   watchdog; the renderer does that every 8 scanlines, and on the Watchy it runs
   on `loopTask`. Hundreds of serial writes per render. Fixed by checking
   `esp_task_wdt_status()` first (`mp_wdt.h`). Subscribing `loopTask` instead --
   tried first -- boot loops, because parsing never feeds the watchdog.
2. **City 2 rebooted the device.** Arduino 3.1 costs ~37KB of baseline heap
   (241KB -> 204KB free at boot). Parsing the largest script then left a 26KB
   largest block, and the 40,000-byte overdraw map could not be allocated --
   with exceptions disabled, `std::vector::resize()` calls `abort()`. Fixed by
   packing that map to **1 bit per pixel** (5,000 bytes here, 64,800 on the
   M5Paper) plus a pre-allocation size check that degrades instead of crashing.

## The lesson, restated

Section 6 said: copy the working reference configuration first, and bisect
before theorising. Both of those, done properly, found the cause in roughly
twenty minutes after a day of hypotheses. Ten wrong theories preceded them, six
of which were real bugs -- but the real bug was in a dependency, and no amount
of reasoning about our own code could have reached it.

**The single most useful action of the whole investigation was flashing a
firmware that did nothing but the one thing under test.**

# Post-mortem: the Watchy NVS hunt, and how it went wrong

**Status: UNRESOLVED.** NVS writes still do not persist on the Watchy. This
document exists because the *process* failed badly, and the failure is more
useful to record than the (still missing) answer.

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

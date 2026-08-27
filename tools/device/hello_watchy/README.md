# hello_watchy — minimal bisect firmware

The smallest possible ESP32 firmware: `Serial.println()` in `loop()`, nothing
else. No display, no libraries, no globals with constructors.

## Why this exists

When a board flashes successfully (hashes verified) but produces no serial
output, the first question is always *"is it my code, or is it the boot/flash
path?"* — and that question is expensive to answer by staring at application
code. Flashing this instead answers it in one step:

- **This prints** -> the boot path is fine, the fault is in your firmware.
- **This is silent** -> the fault is the boot/reset/flash path, and reading more
  of your own application code is wasted effort.

Used exactly this way on 2026-08-27 during the Watchy port. It was silent, which
moved the investigation out of the Micropatterns port entirely. See
`docs/analysis/watchy-port-attempt-log.md` §4.1.

## Use

```bash
cd tools/device/hello_watchy
pio run
# flash with the same offsets/bootloader your real firmware uses, e.g.:
esptool.py --port /dev/cu.usbserial-XXX --baud 115200 \
  --before default_reset --after hard_reset write_flash -z \
  0x1000  ~/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32/bin/bootloader_dio_40m.bin \
  0x8000  .pio/build/hello/partitions.bin \
  0xe000  ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/hello/firmware.bin
```

Flash the real firmware back afterwards — this leaves a test blob on the device.

Note the bootloader path contains `sdk/esp32/`, not `sdk/esp32c3/`. Using the
wrong one fails with `Unexpected chip id in image`.

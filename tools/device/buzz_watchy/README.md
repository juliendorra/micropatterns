# buzz_watchy — positive control for the Watchy

Twenty lines. `pinMode(13, OUTPUT)` then toggle it in `loop()`. No display, no
libraries, no globals with constructors, nothing that can block.

## Why this exists

On 2026-08-27 a Watchy port was debugged for hours on the assumption that the
application was running and only the display was failing. **That assumption was
never tested.** Serial output is useless on this device — the official InkWatchy
image runs visibly while emitting zero bytes over 60 seconds — so silence proved
nothing, and the panel cannot report progress that happens before the panel
works.

This firmware buzzed on the first try. That single fact proved the toolchain,
bootloader, flash mode, partition layout and boot path were all fine, and moved
the entire investigation inside the application. It should have been the FIRST
thing flashed, not the tenth.

**Use it as a positive control:** flash it before concluding anything from a
silent device.

- **It buzzes** → the device runs code you compiled; the fault is your firmware.
- **It is silent** → the fault is the build/flash/boot path; debugging your
  application code is wasted effort.

## Use

```bash
cd tools/device/buzz_watchy && pio run
# flash with the same offsets your real firmware uses:
esptool.py --chip esp32 --port /dev/cu.usbserial-XXX --baud 115200 \
  --before default_reset --after hard_reset write_flash -z \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000  ~/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32/bin/bootloader_dio_40m.bin \
  0x8000  .pio/build/buzz/partitions.bin \
  0xe000  ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
  0x10000 .pio/build/buzz/firmware.bin
```

Note `sdk/esp32/`, not `sdk/esp32c3/` — the wrong one fails with
`Unexpected chip id in image`.

Flash your real firmware back afterwards.

## Related

The Watchy firmware keeps a richer version of this idea behind
`-DMP_STAGE_BUZZ=1` (default off): a FreeRTOS task repeats the furthest boot
stage reached, forever, so it keeps reporting even while `setup()` is blocked.
See `Watchy_MicroPatterns/src/main.cpp` and
`docs/analysis/watchy-port-attempt-log.md` §5.

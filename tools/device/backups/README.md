# Device backups

Output of `tools/device/dump_scripts.sh`. Each directory is one dump:

- `spiffs.bin` — the raw SPIFFS partition image, exactly as read from the device.
- `files/` — the same image unpacked by `mkspiffs`.

## Why these are committed

Micropatterns scripts live **only on the device**. There is no server copy: the
API endpoint 404s because Deno Deploy Classic was sunset on 2026-07-20.

On 2026-08-27 the device deleted every script it had, and
`2026-08-27-143524/` was for a period **the only existing copy** of six of the
user's scripts. It is committed as data, not as a build artifact. See
`docs/incidents/2026-08-27-m5paper-script-loss.md`.

## Restoring

```bash
mkspiffs_espressif32_arduino -c <stamp>/files -b 4096 -p 256 -s 3604480 restore.bin
esptool.py --port <port> --baud 115200 write_flash 0xc90000 restore.bin
```

115200 is not a typo — these USB-UART bridges corrupt transfers at higher rates.

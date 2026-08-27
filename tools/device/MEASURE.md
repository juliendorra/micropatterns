# measure_render.py — on-device A/B render timing

Drives the M5Paper serial console, runs every script N times, and records
`(script, display-list size, generation ms, rasterize ms)` to JSON.

## Why this exists

A claimed "9.5x faster" turned out to be two unrelated samples: a 29-item render
compared against a 351-item one, described as like-for-like. `circuits` is
procedural — its display list changes run to run — so single samples prove
nothing. This tool exists so device claims come from repeated, matched runs.

The honest figure it produced was **~7x** (5.4x–10x by script), not 9.5x.

## Method for a real A/B

```bash
git worktree add /tmp/pre-opt <before-commit>          # build the BEFORE firmware
cd /tmp/pre-opt/M5Paper_MicroPatterns && pio run -e m5stack-fire -t upload --upload-port <port>
python3 tools/device/measure_render.py BEFORE 2

cd <repo>/M5Paper_MicroPatterns && pio run -e m5stack-fire -t upload --upload-port <port>
python3 tools/device/measure_render.py AFTER 2

git worktree remove /tmp/pre-opt --force
```

Compare medians per script. Results land in
`<scratch>/measure_<label>.json`.

## Gotchas that will bite you

- **Opening the port resets the ESP32** (pyserial toggles DTR). A command sent
  immediately after opening hits a booting device. The script syncs on the
  `serial console ready` banner instead of guessing a settle time.
- Every port open also increments the firmware's fresh-start counter. Before a
  2026-08-27 fix, enough of those triggered a full refresh that **wiped all
  scripts on the device**. Fixed, but back up first anyway:
  `tools/device/dump_scripts.sh`.
- **Device numbers are noisy**: `circuits` measured 835 ms and 1638 ms on
  consecutive runs of the same firmware. Take medians, and prefer the host
  harness (`tools/host_harness`) when you need a deterministic figure.
- The rasterize/generation split it reports does NOT include the panel push.
  See `docs/analysis/m5paper-panel-refresh.md`.

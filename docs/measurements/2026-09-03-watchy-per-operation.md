# Watchy per-operation benchmark, before/after — 2026-09-03

First on-device measurement of the Watchy, and the first before/after this
project has taken on real hardware rather than on the host harness.

Measures the sine table + angle accumulator + the two bounds bug fixes, all from
2026-09-03. **Headline: rasterisation is unchanged, display-list generation is
6–24% faster, and two shapes now render that previously did not.**

## First, a correction to the record

`docs/analysis/watchy-port-attempt-log.md` concludes that serial output is
unusable on this device ("serial is worthless here", §5.1), and the vibration
motor was built into a telemetry channel because of it.

**Serial works.** 115200, normal DTR/RTS polarity, and the firmware is chatty:
boot banner, heap logs, RTC, script list, per-render timings. What made it look
dead is that the Watchy **deep-sleeps between renders** — a sleeping ESP32
transmits nothing, so a passive listener attached at the wrong moment sees
silence. Reset the board and it talks immediately.

That is the third instrument artifact in this device's history, after
`--after no_reset` being read as device state and macOS lacking `timeout`. §5.4
of the attempt log states the lesson exactly; this is one more instance of it.

Consequence: `-DMP_STAGE_BUZZ` is no longer the only way to get telemetry off
this watch, and the per-operation benchmark below is possible at all.

## Method

- Device: Watchy 2.0, ESP32-PICO-D4 @ 240 MHz, no PSRAM, 200x200 SSD1681.
  `/dev/cu.usbserial-110` (CP2102N).
- Firmware: `env:watchy2-bench` (`Watchy_MicroPatterns/src/bench/`). Its own
  `main()`, and `build_src_filter` drops `src/*` — no SPIFFS, ScriptManager,
  WiFi, BLE or RTC is linked, so nothing competes with the renderer for the
  PICO-D4's ~180 KB heap.
- "Before" is `env:watchy2-bench-before`, the same firmware over the renderer
  sources extracted from git HEAD. Only the seven shared `.cpp` files and their
  headers differ between the two binaries.
- 7 reps per script, `esp_timer` microseconds, **minimum** reported (median
  tracks it within 2%). Seed `counter=0 12:34:56`, matching the host harness's
  `c0_123456` golden.
- **The panel is never driven.** `render()` draws into the GxEPD2 framebuffer
  and `nextPage()` is never called, so the ~600–2000 ms e-paper waveform is
  excluded. These are compute numbers.
- Corpus: 10 single-operation probes (`tools/device_bench/ops/`) plus the 7
  host-harness corpus scripts, all compiled in.

Collect with:

```
pio run -e watchy2-bench -t upload --upload-port /dev/cu.usbserial-110
python3 tools/device_bench/collect_watchy.py --port /dev/cu.usbserial-110 --out after.json
python3 tools/device_bench/collect_watchy.py --compare before.json after.json
```

## The noise floor, established by a control

`parse` contains no trigonometry, no transform and no rasterisation. Nothing in
this change could affect it. Across 17 scripts it moved by −3.9% to +4.5%.

**So ±5% is noise here.** Any smaller delta below is not a result, and the
rasterisation numbers have to be read against that bar.

## Result — per operation

Rasterisation, min of 7, milliseconds:

| operation | before | after | delta | |
|---|---|---|---|---|
| `op_fill_rect_pattern_rot` | 59.74 | 59.77 | +0.0% | |
| `op_fill_rect_pattern` | 42.95 | 42.95 | +0.0% | |
| `op_fill_rect_solid` | 32.72 | 32.74 | +0.1% | |
| `op_draw_asset` | 36.06 | 36.08 | +0.1% | |
| `op_fill_circle` | 35.56 | 35.54 | −0.1% | |
| `op_line` | 5.46 | 5.46 | +0.0% | control |
| `op_rect_outline` | 2.26 | 2.30 | +1.8% | control |
| `op_fill_pixel` | 1.97 | 2.00 | +1.3% | |
| `op_circle_outline` | 4.22 | 4.06 | **−3.8%** | 2 `sqrtf` removed |
| `op_fill_circle_pattern_rot` | 47.00 | 49.79 | **+5.9%** | **more work, see below** |

Eight of ten are inside ±2%, well under the ±5% noise floor. That is the
predicted result stated as a measurement: `matrix_make_rotation` runs once per
`ROTATE`, never per pixel, so replacing it cannot move a per-pixel number.

The two that did move are both explicable and neither is a regression:

- **`op_circle_outline` −3.8%.** Its two `sqrtf` calls were computing the norm
  of a matrix column that is a unit vector. Removing them is the only change to
  this path, and 3.8% of 4.22 ms across 40 circles is ~0.15 ms — the right order
  for 80 `sqrtf` on an FPU with no hardware square root.
- **`op_fill_circle_pattern_rot` +5.9%.** This one is *slower because it is now
  correct*: the octagon-AABB bug was clipping 7.6% of a rotated disk's radius,
  so the "before" number is the cost of drawing an incomplete shape. Paying 5.9%
  more time to stop dropping 4.4% of the disk's area is the trade, and the
  comparison is between different workloads, not different speeds.

## Result — per phase, whole scripts

Display-list generation, min of 7, milliseconds:

| script | before | after | delta |
|---|---|---|---|
| `bounds` | 0.33 | 0.25 | −24.4% |
| `artdeco_default` | 0.49 | 0.40 | −18.1% |
| `op_circle_outline` | 0.82 | 0.71 | −13.5% |
| `op_fill_circle_pattern_rot` | 0.28 | 0.25 | −12.5% |
| `op_rect_outline` | 0.80 | 0.70 | −11.7% |
| `emulator_welcome` | 0.88 | 0.79 | −9.7% |
| `prims` | 0.76 | 0.69 | −8.8% |
| `op_fill_rect_pattern_rot` | 0.27 | 0.25 | −6.3% |
| `city` | 103.61 | 103.60 | −0.0% |
| `nest` | 1.27 | 1.27 | −0.2% |

Every script that issues `ROTATE` got faster, by 6–24%, in one direction, with
no counter-example. `city` and `nest` — which never rotate — did not move at
all, which is the control that makes the rest of the column mean something.

This is where the win actually is, and it is bigger than expected. Not just the
table replacing `sinf`: `TRANSLATE` and `ROTATE` no longer call
`matrix_multiply` **or** `matrix_invert`, so each transform command lost a 2x3
matrix product, a general 2x2 inversion, a determinant and a division.

In absolute terms it is small — 0.09 ms on `artdeco_default` — because
display-list generation is a fraction of a millisecond on scripts that are not
loop-heavy. `city` shows where the real interpreter cost lives: 1,601 items and
**103.6 ms**, untouched by any of this.

Whole-script rasterisation moved as the per-operation numbers predict:
`artdeco_default` −7.0% (it draws circles), `nest` −5.9%, `prims` −0.6%,
`city` +0.2%, `emulator_welcome` +0.0%.

## The bug fixes, visible on hardware

`bounds` — the regression script added for the two bounds bugs — went from
**0 rendered items to 2** on the device. Its rasterisation time went from 0.24 ms
to 0.61 ms, which is the cost of drawing shapes that the old renderer discarded.
On a 200x200 canvas most of that script falls off-screen (its coordinates are
sized for the M5Paper's 960x540), so 2 is the correct count here; the full gate
is the host golden at 960x540.

## Binary size

| | before | after |
|---|---|---|
| `watchy2-bench` firmware | 393,084 B | 389,756 B |

**−3,328 bytes** of flash on the real target: libm's `sinf`/`cosf` and their
argument-reduction tables, plus `matrix_multiply` / `matrix_invert`, minus the
1,440-byte `int32_t` sine table.

## What this does not answer

The per-pixel transform is still `float`, and nothing here measures a
fixed-point alternative. What it does establish is the instrument: any renderer
change can now be priced on real ESP32 hardware in about two minutes, per
operation, with a ±5% noise floor and a control group that proves the noise
floor. That is what the Q16.16 question needs, and it did not exist before today.

# M5Paper render baseline — 2026-08-27

First real phase-split measurement of the M5Paper client. Captured from Julien's device over USB
serial while it ran his current firmware build. Raw log: `m5paper-baseline-serial.log`.

**Why this file exists.** The previous benchmark record was `runtime optimizations.txt` at the repo
root — a hand-pasted serial log. The log line it quotes (`main.cpp:527 ... execution and rendering
took 48072 ms`) **no longer exists in the firmware**; the end-to-end total was dropped during the RTOS
rearchitecture (`6c1b5cd`). So the measurement justifying the last optimization became
irreproducible. Measurements now get committed.

## Method

- Device: M5Paper, `/dev/cu.usbserial-537A0106041` (CH9102, VID 0x1a86 / PID 0x55d4).
- Firmware: Julien's existing build, already flashed. **Nothing was flashed or modified.**
- Capture: passive read at 115200 with **DTR and RTS held low** so the device is not reset
  (`scratchpad/cap.py`, pyserial 3.5 from the PlatformIO venv).
- Source of timings: the two existing `millis()` brackets at
  `M5Paper_MicroPatterns/src/render_controller.cpp:60-93`. Parse and push times are *derived* from
  log timestamp deltas, not directly instrumented — see Caveats.

## Result — script `circuits`, display list of 25 items

| Phase | Run 1 | Run 2 | Share of compute | Source |
|---|---|---|---|---|
| Parse | 67 ms | 75 ms | 2.6 % | derived (log ts delta) |
| Display-list generation | 166 ms | 161 ms | 6.4 % | **instrumented** |
| **Rasterization** | **2350 ms** | **2315 ms** | **91 %** | **instrumented** |
| E-paper push | ~666 ms | — | (fixed floor) | derived (log ts delta) |
| Compute subtotal | ~2583 ms | ~2551 ms | | |

Run 1 = initial render at counter 335. Run 2 = re-render, same counter, `useAsIsState: true`.

**Rasterization variance across runs: ~1.5 %.** Low enough that this is a usable benchmark signal.

## What this settles

1. **Rasterization is ~91 % of compute.** The optimization priority question is answered: the
   rasterizer track wins, the interpreter data-structure track is real but low-leverage. The
   rasterizer analysis carried the caveat "if rasterization is under ~50 %, priority should flip" —
   it is not, so its P1-P5 ranking stands.
   See `../analysis/m5paper-rasterizer-perf.md` and `../analysis/m5paper-interpreter-perf.md`.

2. **The ~650 ms e-paper floor model is validated.** `m5paper-platform-perf.md` predicted ~450 ms
   GC16 waveform + ~200 ms SPI transfer at the M5EPD library's hardcoded 10 MHz. Observed ~666 ms.
   Close enough to trust the model — and it means display hardware is *not* the problem, compute is.

3. **Generation cost is ~6.6 ms per display-list item** (166 ms / 25). Slow per item, but irrelevant
   at this list size. This is the number that would change on a loop-heavy script.

## Caveats — do not over-read this

- **`circuits` is not `art-deco-3`.** The 10-19 s figure in `runtime optimizations.txt` was
  `art-deco-3`; `circuits` totals ~3.2 s. **This baseline covers a small display list (25 items)
  only.** A script with deep `REPEAT` nesting could shift the balance toward generation — the
  interpreter analysis estimates `city.json` at ~20k loop iterations. **`art-deco-3` and a
  loop-heavy script still need measuring before concluding one optimization story covers everything.**
- Parse and push times are derived from log timestamp deltas between adjacent `log_i` calls, so they
  include logging overhead and any scheduler latency in between. Treat as approximate. The device
  benchmark harness (`tools/device_bench/`) will instrument these directly with
  `esp_timer_get_time()`.
- Only two runs. Enough to show low variance, not enough for a distribution.
- Timings come from a `CORE_DEBUG_LEVEL=5` build, which is what Julien runs. Serial logging is
  blocking; a quieter build would likely be somewhat faster overall.

## Incidental finding — GPIO 39 interrupt noise

Throughout the capture, roughly every 300 ms:

```
[D][input_manager.cpp:101] InputTask: Received raw event from ISR for GPIO 39. ISR is now disabled for this pin.
[D][input_manager.cpp:160] InputTask: GPIO 39 was noise (HIGH after 50ms debounce). Re-enabling ISR.
```

Each occurrence costs an ISR, a task wakeup, a 50 ms debounce wait, and two blocking serial writes.
During a 2.35 s rasterization that is roughly 8 interruptions of the render task. Not the bottleneck,
but it is real overhead, real battery drain, and a persistent noise source in every measurement taken
on this device. Worth fixing independently of the performance work.

## Operational note for future captures

The M5Paper enters light sleep after an idle timeout (`main.cpp:577`, ~7 s here), and **its USB-serial
bridge powers down with it**, which drops `/dev/cu.usbserial-*` mid-capture. That is what ended this
capture — not a tool failure. Either capture immediately after triggering a render, or give the
benchmark build a mode that inhibits sleep for the duration of a run.

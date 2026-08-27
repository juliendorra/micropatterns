# M5Paper serial command console

A programmatic channel for driving the device over USB: select and run a script
without touching the buttons. Added 2026-08-27 at the project owner's request,
for debugging.

## Not CDC

The M5Paper is a classic **ESP32-D0WDQ6-V3** — no native USB. This is plain
UART0 through the CH9102 bridge at `monitor_speed` (115200). The request was
phrased as "a CDC command"; the ergonomics are the same, the mechanism is not.

## Commands

One per line, `\n` or `\r\n` terminated. Every reply is prefixed `MPCON|` so it
can be grepped out of the firmware's own log chatter (`CORE_DEBUG_LEVEL=5` is
extremely noisy).

| Command | Effect |
|---|---|
| `list` | script list as `<index>\t<humanId>\t<name>` |
| `run <id\|index>` | run a script by human id (`eyes`) or list index (`3`) |
| `next` / `prev` | same as the UP/DOWN buttons |
| `current` | currently loaded script id |
| `help` | command list |

## Design

Commands are injected into `g_inputEventQueue` — **the same queue the buttons
feed** — via a new `InputEventType::RUN_SCRIPT_BY_ID` carrying a `script_id`
payload. This is the whole trick: the console inherits correct interaction with
sleep, render-interrupt and app state for free, rather than reimplementing it.
`InputEvent` gained a default-initialised `char script_id[]`, so existing
producers (`InputManager`) needed no change.

`list` and `current` are answered directly on the console task rather than round
-tripping through the queue; `ScriptManager` guards SPIFFS with its own mutex,
so that is safe and gives immediate output.

## UART wakeup

`SLEEP_IDLE_THRESHOLD_MS` is **3 seconds**, and light sleep was armed for timer
and GPIO wakeup only. Without UART wakeup the device is asleep almost all the
time and typed commands are simply never read, which makes the console close to
useless for its stated purpose. `configureWakeupSources()` now also calls:

```cpp
uart_set_wakeup_threshold(UART_NUM_0, 3);
esp_sleep_enable_uart_wakeup(UART_NUM_0);
```

**Verified on hardware:** the device wakes with `Cause: 8` (UART) and acts on
the command.

The threshold is in RX edges, so the wakeup fires partway through the first
character and that character is corrupted and dropped. Commands are newline
terminated, so **send a bare newline first to wake it, then the command.** The
tooling does this.

## Gotcha for anyone testing this

Opening the port with `pyserial` toggles DTR and **resets the ESP32**. A query
issued immediately after opening therefore hits a booting device and gets
`error: could not load script list`, which looks exactly like data loss and is
not. Wait ~22 s after opening, or read passively without asserting the reset
lines. This cost real time on 2026-08-27; it also silently incremented the
fresh-start counter that triggered the script wipe (see the incident doc).

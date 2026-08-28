# Device tools

Everything here talks to real hardware. Nothing is needed to build the
firmwares; all of it exists because something went wrong once and guessing was
more expensive than measuring.

| | |
|---|---|
| `mpcon.py` | Drive and capture either device over its serial console. Start here. |
| `probes/` | Minimal single-purpose firmwares, each recording what it proved. |
| `measure_render.py` | A/B render timings between two builds. See `MEASURE.md`. |
| `dump_scripts.sh` | Pull a device's scripts off SPIFFS before flashing it. |
| `backups/` | What `dump_scripts.sh` produced. Scripts live only on the device. |
| `buzz_watchy/`, `hello_watchy/` | Early Watchy bring-up sketches. |
| `SERIAL-TROUBLESHOOTING.md` | When a board appears dead. Read before theorising. |

## `mpcon.py`

    mpcon.py --list-ports
    mpcon.py --port /dev/cu.usbserial-110 capture --seconds 60
    mpcon.py --port /dev/cu.usbserial-110 send next --repeat 5 --gap 0.15
    mpcon.py --port /dev/cu.usbserial-110 sync
    mpcon.py --port /dev/cu.usbserial-110 browse-timing --steps 5

Run it with `~/.platformio/penv/bin/python`, which already has pyserial.

`browse-timing` is the regression check for title browsing: it steps through
scripts and reports the interval between titles and how many renders started.
**Exactly one render is expected** -- only the title you stop on should render.
Current M5Paper reading is ~265ms per title with 1 render; before that work it
was ~1080ms with a render per step.

Two traps this tool exists to stop repeating:

* Opening the port asserts DTR/RTS and **resets the board**, so every run starts
  from a fresh boot. `--settle` waits for it; `--no-reset` avoids it.
* The Watchy cannot be woken by serial at all -- bytes sent to a sleeping
  device are dropped, not delayed. Its firmware stays awake 60s after boot and
  re-arms on every byte received. See `docs/JOURNAL.md`.

Counting log lines is fiddlier than it looks: the M5Paper emits both a console
echo and a firmware log per step, and "Triggering render" also matches the 77s
timer wake. Both produced false regression reports before the patterns were
tightened. The comments in `cmd_browse_timing` say which marker to trust.

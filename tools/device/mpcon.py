#!/usr/bin/env python3
"""Drive and capture a MicroPatterns device over its serial console.

Both firmwares expose the same line console (M5Paper: serial_console.cpp,
Watchy: pollSerial() in main.cpp), which is the only way to exercise script
switching, syncing and rendering without standing at the device pressing
buttons. This wraps it so a regression check is one command rather than a
throwaway script -- during the sync work the same twenty lines were retyped a
dozen times, and small differences between them cost real debugging time.

Usage
-----
    mpcon.py --port /dev/cu.usbserial-110 capture --seconds 60
    mpcon.py --port /dev/cu.usbserial-110 send list
    mpcon.py --port /dev/cu.usbserial-110 send next --repeat 5 --gap 0.15
    mpcon.py --port /dev/cu.usbserial-110 sync
    mpcon.py --port /dev/cu.usbserial-110 browse-timing --steps 6
    mpcon.py --list-ports

Commands understood by the devices: list | run <index|id> | next | prev |
sync | current | help.  (`prev` and `current` are not on every build.)

Notes that cost time to learn
-----------------------------
* Opening the port asserts DTR/RTS and RESETS the board. Every invocation
  therefore starts from a fresh boot; `--settle` waits for it. Pass
  `--no-reset` to observe a device without disturbing it (this cannot recover
  a session already in progress, it only avoids starting a new one).
* The Watchy light-sleeps and CANNOT be woken by serial: bytes sent to a
  sleeping device are dropped, not delayed, because the RX FIFO is unclocked.
  Its firmware stays awake for 60s after boot and re-arms that window on every
  byte received -- so send commands promptly, and keep sending to stay awake.
  See docs/JOURNAL.md, "Watchy sleep, and two more serial dead ends".
* The M5Paper light-sleeps too but DOES wake on UART. The wakeup fires partway
  through the first character, which is corrupted and dropped -- so a bare
  newline is sent before each command to absorb it.
"""

import argparse
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not installed. Try:\n"
             "  ~/.platformio/penv/bin/python -m pip install pyserial\n"
             "or run this with ~/.platformio/penv/bin/python, which already has it.")


def open_port(args):
    s = serial.Serial()
    s.port = args.port
    s.baudrate = args.baud
    s.timeout = 1
    if args.no_reset:
        # Must be set before open() or pyserial pulses them on the way in.
        s.dtr = False
        s.rts = False
    s.open()
    if not args.no_reset and args.settle:
        time.sleep(args.settle)
    s.reset_input_buffer()
    return s


def pump(s, seconds, out=sys.stdout, keep=None):
    """Read lines for `seconds`, printing them; return the ones matching `keep`."""
    t0 = time.time()
    collected = []
    while time.time() - t0 < seconds:
        raw = s.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").rstrip()
        print(f"[{time.time() - t0:6.2f}s] {line}", file=out, flush=True)
        if keep is None or keep.search(line):
            collected.append((time.time() - t0, line))
    return collected


def send(s, command):
    # Bare newline first: on a UART-woken device the first character is eaten.
    s.write(b"\n")
    s.flush()
    time.sleep(0.25)
    s.write(command.encode() + b"\n")
    s.flush()


def cmd_capture(args, s):
    pump(s, args.seconds)


def cmd_send(args, s):
    for i in range(args.repeat):
        send(s, args.command)
        if i + 1 < args.repeat:
            time.sleep(args.gap)
    pump(s, args.seconds)


def cmd_sync(args, s):
    send(s, "sync")
    lines = pump(s, args.seconds, keep=re.compile(r"content done|Sync finished|Fetch"))
    print("\n--- sync result ---")
    for _, line in lines:
        print(line)
    if not lines:
        print("(nothing matched -- device may have been asleep, or the sync is slower "
              "than --seconds)")


def cmd_browse_timing(args, s):
    """Step through scripts and report the interval between titles.

    This is the regression check for title browsing: a press should show the
    next name immediately and only the one you stop on should render. Before
    that work the M5Paper took ~1080ms per title; it is ~230ms now.
    """
    for i in range(args.steps):
        send(s, "next")
        time.sleep(args.gap)

    # Counting these is fiddlier than it looks, and getting it wrong reports a
    # regression that is not there.
    #
    # Titles: the M5Paper emits BOTH a console echo ("MPCON|ok next") and a
    # firmware log ("MainCtrl: Selected '<id>'") per step, so matching both
    # doubles the count. Prefer the firmware log where it exists and fall back
    # to the echo, which is all the Watchy gives -- showScriptName() logs
    # nothing.
    #
    # Renders: match the settle specifically. "Triggering render" alone also
    # catches "MainCtrl: Woke up. Triggering render for script '<id>'", the 77s
    # timer re-render, which has nothing to do with the burst.
    fw_title_re   = re.compile(r"MainCtrl: Selected '")
    echo_title_re = re.compile(r"MPCON\|ok (next|prev)")
    render_re     = re.compile(r"Title settled on .*Triggering render|=== Rendering")

    keep = re.compile("|".join([fw_title_re.pattern, echo_title_re.pattern,
                                render_re.pattern]))
    lines = pump(s, args.seconds, keep=keep)

    fw     = [t for t, line in lines if fw_title_re.search(line)]
    echoes = [t for t, line in lines if echo_title_re.search(line)]
    titles = fw if fw else echoes
    renders = [line for _, line in lines if render_re.search(line)]

    print("\n--- browse timing ---")
    print(f"steps requested:   {args.steps}")
    print(f"title frames seen: {len(titles)}"
          f"{' (from firmware log)' if fw else ' (from console echo)'}")
    if len(titles) > 1:
        gaps = [b - a for a, b in zip(titles, titles[1:])]
        print("title-to-title:    " + ", ".join(f"{g * 1000:.0f}ms" for g in gaps))
        print(f"mean:              {sum(gaps) / len(gaps) * 1000:.0f}ms")
    print(f"renders started:   {len(renders)} "
          f"(expect 1 -- only the title you stop on should render)")
    if len(renders) > 1:
        print("REGRESSION: more than one render for one browse burst. The settle "
              "timer is not holding the render off.")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--settle", type=float, default=14.0,
                   help="seconds to wait after the open-induced reset (default 14, "
                        "which covers a Watchy boot; use ~8 for the M5Paper)")
    p.add_argument("--no-reset", action="store_true",
                   help="do not assert DTR/RTS on open, so the device keeps running")
    p.add_argument("--list-ports", action="store_true")
    sub = p.add_subparsers(dest="cmd")

    c = sub.add_parser("capture"); c.add_argument("--seconds", type=float, default=60)
    c.set_defaults(func=cmd_capture)

    c = sub.add_parser("send"); c.add_argument("command")
    c.add_argument("--repeat", type=int, default=1)
    c.add_argument("--gap", type=float, default=0.15)
    c.add_argument("--seconds", type=float, default=15)
    c.set_defaults(func=cmd_send)

    c = sub.add_parser("sync"); c.add_argument("--seconds", type=float, default=120)
    c.set_defaults(func=cmd_sync)

    c = sub.add_parser("browse-timing")
    c.add_argument("--steps", type=int, default=5)
    c.add_argument("--gap", type=float, default=0.15)
    c.add_argument("--seconds", type=float, default=25)
    c.set_defaults(func=cmd_browse_timing)

    args = p.parse_args()

    if args.list_ports:
        for pi in list_ports.comports():
            print(f"{pi.device}\t{pi.description}")
        return

    if not args.port or not args.cmd:
        p.print_help()
        sys.exit(1)

    s = open_port(args)
    try:
        args.func(args, s)
    finally:
        s.close()


if __name__ == "__main__":
    main()

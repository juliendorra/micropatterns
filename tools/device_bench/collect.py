#!/usr/bin/env python3
"""collect.py -- host-side collector for the M5Paper on-device benchmark.

Tier 2 of the test strategy: the device is the ground truth. This script talks
to the benchmark firmware (env:m5paper-bench), scrapes the `MPBENCH|` lines out
of an otherwise noisy serial stream, and writes a timestamped JSON results file.
It can also diff two results files.

    python3 tools/device_bench/collect.py list
    python3 tools/device_bench/collect.py collect --port /dev/cu.usbserial-XXXX
    python3 tools/device_bench/collect.py parse --input captured.log
    python3 tools/device_bench/collect.py compare before.json after.json

Python 3 stdlib ONLY -- no pip installs. pyserial is used if it happens to be
importable, otherwise a small raw termios reader does the job (see SerialPort
below). `parse --input` needs no serial access at all, so a log captured with
`pio device monitor | tee run.log` always works as a fallback.

SAFETY: this script only ever READS from the port you name. It never writes
anything except the single ASCII 'g' start trigger, and only with --trigger.
Do not point it at a device that is not running the benchmark firmware.
"""

import argparse
import datetime
import glob
import json
import os
import select
import statistics
import sys
import time

MARKER = "MPBENCH|"
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT_DIR = os.path.join(HERE, "results")
PHASES = ["parse", "displaylist", "rasterize", "push", "total"]


# ---------------------------------------------------------------------------
# Serial
# ---------------------------------------------------------------------------
class SerialPort(object):
    """Minimal read-mostly serial port.

    Prefers pyserial when available (it handles odd platforms better). Falls
    back to a raw termios/tty implementation, which is enough here: we need one
    standard baud rate (115200), 8N1, no flow control, and line reads.
    """

    def __init__(self, port, baud=115200):
        self.port = port
        self.baud = baud
        self._ser = None
        self._fd = None
        try:
            import serial  # noqa: F401  (pyserial, optional)
            self._open_pyserial()
        except ImportError:
            self._open_termios()

    def _open_pyserial(self):
        import serial
        self._ser = serial.Serial(self.port, self.baud, timeout=0.2)
        self.backend = "pyserial"

    def _open_termios(self):
        import termios
        import tty
        speed = getattr(termios, "B%d" % self.baud, None)
        if speed is None:
            raise SystemExit("baud %d is not a standard rate this backend supports; "
                             "use --input with a captured log instead" % self.baud)
        fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
        cflag |= termios.CREAD | termios.CLOCAL
        cflag &= ~termios.CRTSCTS if hasattr(termios, "CRTSCTS") else cflag
        cflag &= ~termios.CSTOPB
        cflag &= ~termios.PARENB
        cflag = (cflag & ~termios.CSIZE) | termios.CS8
        iflag &= ~(termios.IXON | termios.IXOFF | termios.IXANY)
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW,
                          [iflag, oflag, cflag, lflag, speed, speed, cc])
        self._fd = fd
        self.backend = "termios"

    def write(self, data):
        if self._ser is not None:
            self._ser.write(data)
        else:
            os.write(self._fd, data)

    def read_some(self, timeout=0.5):
        if self._ser is not None:
            return self._ser.read(4096)
        r, _, _ = select.select([self._fd], [], [], timeout)
        if not r:
            return b""
        try:
            return os.read(self._fd, 4096)
        except OSError:
            return b""

    def close(self):
        if self._ser is not None:
            self._ser.close()
        elif self._fd is not None:
            os.close(self._fd)


def iter_lines(port, overall_timeout, quiet=False):
    """Yield decoded lines from the port until DONE or timeout."""
    buf = b""
    deadline = time.time() + overall_timeout
    while time.time() < deadline:
        chunk = port.read_some(0.5)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip("\r")
            if not quiet:
                sys.stderr.write(text + "\n")
            yield text


# ---------------------------------------------------------------------------
# Parsing MPBENCH| lines
# ---------------------------------------------------------------------------
def parse_marker_line(line):
    """Return (kind, payload_obj_or_str) or None for a non-bench line.

    Robust against a serial stream where an ESP log line got interleaved on the
    same physical line: we search for the marker rather than anchoring at ^.
    """
    idx = line.find(MARKER)
    if idx < 0:
        return None
    rest = line[idx + len(MARKER):]
    if "|" not in rest:
        return None
    kind, payload = rest.split("|", 1)
    payload = payload.strip()
    if payload.startswith("{"):
        try:
            return kind, json.loads(payload)
        except ValueError:
            return kind, {"_unparsed": payload}
    return kind, payload


def build_results(lines, source):
    meta = {}
    runs = []
    stats = []
    errors = []
    started = False
    for line in lines:
        parsed = parse_marker_line(line)
        if not parsed:
            continue
        kind, payload = parsed
        if kind == "META":
            meta = payload
        elif kind == "START":
            started = True
        elif kind == "RUN":
            runs.append(payload)
        elif kind == "STAT":
            stats.append(payload)
        elif kind in ("ERROR", "FATAL", "ERRTEXT"):
            errors.append({"kind": kind, "payload": payload})
        elif kind == "DONE":
            meta["elapsed_ms"] = payload.get("elapsed_ms") if isinstance(payload, dict) else None
            break
    if not started and not runs:
        return None
    return assemble(meta, runs, stats, errors, source)


def _stat_block(values_us):
    """min/median/mean in BOTH us (device native) and ms (host-comparable)."""
    if not values_us:
        return {}
    return {
        "min_us": min(values_us),
        "median_us": int(statistics.median(values_us)),
        "mean_us": round(statistics.mean(values_us), 1),
        "min": round(min(values_us) / 1000.0, 3),
        "median": round(statistics.median(values_us) / 1000.0, 3),
        "mean": round(statistics.mean(values_us) / 1000.0, 3),
        "max_us": max(values_us),
        "spread_pct": round(100.0 * (max(values_us) - min(values_us)) / max(1, min(values_us)), 1),
    }


def assemble(meta, runs, stats, errors, source):
    by_case = {}
    order = []
    for r in runs:
        key = (r.get("script"), r.get("seed"))
        if key not in by_case:
            by_case[key] = []
            order.append(key)
        by_case[key].append(r)

    stat_by_case = {(s.get("script"), s.get("seed")): s for s in stats}

    cases = []
    for key in order:
        reps = by_case[key]
        case = {
            "script": key[0],
            "seed": key[1],
            "reps": len(reps),
            "counters": {},
            "checksums": sorted(set(r.get("canvas_fnv1a") for r in reps)),
        }
        for phase, field in (("parse", "parse_us"), ("displaylist", "displaylist_us"),
                             ("rasterize", "rasterize_us"), ("push", "push_us"),
                             ("total", "total_us")):
            case[phase] = _stat_block([r[field] for r in reps if field in r])
        # push split, device-only detail
        case["push_xfer"] = _stat_block([r["push_xfer_us"] for r in reps if "push_xfer_us" in r])
        case["push_wait"] = _stat_block([r["push_wait_us"] for r in reps if "push_wait_us" in r])
        case["checksum_cost"] = _stat_block([r["checksum_us"] for r in reps if "checksum_us" in r])
        first = reps[0]
        for c in ("display_list_items", "rendered_items", "culled_offscreen",
                  "culled_occlusion", "overdraw_skipped_px", "non_white_px"):
            if c in first:
                case["counters"][c] = first[c]
        case["canvas_fnv1a"] = first.get("canvas_fnv1a")
        case["checksum_stable"] = len(case["checksums"]) == 1
        dev_stat = stat_by_case.get(key)
        if dev_stat:
            case["sha256_16"] = dev_stat.get("sha256_16")
            case["device_reported_stats"] = dev_stat
        case["runs"] = reps
        cases.append(case)

    return {
        "harness": "mp_device_bench",
        "tier": 2,
        "note": ("DEVICE wall-clock on real M5Paper silicon. This is the ground truth. "
                 "Host harness (tools/host_harness) numbers are NOT comparable in absolute "
                 "terms; only canvas_fnv1a and the counters are directly comparable."),
        "collected_utc": datetime.datetime.utcnow().replace(microsecond=0).isoformat() + "Z",
        "source": source,
        "meta": meta,
        "errors": errors,
        "cases": cases,
    }


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def cmd_list(args):
    ports = sorted(glob.glob("/dev/cu.*") + glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        print("no serial ports found")
        return 0
    print("serial ports:")
    for p in ports:
        print("  " + p)
    print("\nPick the M5Paper's port explicitly. Do not guess: other devices may be attached.")
    return 0


def default_out_path():
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    return os.path.join(DEFAULT_OUT_DIR, "device-bench-%s.json" % ts)


def cmd_collect(args):
    port = SerialPort(args.port, args.baud)
    sys.stderr.write("[collect] %s @ %d (backend: %s)\n" % (args.port, args.baud, port.backend))
    if args.trigger:
        # The firmware self-starts after MP_BENCH_AUTOSTART_MS anyway; this just
        # skips the wait. It is the ONLY byte this tool ever writes.
        time.sleep(args.trigger_delay)
        port.write(b"g")
        sys.stderr.write("[collect] sent start trigger 'g'\n")
    try:
        results = build_results(iter_lines(port, args.timeout, args.quiet), args.port)
    finally:
        port.close()
    if results is None:
        sys.stderr.write("[collect] no MPBENCH| output seen in %ds. Is the bench firmware flashed?\n"
                         % args.timeout)
        return 2
    return write_results(results, args.out)


def cmd_parse(args):
    with open(args.input, "r", errors="replace") as f:
        results = build_results((l.rstrip("\n") for l in f), args.input)
    if results is None:
        sys.stderr.write("[parse] no MPBENCH| lines found in %s\n" % args.input)
        return 2
    return write_results(results, args.out)


def write_results(results, out):
    out = out or default_out_path()
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w") as f:
        json.dump(results, f, indent=2)
        f.write("\n")
    print("\nwrote %s" % out)
    print_summary(results)
    return 0


def print_summary(results):
    print("\n%-20s %-12s %9s %9s %9s %9s %10s  %s" %
          ("script", "seed", "parse", "displist", "raster", "push", "total", "fnv1a"))
    for c in results["cases"]:
        def med(p):
            return c.get(p, {}).get("median", 0.0)
        print("%-20s %-12s %8.1fm %8.1fm %8.1fm %8.1fm %9.1fm  %s%s" %
              (c["script"], c["seed"], med("parse"), med("displaylist"),
               med("rasterize"), med("push"), med("total"),
               c.get("canvas_fnv1a"), "" if c.get("checksum_stable") else "  UNSTABLE!"))
    print("(medians, milliseconds; 'm' suffix = ms)")


def cmd_compare(args):
    with open(args.before) as f:
        a = json.load(f)
    with open(args.after) as f:
        b = json.load(f)

    a_cases = {(c["script"], c["seed"]): c for c in a["cases"]}
    b_cases = {(c["script"], c["seed"]): c for c in b["cases"]}
    keys = [k for k in a_cases if k in b_cases]
    only_a = sorted(set(a_cases) - set(b_cases))
    only_b = sorted(set(b_cases) - set(a_cases))

    print("BEFORE: %s (%s)" % (args.before, a.get("collected_utc")))
    print("AFTER : %s (%s)\n" % (args.after, b.get("collected_utc")))

    checksum_fail = []
    unstable = []
    print("%-20s %-12s %-12s %10s %10s %8s" %
          ("script", "seed", "phase", "before ms", "after ms", "delta"))
    for k in sorted(keys):
        ca, cb = a_cases[k], b_cases[k]
        for phase in PHASES:
            va = ca.get(phase, {}).get("median")
            vb = cb.get(phase, {}).get("median")
            if va is None or vb is None:
                continue
            delta = ((vb - va) / va * 100.0) if va else 0.0
            print("%-20s %-12s %-12s %10.2f %10.2f %+7.1f%%" %
                  (k[0], k[1], phase, va, vb, delta))
        if not ca.get("checksum_stable", True) or not cb.get("checksum_stable", True):
            unstable.append(k)
        if ca.get("canvas_fnv1a") != cb.get("canvas_fnv1a"):
            checksum_fail.append((k, ca.get("canvas_fnv1a"), cb.get("canvas_fnv1a")))
        print("")

    if only_a or only_b:
        print("NOTE: cases present in only one file: before-only=%s after-only=%s"
              % (only_a, only_b))

    print("=" * 72)
    if not keys:
        print("FAIL: no cases in common -- nothing was compared.")
        return 1
    if unstable:
        print("WARNING: checksum varied BETWEEN REPETITIONS on the same firmware for: %s"
              % ", ".join("%s/%s" % k for k in unstable))
        print("         The render is not deterministic; fix that before trusting anything.")
    if checksum_fail:
        print("CHECKSUM: FAIL -- the two builds do NOT produce identical canvases.")
        for k, va, vb in checksum_fail:
            print("  %-20s %-12s  %s -> %s" % (k[0], k[1], va, vb))
        print("\nAn optimization that changes pixels is not an optimization; it is a"
              "\nbehaviour change. Either it was intended (re-bake host goldens too) or"
              "\nit is a bug.")
        return 1
    print("CHECKSUM: PASS -- every case produced an identical canvas on both builds.")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd")

    sp = sub.add_parser("list", help="list candidate serial ports")
    sp.set_defaults(func=cmd_list)

    sp = sub.add_parser("collect", help="read a bench run from the device over serial")
    sp.add_argument("--port", required=True, help="e.g. /dev/cu.usbserial-XXXX")
    sp.add_argument("--baud", type=int, default=115200)
    sp.add_argument("--timeout", type=int, default=900, help="seconds to wait for DONE")
    sp.add_argument("--trigger", action="store_true", help="send 'g' to start immediately")
    sp.add_argument("--trigger-delay", type=float, default=2.0,
                    help="seconds to wait after opening the port before sending 'g'")
    sp.add_argument("--out", default=None)
    sp.add_argument("--quiet", action="store_true", help="do not echo the serial stream")
    sp.set_defaults(func=cmd_collect)

    sp = sub.add_parser("parse", help="parse a captured serial log (e.g. from pio device monitor)")
    sp.add_argument("--input", required=True)
    sp.add_argument("--out", default=None)
    sp.set_defaults(func=cmd_parse)

    sp = sub.add_parser("compare", help="diff two results files")
    sp.add_argument("before")
    sp.add_argument("after")
    sp.set_defaults(func=cmd_compare)

    args = p.parse_args()
    if not getattr(args, "func", None):
        p.print_help()
        return 1
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

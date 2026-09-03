#!/usr/bin/env python3
"""Collect MPBENCH| lines from the Watchy bench firmware (env:watchy2-bench).

Resets the board via DTR/RTS -- the bench runs once in setup(), so a reset is
how you ask for a fresh run -- then reads until MPBENCH|end.

Python 3 stdlib only; no pyserial. pyserial's default open asserts DTR and
resets the ESP32 as a side effect, which is fine here (we want a reset) but is
the same behaviour that made an afternoon of serial probing look like device
misbehaviour in the 2026-08-27 session. Doing it explicitly is clearer.

    python3 collect_watchy.py --port /dev/cu.usbserial-110 --out before.json
    python3 collect_watchy.py --compare before.json after.json
"""
import argparse, json, os, re, select, statistics, struct, sys, termios, fcntl, time

TIOCMBIS, TIOCMBIC = 0x8004746c, 0x8004746b
DTR, RTS = 0x002, 0x004
LINE = re.compile(r"MPBENCH\|(.*)")

def open_port(port):
    fd = os.open(port, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[4] = a[5] = termios.B115200
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd

def reset(fd):
    def line(w, on): fcntl.ioctl(fd, TIOCMBIS if on else TIOCMBIC, struct.pack('I', w))
    line(DTR, False)   # GPIO0 high -> boot the application
    time.sleep(0.05)
    line(RTS, True)    # EN low  -> reset
    time.sleep(0.12)
    line(RTS, False)   # EN high -> run
    termios.tcflush(fd, termios.TCIFLUSH)

def capture(port, timeout):
    fd = open_port(port)
    reset(fd)
    end = time.time() + timeout
    buf = ""
    records, meta = [], {}
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.3)
        if not r:
            continue
        try:
            buf += os.read(fd, 8192).decode('utf-8', 'replace')
        except (BlockingIOError, OSError):
            continue
        *lines, buf = buf.split("\n")
        for ln in lines:
            m = LINE.search(ln)
            if not m:
                continue
            body = m.group(1).strip()
            if body == "end":
                os.close(fd)
                return meta, records
            fields = {}
            for tok in body.split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    fields[k] = int(v) if v.lstrip('-').isdigit() else v
            if body.startswith("begin"):
                meta = fields
            elif "raster_us" in fields:
                records.append(fields)
            else:
                print("  note:", body, file=sys.stderr)
    os.close(fd)
    print("TIMEOUT before MPBENCH|end", file=sys.stderr)
    return meta, records

def aggregate(records):
    by = {}
    for r in records:
        by.setdefault((r["kind"], r["name"], r.get("path", "float")), []).append(r)
    out = {}
    for (kind, name, path), rs in by.items():
        e = {"kind": kind, "path": path, "items": rs[0].get("items"), "rendered": rs[0].get("rendered")}
        for phase in ("parse_us", "dl_us", "raster_us"):
            vals = sorted(x[phase] for x in rs)
            e[phase] = {"min": vals[0], "median": statistics.median(vals), "n": len(vals)}
        e["total_us"] = e["parse_us"]["min"] + e["dl_us"]["min"] + e["raster_us"]["min"]
        out[name if path == "float" else name + "@" + path] = e
    return out

def show(meta, agg, title):
    print(f"\n{title}   {meta.get('build','?')}  {meta.get('canvas','?')}  reps={meta.get('reps','?')}")
    print(f"{'script':30s} {'items':>6s} {'rend':>5s} {'parse':>9s} {'dlist':>9s} {'raster':>10s}")
    for kind in ("op", "script", "real"):
        names = [n for n, e in agg.items() if e["kind"] == kind]
        if not names: continue
        print(f"-- {kind} " + "-"*62)
        for n in sorted(names, key=lambda k: -agg[k]["raster_us"]["min"]):
            e = agg[n]
            print(f"{n:30s} {e['items']:>6} {e['rendered']:>5} "
                  f"{e['parse_us']['min']/1000:>8.2f}m {e['dl_us']['min']/1000:>8.2f}m "
                  f"{e['raster_us']['min']/1000:>9.2f}m")

def compare_paths_same_run(path):
    """float vs fixed from ONE capture -- the paths alternated per rep inside a
    single firmware run, so no cross-flash variation can leak into the delta."""
    agg = json.load(open(path))["agg"]
    print(f"{'script':26s} {'float':>9s} {'fixed':>9s} {'vs flt':>8s} {'int':>9s} {'vs fix':>8s}")
    for kind in ("op", "script", "real"):
        rows = [(n, agg[n], agg.get(n + "@fixed"), agg.get(n + "@int")) for n in sorted(agg)
                if "@" not in n and agg[n]["kind"] == kind and agg.get(n + "@fixed")]
        if not rows: continue
        print(f"-- {kind} " + "-"*62)
        for n, f, x, i in rows:
            a = f["raster_us"]["min"]
            b = x["raster_us"]["min"]
            db = (b - a) / a * 100 if a else 0
            if i:
                c = i["raster_us"]["min"]
                dc = (c - b) / b * 100 if b else 0
                print(f"{n:26s} {a/1000:>8.2f}m {b/1000:>8.2f}m {db:>+7.1f}% {c/1000:>8.2f}m {dc:>+7.1f}%")
            else:
                print(f"{n:26s} {a/1000:>8.2f}m {b/1000:>8.2f}m {db:>+7.1f}% {'-':>9s} {'-':>8s}")

def compare(a_path, b_path):
    A, B = json.load(open(a_path)), json.load(open(b_path))
    a, b = A["agg"], B["agg"]
    print(f"{'script':30s} {'phase':>8s} {'before':>10s} {'after':>10s} {'delta':>9s}")
    for kind in ("op", "script", "real"):
        names = [n for n in sorted(set(a) & set(b)) if a[n]["kind"] == kind]
        if not names: continue
        print(f"-- {kind} " + "-"*60)
        for n in names:
            for phase, label in (("raster_us", "raster"), ("dl_us", "dlist"), ("parse_us", "parse")):
                x, y = a[n][phase]["min"], b[n][phase]["min"]
                if x < 200 and y < 200:      # sub-0.2ms: below the noise worth printing
                    continue
                d = (y - x) / x * 100 if x else 0
                print(f"{n:30s} {label:>8s} {x/1000:>9.2f}m {y/1000:>9.2f}m {d:>+8.1f}%")
            ia, ib = a[n].get("rendered"), b[n].get("rendered")
            if ia != ib:
                print(f"{n:30s} {'RENDERED ITEMS':>8s} {ia} -> {ib}   (workload changed, times not comparable)")

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port")
    p.add_argument("--out")
    p.add_argument("--timeout", type=float, default=300)
    p.add_argument("--compare", nargs=2)
    p.add_argument("--paths", help="float vs fixed within one results file")
    args = p.parse_args()
    if args.paths:
        compare_paths_same_run(args.paths); return
    if args.compare:
        compare(*args.compare); return
    meta, records = capture(args.port, args.timeout)
    agg = aggregate(records)
    show(meta, agg, "WATCHY BENCH")
    if args.out:
        json.dump({"meta": meta, "agg": agg, "raw": records}, open(args.out, "w"), indent=1)
        print(f"\n{len(records)} samples -> {args.out}")

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Collect MPPROF| lines from the Watchy profiling bench (env:watchy2-profile,
or env:watchy2-bench built with -DMP_PROFILE_ITEMS=1).

The profiling firmware renders each selected script once on one path and prints,
per script, the total render time and the time spent INSIDE each primitive's
rasteriser (drawPixel, fillRect, ...). Total minus the per-op sum is what the
renderer spends AROUND the rasteriser: bounds, occlusion test, occlusion update,
loop overhead. That split is what showed, on 2026-09-05, that a "2.75x slower
PIXEL" was 2.4x slower in code that had not changed -- i.e. flash-cache
placement, not the change under test (docs/measurements/2026-09-04-integer-dda.md,
step 7).

Which scripts and which path the profiling build runs is chosen in
watchy_bench_main.cpp (the MP_PROFILE_ITEMS branch of setup()); edit that to
point it at the script you are chasing.

    python3 profile_watchy.py --port /dev/cu.usbserial-110 --out prof.txt
    python3 profile_watchy.py --compare before.txt after.txt

Python 3 stdlib only; reuses collect_watchy.py's port handling (explicit DTR/RTS
reset, see the note there).
"""
import argparse, os, re, select, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from collect_watchy import open_port, reset

def capture(port, timeout):
    fd = open_port(port); reset(fd)
    end = time.time() + timeout; buf = ""; lines = []
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.3)
        if not r: continue
        try: buf += os.read(fd, 8192).decode("utf-8", "replace")
        except OSError: continue
        *ls, buf = buf.split("\n")
        for l in ls:
            l = l.strip()
            if l.startswith("MPPROF|") or l.startswith("MPBENCH|"): lines.append(l)
            if l.startswith("MPBENCH|end"):
                os.close(fd); return lines
    print("TIMEOUT before MPBENCH|end", file=sys.stderr); os.close(fd); return lines

def parse(lines):
    """-> {(name, path): {'total': us, 'ops': {op: (count, us)}}}"""
    out = {}
    for l in lines:
        if not l.startswith("MPPROF|"): continue
        f = dict(t.split("=", 1) for t in l[7:].split() if "=" in t)
        k = (f["name"], f["path"]); e = out.setdefault(k, {"total": 0, "ops": {}})
        if "total_us" in f: e["total"] = int(f["total_us"])
        elif "op" in f: e["ops"][f["op"]] = (int(f["count"]), int(f["us"]))
    return out

def show(a, b=None):
    for k in sorted(a):
        ea = a[k]; eb = b.get(k) if b else None
        ops_a = sum(u for _, u in ea["ops"].values()); out_a = ea["total"] - ops_a
        line = f"{k[0]}@{k[1]}: total {ea['total']}us  inside-rasteriser {ops_a}us  around-it {out_a}us"
        if eb:
            ops_b = sum(u for _, u in eb["ops"].values()); out_b = eb["total"] - ops_b
            line += f"   ->  total {eb['total']}us ({(eb['total']-ea['total'])/max(ea['total'],1)*100:+.0f}%)  inside {ops_b}us ({(ops_b-ops_a)/max(ops_a,1)*100:+.0f}%)  around {out_b}us ({(out_b-out_a)/max(out_a,1)*100:+.0f}%)"
        print(line)
        for op, (c, u) in sorted(ea["ops"].items()):
            s = f"    {op:12s} x{c:<5d} {u:8d}us  ({u/max(c,1):.1f}us/item)"
            if eb and op in eb["ops"]:
                cb, ub = eb["ops"][op]; s += f"  ->  {ub:8d}us ({ub/max(cb,1):.1f}us/item, {(ub-u)/max(u,1)*100:+.0f}%)"
            print(s)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port"); ap.add_argument("--out"); ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"))
    a = ap.parse_args()
    if a.compare:
        show(parse(open(a.compare[0]).read().splitlines()), parse(open(a.compare[1]).read().splitlines())); sys.exit(0)
    if not a.port: ap.error("--port or --compare")
    lines = capture(a.port, a.timeout)
    if a.out: open(a.out, "w").write("\n".join(lines) + "\n"); print(len(lines), "lines ->", a.out, file=sys.stderr)
    show(parse(lines))

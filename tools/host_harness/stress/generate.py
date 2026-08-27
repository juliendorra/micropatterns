#!/usr/bin/env python3
"""Deterministic MicroPatterns script generator for the sanitizer soak.

Why this exists
---------------
The golden corpus is expensive to extend: every script needs baked reference
images, and a golden only proves the picture did not change -- it says nothing
about whether the renderer read freed memory or indexed a slot out of range on
the way there. `mpsoak` needs the opposite trade: many structurally varied
scripts, no goldens, run under AddressSanitizer.

So this emits scripts from a seed. The same seed always produces the same
script, which means a soak failure is reproducible from its seed alone --
regenerate that one file and rerun.

What it deliberately covers
---------------------------
- Every drawing primitive, including the four whose display-list parameter slots
  were remapped (LINE/RECT/CIRCLE/PIXEL) and which the golden corpus did not
  exercise at all when that remap landed.
- REPEAT and IF nested up to two deep, with $INDEX read from inside nested
  bodies, because that is what drives the transform-snapshot pool and the
  loop-index path.
- Transform churn -- TRANSLATE / ROTATE / SCALE / RESET_TRANSFORMS interleaved
  with primitives, so consecutive items sometimes share a pooled snapshot and
  sometimes do not.
- Variables declared inside loop bodies and read after, which is what exercises
  the interned-slot table and its memos.

Usage:
    python3 generate.py --count 40 --out-dir generated
    python3 generate.py --seed 17 > one.mp
"""

import argparse
import os
import random
import sys

PRIMITIVES = ["LINE", "RECT", "FILL_RECT", "CIRCLE", "FILL_CIRCLE",
              "PIXEL", "FILL_PIXEL", "DRAW"]
ENV_VARS = ["$WIDTH", "$HEIGHT", "$HOUR", "$MINUTE", "$SECOND", "$COUNTER"]
OPERATORS = ["+", "-", "*", "/", "%"]
COMPARISONS = ["==", "!=", "<", ">", "<=", ">="]


def generate(seed):
    rnd = random.Random(seed)
    lines = []
    declared = []

    def pattern(name, w, h):
        bits = "".join(rnd.choice("01") for _ in range(w * h))
        lines.append('DEFINE PATTERN NAME="%s" WIDTH=%d HEIGHT=%d DATA="%s"' % (name, w, h, bits))

    pattern("pa", 4, 4)
    pattern("pb", 8, 2)

    def atom(in_loop):
        # A single token. Parameter values must be one token -- the parser only
        # accepts multi-token expressions in VAR / LET / IF.
        roll = rnd.random()
        if in_loop and roll < 0.2:
            return "$INDEX"
        if roll < 0.55 or not declared:
            return str(rnd.randint(-40, 220))
        return rnd.choice(declared + ENV_VARS)

    def expression(in_loop):
        if rnd.random() < 0.45:
            return "%s %s %s" % (atom(in_loop), rnd.choice(OPERATORS), atom(in_loop))
        return atom(in_loop)

    def emit(indent, depth, in_loop, count=None):
        for _ in range(count if count is not None else rnd.randint(1, 5)):
            roll = rnd.random()
            if roll < 0.10:
                name = "V%d" % len(declared)
                lines.append("%sVAR $%s = %s" % (indent, name, expression(in_loop)))
                declared.append("$" + name)
            elif roll < 0.16 and declared:
                lines.append("%sLET %s = %s" % (indent, rnd.choice(declared), expression(in_loop)))
            elif roll < 0.22:
                lines.append("%sCOLOR NAME=%s" % (indent, rnd.choice(["BLACK", "WHITE"])))
            elif roll < 0.28:
                lines.append("%sFILL NAME=%s" % (indent, rnd.choice(["SOLID", '"pa"', '"pb"'])))
            elif roll < 0.34:
                lines.append("%sTRANSLATE DX=%s DY=%s" % (indent, atom(in_loop), atom(in_loop)))
            elif roll < 0.39:
                lines.append("%sROTATE DEGREES=%s" % (indent, atom(in_loop)))
            elif roll < 0.44:
                lines.append("%sSCALE FACTOR=%d" % (indent, rnd.randint(1, 8)))
            elif roll < 0.47:
                lines.append("%sRESET_TRANSFORMS" % indent)
            elif roll < 0.60 and depth < 2:
                lines.append("%sREPEAT COUNT=%d" % (indent, rnd.randint(0, 8)))
                emit(indent + " ", depth + 1, True)
                lines.append("%sENDREPEAT" % indent)
            elif roll < 0.68 and depth < 2:
                lines.append("%sIF %s %s %s THEN"
                             % (indent, expression(in_loop), rnd.choice(COMPARISONS), expression(in_loop)))
                emit(indent + " ", depth + 1, in_loop)
                if rnd.random() < 0.5:
                    lines.append("%sELSE" % indent)
                    emit(indent + " ", depth + 1, in_loop)
                lines.append("%sENDIF" % indent)
            else:
                prim = rnd.choice(PRIMITIVES)
                v = lambda: atom(in_loop)
                if prim == "LINE":
                    lines.append("%sLINE X1=%s Y1=%s X2=%s Y2=%s" % (indent, v(), v(), v(), v()))
                elif prim in ("RECT", "FILL_RECT"):
                    lines.append("%s%s X=%s Y=%s WIDTH=%s HEIGHT=%s" % (indent, prim, v(), v(), v(), v()))
                elif prim in ("CIRCLE", "FILL_CIRCLE"):
                    lines.append("%s%s X=%s Y=%s RADIUS=%s" % (indent, prim, v(), v(), v()))
                elif prim in ("PIXEL", "FILL_PIXEL"):
                    lines.append("%s%s X=%s Y=%s" % (indent, prim, v(), v()))
                else:
                    lines.append('%sDRAW NAME=%s X=%s Y=%s'
                                 % (indent, rnd.choice(['"pa"', '"pb"']), v(), v()))

    # A meaty top level: short scripts generate almost no display-list items,
    # and an empty display list exercises nothing.
    emit("", 0, False, count=rnd.randint(8, 16))
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, help="emit one script for this seed on stdout")
    ap.add_argument("--count", type=int, default=40, help="how many scripts to write (default 40)")
    ap.add_argument("--out-dir", default=None, help="directory to write gen_<seed>.mp files into")
    args = ap.parse_args()

    if args.seed is not None and not args.out_dir:
        sys.stdout.write(generate(args.seed))
        return 0

    out_dir = args.out_dir or "generated"
    os.makedirs(out_dir, exist_ok=True)
    for seed in range(1, args.count + 1):
        with open(os.path.join(out_dir, "gen_%03d.mp" % seed), "w") as fh:
            fh.write(generate(seed))
    print("wrote %d scripts to %s" % (args.count, out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())

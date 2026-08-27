#!/usr/bin/env python3
"""Read a unified diff on stdin, emit only hunks matching/not-matching keywords.

Usage:  git diff -- FILE | hunkpick.py [--drop] KEYWORD [KEYWORD...]
Default keeps hunks containing ANY keyword; --drop keeps hunks containing NONE.
Emits a patch suitable for `git apply --cached -`.
"""
import sys

args = sys.argv[1:]
drop = False
if args and args[0] == '--drop':
    drop = True; args = args[1:]
keys = args

text = sys.stdin.read()
lines = text.splitlines(keepends=True)

header, hunks, cur = [], [], None
for ln in lines:
    if ln.startswith('@@'):
        if cur is not None: hunks.append(cur)
        cur = [ln]
    elif cur is None:
        header.append(ln)
    else:
        cur.append(ln)
if cur is not None: hunks.append(cur)

kept = []
for h in hunks:
    body = ''.join(h)
    hit = any(k in body for k in keys)
    if (not hit) if drop else hit:
        kept.append(h)

if not kept:
    sys.stderr.write("hunkpick: no hunks selected\n"); sys.exit(1)
sys.stdout.write(''.join(header))
for h in kept: sys.stdout.write(''.join(h))
sys.stderr.write(f"hunkpick: kept {len(kept)}/{len(hunks)} hunks\n")

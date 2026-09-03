#!/usr/bin/env python3
"""Embed the benchmark corpus into a header for the Watchy bench firmware.

The Watchy has no filesystem access in the bench build on purpose -- SPIFFS,
ScriptManager and the network stack are all excluded so the firmware measures
the renderer and nothing else. Scripts therefore ship as string literals.

Three sets, because a renderer change has to be judged at three scales:

  ops/*.mp                      one primitive each, sized so its inner loop
                                dominates -- the per-OPERATION numbers
  ../host_harness/corpus/*.mp   whole scripts under the golden gate -- the
                                per-PHASE numbers, comparable with
                                docs/measurements/2026-08-27-m5paper-baseline.md
  ../device/backups/<latest>/   THE REAL ART. The scripts actually on the
                                device, pulled from the newest SPIFFS dump.
                                The corpus above is deliberately synthetic and
                                small; these are what the watch spends its
                                battery on, and a change that helps the probes
                                but not these has not helped anything.

Regenerate with:
    python3 tools/device_bench/gen_ops_corpus.py
"""
import os, glob, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
OUT  = os.path.join(ROOT, 'Watchy_MicroPatterns', 'src', 'bench', 'watchy_bench_corpus.h')

def collect():
    out = []
    for p in sorted(glob.glob(os.path.join(HERE, 'ops', 'op_*.mp'))):
        out.append(('op', os.path.basename(p)[:-3], p))
    for p in sorted(glob.glob(os.path.join(ROOT, 'tools', 'host_harness', 'corpus', '*.mp'))):
        out.append(('script', os.path.basename(p)[:-3], p))

    # The newest device backup, by directory name. Content files are named by
    # fileId (s0, s1, ...), so list.json supplies the human names.
    backups = sorted(glob.glob(os.path.join(ROOT, 'tools', 'device', 'backups', '20*')))
    if backups:
        newest = backups[-1]
        listing = os.path.join(newest, 'files', 'scripts', 'list.json')
        if os.path.exists(listing):
            import json
            for entry in json.load(open(listing)):
                body = os.path.join(newest, 'files', 'scripts', 'content', entry['fileId'])
                if os.path.exists(body):
                    ident = ''.join(c if c.isalnum() else '_' for c in entry['id'])
                    out.append(('real', ident, body))
    return out

def main():
    items = collect()
    L = []
    L.append('// GENERATED FILE -- do not edit by hand.')
    L.append('// Regenerate with: python3 tools/device_bench/gen_ops_corpus.py')
    L.append('#ifndef WATCHY_BENCH_CORPUS_H')
    L.append('#define WATCHY_BENCH_CORPUS_H')
    L.append('')
    L.append('#if MP_BENCH')
    L.append('')
    L.append('struct MPBenchScript { const char* kind; const char* name; const char* src; };')
    L.append('')
    for kind, name, path in items:
        src = open(path, 'r').read()
        assert ')MPB"' not in src, path
        sha = hashlib.sha256(src.encode()).hexdigest()[:16]
        ident = name.replace('-', '_')
        L.append('// %s  (%d bytes, sha256:%s)' % (os.path.relpath(path, ROOT), len(src), sha))
        L.append('static const char kMPB_%s[] = R"MPB(%s)MPB";' % (ident, src))
        L.append('')
    L.append('static const MPBenchScript kMPBenchScripts[] = {')
    for kind, name, path in items:
        L.append('    { "%s", "%s", kMPB_%s },' % (kind, name, name.replace('-', '_')))
    L.append('};')
    L.append('static const int MPBENCH_SCRIPT_COUNT = %d;' % len(items))
    L.append('')
    L.append('#endif // MP_BENCH')
    L.append('#endif // WATCHY_BENCH_CORPUS_H')
    open(OUT, 'w').write('\n'.join(L) + '\n')
    print('wrote %s  (%d scripts)' % (os.path.relpath(OUT, ROOT), len(items)))

if __name__ == '__main__':
    main()

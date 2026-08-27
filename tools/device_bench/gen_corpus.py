#!/usr/bin/env python3
"""Generate the embedded benchmark corpus header for the M5Paper bench firmware.

Reads tools/host_harness/corpus/*.mp (the SAME corpus the host harness uses --
tier 1 and tier 2 must benchmark identical bytes) and emits
M5Paper_MicroPatterns/src/bench/mp_bench_corpus.h as C++ raw string literals.

Why string literals and not SPIFFS/LittleFS:
  * The whole corpus is ~10 KB. It fits in flash .rodata with no fuss.
  * A filesystem image means a SECOND flash step (`pio run -t uploadfs`), a
    data/ directory, and a whole extra class of "the numbers are wrong because
    the device still had the old corpus" failure. One `pio run -t upload`
    flashes firmware and corpus together, so the corpus can never drift from
    the firmware that reports it.
  * The production firmware already keeps a built-in default script as a C
    string, so this matches existing practice.

Usage:  python3 tools/device_bench/gen_corpus.py
"""

import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CORPUS_DIR = os.path.join(ROOT, "tools", "host_harness", "corpus")
OUT = os.path.join(ROOT, "M5Paper_MicroPatterns", "src", "bench", "mp_bench_corpus.h")

DELIM = "MPBCORPUS"


def main():
    if not os.path.isdir(CORPUS_DIR):
        sys.exit("corpus dir not found: %s (is the host harness present?)" % CORPUS_DIR)
    names = sorted(n for n in os.listdir(CORPUS_DIR) if n.endswith(".mp"))
    if not names:
        sys.exit("no .mp files in %s" % CORPUS_DIR)

    parts = []
    parts.append("// GENERATED FILE -- do not edit by hand.\n")
    parts.append("// Regenerate with: python3 tools/device_bench/gen_corpus.py\n")
    parts.append("// Source: tools/host_harness/corpus/*.mp (shared with the host harness)\n")
    parts.append("#ifndef MP_BENCH_CORPUS_H\n#define MP_BENCH_CORPUS_H\n\n")
    parts.append("#if MP_BENCH\n\n")

    entries = []
    for n in names:
        path = os.path.join(CORPUS_DIR, n)
        with open(path, "rb") as f:
            data = f.read()
        text = data.decode("utf-8")
        if (')' + DELIM + '"') in text:
            sys.exit("raw-string delimiter collision in %s" % n)
        stem = os.path.splitext(n)[0]
        sym = "kScript_" + "".join(c if c.isalnum() else "_" for c in stem)
        sha = hashlib.sha256(data).hexdigest()[:16]
        parts.append('// %s  (%d bytes, sha256:%s)\n' % (n, len(data), sha))
        parts.append('static const char %s[] = R"%s(%s)%s";\n\n' % (sym, DELIM, text, DELIM))
        entries.append((stem, sym, len(data), sha))

    parts.append("struct MPBenchScript {\n"
                 "    const char* name;\n"
                 "    const char* text;\n"
                 "    unsigned int bytes;\n"
                 "    const char* sha256_16;  // first 16 hex chars of sha256 of the source bytes\n"
                 "};\n\n")
    parts.append("static const MPBenchScript kMPBenchCorpus[] = {\n")
    for stem, sym, ln, sha in entries:
        parts.append('    {"%s", %s, %d, "%s"},\n' % (stem, sym, ln, sha))
    parts.append("};\n")
    parts.append("static const int kMPBenchCorpusCount = %d;\n\n" % len(entries))
    parts.append("#endif // MP_BENCH\n#endif // MP_BENCH_CORPUS_H\n")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w") as f:
        f.write("".join(parts))
    print("wrote %s (%d scripts)" % (OUT, len(entries)))
    for stem, _sym, ln, sha in entries:
        print("  %-24s %6d bytes  sha256:%s" % (stem, ln, sha))


if __name__ == "__main__":
    main()

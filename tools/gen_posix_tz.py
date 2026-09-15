#!/usr/bin/env python3
"""Generate micropatterns_emulator/posix_tz.js from the IANA tzdata.

WHY A GENERATED TABLE, and not something computed in the browser.

The device needs a POSIX TZ string -- "CET-1CEST,M3.5.0,M10.5.0/3" -- because
that is the only form of "timezone" its libc can act on (see mp_clock.h). The
browser knows the user's zone as an IANA NAME ("Europe/Paris") and will not hand
over a POSIX string, so something has to map one to the other.

The first attempt at this derived the string by probing: ask Intl for the zone's
offset on every day of the year, find where it changes, and describe the changes
as POSIX rules. It agreed with a reference libc on 414 of 418 zones, which looks
convincing and is a trap. Every POSIX string ALREADY EXISTS, published by IANA
in the footer of each compiled TZif file -- put there for exactly this purpose,
so an implementation can state a zone's future rules in POSIX form. Reading them
is authoritative; re-deriving them is guesswork that happens to agree.

Concretely, on the four zones where the two disagreed:

  * America/Vancouver -- tzdata 2026c publishes "MST7": British Columbia on
    permanent UTC-7. A single year's probe sees a spring-forward and a
    fall-back and confidently writes a DST rule that no longer exists. No
    amount of probing one year can see a rule that was abolished.
  * Africa/Casablanca -- tzdata publishes "<+00>0" and declines to express the
    Ramadan shift at all, because POSIX has room for one DST period a year and
    Morocco has two. The probe invented a plausible two-rule string that is
    wrong on dates nobody would think to check.

So: read the footers. This is also the source that github.com/nayarsystems/
posix_tz_db -- the table most ESP32 projects use for this -- is generated from;
we generate our own so the tzdata version is ours to see and to refresh, and so
there is nothing to vendor from a third party.

USAGE

    python3 tools/gen_posix_tz.py                  # uses /usr/share/zoneinfo
    python3 tools/gen_posix_tz.py --zoneinfo PATH
    python3 tools/gen_posix_tz.py --check          # fail if the output is stale

Re-run it when the host's tzdata is updated -- a zone abolishing DST is exactly
the kind of change that otherwise leaves watches an hour out. The generated file
records the tzdata version it came from, and the editor shows that version in the
setup panel so the provenance is visible rather than implied.
"""

import argparse
import json
import pathlib
import sys

DEFAULT_ZONEINFO = "/usr/share/zoneinfo"
OUTPUT = "micropatterns_emulator/posix_tz.js"

# Subtrees that are alternative encodings of the same zones, not zones of their
# own. "right/" counts leap seconds (its epoch is not Unix time) and "posix/"
# duplicates the default tree; including either would put two spellings of every
# zone in the picker.
SKIP_PREFIXES = ("right/", "posix/", "SystemV/")

# Not zones: the version stamp, the file tzcode uses for its own DST fallback,
# and the source tarball leftovers some distributions ship.
SKIP_NAMES = {"+VERSION", "posixrules", "localtime", "Factory"}


def posix_footer(raw: bytes):
    """The POSIX TZ string a compiled TZif file carries, or None.

    TZif version 2 and later append the string to the end of the file, enclosed
    in newlines (RFC 8536 section 3.3), so it is whatever sits between the last
    two newline bytes. Version 1 files have no footer at all -- their version
    byte is NUL -- and are skipped rather than guessed at.
    """
    if raw[:4] != b"TZif":
        return None
    if raw[4:5] == b"\x00":  # version 1: no footer to read
        return None
    end = raw.rfind(b"\n")
    if end < 0:
        return None
    start = raw.rfind(b"\n", 0, end)
    if start < 0:
        return None
    text = raw[start + 1:end].decode("ascii", errors="strict").strip()
    return text or None


def collect(zoneinfo: pathlib.Path):
    """{zone name: POSIX TZ string} for every zone in a zoneinfo tree."""
    zones = {}
    for path in sorted(zoneinfo.rglob("*")):
        if not path.is_file():
            continue
        name = str(path.relative_to(zoneinfo))
        if name in SKIP_NAMES or name.startswith(SKIP_PREFIXES):
            continue
        try:
            raw = path.read_bytes()
        except OSError:
            continue
        footer = posix_footer(raw)
        if footer:
            zones[name] = footer
    return zones


def tzdata_version(zoneinfo: pathlib.Path) -> str:
    version_file = zoneinfo / "+VERSION"
    if version_file.exists():
        return version_file.read_text().strip()
    return "unknown"


def render(zones: dict, version: str) -> str:
    # Sorted, one zone per line: a diff after a tzdata update then shows exactly
    # which zones changed their rules, which is the interesting part.
    entries = ",\n".join(
        f"    {json.dumps(name)}: {json.dumps(tz)}" for name, tz in sorted(zones.items())
    )
    return f'''// GENERATED FILE -- do not edit by hand.
//
// Regenerate with:  python3 tools/gen_posix_tz.py
//
// IANA time zone name -> the POSIX TZ string that zone's compiled TZif file
// publishes in its footer (RFC 8536 section 3.3). That footer is IANA's own
// statement of the zone's ongoing rules in POSIX form, which is the only form
// the devices' libc can act on -- see M5Paper_MicroPatterns/src/mp_clock.h and
// tools/gen_posix_tz.py for why this is read rather than computed.
//
// tzdata version: {version}
// zones: {len(zones)}

export const TZDATA_VERSION = {json.dumps(version)};

export const POSIX_TZ = {{
{entries}
}};
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zoneinfo", default=DEFAULT_ZONEINFO,
                        help=f"compiled tzdata tree (default {DEFAULT_ZONEINFO})")
    parser.add_argument("--output", default=None,
                        help=f"where to write (default {OUTPUT}, relative to the repo root)")
    parser.add_argument("--check", action="store_true",
                        help="do not write; exit non-zero if the file would change")
    args = parser.parse_args()

    zoneinfo = pathlib.Path(args.zoneinfo)
    if not zoneinfo.is_dir():
        print(f"error: no zoneinfo tree at {zoneinfo}", file=sys.stderr)
        return 2

    zones = collect(zoneinfo)
    if not zones:
        print(f"error: {zoneinfo} held no TZif files with a POSIX footer", file=sys.stderr)
        return 2

    version = tzdata_version(zoneinfo)
    text = render(zones, version)

    repo_root = pathlib.Path(__file__).resolve().parent.parent
    out = pathlib.Path(args.output) if args.output else repo_root / OUTPUT

    if args.check:
        current = out.read_text() if out.exists() else ""
        if current != text:
            print(f"{out} is stale: regenerate with python3 tools/gen_posix_tz.py",
                  file=sys.stderr)
            return 1
        print(f"{out} is up to date ({len(zones)} zones, tzdata {version})")
        return 0

    out.write_text(text)
    print(f"wrote {out}: {len(zones)} zones from tzdata {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# Timezone provisioning: the record of doing it (2026-09-15)

Companion to `device-provisioning-design.md`, which carries the resulting wire
format and storage keys. This file is the **history**: what was decided and
why, the approach that was built first and thrown away, the cross-check that
was misread, and how each claim in the shipped code was actually verified. It
is longer than the change deserves on its own, because the discarded approach
looked right on 414 of 418 zones and the reason it was wrong is not visible
anywhere in the code that replaced it.

**Status: built, builds on both devices, exercised end to end in a browser
against a fake device. Not yet run on hardware.** See §8.

Commits, in dependency order: `3529cd9` (provisioning stores the rule),
`5682f25` (shared `mp_clock`), `8364b98` (resync on every script sync),
`57d3682` (the zone table and its generator), `48c32ca` (the editor panel),
`d6423e9` (design doc). The README paragraph under *Environment Variables*
went in with `da124c2`, swept up alongside unrelated renderer work.

## 1. The ask, and what the codebase made of it

"Set the timezone of the watch, defaulting to the browser's zone, as a new
setting in the editor's Set up block."

Before this, the state was:

- **M5Paper** stored an `int8` `timezone` in NVS (`SystemManager`, default 1)
  and used it as `configTime(_timezone * 3600, 0, ...)`. Nothing anywhere could
  write that key. It was a setting in name only.
- **Watchy** had `static const int MP_TZ_OFFSET_HOURS = 1;` with a comment
  admitting that changing zone meant changing the line, and that DST was
  handled on neither device.
- Both write **local wall time** into the RTC and re-derive it only at an NTP
  sync. The M5Paper syncs only when the RTC reads exactly `00:00:00` — i.e.
  essentially once. The Watchy syncs at a cold clock or a button hold.
- `WatchyRTC::now()` returns hour, minute, second. **No date.**

That last point decided everything that follows. A stored offset — any stored
offset — cannot self-correct for daylight saving on these devices, because
nothing on them can tell whether today is past the March switch. DST can only
ever be applied *at a sync*.

## 2. The three positions, and the one taken

Put to the user as a fork before building anything:

| Option | Summer behaviour |
|---|---|
| Fixed offset in minutes | Re-send from the editor twice a year. Status quo plus a setting. |
| Offset **and** the switch dates, applied at sync | Right whenever the device next syncs. |
| Rules, and move the RTC to UTC with conversion on read | Always right, no sync needed. Rewrites the RTC contract in both firmwares and the meaning of the RTC contents on deployed devices. |

The user picked fixed offset, then asked "but what about daylight savings?" —
the right question. The middle row won: a POSIX TZ string *is* "a fixed offset
plus the two dates it changes on" in one field, the ESP32's newlib parses it
for free (`configTzTime(tz, server)` replaces `configTime(offset, 0, server)`),
and nothing else in the firmware has to understand it.

The catch was sync frequency: with the M5Paper syncing once in its life, DST
would land months late. So `mp_sync_scripts()` now resyncs the clock on
**every** script sync, while WiFi is already up. No "is it due yet"
bookkeeping — that would need the date the Watchy cannot read — and one SNTP
round trip is a rounding error against fetching every script. A device now
corrects itself within one script sync of a switch.

RTC-to-UTC was rejected for scope, not on merit. It would be the correct
long-term shape; it is also a change to `WatchyRTC::set/now`, `SystemManager`,
and every deployed device's RTC contents, for a feature request that was "add a
setting".

## 3. Dead end: deriving the POSIX string in the browser

The browser knows the zone as an IANA name (`Europe/Paris`) and will not hand
over a POSIX string. Something has to map one to the other.

### 3.1 What was built first

A ~300-line `timezone.js` that *computed* the string from `Intl`:

1. Sample the zone's offset on every day of the year (365 `Intl.DateTimeFormat`
   calls — a few milliseconds).
2. Where it changes, binary-search the transition down to the second.
3. Two transitions → standard = smaller offset, daylight = larger; write each
   as `Mm.w.d/h` using the wall-clock reading *before* the change (POSIX's
   convention — the EU spring switch is `/2`, not `/3`). Zero transitions →
   fixed string. Anything else → fall back to a fixed offset with a flag.
4. Abbreviations: pass through if purely alphabetic, else synthesise.

It produced the canonical `CET-1CEST,M3.5.0,M10.5.0/3` for Paris,
`EST5EDT4,M3.2.0,M11.1.0` for New York, quarter-hour Chatham, half-hour Lord
Howe, southern-hemisphere Sydney. It handled Morocco's four-transition 2029 by
falling back. It looked finished.

### 3.2 The cross-check, and how it was misread

Every derived string was fed to macOS's libc (`TZ=<string>`, Python
`time.localtime`) and compared hourly across 2026 against `TZ=<IANA name>`.
Result: **414 of 418 zones identical.** The four mismatches:

- `America/Vancouver`, `America/Edmonton` — macOS said they never left DST in
  November 2026; derived string said they did.
- `Africa/Casablanca`, `Africa/El_Aaiun` — macOS had a third transition in
  September; derived string had two.

Since `America/Los_Angeles` — identical rules to Vancouver — matched, these
were written off as macOS's tzdata being stale or odd. **That was backwards.**
macOS ships tzdata **2026c**; Node's ICU (which the derivation ran on) is on
**2026a**. British Columbia moved to permanent UTC−7 in between, which tzdata
expresses as `MST7`. The derivation had confidently invented a DST rule for a
zone that no longer has one, because a single year's probe cannot see a rule
that was abolished. Morocco was the same story with a different tzdata delta.

The fresher source was right, and the "convincing" 414/418 was the trap: the
method is structurally unable to know about the cases where it is wrong.

### 3.3 The user's push, and what replaced it

Mid-way through this the user said: *do something industry standard and well
known, don't reinvent.* Correct on two counts — the mechanism already existed,
and I had been ignoring it while reverse-engineering its output.

Every compiled TZif file in the IANA tzdata ends with a **POSIX TZ string in
its footer** (RFC 8536 §3.3), put there precisely so an implementation can
state the zone's ongoing rules in POSIX form. That footer is what
`nayarsystems/posix_tz_db` — the table most ESP32 projects use for this — is
generated from. Reading `/usr/share/zoneinfo/Europe/Paris` gives
`CET-1CEST,M3.5.0,M10.5.0/3` directly; Vancouver gives `MST7`; Casablanca
gives `<+00>0` — IANA *declining* to express the Ramadan rule in POSIX at all,
which is a far better answer than a plausible two-rule guess.

So: `tools/gen_posix_tz.py` reads the footers out of the host's zoneinfo tree
(nothing downloaded, nothing vendored from a third party) into
`micropatterns_emulator/posix_tz.js` — 597 zones, 23 KB, tzdata 2026c, one
zone per line so a diff after a tzdata update shows exactly which rules
changed. `--check` fails when the file is stale. `timezone.js` shrank to a
lookup plus display helpers.

Coverage check: every one of the 418 zones `Intl.supportedValuesOf('timeZone')`
can report is in the table.

## 4. Sub-decision: the quoted `<+1030>` abbreviations

Unquoted POSIX names may only contain letters, but modern tzdata writes many
zones as `<+0530>-5:30`. In the probing version I had *avoided* the quoted form
entirely — synthesising `STD`/`DST` — on the reasoning that "newlib is believed
to accept it" was not good enough when a rejected string silently falls back to
UTC, which is the exact failure being fixed.

The footers use the quoted form in **231 of 597 zones**, so avoidance stopped
being an option and the belief had to become a fact. Verified in the toolchain's
own libc rather than from documentation:

```
ar p ~/.platformio/packages/xtensa-esp-elf/xtensa-esp-elf/lib/esp32/libc.a \
     libc_a-tzset_r.o > tzr.o
xtensa-esp32-elf-objdump -d tzr.o | grep -E "movi.*(60|62)$"   # '<' and '>'
strings tzr.o                                                   # the scanners
```

`_tzset_unlocked_r` tests for `'<'` (60) and `'>'` (62) at several sites and
carries both `%11[A-Za-z]%n` and `%11[-+0-9A-Za-z]%n` — the unquoted and quoted
name scanners. So the form is supported, and names are capped at 11 characters.
The longest name in any footer is 7; the longest whole footer is 44 characters
(`NZ-CHAT`); `MAX_TZ_LEN` is 63.

Note the first archive member tried, `libc_a-tzset.o`, contains only the
locking wrapper — the parser is in `tzset_r.o`. `nm --defined-only -A` on the
archive finds which.

## 5. What survived from the dead end

One piece, repurposed from *generator* to *cross-check*: `posixOffsets()` parses
the two numeric offset fields out of a rule — deliberately not the switch dates,
so it is not a rule engine — and the panel compares them with the browser's
live offset for the zone. If the live offset is neither the standard nor the
daylight offset the rule can produce, the panel warns.

Across all 597 zones that fires for exactly two: `Africa/Casablanca` and
`Africa/El_Aaiun`, where IANA's `<+00>0` cannot produce the +01:00 the zone is
on for most of the year. It does **not** fire for Vancouver, because the
browser's (older) live offset in September is −07:00 and `MST7` produces
−07:00 — which is the point: the check is about what the rule can express,
never about which tzdata is newer.

An earlier idea — probing the year via `Intl` to warn "your browser thinks this
zone switches but the rule has no switch" — was dropped, because with Vancouver
it would have warned about the *correct* rule.

## 6. Protocol and storage decisions, with reasons

- **Three independent optional parts** (`userId`, `networks`, `tz`), at least
  one required. "I moved country" is then a one-field write that does not ask
  for WiFi passwords, the same way ID rotation already avoided it.
- **`forget` keeps the timezone.** `prefs.clear()` empties the namespace; the
  zone is read before and written back after. The button says *Erase WiFi +
  ID*; a zone is not a credential; nobody pressing it means "and put my watch
  back on UTC".
- **Length-checked before the write, not after.** NVS would truncate an
  over-long string into one that still parses — `CET-1CE` is a valid
  standard-only zone — so the device would run on a plausible wrong offset with
  no sign anything happened. Read-back verification (`tzStored == tz`) is there
  too, on the pattern the existing network write already uses.
- **Wire version 1 → 2.** A v1 device silently ignores `tz` and answers `ok`,
  which reads exactly like a write that landed. The editor reads `version` from
  the `status` it already sends on connect, warns, drops `tz` from a mixed
  write, and refuses a tz-only write outright.
- **`tzName` is stored and never read.** Purely so `status` can say
  `Europe/Paris` rather than only the rule. There is no tzdata on the device to
  look it up in, and the code says so where the key is defined.
- **Fallback when nothing is provisioned is `STD-1`** (UTC+1, no DST) — not
  UTC, because that is what both firmwares already did, so an unprovisioned
  device reads exactly as before rather than jumping an hour. The M5Paper feeds
  its legacy NVS `int8` into the fallback via `setFallbackOffsetHours`, on the
  principle that silently ignoring a stored setting because we assume its value
  is its own small bug — even though in practice it is always 1.

## 7. The cross-task RTC write on the M5Paper

Resyncing inside `mp_sync_scripts()` means `M5.RTC.setTime/setDate` now run on
**FetchTask**, where before every RTC access was on MainControlTask. Checked
rather than assumed: Arduino-ESP32's `TwoWire` (`Wire.cpp` lines ~110–130 in the
installed framework) holds a per-instance mutex around each transaction, so a
register read on one task cannot interleave with a write on the other. What is
not atomic is the `setTime`/`setDate` pair: a reader landing between them sees
the new time with the old date, for well under a millisecond, only at a sync.
Nothing reads the two together and cares — `updateLastFetchTimestamp()` already
reads them as two calls. Noted in a comment on `writeM5PaperRTC` rather than
locked.

## 8. How it was verified — and what was not

**Builds.** Both firmwares, clean. The M5Paper's `m5paper-bench` environment
fails, but that predates this work: `src/bench/mp_bench.cpp` calls
`DisplayListRenderer` with a 4-argument constructor the header (changed by the
concurrent rasteriser work) no longer has.

**The editor, end to end.** Web Bluetooth needs a user gesture and a real
radio, so a **fake device** was installed into the page by replacing
`navigator.bluetooth.requestDevice` with one returning a scripted GATT server
that mirrors `handleCommand()`'s semantics — independent parts, empty-list
refusal, blank-password-keeps-stored, `forget` keeping the zone, and a
configurable wire version. Notifications are emitted in 20-byte chunks, the
pessimistic pre-negotiation MTU. Exercised through the real buttons and
handlers:

- Connect → automatic `status` → panel says "no timezone stored yet".
- Send with all three ticked → exactly
  `{"cmd":"provision","tz":"CET-1CEST,M3.5.0,M10.5.0/3","tzName":"Europe/Paris","userId":…,"networks":[…]}`
  on the wire; reply rendered; password-keep path still works.
- `forget` → reply carries the zone; panel says it was kept.
- v1 device → warning on connect; mixed write goes out **without** `tz`; a
  tz-only write is refused with nothing sent.
- All eight toggle combinations → button label, tooltip and section dimming
  correct. Zone choice persisted; *This computer* clears it.
- Casablanca → bold warning; Kolkata/Vancouver → neutral "no switch"; Chatham
  → rule shown verbatim.

The fake lives at `fakedevice.js` in the session scratchpad and was **not
committed**. It is ~150 lines and would be a reasonable `tools/` addition if
the panel is touched again.

**Two environment traps** cost time and are worth knowing:

- The Browser pane's preview server is sandboxed to the scratchpad — `EPERM`
  on the repo — so the editor had to be served from a copy there.
- Screenshots of the page came back uniformly blank in that pane, though the
  DOM, computed styles and geometry were all readable. Layout was verified
  numerically (picker 309×51 at y=516, readout below, dark on white, nothing
  hidden). Nobody has *looked* at the panel yet.

**Not done.** No hardware run. Specifically unproven on a device: that
`configTzTime` with a quoted-name rule sets the RTC to the expected local hour;
that the resync inside a script sync does not disturb the sync's own timing on
the Watchy; that `status` round-trips a 44-character rule intact over a real
MTU.

## 9. Open items

- **Hardware test**, per §8. A Chatham or Lord Howe rule is the one to try,
  since those exercise both the quoted names and the non-hour offsets.
- **Regenerate the table when tzdata updates.** A zone abolishing DST is exactly
  the change that otherwise leaves watches an hour out. `tools/gen_posix_tz.py
  --check` can gate CI on it. The editor shows the tzdata version in the panel
  so the provenance is visible.
- **Morocco is wrong by an hour for part of every year** and will stay so: POSIX
  has room for one DST period and the Ramadan shift needs a second. The panel
  says so; nothing more can be done short of the RTC-to-UTC redesign plus a
  rule engine, which is out of proportion.
- **No serial-console command** on the M5Paper to show or set the zone. `status`
  over BLE reports it, which is enough for now.
- **RTC to UTC** remains the correct long-term shape if DST-without-a-sync is
  ever wanted. Everything here is compatible with it: the rule is already
  stored, and `MPClock::apply()` already sets `TZ` for `localtime()`.

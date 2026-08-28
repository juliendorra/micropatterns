# Panel messages

Every user-facing string either firmware can draw, and the layout rules that
decide whether it is readable.

`panel-messages.html` in this directory is the visual companion: it redraws all
of these frames at real font metrics, running the firmware's own wrap function,
so a wording change can be checked for fit before it is flashed. Open it in a
browser -- it is self-contained.

## Where the strings live

`M5Paper_MicroPatterns/src/mp_messages.h`, compiled by both firmwares.

They used to be written inline at each call site and drifted apart. The M5Paper
said `NetMgr Fail!`, `Fetch: Fetch OK`, `Render Fail: eyes`; the Watchy said
`Sync failed`, `no WiFi`, `Render error`. Same events, two vocabularies, and the
M5Paper's named the class that failed rather than what went wrong.

Sync outcomes are not in the display code at all -- they travel in
`ScriptSyncResult::message` from the shared `mp_sync_scripts()`, so a sync
reports itself in identical words on either device.

Rules for anything added:

- say what happened, not which class failed. "No WiFi", not "NetMgr Fail!".
- no abbreviations, no trailing exclamation marks.
- titles must fit 16 glyphs -- one line at size 2 on the Watchy. Bodies wrap,
  titles do not.

## Metrics

Both panels draw the same 6x8 bitmap font at different scales.

| | Watchy | M5Paper |
|---|---|---|
| Panel | 200x200 | 540x960 |
| Text size | 2 (title and body) | 3 |
| Glyph advance | 12 px | 18 px |
| Glyphs per line | **16** | **30** |
| Wrapping | yes, `drawWrapped()` | none -- one centred line per call |
| Line height | 20 px | fixed y offsets per call |

Two things follow from the table, both of which were bugs:

- Watchy body text was size 1 (6 px per glyph) and unreadable on a 200 px panel.
  It is size 2 now, which is why the wrap exists: 16 glyphs is not many.
- The M5Paper does not wrap, so `Render error: city-2-by-telohtrab` was 33
  glyphs on a 30-glyph line and lost both ends. It is two calls now -- title,
  then script name -- which also matches the Watchy's error frame.

## The frames

**Watchy** (`showNotice()`: title at y=26, body from y=56, 20 px per line)

- `No scripts` -- nothing stored, or a first sync found nothing
- `Syncing` -- top-left held 5s, or a boot with no scripts
- `Sync failed` + reason + `hold top-left to retry`
- `Render error` + script name + reason
- script name alone, one frame, when the script changes

**M5Paper** (`showMessage()`, one centred line at a given y)

- `Startup failed` (y 150)
- `Syncing` (y 200)
- `Render error` (y 200) + script id (y 250)
- sync outcome (y 350), `New scripts` (y 400)
- script name (y 250)

## Sync outcomes

Shared by both, from `ScriptSyncResult::message`:

| Constant | Text |
|---|---|
| `MP_MSG_SYNC_OK` | Scripts up to date |
| `MP_MSG_SYNC_NO_WIFI` | No WiFi |
| `MP_MSG_SYNC_NO_SERVER` | Server unreachable |
| `MP_MSG_SYNC_BAD_REPLY` | Unexpected server reply |
| `MP_MSG_SYNC_NO_SAVE` | Could not save scripts |
| `MP_MSG_SYNC_PARTIAL` | Some scripts missing |
| `MP_MSG_SYNC_STOPPED` | Sync stopped |

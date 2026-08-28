#ifndef MP_MESSAGES_H
#define MP_MESSAGES_H

// Every user-facing string the two firmwares show on their panels.
//
// They used to be written inline at each call site, and drifted: the M5Paper
// said "NetMgr Fail!", "Fetch: Fetch OK" and "Render Fail: eyes" while the
// Watchy said "Sync failed", "no WiFi" and "Render error". Same events, two
// vocabularies, neither of them a sentence anyone would say out loud.
//
// Rules for anything added here:
//   - say what happened, not which class failed. "No WiFi", not "NetMgr Fail!".
//   - no abbreviations and no trailing exclamation marks.
//   - titles fit 16 glyphs, which is one line at size 2 on the Watchy's 200px
//     panel. Body lines wrap, titles do not.

// Titles
#define MP_MSG_RENDER_ERROR   "Render error"
#define MP_MSG_NO_SCRIPTS     "No scripts"
#define MP_MSG_SYNCING        "Syncing"
#define MP_MSG_SYNC_FAILED    "Sync failed"
#define MP_MSG_STARTUP_FAILED "Startup failed"

// Body lines
#define MP_MSG_CONNECTING     "connecting to WiFi"
#define MP_MSG_NO_SCRIPTS_HOW "set up WiFi in the web editor, then hold"
#define MP_MSG_NO_SCRIPTS_HOW2 "top-left to sync"
#define MP_MSG_RETRY_SYNC     "hold top-left to retry"
#define MP_MSG_SCRIPT_MISSING "content missing on device"
#define MP_MSG_PARSE_FAILED   "script did not parse"

// Sync outcomes. Returned in ScriptSyncResult::message, so both firmwares
// report a sync in the same words.
#define MP_MSG_SYNC_OK        "Scripts up to date"
#define MP_MSG_SYNC_NO_WIFI   "No WiFi"
#define MP_MSG_SYNC_NO_SERVER "Server unreachable"
#define MP_MSG_SYNC_BAD_REPLY "Unexpected server reply"
#define MP_MSG_SYNC_NO_SAVE   "Could not save scripts"
#define MP_MSG_SYNC_PARTIAL   "Some scripts missing"
#define MP_MSG_SYNC_STOPPED   "Sync stopped"

#endif // MP_MESSAGES_H

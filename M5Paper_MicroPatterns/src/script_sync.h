#ifndef SCRIPT_SYNC_H
#define SCRIPT_SYNC_H

// Server sync, shared by both firmwares.
//
// This used to live inline inside the M5Paper's FetchTask_Function(). The Watchy
// needs exactly the same behaviour -- connect, fetch the list, save it, fetch
// every script's content, save it, then clean up whatever the server no longer
// lists -- and reimplementing it there would have meant two copies of a
// procedure whose subtleties were paid for by an incident (see
// docs/incidents/2026-08-27-m5paper-script-loss.md). So it is one function,
// synchronous, with no knowledge of tasks, queues or displays.
//
// The M5Paper calls it from its FetchTask; the Watchy calls it straight from
// loop(). Progress is reported through an optional callback so each firmware can
// draw it however it likes.

#include <Arduino.h>
#include "event_defs.h"

class MPNetworkManager;
class ScriptManager;

struct ScriptSyncResult
{
    FetchResultStatus status = FetchResultStatus::GENUINE_ERROR;
    String message;
    bool newScriptsAvailable = false;
    int successCount = 0;
    int failCount = 0;
    int serverCount = 0;
};

// Called before each long step so a firmware can show progress. May be null.
typedef void (*ScriptSyncProgressFn)(const char *stage, void *ctx);

// Runs a complete sync. Blocks until done. `interruptFlag`, if non-null, is
// polled by the network layer and aborts the sync when set. WiFi is left
// disconnected on return.
ScriptSyncResult mp_sync_scripts(MPNetworkManager &net,
                                 ScriptManager &scripts,
                                 bool fullRefresh,
                                 volatile bool *interruptFlag = nullptr,
                                 ScriptSyncProgressFn progress = nullptr,
                                 void *progressCtx = nullptr);

#endif // SCRIPT_SYNC_H

#include "script_sync.h"

#include <ArduinoJson.h>

#include "network_manager.h"
#include "script_manager.h"
#include "mp_wdt.h"

#define SCRIPT_SYNC_CONTENT_JSON_CAPACITY (MAX_SCRIPT_CONTENT_LEN + 512)

namespace
{
    inline void report(ScriptSyncProgressFn fn, void *ctx, const char *stage)
    {
        if (fn)
        {
            fn(stage, ctx);
        }
    }
}

ScriptSyncResult mp_sync_scripts(MPNetworkManager &net,
                                 ScriptManager &scripts,
                                 bool fullRefresh,
                                 volatile bool *interruptFlag,
                                 ScriptSyncProgressFn progress,
                                 void *progressCtx)
{
    ScriptSyncResult result;

    net.setInterruptFlag(interruptFlag);

    mp_wdt_reset();
    report(progress, progressCtx, "WiFi");
    if (!net.connectWiFi())
    {
        result.status = FetchResultStatus::NO_WIFI;
        result.message = "WiFi Connect Fail";
        return result;
    }

    // NOTE: a full refresh used to call clearAllScriptData() HERE, before the
    // fetch. That destroyed every local script up front, so any later failure --
    // a 404, a dropped connection, a malformed list -- left the device with
    // nothing and no way to recover, since the scripts exist only on the device.
    // That is exactly what happened when the API began returning 404 for this
    // device id.
    //
    // The wipe was also redundant: saveScriptList() below replaces list.json
    // wholesale, the content loop overwrites each file, and
    // cleanupOrphanedContent()/cleanupOrphanedStates() remove anything the
    // server no longer lists -- but only once the sync has fully succeeded.
    // Deleting nothing up front is strictly safer and ends in the same state.
    (void)fullRefresh;

    DynamicJsonDocument serverListDoc(JSON_DOC_CAPACITY_SCRIPT_LIST);
    DynamicJsonDocument scriptContentDoc(SCRIPT_SYNC_CONTENT_JSON_CAPACITY);

    mp_wdt_reset();
    report(progress, progressCtx, "List");
    result.status = net.fetchScriptList(serverListDoc);
    mp_wdt_reset();

    if (result.status != FetchResultStatus::SUCCESS)
    {
        result.message = (result.status == FetchResultStatus::INTERRUPTED_BY_USER)
                             ? "Fetch Interrupted"
                             : "Fetch List Fail";
        net.disconnectWiFi();
        return result;
    }

    if (!serverListDoc.is<JsonArray>())
    {
        log_e("Sync: server list is not a JSON array");
        result.status = FetchResultStatus::GENUINE_ERROR;
        result.message = "Invalid List Format";
        net.disconnectWiFi();
        return result;
    }

    JsonArray serverList = serverListDoc.as<JsonArray>();
    result.serverCount = (int)serverList.size();
    log_i("Sync: server list has %d scripts", result.serverCount);

    {
        JsonDocument localListDoc;
        const bool localListExists = scripts.loadScriptList(localListDoc);
        if (!localListExists || (int)localListDoc.as<JsonArray>().size() != result.serverCount)
        {
            result.newScriptsAvailable = true;
        }
    }

    mp_wdt_reset();
    if (!scripts.saveScriptList(serverListDoc))
    {
        log_e("Sync: failed to save script list (%d items)", result.serverCount);
        result.status = FetchResultStatus::GENUINE_ERROR;
        result.message = "Save List Fail";
        net.disconnectWiFi();
        return result;
    }
    mp_wdt_reset();

    bool allContentFetched = true;

    for (JsonObject scriptInfo : serverList)
    {
        mp_wdt_reset();

        const char *humanId = scriptInfo["id"].as<const char *>();
        if (!humanId || strlen(humanId) == 0)
        {
            log_w("Sync: skipping script with missing/empty ID");
            allContentFetched = false;
            result.failCount++;
            continue;
        }

        report(progress, progressCtx, humanId);
        scriptContentDoc.clear();
        const FetchResultStatus contentStatus = net.fetchScriptContent(humanId, scriptContentDoc);
        mp_wdt_reset();

        if (contentStatus == FetchResultStatus::INTERRUPTED_BY_USER)
        {
            log_i("Sync: content fetch for '%s' interrupted", humanId);
            result.status = FetchResultStatus::INTERRUPTED_BY_USER;
            allContentFetched = false;
            break;
        }
        if (contentStatus != FetchResultStatus::SUCCESS)
        {
            log_e("Sync: failed to fetch content for '%s' (status %d)", humanId, (int)contentStatus);
            allContentFetched = false;
            result.failCount++;
            continue;
        }

        if (!scriptContentDoc.is<JsonObject>() || !scriptContentDoc["content"].is<const char *>())
        {
            log_e("Sync: invalid content response for '%s'", humanId);
            allContentFetched = false;
            result.failCount++;
            continue;
        }
        const char *content = scriptContentDoc["content"].as<const char *>();
        if (!content || strlen(content) == 0)
        {
            log_e("Sync: empty content for '%s'", humanId);
            allContentFetched = false;
            result.failCount++;
            continue;
        }

        String fileId;
        if (!scriptInfo["fileId"].isNull() && scriptInfo["fileId"].is<const char *>())
        {
            fileId = scriptInfo["fileId"].as<String>();
        }
        if (fileId.isEmpty() || fileId == "null" || !fileId.startsWith("s"))
        {
            fileId = scripts.generateShortFileId(humanId);
            scriptInfo["fileId"] = fileId;
        }

        mp_wdt_reset();
        if (!scripts.saveScriptContent(fileId, content))
        {
            log_e("Sync: failed to save content for '%s'", humanId);
            allContentFetched = false;
            result.failCount++;
        }
        else
        {
            result.successCount++;
        }
        mp_wdt_reset();

        if (interruptFlag && *interruptFlag)
        {
            log_i("Sync: content loop interrupted by user");
            result.status = FetchResultStatus::INTERRUPTED_BY_USER;
            allContentFetched = false;
            break;
        }
    }

    log_i("Sync: content done - ok %d, failed %d, total %d",
          result.successCount, result.failCount, result.serverCount);

    if (result.status != FetchResultStatus::INTERRUPTED_BY_USER)
    {
        if (allContentFetched)
        {
            result.status = FetchResultStatus::SUCCESS;
            result.message = fullRefresh ? "Full Refresh OK" : "Fetch OK";
            mp_wdt_reset();
            report(progress, progressCtx, "Cleanup");
            scripts.cleanupOrphanedContent(serverList);
            mp_wdt_reset();
            scripts.cleanupOrphanedStates(serverList);
            mp_wdt_reset();
        }
        else
        {
            result.status = FetchResultStatus::GENUINE_ERROR;
            result.message = "Partial Fetch";
        }
    }
    else
    {
        result.message = "Fetch Interrupted";
    }

    net.disconnectWiFi();
    mp_wdt_reset();
    return result;
}

#include "main.h"
#include "serial_console.h"

// Replies are prefixed so they survive the firmware's own log output.
#define CON_PREFIX "MPCON|"

static void con_printf(const char *fmt, ...)
{
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print(CON_PREFIX);
    Serial.println(buf);
}

static void con_help()
{
    con_printf("commands:");
    con_printf("  list            script list as <index>\\t<humanId>\\t<name>");
    con_printf("  run <id|index>  run a script by human id or list index");
    con_printf("  next | prev     same as the UP/DOWN buttons");
    con_printf("  current         currently loaded script id");
    con_printf("  sync            re-fetch all scripts from the server");
    con_printf("  help            this list");
}

// Sends an event onto the same queue the buttons feed. `id` may be NULL.
static bool con_send(InputEventType type, const char *id)
{
    InputEvent ev;
    ev.type = type;
    if (id) {
        strncpy(ev.script_id, id, MAX_SCRIPT_ID_LEN - 1);
        ev.script_id[MAX_SCRIPT_ID_LEN - 1] = '\0';
    }
    if (xQueueSend(g_inputEventQueue, &ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        con_printf("error: input queue full");
        return false;
    }
    return true;
}

// Prints the script list. Runs on the console task; ScriptManager guards
// SPIFFS with its own mutex, so this is safe to call from here.
static void con_list()
{
    if (!g_scriptManager) {
        con_printf("error: script manager not ready");
        return;
    }
    JsonDocument listDoc;
    if (!g_scriptManager->loadScriptList(listDoc)) {
        con_printf("error: could not load script list");
        return;
    }
    JsonArrayConst arr = listDoc.as<JsonArrayConst>();
    int i = 0;
    for (JsonObjectConst o : arr) {
        con_printf("%d\t%s\t%s", i++,
                   o["id"].as<const char *>() ? o["id"].as<const char *>() : "?",
                   o["name"].as<const char *>() ? o["name"].as<const char *>() : "?");
    }
    con_printf("%d script(s)", i);
}

// Resolves a bare list index ("3") to its human id. Returns "" if `token` is
// not all-digits or is out of range, in which case it is used as a human id.
static String con_resolve_index(const String &token)
{
    if (token.isEmpty()) return String();
    for (unsigned int i = 0; i < token.length(); i++) {
        if (!isdigit((unsigned char)token[i])) return String();
    }
    if (!g_scriptManager) return String();

    JsonDocument listDoc;
    if (!g_scriptManager->loadScriptList(listDoc)) return String();

    long want = token.toInt();
    JsonArrayConst arr = listDoc.as<JsonArrayConst>();
    long i = 0;
    for (JsonObjectConst o : arr) {
        if (i++ == want) {
            const char *id = o["id"].as<const char *>();
            return id ? String(id) : String();
        }
    }
    return String();
}

static void con_handle_line(String line)
{
    line.trim();
    if (line.isEmpty()) return;

    String cmd = line;
    String arg = "";
    int sp = line.indexOf(' ');
    if (sp >= 0) {
        cmd = line.substring(0, sp);
        arg = line.substring(sp + 1);
        arg.trim();
    }
    cmd.toLowerCase();

    if (cmd == "help" || cmd == "?") {
        con_help();
    } else if (cmd == "list") {
        con_list();
    } else if (cmd == "next") {
        if (con_send(InputEventType::NEXT_SCRIPT, NULL)) con_printf("ok next");
    } else if (cmd == "prev") {
        if (con_send(InputEventType::PREVIOUS_SCRIPT, NULL)) con_printf("ok prev");
    } else if (cmd == "current") {
        String cur;
        if (g_scriptManager && g_scriptManager->getCurrentScriptId(cur)) {
            con_printf("current %s", cur.c_str());
        } else {
            con_printf("error: no current script");
        }
    } else if (cmd == "sync") {
        // Same job the full-refresh gesture queues. The sync itself is shared
        // with the Watchy firmware (script_sync.cpp), so exercising it here
        // exercises both.
        FetchJob job;
        job.full_refresh = true;
        if (xQueueSend(g_fetchCommandQueue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
            con_printf("error: fetch queue full");
        } else {
            con_printf("ok sync queued");
        }
    } else if (cmd == "run") {
        if (arg.isEmpty()) {
            con_printf("usage: run <id|index>");
            return;
        }
        // A bare number is treated as a list index; anything else as a human id.
        String resolved = con_resolve_index(arg);
        const String &target = resolved.isEmpty() ? arg : resolved;
        if (con_send(InputEventType::RUN_SCRIPT_BY_ID, target.c_str())) {
            con_printf("ok run %s", target.c_str());
        }
    } else {
        con_printf("unknown command '%s' -- try 'help'", cmd.c_str());
    }
}

void SerialConsoleTask_Function(void *pvParameters)
{
    (void)pvParameters;

    char line[SERIAL_CONSOLE_MAX_LINE];
    size_t len = 0;
    bool overflow = false; // Discarding the tail of an over-long line.

    con_printf("serial console ready -- 'help' for commands");

    for (;;) {
        while (Serial.available() > 0) {
            int c = Serial.read();
            if (c < 0) break;

            if (c == '\n' || c == '\r') {
                if (overflow) {
                    con_printf("error: line too long (max %d)", SERIAL_CONSOLE_MAX_LINE - 1);
                    overflow = false;
                } else if (len > 0) {
                    line[len] = '\0';
                    con_handle_line(String(line));
                }
                len = 0;
                continue;
            }

            if (overflow) continue; // Swallow until end of line.

            if (len < sizeof(line) - 1) {
                line[len++] = (char)c;
            } else {
                overflow = true;
                len = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

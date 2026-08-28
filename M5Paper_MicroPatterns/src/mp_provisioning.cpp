#include "mp_provisioning.h"

#include <Preferences.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp32-hal-log.h"

namespace MPProvisioning {
namespace {

Preferences prefs;
const char* NVS_NS = "mpprov";
esp_err_t g_nvsInitErr = ESP_FAIL;   // result of nvs_flash_init(), for `diag`

BLEServer*         g_server = nullptr;
BLECharacteristic* g_notify = nullptr;
bool          g_windowOpen = false;
unsigned long g_windowEndsAt = 0;
bool          g_bleInited = false;

// Negotiated ATT MTU. Starts at the BLE default of 23, which allows only
// 23 - 3 = 20 payload bytes per notification. Anything larger is SILENTLY
// TRUNCATED by the stack -- which is why long replies (diag, status with
// several SSIDs) arrived gutted while short ones looked fine.
uint16_t g_mtu = 23;

// Reassembly buffer. A 3-network payload exceeds the ~185 byte MTU, so writes
// arrive in pieces; a newline terminates the message.
String g_rx;
volatile bool g_overflowed = false;

// A complete command, handed from the BLE callback to tick() for processing.
//
// onWrite() runs on the BLE STACK'S OWN TASK. Parsing JSON, writing NVS and
// notifying (with per-chunk delays) directly from there blocks that task for
// hundreds of milliseconds, which is a good way to wedge the stack -- and the
// Watchy, with far less headroom than the M5Paper, is where that showed up as
// a freeze. The callback now does the minimum: reassemble bytes, hand over a
// finished line, return.
// A SMALL QUEUE, not a single slot. With one slot, a command arriving while
// another was still queued was silently DROPPED -- and the editor now sends
// `status` automatically on connect, so a Diagnostics click landing moments
// later disappeared with no reply and no error. Silent loss is the worst
// possible failure for a debugging channel.
const int PENDING_MAX = 4;
String g_pending[PENDING_MAX];
volatile int g_pendHead = 0, g_pendTail = 0;

inline bool pendingEmpty() { return g_pendHead == g_pendTail; }
inline bool pendingFull()  { return ((g_pendTail + 1) % PENDING_MAX) == g_pendHead; }

String key(const char* fmt, int i) { char b[24]; snprintf(b, sizeof(b), fmt, i); return String(b); }

void reply(const String& json)
{
    if (!g_notify) return;
    // Chunked to stay under the negotiated MTU. 180 leaves room for ATT
    // overhead; the editor reassembles on the newline we append at the end.
    // Payload limit is MTU - 3 (ATT opcode + handle). Never assume a larger MTU
    // was negotiated: Web Bluetooth usually asks for ~185, but if that
    // negotiation has not happened yet we must still be correct at 23.
    const size_t CHUNK = (g_mtu > 23) ? (size_t)(g_mtu - 3) : 20;
    String out = json + "\n";
    for (size_t off = 0; off < out.length(); off += CHUNK) {
        String piece = out.substring(off, min(off + CHUNK, (size_t)out.length()));
        g_notify->setValue((uint8_t*)piece.c_str(), piece.length());
        g_notify->notify();
        delay(12); // let the stack drain; back-to-back notifies get dropped
    }
}

void handleCommand(const String& line)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
        log_w("Provisioning: bad JSON (%s)", err.c_str());
        reply("{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }

    const char* cmd = doc["cmd"] | "";

    if (strcmp(cmd, "status") == 0) {
        JsonDocument out;
        out["ok"] = true;
        out["userId"] = userId();
        out["provisioned"] = isProvisioned();
        out["count"] = networkCount();
        out["version"] = MP_PROVISIONING_VERSION;
        // SSIDs only -- stored passwords are never readable back, by design.
        JsonArray a = out["ssids"].to<JsonArray>();
        for (int i = 0; i < networkCount(); i++) a.add(networkSSID(i));
        out["lastGood"] = lastGoodIndex();
        // The SSID currently associated, if any, plus signal strength. This is
        // live state, not stored configuration -- it is what tells the user
        // whether provisioning actually worked.
        if (WiFi.status() == WL_CONNECTED) {
            out["ssid"] = WiFi.SSID();
            out["rssi"] = WiFi.RSSI();
        }
        String s; serializeJson(out, s); reply(s);
        return;
    }

    if (strcmp(cmd, "diag") == 0) {
        // Reports NVS health over BLE. Exists because the Watchy emits nothing
        // over serial, so this is the only way to see why a write failed.
        JsonDocument out;
        out["ok"] = true;
        out["nvsInit"] = esp_err_to_name(g_nvsInitErr);

        bool rw = prefs.begin(NVS_NS, false);
        out["openRW"] = rw;
        if (rw) {
            size_t wrote = prefs.putUChar("diag.probe", 42);
            out["probeWrote"] = wrote;
            out["probeRead"] = prefs.getUChar("diag.probe", 0);
            prefs.remove("diag.probe");
            prefs.end();
        }

        // RAW NVS PROBE.
        //
        // Preferences reported a successful write whose value then read back as
        // the default, with the entry count still zero -- i.e. it is swallowing
        // an error. These calls report the actual esp_err_t at every step, so
        // the failing operation names itself instead of being inferred.
        {
            nvs_handle_t h;
            esp_err_t eo = nvs_open("mpdiag", NVS_READWRITE, &h);
            out["rawOpen"] = esp_err_to_name(eo);
            if (eo == ESP_OK) {
                esp_err_t es = nvs_set_u8(h, "k", 42);
                out["rawSet"] = esp_err_to_name(es);
                esp_err_t ec = nvs_commit(h);
                out["rawCommit"] = esp_err_to_name(ec);
                uint8_t v = 0;
                esp_err_t eg = nvs_get_u8(h, "k", &v);
                out["rawGet"] = esp_err_to_name(eg);
                out["rawValue"] = v;
                nvs_erase_key(h, "k");
                nvs_commit(h);
                nvs_close(h);
            }
        }

        nvs_stats_t st;
        if (nvs_get_stats(NULL, &st) == ESP_OK) {
            out["nvsUsed"] = st.used_entries;
            out["nvsFree"] = st.free_entries;
            out["nvsTotal"] = st.total_entries;
        }
        out["heap"] = ESP.getFreeHeap();
        String j; serializeJson(out, j); reply(j);
        return;
    }

    if (strcmp(cmd, "forget") == 0) {
        prefs.begin(NVS_NS, false);
        prefs.clear();
        prefs.end();
        log_w("Provisioning: credentials wiped");
        reply("{\"ok\":true,\"forgotten\":true}");
        return;
    }

    if (strcmp(cmd, "provision") == 0) {
        // BOTH FIELDS ARE OPTIONAL, but at least one must be present.
        //
        //   userId only    -> rotate the ID, leave the networks untouched.
        //                     Needed: the original ID leaked publicly, so
        //                     rotating it without retyping WiFi passwords is a
        //                     real workflow.
        //   networks only  -> replace the list, keep the current ID.
        //   both           -> do both.
        //   networks: []   -> REFUSED. An empty array is ambiguous between "I
        //                     sent nothing" and "erase everything", and it used
        //                     to silently wipe the lot. Clearing is deliberate
        //                     and has its own command.
        const char* uid = doc["userId"] | "";
        const bool haveUid = strlen(uid) > 0;
        const bool haveNets = doc["networks"].is<JsonArray>();
        JsonArray nets = doc["networks"].as<JsonArray>();

        if (!haveUid && !haveNets) {
            reply("{\"ok\":false,\"error\":\"send userId, networks, or both\"}");
            return;
        }
        if (haveNets && nets.size() == 0) {
            reply("{\"ok\":false,\"error\":\"empty network list; use forget to clear\"}");
            return;
        }

        // Snapshot what is stored, so a blank password can mean "keep the one
        // you already have" instead of destroying it.
        //
        // This matters because the editor deliberately does NOT persist
        // passwords in the browser. After a page reload it has your SSIDs and
        // empty password fields, and re-sending that list would otherwise wipe
        // working credentials while answering "ok".
        String prevSsid[MAX_NETWORKS], prevPsk[MAX_NETWORKS];
        const int prevCount = networkCount();
        for (int i = 0; i < prevCount && i < MAX_NETWORKS; i++) {
            prevSsid[i] = networkSSID(i);
            prevPsk[i]  = networkPSK(i);
        }

        if (!prefs.begin(NVS_NS, false)) {
            log_e("Provisioning: could not open NVS for writing");
            reply("{\"ok\":false,\"error\":\"nvs open failed\"}");
            return;
        }
        size_t wUid = 0, wCount = 0, wSsid0 = 0, wPsk0 = 0;
        if (haveUid) wUid = prefs.putString("user.id", uid);

        int n = prevCount, kept = 0, blanks = 0;   // unchanged if no list sent
        if (haveNets) {
        n = 0;
        for (JsonObject net : nets) {
            if (n >= MAX_NETWORKS) break;
            const char* ssid = net["ssid"] | "";
            String psk = String(net["psk"] | "");
            if (strlen(ssid) == 0) continue;

            if (psk.length() == 0) {
                // Blank password: reuse the stored one for this SSID if we have
                // it. If we do not, keep it blank -- open networks are a real
                // thing -- but say so in the reply so it is never a surprise.
                bool found = false;
                for (int i = 0; i < prevCount && i < MAX_NETWORKS; i++) {
                    if (prevSsid[i] == ssid) { psk = prevPsk[i]; found = true; break; }
                }
                if (found && psk.length() > 0) kept++;
                else blanks++;
            }

            size_t a = prefs.putString(key("w%d.ssid", n).c_str(), ssid);
            size_t b = prefs.putString(key("w%d.psk",  n).c_str(), psk.c_str());
            if (n == 0) { wSsid0 = a; wPsk0 = b; }
            n++;
        }
        wCount = prefs.putUChar("w.count", (uint8_t)n);
        prefs.putChar("w.last", -1);   // force a fresh scan on the new list
        }
        prefs.end();

        // READ BACK before claiming success. The previous version reported the
        // number of networks it had PARSED, so a completely failed write still
        // answered "2 network(s) written" -- and the next status call then said
        // "0 stored, not provisioned", which is how this bug surfaced.
        const int verified = networkCount();
        const bool idOk = isProvisioned();
        if (verified != n || (haveUid && !idOk)) {
            // Report every write's return value. Guessing at this has already
            // cost several wrong hypotheses; the numbers say which call failed.
            log_e("Provisioning: write did not persist (asked %d, stored %d, id=%s)",
                  n, verified, idOk ? "yes" : "no");
            JsonDocument e;
            e["ok"] = false;
            e["error"] = "write did not persist";
            e["asked"] = n;
            e["stored"] = verified;
            e["idStored"] = idOk;
            e["wroteUserId"] = wUid;      // bytes written, 0 = the put failed
            e["wroteCount"] = wCount;
            e["wroteSsid0"] = wSsid0;
            e["wrotePsk0"] = wPsk0;
            e["reopenRO"] = prefs.begin(NVS_NS, true);   // can we even read it back?
            e["countRaw"] = prefs.getUChar("w.count", 255);
            e["ssid0Raw"] = prefs.getString(key("w%d.ssid", 0).c_str(), "<none>");
            prefs.end();
            String out; serializeJson(e, out); reply(out);
            return;
        }
        log_i("Provisioning: stored and verified %d network(s), %d password(s) kept from device, %d left blank",
              verified, kept, blanks);
        String s = String("{\"ok\":true,\"networks\":") + verified
                 + ",\"keptPasswords\":" + kept
                 + ",\"blankPasswords\":" + blanks + "}";
        reply(s);
        return;
    }

    reply("{\"ok\":false,\"error\":\"unknown cmd\"}");
}

class WriteCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        // Runs on the BLE task: reassemble only, never process. See g_pending.
        // getValue() returns std::string on Arduino 2.x and String on 3.x, and
        // this file is compiled by BOTH firmwares. length()/operator[] is the
        // interface they share, so index rather than range-for.
        auto v = c->getValue();
        const size_t vlen = v.length();
        for (size_t vi = 0; vi < vlen; ++vi) {
            const char ch = v[vi];
            if (ch == '\n') {
                if (g_rx.length()) {
                    if (!pendingFull()) {
                        g_pending[g_pendTail] = g_rx;
                        g_pendTail = (g_pendTail + 1) % PENDING_MAX;
                    } else {
                        // Never drop in silence -- say so, so an overrun is
                        // visible instead of looking like a dead device.
                        g_overflowed = true;
                    }
                }
                g_rx = "";
            } else if (g_rx.length() < 2048) {
                g_rx += ch;
            }
        }
    }
};

class ServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer* s, esp_ble_gatts_cb_param_t* param) override {
        g_mtu = 23;   // reset; the central renegotiates per connection
        log_i("Provisioning: central connected");
    }
    void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
        g_mtu = param->mtu.mtu;
        log_i("Provisioning: MTU negotiated to %u (%u payload bytes per notify)",
              (unsigned)g_mtu, (unsigned)(g_mtu - 3));
    }
    void onConnect(BLEServer*) override { log_i("Provisioning: central connected"); }
    void onDisconnect(BLEServer* s) override {
        log_i("Provisioning: central disconnected");
        if (g_windowOpen) s->startAdvertising();  // stay reachable until the window closes
    }
};

} // namespace

void begin()
{
    // Initialise NVS here rather than assuming the caller did.
    //
    // The M5Paper does this in systeminit.cpp; the Watchy firmware never did,
    // so Preferences::begin() failed there and EVERY WRITE SILENTLY DID
    // NOTHING -- provisioning reported success and stored nothing. Doing it in
    // the shared module means both devices get it, which is the point of the
    // module being shared.
    esp_err_t nvsErr = nvs_flash_init();
    g_nvsInitErr = nvsErr;
    if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        log_w("Provisioning: NVS needs erasing (%s); erasing and retrying", esp_err_to_name(nvsErr));
        nvs_flash_erase();
        nvsErr = nvs_flash_init();
        g_nvsInitErr = nvsErr;
    }
    if (nvsErr != ESP_OK) {
        log_e("Provisioning: nvs_flash_init failed: %s -- credentials CANNOT be stored",
              esp_err_to_name(nvsErr));
    }

    prefs.begin(NVS_NS, true);
    int n = prefs.getUChar("w.count", 0);
    prefs.end();
    log_i("Provisioning: %d stored network(s), provisioned=%s",
          n, isProvisioned() ? "yes" : "no");

    // Build the BLE stack HERE, at boot, and never again.
    //
    // It used to be created lazily inside openWindow(), i.e. from loop() on a
    // button press -- after the renderer, the display list and GxEPD2 had all
    // allocated. BLEDevice::init() wants a large contiguous block, and on the
    // Watchy (~300KB heap, no PSRAM, fragmented by then) that crashed the
    // firmware. Doing it at boot, while the heap is clean and contiguous, is
    // both cheaper and far more predictable.
    //
    // The cost is the stack's memory for the life of the device rather than
    // only during a window. Measured at +16KB static on the Watchy, which is
    // affordable; a crash is not.
    log_i("Provisioning: free heap before BLE init: %u (largest block %u)",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    BLEDevice::init("MicroPatterns");
    // Ask for a large MTU. The central decides the final value; we only ever
    // send what the negotiated MTU allows.
    BLEDevice::setMTU(185);
    g_server = BLEDevice::createServer();
    g_server->setCallbacks(new ServerCB());
    BLEService* svc = g_server->createService(SERVICE_UUID);

    BLECharacteristic* w = svc->createCharacteristic(
        CHAR_WRITE_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    w->setCallbacks(new WriteCB());

    g_notify = svc->createCharacteristic(
        CHAR_NOTIFY_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    g_notify->addDescriptor(new BLE2902());

    svc->start();
    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->setScanResponse(true);
    g_bleInited = true;

    // Built but SILENT. Nothing is advertised until a button press.
    log_i("Provisioning: BLE ready (not advertising), free heap now %u", ESP.getFreeHeap());
}

void openWindow(uint32_t ms)
{
    if (!g_bleInited) {
        log_w("Provisioning: BLE not initialised; begin() was not called");
        return;
    }
    // Advertising only -- the stack was built at boot. Absolute deadline,
    // deliberately REPLACED on each call rather than added to, so repeated
    // presses end the window 20s after the LAST press and never later.
    const bool wasOpen = g_windowOpen;
    BLEDevice::startAdvertising();
    g_windowOpen = true;
    g_windowEndsAt = millis() + ms;
    log_i("Provisioning: window %s, closes in %lus",
          wasOpen ? "EXTENDED" : "OPEN", (unsigned long)(ms / 1000));
}

void closeWindow()
{
    if (!g_windowOpen) return;
    BLEDevice::stopAdvertising();
    g_windowOpen = false;
    g_rx = "";
    // The stack stays built. Tearing it down and rebuilding it per window is
    // what caused the crash described in begin(); the radio is idle when not
    // advertising, and BLE and WiFi coexist on this controller.
    log_i("Provisioning: window closed (BLE idle, still initialised)");
}

bool windowOpen() { return g_windowOpen; }

void tick()
{
    // Process at most one command per call, on the CALLER'S task (the main loop
    // on the Watchy, MainControlTask on the M5Paper) rather than on the BLE
    // stack's task. Everything slow -- JSON, NVS, chunked notifies -- happens
    // here where blocking is safe.
    // Drain the whole queue, not one entry: several commands can arrive while a
    // render is in progress.
    while (!pendingEmpty()) {
        String line = g_pending[g_pendHead];
        g_pending[g_pendHead] = "";
        g_pendHead = (g_pendHead + 1) % PENDING_MAX;
        handleCommand(line);
    }
    if (g_overflowed) {
        g_overflowed = false;
        reply("{\"ok\":false,\"error\":\"command queue overflow; send them slower\"}");
    }

    if (g_windowOpen && (long)(millis() - g_windowEndsAt) >= 0) closeWindow();
}

bool isProvisioned()
{
    prefs.begin(NVS_NS, true);
    bool has = prefs.isKey("user.id");
    prefs.end();
    return has;
}

String userId()
{
    prefs.begin(NVS_NS, true);
    String v = prefs.getString("user.id", "");
    prefs.end();
    return v;
}

int networkCount()
{
    prefs.begin(NVS_NS, true);
    int n = prefs.getUChar("w.count", 0);
    prefs.end();
    return n;
}

String networkSSID(int i)
{
    if (i < 0 || i >= MAX_NETWORKS) return "";
    prefs.begin(NVS_NS, true);
    String v = prefs.getString(key("w%d.ssid", i).c_str(), "");
    prefs.end();
    return v;
}

String networkPSK(int i)
{
    if (i < 0 || i >= MAX_NETWORKS) return "";
    prefs.begin(NVS_NS, true);
    String v = prefs.getString(key("w%d.psk", i).c_str(), "");
    prefs.end();
    return v;
}

int lastGoodIndex()
{
    prefs.begin(NVS_NS, true);
    int v = prefs.getChar("w.last", -1);
    prefs.end();
    return v;
}

void setLastGood(int index)
{
    prefs.begin(NVS_NS, false);
    prefs.putChar("w.last", (int8_t)index);
    prefs.end();
}

} // namespace MPProvisioning

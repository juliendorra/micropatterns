#include "mp_provisioning.h"

#include <Preferences.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include "esp32-hal-log.h"

namespace MPProvisioning {
namespace {

Preferences prefs;
const char* NVS_NS = "mpprov";

BLEServer*         g_server = nullptr;
BLECharacteristic* g_notify = nullptr;
bool          g_windowOpen = false;
unsigned long g_windowEndsAt = 0;
bool          g_bleInited = false;

// Reassembly buffer. A 3-network payload exceeds the ~185 byte MTU, so writes
// arrive in pieces; a newline terminates the message.
String g_rx;

String key(const char* fmt, int i) { char b[24]; snprintf(b, sizeof(b), fmt, i); return String(b); }

void reply(const String& json)
{
    if (!g_notify) return;
    // Chunked to stay under the negotiated MTU. 180 leaves room for ATT
    // overhead; the editor reassembles on the newline we append at the end.
    const size_t CHUNK = 180;
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

    if (strcmp(cmd, "forget") == 0) {
        prefs.begin(NVS_NS, false);
        prefs.clear();
        prefs.end();
        log_w("Provisioning: credentials wiped");
        reply("{\"ok\":true,\"forgotten\":true}");
        return;
    }

    if (strcmp(cmd, "provision") == 0) {
        const char* uid = doc["userId"] | "";
        JsonArray nets = doc["networks"].as<JsonArray>();
        if (strlen(uid) == 0 || nets.isNull()) {
            reply("{\"ok\":false,\"error\":\"userId and networks required\"}");
            return;
        }
        prefs.begin(NVS_NS, false);
        prefs.putString("user.id", uid);
        int n = 0;
        for (JsonObject net : nets) {
            if (n >= MAX_NETWORKS) break;
            const char* ssid = net["ssid"] | "";
            const char* psk  = net["psk"]  | "";
            if (strlen(ssid) == 0) continue;
            prefs.putString(key("w%d.ssid", n).c_str(), ssid);
            prefs.putString(key("w%d.psk",  n).c_str(), psk);
            n++;
        }
        prefs.putUChar("w.count", (uint8_t)n);
        prefs.putChar("w.last", -1);   // force a fresh scan on the new list
        prefs.end();
        log_i("Provisioning: stored %d network(s) and a user ID", n);
        String s = String("{\"ok\":true,\"networks\":") + n + "}";
        reply(s);
        return;
    }

    reply("{\"ok\":false,\"error\":\"unknown cmd\"}");
}

class WriteCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        std::string v = c->getValue();
        for (char ch : v) {
            if (ch == '\n') { String line = g_rx; g_rx = ""; if (line.length()) handleCommand(line); }
            else if (g_rx.length() < 2048) g_rx += ch;
        }
    }
};

class ServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer*) override { log_i("Provisioning: central connected"); }
    void onDisconnect(BLEServer* s) override {
        log_i("Provisioning: central disconnected");
        if (g_windowOpen) s->startAdvertising();  // stay reachable until the window closes
    }
};

} // namespace

void begin()
{
    prefs.begin(NVS_NS, true);
    int n = prefs.getUChar("w.count", 0);
    prefs.end();
    log_i("Provisioning: %d stored network(s), provisioned=%s",
          n, isProvisioned() ? "yes" : "no");
}

void openWindow(uint32_t ms)
{
    if (!g_bleInited) {
        BLEDevice::init("MicroPatterns");
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
    }
    // Absolute deadline, deliberately REPLACED on each call rather than added
    // to: pressing five buttons must not leave the radio up for five windows.
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

    // Tear the stack down rather than leaving it idle. Two reasons: BLE and
    // WiFi share the 2.4GHz radio and the M5Paper needs WiFi immediately after
    // provisioning, and the BLE stack's RUNTIME heap use (beyond the +16KB
    // static measured) matters on the Watchy's ~300KB heap with no PSRAM.
    // Provisioning is rare; paying the init cost per window is the right trade.
    BLEDevice::deinit(true);
    g_bleInited = false;
    g_server = nullptr;
    g_notify = nullptr;
    g_rx = "";
    log_i("Provisioning: window closed, BLE torn down");
}

bool windowOpen() { return g_windowOpen; }

void tick()
{
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

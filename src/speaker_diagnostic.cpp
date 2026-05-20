// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Diagnostic Snapshot Implementation

#include "speaker_diagnostic.h"
#include "config.h"
#include "speaker_inventory.h"
#include "speaker_telnet.h"
#include "version.h"
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>

namespace bosefix {

namespace {

constexpr const char* SNAPSHOT_DIR  = "/snapshots";
constexpr size_t      MAX_BODY_BYTES = 8192;

struct Endpoint { const char* key; const char* path; };

// Reihenfolge bewusst: /info zuerst, weil das der einzige Pflicht-Endpoint ist.
// Wenn der schon fehlschlaegt, ist der Speaker offline und der Rest macht keinen
// Sinn. /presets + /sources sind die fuer Source-Type-Reverse-Engineering
// wichtigsten Felder.
const Endpoint ENDPOINTS[] = {
    { "info",              "/info"              },
    { "presets",           "/presets"           },
    { "sources",           "/sources"           },
    { "now_playing",       "/now_playing"       },
    { "getZone",           "/getZone"           },
    { "getGroup",          "/getGroup"          },
    { "listMediaServers",  "/listMediaServers"  },
    { "sourceServiceList", "/sourceServiceList" },
};

bool fetchOne_(const String& ip, const char* path, int& httpCodeOut, String& bodyOut) {
    HTTPClient http;
    http.setReuse(false);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + path;
    http.setConnectTimeout(2000);
    http.setTimeout(3000);
    if (!http.begin(url)) {
        httpCodeOut = 0;
        return false;
    }
    int code = http.GET();
    httpCodeOut = code;
    if (code != 200) {
        http.end();
        return false;
    }
    bodyOut = http.getString();
    http.end();
    if (bodyOut.length() > MAX_BODY_BYTES) {
        bodyOut.remove(MAX_BODY_BYTES);
        bodyOut += "\n<!-- truncated by BoseFix32 diagnostic capture -->";
    }
    return true;
}

void ensureSnapshotDir_() {
    if (!LittleFS.exists(SNAPSHOT_DIR)) {
        LittleFS.mkdir(SNAPSHOT_DIR);
    }
}

String snapshotPath_(const String& deviceId) {
    String p = SNAPSHOT_DIR;
    p += "/";
    p += deviceId;
    p += ".json";
    return p;
}

} // namespace

bool captureLiveSnapshot(const String& deviceId, JsonDocument& out) {
    auto& inv = SpeakerInventory::instance();
    String ip, name, model, firmware, statusStr;
    bool ownedByUs = false;
    String cloudUrl;
    {
        SpeakerInventory::LockGuard g(inv);
        auto* sp = inv.findById(deviceId);
        if (!sp) return false;
        ip        = sp->ip;
        name      = sp->name;
        model     = sp->model;
        firmware  = sp->firmware;
        statusStr = migrationStatusToStr(sp->status);
        ownedByUs = sp->ownedByUs;
        cloudUrl  = sp->cloudUrl;
    }
    if (ip.length() == 0) return false;

    out["bosefix_version"]  = FW_VERSION_STRING;
    out["bosefix_build"]    = FW_BUILD_DATE;
    out["captured_at_ms"]   = (uint32_t)millis();
    out["esp_base_url"]     = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);

    JsonObject sp = out["speaker"].to<JsonObject>();
    sp["device_id"]         = deviceId;
    sp["ip"]                = ip;
    sp["name"]              = name;
    sp["model"]             = model;
    sp["firmware"]          = firmware;
    sp["status_at_capture"] = statusStr;
    sp["owned_by_us"]       = ownedByUs;
    sp["cloud_url"]         = cloudUrl;

    JsonObject bmx = out["bmx"].to<JsonObject>();
    bool infoOk = false;
    for (auto& e : ENDPOINTS) {
        int code = 0; String body;
        bool ok = fetchOne_(ip, e.path, code, body);
        JsonObject entry = bmx[e.key].to<JsonObject>();
        entry["http"] = code;
        entry["ok"]   = ok;
        entry["body"] = body;
        if (strcmp(e.key, "info") == 0 && ok) infoOk = true;
        delay(50);
    }

    JsonObject telnet = out["telnet"].to<JsonObject>();
    JsonObject tcfg   = telnet["getpdo_currentsystemconfiguration"].to<JsonObject>();
    String cfg;
    bool tok = captureSysConfigurationList(ip, cfg);
    tcfg["ok"]   = tok;
    tcfg["body"] = cfg;

    Serial.printf("[diag] snapshot %s ip=%s info=%d telnet=%d size~%u\n",
                  deviceId.c_str(), ip.c_str(), (int)infoOk, (int)tok,
                  (unsigned)out.memoryUsage());
    return infoOk;
}

void persistPreMigrateSnapshot(const String& deviceId, bool force) {
    if (deviceId.length() == 0) return;
    ensureSnapshotDir_();
    String path = snapshotPath_(deviceId);
    if (!force && LittleFS.exists(path)) {
        Serial.printf("[diag] pre-migrate snapshot for %s already exists — skip\n",
                      deviceId.c_str());
        return;
    }
    JsonDocument doc;
    if (!captureLiveSnapshot(deviceId, doc)) {
        Serial.printf("[diag] capture failed for %s — no snapshot written\n",
                      deviceId.c_str());
        return;
    }
    doc["snapshot_kind"] = force ? "manual" : "pre_migrate";
    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf("[diag] open(%s, w) failed\n", path.c_str());
        return;
    }
    size_t n = serializeJson(doc, f);
    f.close();
    Serial.printf("[diag] persisted %s (%u bytes)\n", path.c_str(), (unsigned)n);
}

bool loadStoredSnapshot(const String& deviceId, String& outJson) {
    String path = snapshotPath_(deviceId);
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    outJson = f.readString();
    f.close();
    return outJson.length() > 0;
}

bool hasStoredSnapshot(const String& deviceId) {
    return LittleFS.exists(snapshotPath_(deviceId));
}

} // namespace bosefix

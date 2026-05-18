// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Auto-Mode (Zero-Touch Migration + Preset-Restore)

#include "auto_mode.h"
#include "config.h"
#include "nvs_helper.h"
#include "preset_store.h"
#include "source_normalizer.h"
#include "speaker_inventory.h"
#include "speaker_telnet.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace bosefix {

namespace {

constexpr const char* NVS_NS  = "bosefix-auto";
constexpr const char* NVS_KEY = "config";

AutoModeStatus    g_status;
SemaphoreHandle_t g_statusMx   = nullptr;
volatile bool     g_taskSpawned = false;

void initMutex_() {
    if (!g_statusMx) g_statusMx = xSemaphoreCreateMutex();
}

// Mutex-Guard im Mini-Format. Lock/unlock im Block-Scope.
struct Lock_ {
    Lock_()  { if (g_statusMx) xSemaphoreTake(g_statusMx, portMAX_DELAY); }
    ~Lock_() { if (g_statusMx) xSemaphoreGive(g_statusMx); }
};

void setState_(const String& s) {
    {
        Lock_ l;
        g_status.state = s;
    }
    Serial.printf("[auto] state=%s\n", s.c_str());
}

void setError_(const String& s) {
    {
        Lock_ l;
        g_status.lastError = s;
    }
    Serial.printf("[auto] ERROR %s\n", s.c_str());
}

String myBase_() {
    return "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
}

// Mini-XML-Helper (lokal, vermeidet Cross-Datei-Coupling mit api_endpoints).
String xmlAttr_(const String& xml, int beg, int end, const char* attr) {
    String needle = String(attr) + "=\"";
    int s = xml.indexOf(needle, beg);
    if (s < 0 || s > end) return "";
    s += needle.length();
    int e = xml.indexOf('"', s);
    if (e < 0 || e > end) return "";
    return xml.substring(s, e);
}

String xmlTag_(const String& xml, int beg, int end, const char* tag) {
    String open  = String("<")  + tag + ">";
    String close = String("</") + tag + ">";
    int s = xml.indexOf(open, beg);
    if (s < 0 || s > end) return "";
    s += open.length();
    int e = xml.indexOf(close, s);
    if (e < 0 || e > end) return "";
    return xml.substring(s, e);
}

// Holt /presets vom Speaker, normalisiert die Slots, schreibt in PresetStore.
// Liefert (converted, abandoned, totalParsed). Returns false bei HTTP-Fehler.
bool importAndNormalizePresets_(const Speaker& s,
                                int& converted, int& abandoned, int& total) {
    HTTPClient http;
    http.setConnectTimeout(2000); http.setTimeout(4000);
    String url = "http://" + s.ip + ":" + String(BOSE_BMX_PORT) + "/presets";
    if (!http.begin(url)) return false;
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String xml = http.getString();
    http.end();

    int pos = 0;
    while (true) {
        int presetOpen = xml.indexOf("<preset id=\"", pos);
        if (presetOpen < 0) break;
        int idStart = presetOpen + 12;
        int idEnd   = xml.indexOf('"', idStart);
        if (idEnd < 0) break;
        int presetClose = xml.indexOf("</preset>", idEnd);
        if (presetClose < 0) break;

        uint8_t slot = xml.substring(idStart, idEnd).toInt();
        if (slot < 1 || slot > 6) { pos = presetClose; continue; }
        ++total;

        String src  = xmlAttr_(xml, idEnd, presetClose, "source");
        String loc  = xmlAttr_(xml, idEnd, presetClose, "location");
        String name = xmlTag_ (xml, idEnd, presetClose, "itemName");
        String img  = xmlTag_ (xml, idEnd, presetClose, "containerArt");

        Preset p;
        p.slot = slot;
        auto nr = normalizePreset(src, loc, name, img, p);
        Serial.printf("[auto]   slot %u source=%s → %s (%s)\n",
                      slot, src.c_str(),
                      normalizeStatusToStr(nr.status), nr.reason.c_str());

        if (nr.status == NormalizeStatus::ABANDONED) {
            ++abandoned;
        } else {
            if (nr.status == NormalizeStatus::OK_CONVERTED) ++converted;
            PresetStore::instance().set(s.deviceId, p);
        }
        pos = presetClose;
    }
    return true;
}

// Wartet bis Speaker nach Reboot wieder /info beantwortet.
bool waitForSpeakerBack_(const String& ip, uint32_t timeoutMs) {
    uint32_t start = millis();
    // erste 30 s: Speaker reboot't gerade — Probes sind aussichtslos.
    delay(30000);
    while (millis() - start < timeoutMs) {
        HTTPClient http;
        http.setConnectTimeout(1500); http.setTimeout(2000);
        String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/info";
        if (http.begin(url)) {
            int code = http.GET();
            http.end();
            if (code == 200) return true;
        }
        delay(5000);
    }
    return false;
}

bool isEligible_(const Speaker& s, const String& myBase) {
    if (s.ownedByUs) return false;
    if (s.cloudUrl == myBase) return false;
    if (s.ip.length() == 0) return false;
    // Modell-Whitelist — auto-mode nur fuer Geraete-Familie, an der wir
    // den Telnet-Pfad empirisch verifiziert haben.
    bool modelOk = s.model.indexOf("SoundTouch 10") >= 0
                || s.model.indexOf("SoundTouch 20") >= 0
                || s.model.indexOf("SoundTouch 30") >= 0;
    if (!modelOk) return false;
    // FW-Whitelist: 27.0.6.x (an Kueche empirisch verifiziert) und
    // 27.0.3.x (an Emma noch NICHT verifiziert — Annahme: gleiche
    // Diag-Shell-Syntax wie 27.0.6, der Speaker stammt aus derselben
    // Generations-Familie sm2/mojo).
    bool fwOk = s.firmware.indexOf("27.0.6.") >= 0
             || s.firmware.indexOf("27.0.3.") >= 0;
    if (!fwOk) return false;
    return true;
}

void migrateOne_(Speaker& live) {
    {
        Lock_ l;
        g_status.currentDeviceId = live.deviceId;
    }
    Serial.printf("[auto] === migrate %s (%s @ %s) ===\n",
                  live.name.c_str(), live.deviceId.c_str(), live.ip.c_str());

    setState_("import-presets");
    int converted = 0, abandoned = 0, total = 0;
    if (!importAndNormalizePresets_(live, converted, abandoned, total)) {
        setError_("preset import failed for " + live.deviceId);
        return;
    }
    {
        Lock_ l;
        g_status.slotsNormalized += (total - abandoned);
        g_status.slotsConverted  += converted;
        g_status.slotsAbandoned  += abandoned;
    }
    if (total == 0) {
        setError_("speaker " + live.deviceId + " has no readable presets — skip");
        return;
    }
    if (total - abandoned == 0) {
        setError_("speaker " + live.deviceId + " has only unsupported sources — skip");
        return;
    }

    setState_("migrate-telnet");
    auto base = myBase_();
    auto r = migrateSpeaker(live.ip, base);
    if (!r.ok) {
        setError_("telnet migration failed: " + r.message);
        return;
    }

    setState_("wait-reboot");
    if (!waitForSpeakerBack_(live.ip, 180000)) {
        setError_("speaker " + live.ip + " did not come back within 180s");
        return;
    }

    live.status    = MigrationStatus::MIGRATED;
    live.cloudUrl  = base;
    live.ownedByUs = true;
    SpeakerInventory::instance().saveToNVS();

    {
        Lock_ l;
        g_status.speakersMigrated++;
        g_status.currentDeviceId = "";
    }
    Serial.printf("[auto] === %s migrated → %s ===\n",
                  live.deviceId.c_str(), base.c_str());
}

void autoModeTask_(void* /*arg*/) {
    AutoModeConfig cfg = loadAutoModeConfig();
    if (!cfg.enabled) {
        Serial.println("[auto] disabled (NVS auto_migrate_on_boot=false) — exit");
        vTaskDelete(nullptr);
        return;
    }
    Serial.printf("[auto] enabled  bootDelayMs=%u  dryRun=%d  maxPerBoot=%u\n",
                  cfg.bootDelayMs, (int)cfg.dryRun, cfg.maxPerBoot);

    delay(cfg.bootDelayMs);
    if (WiFi.status() != WL_CONNECTED) {
        setError_("WiFi not up after boot delay — abort");
        vTaskDelete(nullptr);
        return;
    }

    {
        Lock_ l;
        g_status.running   = true;
        g_status.startedMs = millis();
    }

    setState_("discovering");
    SpeakerInventory::instance().discover();
    uint32_t waitStart = millis();
    while (SpeakerInventory::instance().isScanRunning()
           && millis() - waitStart < 60000) {
        delay(1000);
    }

    auto snapshot = SpeakerInventory::instance().list();
    String base = myBase_();
    int eligible = 0;
    for (auto& s : snapshot) if (isEligible_(s, base)) ++eligible;
    {
        Lock_ l;
        g_status.speakersSeen     = (int)snapshot.size();
        g_status.speakersEligible = eligible;
    }
    Serial.printf("[auto] discovery done: %d speakers total, %d eligible\n",
                  (int)snapshot.size(), eligible);

    if (cfg.dryRun) {
        Serial.println("[auto] dryRun=true — keine echte Migration");
        setState_("done-dryrun");
    } else if (eligible == 0) {
        setState_("done-nothing-to-do");
    } else {
        uint32_t doneThisBoot = 0;
        for (auto& snap : snapshot) {
            auto* live = SpeakerInventory::instance().findById(snap.deviceId);
            if (!live) continue;
            if (!isEligible_(*live, base)) continue;
            if (doneThisBoot >= cfg.maxPerBoot) {
                Serial.printf("[auto] maxPerBoot=%u reached — stop\n",
                              cfg.maxPerBoot);
                break;
            }
            migrateOne_(*live);
            ++doneThisBoot;
        }
        setState_("done");
    }

    {
        Lock_ l;
        g_status.ran             = true;
        g_status.running         = false;
        g_status.finishedMs      = millis();
        g_status.currentDeviceId = "";
    }
    Serial.printf("[auto] finished. migrated=%d converted=%d abandoned=%d\n",
                  g_status.speakersMigrated, g_status.slotsConverted,
                  g_status.slotsAbandoned);
    vTaskDelete(nullptr);
}

} // anon

AutoModeConfig loadAutoModeConfig() {
    AutoModeConfig c;
    JsonDocument doc;
    if (nvsLoadJson(NVS_NS, NVS_KEY, doc)) {
        // Defensiv: bei vorhandenem NVS-Record aber fehlenden Keys gelten
        // die Image-Defaults aus AutoModeConfig (enabled=true, maxPerBoot=4).
        c.enabled     = doc["enabled"]      | true;
        c.dryRun      = doc["dry_run"]      | false;
        c.bootDelayMs = doc["boot_delay_ms"]| (uint32_t)10000;
        c.maxPerBoot  = doc["max_per_boot"] | (uint32_t)4;
    }
    return c;
}

void saveAutoModeConfig(const AutoModeConfig& cfg) {
    JsonDocument doc;
    doc["enabled"]       = cfg.enabled;
    doc["dry_run"]       = cfg.dryRun;
    doc["boot_delay_ms"] = cfg.bootDelayMs;
    doc["max_per_boot"]  = cfg.maxPerBoot;
    nvsSaveJson(NVS_NS, NVS_KEY, doc);
}

void startAutoModeTask() {
    initMutex_();
    if (g_taskSpawned) return;
    g_taskSpawned = true;
    xTaskCreate(autoModeTask_, "auto-mode", 8192, nullptr,
                tskIDLE_PRIORITY + 1, nullptr);
}

AutoModeStatus getAutoModeStatus() {
    initMutex_();
    Lock_ l;
    return g_status;
}

} // namespace bosefix

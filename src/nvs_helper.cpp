// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "nvs_helper.h"
#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <memory>
#include <vector>

namespace sixback {

bool nvsLoadJson(const char* ns, const char* key, JsonDocument& doc) {
    Preferences p;
    if (!p.begin(ns, true)) return false;
    // Seit dem Blob-Umbau (2026-06-07) liegen JSON-Payloads als NVS-BLOB —
    // nvs_set_blob ist ueber Pages gechunkt, das harte 4000-B-Limit von
    // nvs_set_str entfaellt. Bestands-Daten (<= v0.8.14) liegen noch als
    // STRING unter demselben Key -> Fallback; der naechste Save ersetzt den
    // Eintrag typ-wechselnd durch ein Blob (NVS: neuer Wert ersetzt Typ+Wert).
    size_t blen = p.getBytesLength(key);
    if (blen > 0) {
        std::unique_ptr<char[]> buf(new (std::nothrow) char[blen + 1]);
        if (!buf) { p.end(); return false; }
        size_t got = p.getBytes(key, buf.get(), blen);
        p.end();
        if (got != blen) return false;
        buf[blen] = '\0';
        // const char* erzwingt Copy-Mode in ArduinoJson — mit char* wuerde
        // zero-copy in den gleich freigegebenen Buffer zeigen.
        return deserializeJson(doc, (const char*)buf.get())
               == DeserializationError::Ok;
    }
    String s = p.getString(key, "");
    p.end();
    if (s.length() == 0) return false;
    return deserializeJson(doc, s) == DeserializationError::Ok;
}

bool nvsSaveJson(const char* ns, const char* key, JsonDocument& doc) {
    String s;
    serializeJson(doc, s);
    if (s.length() == 0) return false;
    Preferences p;
    if (!p.begin(ns, false)) {
        Serial.printf("[nvs-save] FAIL begin ns=%s key=%s\n", ns, key);
        return false;
    }
    // BLOB statt String (Lab-Befund 2026-06-07): nvs_set_str kann max
    // 4000 B inkl. NUL und braucht die Entries zusammenhaengend in EINER
    // Page — der Preset-Store ueberschritt das ab ~5 Speakern und JEDER
    // Save schlug fehl, obwohl die Partition zu 2/3 leer war. nvs_set_blob
    // chunkt ueber Pages; Limit jetzt ~Partitionsgroesse.
    size_t n = p.putBytes(key, s.c_str(), s.length());
    p.end();
    if (n != s.length()) {
        Serial.printf("[nvs-save] FAIL putBytes ns=%s key=%s json_len=%u wrote=%u\n",
                      ns, key, (unsigned)s.length(), (unsigned)n);
        return false;
    }
    Serial.printf("[nvs-save] ok ns=%s key=%s json_len=%u (blob)\n",
                  ns, key, (unsigned)s.length());
    return true;
}

bool nvsErase(const char* ns, const char* key) {
    Preferences p;
    if (!p.begin(ns, false)) return false;
    bool ok = p.remove(key);
    p.end();
    return ok;
}

bool nvsEraseAllInNamespace(const char* ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_erase_all(h);
    esp_err_t e2 = nvs_commit(h);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK;
}

void nvsGetStatsJson(JsonDocument& out) {
    nvs_stats_t st{};
    esp_err_t e = nvs_get_stats(NULL, &st);
    if (e != ESP_OK) {
        out["error"] = "nvs_get_stats failed";
        return;
    }
    out["used_entries"]      = (uint32_t)st.used_entries;
    out["free_entries"]      = (uint32_t)st.free_entries;
    out["total_entries"]     = (uint32_t)st.total_entries;
    out["namespace_count"]   = (uint32_t)st.namespace_count;
    out["percent_used"]      = (st.total_entries > 0)
                                ? (100.0f * st.used_entries / st.total_entries)
                                : 0.0f;
}

bool nvsSaveJsonWithCleanup(const char* ns, const char* key, JsonDocument& doc) {
    if (nvsSaveJson(ns, key, doc)) return true;
    Serial.printf("[nvs-cleanup] %s/%s save fail — pass1 purging caches + retry\n", ns, key);
    // Pass 1: Cache-Namespaces wegpurgen (regenerable, kein User-Verlust).
    nvsEraseAllInNamespace("sixback-tune");      // TuneIn-Resolver-Cache
    nvsEraseAllInNamespace("sixback-sys");       // Health/Counters
    if (nvsSaveJson(ns, key, doc)) {
        Serial.printf("[nvs-cleanup] pass1 retry %s/%s -> OK\n", ns, key);
        return true;
    }
    // Pass 2: aggressivere Liste — Snapshot-Persistenz, Keepalive-State.
    // Beides regenerable beim naechsten Pre-Migrate / Boot. Loescht nicht
    // sixback-{pre,inv,spot,wifi,auto,net} — die haben User-Daten.
    Serial.printf("[nvs-cleanup] %s/%s pass1-fail — pass2 wider purge\n", ns, key);
    nvsEraseAllInNamespace("sixback-snapshot");  // Diag-Snapshots im Flash
    nvsEraseAllInNamespace("sixback-keepalive"); // Marge-Keepalive-State
    nvsEraseAllInNamespace("sixback-c");         // Captive-Stub-State
    nvsEraseAllInNamespace("sixback-s");         // Speaker-Discovery-Cache
    nvsEraseAllInNamespace("sixback-esp");       // ESP-Web-Tools-Cache (falls vorhanden)
    if (nvsSaveJson(ns, key, doc)) {
        Serial.printf("[nvs-cleanup] pass2 retry %s/%s -> OK (aggressive purge)\n", ns, key);
        return true;
    }
    // Pass 3: den ZIEL-Key explizit loeschen + retry. Hilft wenn NVS-internes
    // GC die alte Version noch nicht reclaim't hat.
    //
    // ABER (Lab-Befund 2026-06-07): NIEMALS den letzten guten Stand opfern.
    // Die alte Fassung loeschte den Key bedingungslos — wenn der Neuschreib
    // dann ebenfalls scheiterte (damals: String > 4000 B = IMMER), war der
    // letzte persistierte Stand vernichtet -> Total-Preset-Verlust nach dem
    // naechsten Reboot. Deshalb: alten Wert vorher sichern und bei erneutem
    // Fehlschlag zurueckschreiben (die soeben freigegebenen Entries reichen
    // dafuer sicher aus).
    Serial.printf("[nvs-cleanup] %s/%s pass2-fail — pass3 erase target-key + retry\n", ns, key);
    std::vector<uint8_t> oldBlob;
    String oldStr;
    {
        Preferences p;
        if (p.begin(ns, true)) {
            size_t blen = p.getBytesLength(key);
            if (blen > 0) {
                oldBlob.resize(blen);
                if (p.getBytes(key, oldBlob.data(), blen) != blen) oldBlob.clear();
            } else {
                oldStr = p.getString(key, "");
            }
            p.end();
        }
    }
    {
        Preferences p;
        if (p.begin(ns, false)) { p.remove(key); p.end(); }
    }
    bool ok = nvsSaveJson(ns, key, doc);
    if (!ok && (oldBlob.size() > 0 || oldStr.length() > 0)) {
        Preferences p;
        bool restored = false;
        if (p.begin(ns, false)) {
            if (oldBlob.size() > 0) {
                restored = p.putBytes(key, oldBlob.data(), oldBlob.size())
                           == oldBlob.size();
            } else {
                restored = p.putString(key, oldStr) > 0;
            }
            p.end();
        }
        Serial.printf("[nvs-cleanup] pass3 %s/%s restore alter Stand: %s\n",
                      ns, key, restored ? "OK" : "FAIL — Daten verloren!");
    }
    Serial.printf("[nvs-cleanup] pass3 retry %s/%s -> %s%s\n", ns, key,
                  ok ? "OK" : "STILL-FAIL",
                  ok ? "" : " — NVS partition genuinely full");
    return ok;
}

namespace {

// Liefert true wenn der Namespace mindestens einen Eintrag hat.
bool nvsNamespaceHasEntries(const char* ns) {
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", ns, NVS_TYPE_ANY, &it);
    if (err != ESP_OK || it == nullptr) {
        if (it) nvs_release_iterator(it);
        return false;
    }
    nvs_release_iterator(it);
    return true;
}

// Kopiert einen einzelnen Eintrag basierend auf seinem Typ. Gibt true bei
// Erfolg. Auf Fehler wird der Eintrag uebersprungen und false zurueckgegeben.
bool copyEntry(nvs_handle_t src, nvs_handle_t dst, const nvs_entry_info_t& info) {
    switch (info.type) {
        case NVS_TYPE_U8:  { uint8_t  v; if (nvs_get_u8 (src, info.key, &v) == ESP_OK) return nvs_set_u8 (dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I8:  { int8_t   v; if (nvs_get_i8 (src, info.key, &v) == ESP_OK) return nvs_set_i8 (dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U16: { uint16_t v; if (nvs_get_u16(src, info.key, &v) == ESP_OK) return nvs_set_u16(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I16: { int16_t  v; if (nvs_get_i16(src, info.key, &v) == ESP_OK) return nvs_set_i16(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U32: { uint32_t v; if (nvs_get_u32(src, info.key, &v) == ESP_OK) return nvs_set_u32(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I32: { int32_t  v; if (nvs_get_i32(src, info.key, &v) == ESP_OK) return nvs_set_i32(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U64: { uint64_t v; if (nvs_get_u64(src, info.key, &v) == ESP_OK) return nvs_set_u64(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I64: { int64_t  v; if (nvs_get_i64(src, info.key, &v) == ESP_OK) return nvs_set_i64(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_STR: {
            size_t len = 0;
            if (nvs_get_str(src, info.key, nullptr, &len) != ESP_OK || len == 0) return false;
            char* tmp = (char*)malloc(len);
            if (!tmp) return false;
            bool ok = nvs_get_str(src, info.key, tmp, &len) == ESP_OK
                   && nvs_set_str(dst, info.key, tmp) == ESP_OK;
            free(tmp);
            return ok;
        }
        case NVS_TYPE_BLOB: {
            size_t len = 0;
            if (nvs_get_blob(src, info.key, nullptr, &len) != ESP_OK || len == 0) return false;
            uint8_t* tmp = (uint8_t*)malloc(len);
            if (!tmp) return false;
            bool ok = nvs_get_blob(src, info.key, tmp, &len) == ESP_OK
                   && nvs_set_blob(dst, info.key, tmp, len) == ESP_OK;
            free(tmp);
            return ok;
        }
        default:
            return false;
    }
    return false;
}

} // namespace

bool migrateNvsNamespace(const char* oldNs, const char* newNs) {
    // No-op wenn alte Daten gar nicht da sind.
    if (!nvsNamespaceHasEntries(oldNs)) return true;

    // Idempotent: wenn neue Namespace schon Daten hat, gilt Migration als
    // abgeschlossen. Wir loeschen die alte aber NICHT — Sicherheits-Backup
    // bleibt einen Boot lang erhalten falls beim Schreiben in neue NS was
    // schiefging und der naechste Boot retry will. Erst beim 2. Boot mit
    // sauberer neuer NS wird die alte tot.
    if (nvsNamespaceHasEntries(newNs)) {
        // Beide vorhanden: alte ist Cruft, wegputzen.
        nvs_handle_t h;
        if (nvs_open(oldNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
            Serial.printf("[nvs-migrate] %s already migrated, erased leftover\n", oldNs);
        }
        return true;
    }

    nvs_handle_t src = 0, dst = 0;
    if (nvs_open(oldNs, NVS_READWRITE, &src) != ESP_OK) {
        Serial.printf("[nvs-migrate] open %s failed\n", oldNs);
        return false;
    }
    if (nvs_open(newNs, NVS_READWRITE, &dst) != ESP_OK) {
        Serial.printf("[nvs-migrate] open %s failed\n", newNs);
        nvs_close(src);
        return false;
    }

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", oldNs, NVS_TYPE_ANY, &it);
    int copied = 0, failed = 0;
    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        if (copyEntry(src, dst, info)) {
            copied++;
        } else {
            failed++;
            Serial.printf("[nvs-migrate] copy fail %s/%s (type=%d)\n",
                          oldNs, info.key, (int)info.type);
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);

    if (failed == 0 && copied > 0) {
        nvs_commit(dst);
        nvs_erase_all(src);
        nvs_commit(src);
        Serial.printf("[nvs-migrate] %s -> %s (%d keys)\n", oldNs, newNs, copied);
    } else if (copied > 0) {
        nvs_commit(dst);
        Serial.printf("[nvs-migrate] %s -> %s partial (%d ok, %d fail, OLD KEPT)\n",
                      oldNs, newNs, copied, failed);
    }
    nvs_close(src);
    nvs_close(dst);
    return failed == 0;
}

void migrateAllBosefixNvs() {
    static const struct { const char* oldNs; const char* newNs; } map[] = {
        { "bosefix-wifi", "sixback-wifi" },  // ZUERST: WiFi-Creds entscheiden Live-Status
        { "bosefix-pre",  "sixback-pre"  },  // User-Presets
        { "bosefix-inv",  "sixback-inv"  },  // Speaker-Inventory
        { "bosefix-auto", "sixback-auto" },  // Auto-Mode-Settings
        { "bosefix-net",  "sixback-net"  },  // IP-Failsafe
        { "bosefix-sys",  "sixback-sys"  },  // Health-Counters
        { "bosefix-tune", "sixback-tune" },  // TuneIn-Cache (unkritisch)
    };
    for (auto& m : map) {
        migrateNvsNamespace(m.oldNs, m.newNs);
    }
}

} // namespace sixback

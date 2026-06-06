// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ip_failsafe.h"
#include "config.h"
#include "speaker_inventory.h"
#include "speaker_telnet.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <atomic>

namespace sixback {

namespace {
constexpr const char* NVS_NS  = "sixback-net";
constexpr const char* KEY_IP  = "last_ip";

// Retry-Politik bei teil-fehlgeschlagenem Lauf (Speaker offline waehrend
// des Fensters — typisch Router-Tausch: die Bose booten langsamer als der
// ESP): last_ip wird NICHT advanced, alle RETRY_DELAY_MS kommt ein neuer
// Versuch, nach MAX_RETRIES geben wir auf (sonst probt jeder kuenftige
// Boot ewig einen dauerhaft entsorgten Speaker durch).
constexpr uint32_t RETRY_DELAY_MS = 60UL * 1000UL;
constexpr int      MAX_RETRIES    = 20;            // = ~20 min Fenster

std::atomic<bool>     recheckArmed_{false};   // GOT_IP-Event / Test-Endpoint
std::atomic<bool>     checkRunning_{false};   // genau EIN Lauf gleichzeitig
std::atomic<bool>     retryPending_{false};
std::atomic<uint32_t> nextRetryAtMs_{0};
int                   retriesLeft_ = 0;       // nur im Check-Lauf angefasst

// GET http://<ip>:8090/info → margeURL extrahieren.
// Leerstring bei Timeout / HTTP-Fehler / kein Tag — Caller behandelt das
// wie "unknown" und re-migriert vorsichtshalber.
String fetchMargeUrl_(const String& ip) {
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(1500); http.setTimeout(2500);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/info";
    if (!http.begin(url)) return "";
    int code = http.GET();
    if (code != 200) { http.end(); return ""; }
    String xml = http.getString();
    http.end();
    int b = xml.indexOf("<margeURL>");
    if (b < 0) return "";
    b += 10;  // strlen("<margeURL>")
    int e = xml.indexOf("</margeURL>", b);
    if (e < 0) return "";
    return xml.substring(b, e);
}

void rememberIp(const String& ip) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putString(KEY_IP, ip);
    p.end();
}

// GOT_IP feuert bei jedem (Re-)Connect — auch DHCP-Renewal mit gleicher
// IP. Der Check prueft prev==now selbst und ist dann no-op; hier NUR das
// Flag armen, nie blocken (laeuft im WiFi-Event-Task).
void onGotIp_(arduino_event_id_t) {
    recheckArmed_ = true;
}

void failsafeTask_(void*) {
    ipFailsafeCheck();
    checkRunning_ = false;
    vTaskDelete(nullptr);
}
} // anon

String getLastKnownIp() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) return "";
    String s = p.getString(KEY_IP, "");
    p.end();
    return s;
}

void ipFailsafeArmRecheck() { recheckArmed_ = true; }

void ipFailsafeInit() {
    WiFi.onEvent(onGotIp_, ARDUINO_EVENT_WIFI_STA_GOT_IP);
}

void ipFailsafeCheck() {
    if (WiFi.status() != WL_CONNECTED) {
        // Improv-/Captive-Boot ohne STA-IP: ohne den Guard wuerde "0.0.0.0"
        // als last_ip persistiert. Der GOT_IP-Listener armt den Recheck,
        // sobald die echte IP da ist.
        Serial.println("[failsafe] WiFi not connected — skip");
        return;
    }
    String now = WiFi.localIP().toString();
    String prev = getLastKnownIp();
    String newBase = "http://" + now + ":" + String(BOSE_HTTP_PORT);

    if (prev.length() == 0) {
        Serial.printf("[failsafe] first boot, IP=%s persisted\n", now.c_str());
        rememberIp(now);
        return;
    }
    if (prev == now) {
        retryPending_ = false;
        Serial.printf("[failsafe] IP unchanged (%s) — nothing to do\n", now.c_str());
        return;
    }
    if (!retryPending_) retriesLeft_ = MAX_RETRIES;   // neue Wechsel-Serie
    Serial.printf("[failsafe] IP changed: %s -> %s — checking owned speakers\n",
                  prev.c_str(), now.c_str());
    auto& inv = SpeakerInventory::instance();
    int touched = 0, failed = 0, skipped = 0;
    // Iteriere ueber OwnedByUs (NICHT status), weil Status nach Reboot evtl.
    // noch nicht refreshed ist und ein migrated-Speaker mit veralteter IP
    // bereits 'unknown' aussieht.
    auto speakers = inv.list();
    for (auto& s : speakers) {
        if (!s.ownedByUs) continue;
        // Pre-Probe: zeigt der Speaker LIVE schon auf unsere neue Base?
        // Erspart einen Reboot, der nichts aendert. Macht den Lauf zugleich
        // idempotent — ein Retry fasst nur die vorher gescheiterten an.
        String live = fetchMargeUrl_(s.ip);
        if (live == newBase) {
            Serial.printf("[failsafe]   %s (%s) already on %s — skip\n",
                          s.name.c_str(), s.ip.c_str(), newBase.c_str());
            {
                SpeakerInventory::LockGuard g(inv);
                if (auto* p = inv.findById(s.deviceId)) {
                    p->status   = MigrationStatus::MIGRATED;
                    p->cloudUrl = newBase;
                }
            }
            ++skipped;
            continue;
        }
        Serial.printf("[failsafe]   %s (%s) live=%s — re-migrate to %s ...\n",
                      s.name.c_str(), s.ip.c_str(),
                      live.length() ? live.c_str() : "?(unreachable)",
                      newBase.c_str());
        auto r = migrateSpeaker(s.ip, newBase);
        if (r.ok) {
            ++touched;
            SpeakerInventory::LockGuard g(inv);
            if (auto* p = inv.findById(s.deviceId)) {
                p->status   = MigrationStatus::MIGRATED;
                p->cloudUrl = newBase;
            }
        } else {
            ++failed;
            Serial.printf("[failsafe]     FAILED: %s\n", r.message.c_str());
        }
    }
    inv.saveToNVS();
    Serial.printf("[failsafe] done: %d re-migrated, %d skipped (already-up-to-date), %d failed\n",
                  touched, skipped, failed);
    if (failed == 0) {
        rememberIp(now);
        retryPending_ = false;
    } else if (retriesLeft_ > 0) {
        // last_ip NICHT advancen: der Wechsel gilt erst als verarbeitet,
        // wenn ALLE owned Speaker umgebogen sind. Ein Speaker, der waehrend
        // des Laufs noch bootet (Router-Tausch!), bekommt so einen Retry
        // statt fuer immer auf der toten alten Base zu haengen.
        --retriesLeft_;
        nextRetryAtMs_ = millis() + RETRY_DELAY_MS;
        retryPending_  = true;
        Serial.printf("[failsafe] %d speaker(s) failed — last_ip NOT advanced, "
                      "retry in %lus (%d left)\n",
                      failed, (unsigned long)(RETRY_DELAY_MS / 1000), retriesLeft_);
    } else {
        Serial.printf("[failsafe] giving up after %d retries — %d speaker(s) "
                      "remain on stale base http://%s:%d\n",
                      MAX_RETRIES, failed, prev.c_str(), BOSE_HTTP_PORT);
        rememberIp(now);
        retryPending_ = false;
    }
}

void ipFailsafeTick() {
    bool due = recheckArmed_.load();
    if (!due && retryPending_.load()) {
        due = (int32_t)(millis() - nextRetryAtMs_.load()) >= 0;
    }
    if (!due) return;
    if (checkRunning_.exchange(true)) return;  // Lauf aktiv; Flag bleibt gesetzt
    recheckArmed_ = false;
    // One-Shot-Task: der loopTask ist nach healthInit() WDT-subscribed und
    // die Telnet-Re-Migration blockt sekundenlang. 8192 Stack — HTTP- und
    // Telnet-Probes (Lesson: bg-discover-Stack-Overflow bei 4096).
    if (xTaskCreate(failsafeTask_, "ip-failsafe", 8192, nullptr, 1, nullptr) != pdPASS) {
        checkRunning_ = false;   // naechster Tick versucht es erneut
        recheckArmed_ = true;
    }
}

} // namespace sixback

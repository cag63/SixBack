// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Speaker-Bootstrap via Telnet Port 17000
//
// Implementiert die in RESEARCH.md §12 verifizierte Sequenz:
//   sys configuration bmxRegistryUrl <base>/bmx/registry/v1/services
//   sys configuration statsServerUrl <base>
//   sys configuration margeServerUrl <base>
//   sys configuration swUpdateUrl    <base>/updates/soundtouch
//   envswitch boseurls set <base> <base>/updates/soundtouch
//   getpdo CurrentSystemConfiguration
//   sys reboot
//
// Phase 0: vollständig implementiert für manuellen Aufruf (z.B. aus
// Serial-Console). Phase 3: an Web-UI angebunden.

#include "speaker_telnet.h"
#include "config.h"
#include <WiFi.h>

static bool sendAndExpectOK(WiFiClient& client, const String& cmd,
                             String& reply) {
    client.print(cmd + "\n");
    reply = "";
    uint32_t deadline = millis() + 3000;
    while (millis() < deadline) {
        while (client.available()) {
            char c = client.read();
            reply += c;
            if (reply.endsWith("->")) {
                // Bose-Prompt — Antwort komplett. Negative Marker
                // ueberstimmen alles andere: "Setting 'foo' not found" matchte
                // vorher faelschlich "Setting" als success.
                if (reply.indexOf("not found") >= 0 ||
                    reply.indexOf("Usage:")    >= 0 ||
                    reply.indexOf("usage:")    >= 0 ||
                    reply.indexOf("Error")     >= 0 ||
                    reply.indexOf("Invalid")   >= 0 ||
                    reply.indexOf("syntax")    >= 0) {
                    return false;
                }
                return true;
            }
        }
        delay(20);
    }
    return false;
}

// Liest aus einem getpdo-CurrentSystemConfiguration-Reply den Wert eines
// PDO-Felds ('text: "..."' nach dem Feldnamen) heraus. Liefert "" wenn
// nichts gefunden wurde — passt auch zum getpdo-quirk wo manche Felder
// kurzzeitig fehlen.
static String extractPdoField_(const String& reply, const String& field) {
    int p = reply.indexOf(field);
    if (p < 0) return "";
    int t = reply.indexOf("text:", p);
    if (t < 0) return "";
    int q1 = reply.indexOf('"', t);
    int q2 = (q1 >= 0) ? reply.indexOf('"', q1 + 1) : -1;
    if (q1 < 0 || q2 <= q1) return "";
    return reply.substring(q1 + 1, q2);
}

// Bei marginalem WLAN (Paketverlust) kann ein einzelnes Telnet-Kommando ohne
// Antwort bleiben und die ganze Migration abbrechen (Discussion #28: ESP RSSI
// −86, leere Antwort). Daher die gesamte — idempotente — Kommando-Sequenz
// mehrfach versuchen; erst bei Erfolg getpdo-Verify + reboot. Eine LEERE
// Antwort (0 Bytes) wird als Link-/Paketverlust gemeldet (mit RSSI), nicht als
// vom Speaker abgelehntes Kommando.
static const int TELNET_MIGRATE_ATTEMPTS = 3;

MigrationResult migrateSpeaker(const String& speakerIP,
                                const String& serverBaseUrl) {
    MigrationResult r{false, "", ""};

    struct { const char* cmd_prefix; String value; } commands[5] = {
        { "sys configuration bmxRegistryUrl ", serverBaseUrl + "/bmx/registry/v1/services" },
        { "sys configuration statsServerUrl ", serverBaseUrl },
        { "sys configuration margeServerUrl ", serverBaseUrl },
        { "sys configuration swUpdateUrl ",    serverBaseUrl + "/updates/soundtouch" },
        { "envswitch boseurls set ",           serverBaseUrl + " " + serverBaseUrl + "/updates/soundtouch" },
    };

    String lastErr;
    for (int attempt = 1; attempt <= TELNET_MIGRATE_ATTEMPTS; ++attempt) {
        WiFiClient client;
        if (!client.connect(speakerIP.c_str(), BOSE_TELNET_PORT, 5000)) {
            lastErr = "Telnet-Connect zu " + speakerIP + ":17000 fehlgeschlagen "
                      "(ESP-WLAN RSSI=" + String(WiFi.RSSI()) + " dBm)";
            delay(attempt * 500);
            continue;
        }

        // Banner-Prompt abwarten + leeren
        delay(300);
        while (client.available()) client.read();

        String reply;
        bool seqOk = true;
        for (auto& c : commands) {
            String cmd = String(c.cmd_prefix) + c.value;
            if (!sendAndExpectOK(client, cmd, reply)) {
                if (reply.length() == 0) {
                    // 0 Bytes zurueck = Paketverlust/toter Link, KEIN abgelehntes
                    // Kommando. Den wahrscheinlichsten Hebel (WLAN) direkt nennen.
                    lastErr = "keine Antwort vom Speaker-Telnet auf '" + cmd +
                              "' — Paketverlust? ESP-WLAN RSSI=" +
                              String(WiFi.RSSI()) + " dBm (Stick naeher an Router/AP platzieren)";
                } else {
                    lastErr = "Kommando abgelehnt: " + cmd + "\nAntwort: " + reply;
                }
                seqOk = false;
                break;
            }
        }
        if (!seqOk) {
            client.stop();
            Serial.printf("[telnet] migrate attempt %d/%d failed: %s\n",
                          attempt, TELNET_MIGRATE_ATTEMPTS, lastErr.c_str());
            delay(attempt * 500);  // Backoff; delay() speist auch den Task-WDT
            continue;
        }

        // --- Sequenz erfolgreich: Verifikation + reboot ---
        // getpdo soll zeigen dass margeServerUrl jetzt auf unsere base zeigt.
        // Falls Mismatch: NICHT fail-hard, weil getpdo zeitweise inkonsistent ist
        // (Diag-Shell-Quirk, NVS-Cache-Latenz) — Auto-Claim/Release in
        // refreshMigrationStatus reconcile't das beim naechsten Refresh. Das
        // Mismatch bleibt in r.message (wird unten NICHT mehr ueberschrieben),
        // damit der User im Reply-Toast sieht dass die Verifikation nicht clean war.
        delay(200);  // NVS-write am Speaker abklingen lassen
        if (sendAndExpectOK(client, "getpdo CurrentSystemConfiguration", reply)) {
            r.verifiedConfig = reply;
            String actualMarge = extractPdoField_(reply, "margeServerUrl");
            if (actualMarge.length() > 0 && actualMarge != serverBaseUrl) {
                Serial.printf("[telnet] WARN getpdo margeServerUrl=%s expected=%s "
                              "— auto-claim will reconcile next refresh\n",
                              actualMarge.c_str(), serverBaseUrl.c_str());
                r.message = String("WARN: getpdo margeServerUrl=") + actualMarge +
                            " expected=" + serverBaseUrl + " (auto-claim will fix)";
            }
        }

        client.print("sys reboot\n");
        delay(500);
        client.stop();

        r.ok = true;
        if (r.message.length() == 0)
            r.message = "Migration erfolgreich, Speaker rebooted";
        if (attempt > 1)
            r.message += " (nach " + String(attempt) + " Versuchen)";
        return r;
    }

    r.message = lastErr + " — nach " + String(TELNET_MIGRATE_ATTEMPTS) +
                " Versuchen aufgegeben";
    return r;
}

MigrationResult revertSpeaker(const String& speakerIP) {
    MigrationResult r{false, "", ""};
    WiFiClient c;
    if (!c.connect(speakerIP.c_str(), BOSE_TELNET_PORT, 5000)) {
        r.message = "Telnet-Connect fehlgeschlagen";
        return r;
    }
    delay(300);
    while (c.available()) c.read();
    // Original-Bose-URLs zuruecksetzen (auch wenn Cloud tot - das ist der
    // Werks-Zustand)
    struct { const char* cmd; } commands[5] = {
        { "sys configuration bmxRegistryUrl https://content.api.bose.io/bmx/registry/v1/services" },
        { "sys configuration statsServerUrl https://events.api.bosecm.com" },
        { "sys configuration margeServerUrl https://streaming.bose.com" },
        { "sys configuration swUpdateUrl    https://worldwide.bose.com/updates/soundtouch" },
        { "envswitch boseurls set https://streaming.bose.com https://worldwide.bose.com/updates/soundtouch" },
    };
    String reply;
    for (auto& cmd : commands) {
        if (!sendAndExpectOK(c, cmd.cmd, reply)) {
            r.message = String("Kommando fehlgeschlagen: ") + cmd.cmd + "\n" + reply;
            c.stop();
            return r;
        }
    }
    if (!sendAndExpectOK(c, "getpdo CurrentSystemConfiguration", reply)) {
        r.message = "getpdo fehlgeschlagen";
        c.stop();
        return r;
    }
    r.verifiedConfig = reply;
    c.print("sys reboot\n");
    delay(500);
    c.stop();
    r.ok = true;
    r.message = "Revert erfolgreich, Speaker rebooted";
    return r;
}

bool rebootSpeaker(const String& speakerIP) {
    WiFiClient c;
    if (!c.connect(speakerIP.c_str(), BOSE_TELNET_PORT, 3000)) return false;
    delay(200);
    while (c.available()) c.read();
    c.print("sys reboot\n");
    delay(500);
    c.stop();
    return true;
}

bool captureSysConfigurationList(const String& speakerIP, String& out) {
    out = "";
    WiFiClient c;
    if (!c.connect(speakerIP.c_str(), BOSE_TELNET_PORT, 5000)) return false;
    delay(300);
    while (c.available()) c.read();

    c.print("getpdo CurrentSystemConfiguration\n");
    uint32_t deadline = millis() + 4000;
    while (millis() < deadline) {
        while (c.available()) {
            char ch = c.read();
            out += ch;
            if (out.endsWith("->")) {
                out.remove(out.length() - 2);
                while (out.endsWith("\r") || out.endsWith("\n")) {
                    out.remove(out.length() - 1);
                }
                c.stop();
                return true;
            }
        }
        delay(20);
    }
    c.stop();
    return false;
}

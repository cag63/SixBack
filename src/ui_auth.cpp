// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — optionaler Zugriffsschutz fuer die Web-UI (Issue #31). Siehe ui_auth.h.
#include "ui_auth.h"

#include <ArduinoJson.h>
#include <WebAuthentication.h>   // generateDigestHash() aus ESPAsyncWebServer

#include "nvs_helper.h"
#include "wifi_provisioning.h"   // improvIsActive()

namespace sixback {

namespace {

// Fester Realm — MUSS bei HA1-Berechnung UND Challenge identisch sein, sonst
// matcht der Digest nie (HA1 bindet den Realm ein).
constexpr const char* UI_AUTH_REALM = "SixBack";

constexpr const char* NVS_NS  = "sixback-uiauth";
constexpr const char* NVS_KEY = "config";

// Minimal-Passwortlaenge — sanfter Foot-Gun-Guard, kein Sicherheits-Anspruch.
constexpr size_t MIN_PW_LEN = 4;

// Eine globale, dauerhaft am uiServer haengende Middleware. Im Aus-Zustand
// AUTH_NONE → ein einzelner durchwinkender Aufruf pro Request (vernachlaessigbar),
// dafuer Runtime-Umschaltung ohne Reboot.
AsyncAuthenticationMiddleware g_mw;
bool g_attached = false;

// Das Scharfschalten der Auth wird NICHT synchron im Web-Handler gemacht,
// sondern aus dem loop()-Tick (uiAuthSerialTick). Grund: ESPAsyncWebServer
// evaluiert die Server-Middleware bei body-tragenden Requests NACH dem
// Handler erneut — wuerde der Handler die Auth mitten im Request von NONE auf
// DIGEST flippen, beantwortet der Server genau diesen (creds-losen) Enable-
// Request mit 401 statt 200 (on-device verifiziert). Deferred-apply laesst den
// ausloesenden Request mit dem alten Auth-Zustand abschliessen; die Aenderung
// greift ~einen loop-Tick spaeter. NVS wird sofort geschrieben (persistent).
volatile bool g_pendingApply = false;

void save_(const UiAuthConfig& c) {
    JsonDocument doc;
    doc["enabled"]  = c.enabled;
    doc["user"]     = c.username;
    doc["ha1"]      = c.ha1;
    nvsSaveJsonWithCleanup(NVS_NS, NVS_KEY, doc);
}

// Wendet den uebergebenen Stand auf die Live-Middleware an. Reihenfolge bewusst:
// Credentials zuerst setzen, dann authType — so gibt es kein Fenster mit
// authType=DIGEST aber leeren Creds (das waere ohnehin fail-open, aber sauber
// ist sauber). Aus → AUTH_NONE.
void apply_(const UiAuthConfig& c) {
    g_mw.setRealm(UI_AUTH_REALM);
    g_mw.setAuthFailureMessage("SixBack — authentication required");
    if (c.enabled && c.username.length() && c.ha1.length()) {
        g_mw.setUsername(c.username.c_str());
        g_mw.setPasswordHash(c.ha1.c_str());
        g_mw.setAuthType(AsyncAuthType::AUTH_DIGEST);
        Serial.println("[uiauth] web-UI password protection ENABLED (digest)");
    } else {
        g_mw.setAuthType(AsyncAuthType::AUTH_NONE);
        Serial.println("[uiauth] web-UI password protection disabled");
    }
}

} // anon

UiAuthConfig uiAuthLoad() {
    UiAuthConfig c;
    JsonDocument doc;
    if (nvsLoadJson(NVS_NS, NVS_KEY, doc)) {
        c.enabled  = doc["enabled"] | false;
        c.username = doc["user"].as<String>();
        c.ha1      = doc["ha1"].as<String>();
    }
    return c;
}

bool uiAuthIsActive() {
    UiAuthConfig c = uiAuthLoad();
    return c.enabled && c.username.length() && c.ha1.length();
}

void uiAuthInit(AsyncWebServer& ui) {
    if (!g_attached) {
        ui.addMiddleware(&g_mw);
        g_attached = true;
    }
    apply_(uiAuthLoad());
}

bool uiAuthSetCredentials(const String& user, const String& plainPass) {
    if (user.length() == 0 || plainPass.length() < MIN_PW_LEN) return false;
    // HA1 mit der Lib-eigenen Funktion berechnen → garantiert dasselbe Format
    // (lowercase MD5(user:realm:pass)), das die Lib beim Pruefen erwartet.
    String ha1 = generateDigestHash(user.c_str(), plainPass.c_str(), UI_AUTH_REALM);
    if (ha1.length() == 0) return false;   // OOM o.ae.
    UiAuthConfig c;
    c.enabled  = true;
    c.username = user;
    c.ha1      = ha1;
    save_(c);
    g_pendingApply = true;   // deferred (siehe g_pendingApply-Kommentar)
    return true;
}

bool uiAuthSetEnabled(bool enabled) {
    UiAuthConfig c = uiAuthLoad();
    if (enabled && (c.username.length() == 0 || c.ha1.length() == 0)) {
        // Aktivieren ohne Credentials: bewusst kein Aussperren.
        return false;
    }
    c.enabled = enabled;
    save_(c);
    g_pendingApply = true;   // deferred (siehe g_pendingApply-Kommentar)
    return true;
}

void uiAuthReset() {
    nvsErase(NVS_NS, NVS_KEY);
    UiAuthConfig c;          // leer + disabled
    g_pendingApply = false;  // ein evtl. anstehendes Enable verwerfen
    apply_(c);               // Aus ist immer sofort sicher (nie spurious 401)
    Serial.println("[uiauth] reset — web-UI password protection cleared");
}

void uiAuthSerialTick() {
    // Deferred-apply: ausserhalb des Web-Handler-Kontexts scharfschalten,
    // nachdem der ausloesende Request abgeschlossen ist (siehe g_pendingApply).
    if (g_pendingApply) {
        g_pendingApply = false;
        apply_(uiAuthLoad());
    }

    // Solange Improv das UART bedient, NICHT mitlesen (sonst klauen wir
    // Improv-Frames). Nach Schliessen des Windows ist das UART frei.
    if (improvIsActive()) return;
    if (!Serial.available()) return;

    static String line;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            line.trim();
            if (line.length()) {
                if (line.equalsIgnoreCase("auth-reset")) {
                    uiAuthReset();
                } else if (line.equalsIgnoreCase("auth-status")) {
                    UiAuthConfig c = uiAuthLoad();
                    Serial.printf("[uiauth] enabled=%d user='%s' configured=%d\n",
                                  (int)c.enabled, c.username.c_str(),
                                  (int)(c.username.length() && c.ha1.length()));
                }
            }
            line = "";
        } else if (line.length() < 64) {   // bounded — kein Runaway-Buffer
            line += ch;
        }
    }
}

} // namespace sixback

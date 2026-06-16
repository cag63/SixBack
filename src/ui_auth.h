// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — optionaler Zugriffsschutz fuer die Web-UI (Issue #31)
//
// Opt-in HTTP-Auth NUR fuer den uiServer (:80 = menschliche WebUI + /api/*).
// Der speaker-seitige boseServer (:8000, Cloud-Mock/Migration/Push) und das
// Captive-Portal bleiben bewusst UNGEGATED — der Speaker hat keine Credentials
// und darf nie gegen Auth laufen. Weil der Speaker Port 80 nie anfasst, kann
// dieser Gate die BMX-Kompatibilitaet per Konstruktion nicht brechen.
//
// Mechanik:
//   - EINE globale AsyncAuthenticationMiddleware, am uiServer registriert.
//   - Default AUS (AUTH_NONE) → der Normalo-User sieht nie einen Login.
//   - Eingeschaltet → AUTH_DIGEST: das Passwort geht NIE im Klartext ueber die
//     (TLS-lose) LAN-Leitung. Trotzdem ehrlich: Seiteninhalt + /api-Payloads
//     bleiben Klartext, und die Speaker-eigene :8090-API bleibt offen. Auth =
//     Abschreckung gegen versehentlichen Zugriff, kein Isolations-Ersatz.
//   - Persistiert wird NUR user + HA1 = MD5(user:realm:pass), nie das Klartext-
//     Passwort. Runtime-Umschaltung ohne Reboot.
//
// Lockout-Recovery: USB-Serial-Kommando "auth-reset" (siehe uiAuthSerialTick).
#ifndef SIXBACK_UI_AUTH_H
#define SIXBACK_UI_AUTH_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

namespace sixback {

struct UiAuthConfig {
    bool   enabled  = false;
    String username;   // leer wenn nie gesetzt
    String ha1;        // MD5(user:realm:pass) (Digest HA1), leer wenn nie gesetzt
};

// Haengt die Auth-Middleware an den uiServer und wendet den NVS-Stand an.
// MUSS in setup() NACH registerApiEndpoints(uiServer) und VOR uiServer.begin()
// aufgerufen werden (Server-Middleware muss vor begin() registriert sein).
void uiAuthInit(AsyncWebServer& ui);

// Liest die aktuelle Konfiguration aus NVS (Defaults wenn nicht vorhanden).
UiAuthConfig uiAuthLoad();

// True wenn der Gate aktiv UND vollstaendig konfiguriert ist (user+ha1 da).
bool uiAuthIsActive();

// Setzt Credentials (berechnet HA1 aus Klartext, persistiert nur den Hash) und
// aktiviert den Gate. Wendet die Aenderung sofort live an. Returns false bei
// leerem User / zu kurzem Passwort.
bool uiAuthSetCredentials(const String& user, const String& plainPass);

// Schaltet den Gate an/aus ohne Credentials zu aendern. Aktivieren ohne
// gesetzte Credentials ist ein No-op-Guard (returns false → bleibt offen,
// fail-open gegen versehentliches Aussperren).
bool uiAuthSetEnabled(bool enabled);

// Recovery: loescht user+HA1, schaltet den Gate aus, wendet sofort an.
void uiAuthReset();

// In loop() zu rufen — liest serielle Zeilen NUR wenn das Improv-Window
// geschlossen ist (sonst wuerde es Improv-Frames klauen) und reagiert auf
// "auth-reset" / "auth-status". No-op solange improvIsActive().
void uiAuthSerialTick();

} // namespace sixback

#endif // SIXBACK_UI_AUTH_H

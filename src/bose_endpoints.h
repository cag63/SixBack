// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Bose Cloud Replacement Endpoints
//
// Diese Endpoints emuliert der ESP für die Speaker. Verifizierte Liste
// aus den AfterTouch-Live-Logs am Pi5 (siehe /Public/CLAUDE/BOSE/docs/RESEARCH.md §12).
#ifndef BOSEFIX32_BOSE_ENDPOINTS_H
#define BOSEFIX32_BOSE_ENDPOINTS_H

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

void registerBoseEndpoints(AsyncWebServer& server);

// Catch-all-Logger (P3 aus UEBERBOESE_ADOPTION_PLAN.md): jeder unbekannte Path
// landet in einem RAM-Ringbuffer (50 Eintraege). Fuer Endpoint-Forensik
// (welche Paths fragt der Speaker eigentlich an).
void getUnknownRequestsJson(JsonArray out);
void clearUnknownRequests();

// true = letzter Stereo-Paar-Store-Write (bfx_groups) persistierte. false =
// NVS voll trotz Cleanup → Gruppe lebt nur im RAM, nicht reboot-persistent.
// Exponiert in /api/status als groups_persist_ok (Diagnostik; bewusst KEIN
// 500 an die Speaker-FW, die 201/200 erwartet).
bool groupsPersistOk();

#endif // BOSEFIX32_BOSE_ENDPOINTS_H

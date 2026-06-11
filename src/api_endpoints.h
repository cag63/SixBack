// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Verwaltungs-API (REST + Mini-HTML) auf Port 80
//
// Diese API ist NICHT für die Speaker — die sprechen das Cloud-Mock
// auf Port 8000 (siehe bose_endpoints.h). Diese hier ist für:
//   - autonomes Testing (curl / WebUI später)
//   - mDNS-discoverable Status
//   - Migrations-Wizard-Trigger
//   - Preset-Verwaltung
#ifndef BOSEFIX32_API_ENDPOINTS_H
#define BOSEFIX32_API_ENDPOINTS_H

#include <ESPAsyncWebServer.h>
#include "preset_store.h"

void registerApiEndpoints(AsyncWebServer& ui);

namespace sixback {
// Setzt einen TUNEIN/LIR-Sender per /select an einen Speaker (plain HTTP :8090).
// Baut das ContentItem (type="stationurl"; TUNEIN -> /v1/playback/station/<sid>,
// LIR -> ORION /station?data=...). Reines plain-HTTP -> aus jedem FreeRTOS-Task
// aufrufbar (kein -32512-HTTPS-Constraint). Markiert intern gabboMarkSelfSelect.
// Gibt den HTTP-Code zurueck (200 = ok).
int selectStationOnSpeaker(const String& spIp, const Preset& p);
} // namespace sixback

#endif // BOSEFIX32_API_ENDPOINTS_H

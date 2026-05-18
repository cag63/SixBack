// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Captive Portal for first-time WiFi provisioning.
//
// Aktiv wenn keine NVS-Credentials oder STA-Connect-Timeout. ESP oeffnet
// einen offenen AP "BoseFix32-XXYYZZ" (kein PSK), startet DNS-Hijack
// (alle Queries → 192.168.4.1) + HTTP-Server mit SSID-Auswahl-Form. Nach
// erfolgreichem Save: STA-Connect, persistieren, success-Page mit
// `<meta refresh>` an die zugewiesene STA-IP.
#ifndef BOSEFIX32_CAPTIVE_PORTAL_H
#define BOSEFIX32_CAPTIVE_PORTAL_H

#include <Arduino.h>

namespace bosefix {

void captiveStart();
void captiveStop();
void captiveTick();
bool captiveIsActive();
uint32_t captiveWindowRemainingS();

}  // namespace bosefix

#endif

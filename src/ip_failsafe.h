// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — IP-Change-Failsafe
//
// Hintergrund: Bose-Speaker speichern margeServerUrl/statsServerUrl/etc.
// als feste IP-Adressen (kein mDNS-Resolve im Speaker-Stack). Wenn der
// ESP eine neue IP vom DHCP bekommt (Router-Reboot, Lease-Expire), zeigen
// alle bereits migrierten Speaker auf eine tote IP und Presets gehen nicht.
//
// Diese Modul:
//   - Merkt sich die letzte eigene IP in NVS
//   - Beim Boot: vergleicht aktuelle IP mit gespeicherter
//   - Bei Änderung: triggert für alle owned Speaker eine erneute
//     Migration auf die neue ESP-IP (telnet sys configuration ...)
//   - Zur LAUFZEIT: GOT_IP-Event armt einen Recheck, den ipFailsafeTick()
//     (aus loop()) in einem One-Shot-Task ausführt — deckt Reconnect-
//     mit-neuer-IP OHNE Reboot ab (Discussion #19)
//   - Scheitert ein Speaker (offline/settling während des Laufs), wird
//     last_ip NICHT advanced und der Lauf alle 60 s retried (max. 20×),
//     statt den Speaker dauerhaft auf der toten alten IP zu lassen
//   - Loggt jeden Auto-Remigrate im Serial
//
// User-Empfehlung im UI: DHCP-Reservation für ESP-MAC im Router setzen,
// dann tritt der Failsafe nie ein.
#ifndef BOSEFIX32_IP_FAILSAFE_H
#define BOSEFIX32_IP_FAILSAFE_H

#include <Arduino.h>

namespace sixback {

// Beim Boot rufen (in setup(), VOR connectWifi): registriert den
// GOT_IP-Event-Listener, der den Runtime-Recheck armt.
void ipFailsafeInit();

// Beim Boot rufen (nach Connect-WiFi, nach Inventory-Load).
// Loggt + ggf. re-migriert alle bekannten Speaker.
void ipFailsafeCheck();

// Aus loop() rufen: führt einen gearmten Recheck (GOT_IP-Event oder
// fälliger Retry) als One-Shot-Task aus. Der loopTask ist nach
// healthInit() WDT-subscribed — die sekundenlange Telnet-Re-Migration
// darf dort nicht inline laufen.
void ipFailsafeTick();

// Recheck manuell armen (Test-Endpoint /api/test/force-ip-change mit
// trigger_now=true — Runtime-Pfad ohne Reboot testbar).
void ipFailsafeArmRecheck();

// Liefert die zuletzt persistierte ESP-IP (oder "" wenn noch nie gespeichert).
String getLastKnownIp();

} // namespace sixback

#endif

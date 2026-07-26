// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — konfigurierbarer mDNS-/DHCP-Hostname (FHEM 144729: Zwei-Stick-
// Betrieb — ohne Override announcen beide Sticks sixback.local).
//
// Default bleibt MDNS_HOSTNAME ("sixback"); NVS-Override im eigenen
// Namespace "sixback-host" ("sixback-net" ist vom IP-Failsafe belegt).
// Anwendung beim Boot (MDNS.begin + WiFi.setHostname); eine Aenderung via
// PUT /api/hostname wird nur persistiert und greift nach Reboot.
#ifndef SIXBACK_HOST_SETTINGS_H
#define SIXBACK_HOST_SETTINGS_H

#include <Arduino.h>

namespace sixback {

// Effektiver Hostname (NVS-Override oder Default). Erste Nutzung laedt aus
// NVS und cached prozessweit — mDNS/DHCP aendern sich eh nur beim Boot.
const String& hostname();

// Persistierter Wert DIREKT aus NVS (kein Cache) — kann nach einem PUT vom
// laufenden hostname() abweichen (= pending bis Reboot). Fuer GET /api/hostname,
// damit die UI nach dem Save nicht auf den Boot-Wert zurueckspringt.
String persistedHostname();

// Normalisiert in-place (trim + lowercase) und validiert: 1..24 Zeichen,
// [a-z0-9-], kein fuehrendes/abschliessendes '-'. Bei false steht der
// Klartext-Grund in err.
bool isValidHostname(String& candidate, String& err);

// Persistiert candidate (vorher validieren!). Leerstring = Reset auf
// Default. Aendert bewusst NICHT den laufenden Cache (reboot_required).
bool saveHostname(const String& candidate);

} // namespace sixback

#endif

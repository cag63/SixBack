// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — global constants
#ifndef SIXBACK_CONFIG_H
#define SIXBACK_CONFIG_H

// HTTP-Server für Speaker-Anfragen (das was Bose-Cloud früher war)
#define BOSE_HTTP_PORT 8000

// Web-UI / REST-API für User
#define UI_HTTP_PORT 80

// mDNS-Hostname
#define MDNS_HOSTNAME "sixback"

// Telnet-Port am Speaker (Bose Diagnostic Shell)
#define BOSE_TELNET_PORT 17000

// Bose BMX-API am Speaker
#define BOSE_BMX_PORT 8090

// Bose gabbo-Notification-WebSocket am Speaker (LAN, wsapiproxy=true). #15:
// physische Hardware-Tastendruecke werden hier als <nowSelectionUpdated> publiziert.
#define BOSE_GABBO_WS_PORT 8080

// Content-Security-Policy fuer die Geraete-WebUI (Defense-in-Depth gegen XSS aus
// untrusted Daten: RadioBrowser/TuneIn/DLNA-Namen, Custom-Stream-URLs). Die SPA
// ist inline-lastig (1 inline <script> + ~87 on*-Handler + inline styles) -> ein
// striktes script-src 'self' wuerde sie brechen; 'unsafe-inline' ist Pflicht. Der
// Haertungswert liegt damit in frame-ancestors/object-src/base-uri/form-action +
// https-only fuer img/connect (Clickjacking/Exfiltration). Idee aus STR (THREAT-MODEL).
#define SIXBACK_CSP_HEADER \
  "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; " \
  "img-src 'self' data: https:; connect-src 'self' https:; frame-ancestors 'none'; " \
  "base-uri 'self'; object-src 'none'; form-action 'self'"

// WiFi-Credentials werden im NVS persistiert (Namespace "sixback-wifi").
// Erstes Provisioning via Improv-Serial (tools/improv_client.py oder
// ESP Web Tools im Browser). Siehe wifi_provisioning.{h,cpp}.

#endif // SIXBACK_CONFIG_H

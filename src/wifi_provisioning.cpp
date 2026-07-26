// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — WiFi-Provisionierung

#include "wifi_provisioning.h"
#include "captive_portal.h"
#include "host_settings.h"
#include "version.h"
#include <WiFi.h>
#include <Preferences.h>
#include "ImprovWiFiLibrary.h"
#include <esp_wifi.h>
#if SIXBACK_IMPROV_DUAL
// Arduino.h zieht HWCDC.h NICHT mit — ohne diesen Include ist die Klasse
// HWCDC nicht bekannt. Muss auf Datei-Ebene stehen, nicht im anonymen
// Namespace (sonst landet die Deklaration im falschen Scope).
#include "HWCDC.h"
#endif

namespace sixback {

namespace {

constexpr const char* NVS_NS    = "sixback-wifi";
constexpr const char* KEY_SSID  = "ssid";
constexpr const char* KEY_PSK   = "psk";

// Idle-basiertes Window (RFNETHM-Pattern): keine Activity fuer idle_ms → zu.
//   Keine NVS-Creds:  120 s (Erstprovisioning, langsamer User)
//   NVS-Creds da:      30 s (schnelle Reconfig, sonst sofort zu)
// Jedes UART-Byte verlaengert das Window auf "idle_ms ab jetzt".
constexpr uint32_t IMPROV_IDLE_FRESH    = 120 * 1000;
constexpr uint32_t IMPROV_IDLE_HASCREDS =  30 * 1000;

ImprovWiFi  improvSerial(&Serial);

// --- Dual-Transport-Experiment (PR #39 / Matrix-Achse "Serial-Transport") ---
// Auf S3-Boards OHNE UART-Bridge (Arduino Nano ESP32, XIAO u.ae.) haengt der
// Host am nativen USB-Serial-JTAG, nicht an UART0. Bisher entschied das ein
// Compile-Flag (ARDUINO_USB_CDC_ON_BOOT) -> pro Board-Topologie ein eigenes
// Build-Target (genau der Anlass von PR #39). Hier haengen wir stattdessen je
// eine Improv-Instanz an BEIDE Transporte; der User darf stecken, wo er will.
// Zulaessig, weil die Lib voll instanzbasiert ist (eigener serial_, backend_,
// core_ je Objekt, kein globaler mutable State).
#if SIXBACK_IMPROV_DUAL
// Arduino-Core 3.3.7 deklariert die globale HWCDC-Instanz (HWCDCSerial) NUR
// bei ARDUINO_USB_CDC_ON_BOOT=1 — dann waere `Serial` aber selbst das
// USB-Objekt (HardwareSerial.h:429-431 macht `Serial` zum Makro) und die
// Logs waeren vom CH343 verschwunden. Wir wollen beides gleichzeitig:
// Serial = UART0 (Logs bleiben, wo sie sind) PLUS ein zweiter Improv-
// Transport auf dem nativen USB-Serial-JTAG. Also legen wir die Instanz
// selbst an — zulaessig, weil der Core bei CDC_ON_BOOT=0 gar keine globale
// anlegt (HWCDC.cpp:617-620, kein Doppelzugriff auf die Peripherie) und der
// Konstruktor public ist (HWCDC.h:52).
HWCDC       usbCdc;
ImprovWiFi  improvUsb(&usbCdc);

// ⚠️ NIEMALS usbCdc.end() aufrufen — auch nicht "aufraeumend" beim Window-Close.
// HWCDC::end() ruft setDebugOutput(false) (HWCDC.cpp:386), und HWCDC::setDebugOutput()
// fuehrt in BEIDEN Zweigen unbedingt ets_install_putc1(NULL) aus
// (HWCDC.cpp:614, Kommentar im Core: "closes UART log output"). Danach ist der
// ROM-Konsolenkanal 1 abgehaengt und ALLE ARDUHAL-Logs (log_e/log_w/log_i) sind
// bis zum Reboot auf JEDEM Port weg — auch auf UART0/CH343. Der Port darf einfach
// offen bleiben; der Guard unten stellt sicher, dass niemand mitliest.
#endif

#if SIXBACK_IMPROV_DUAL

// --- First-come-wins-Guard ---------------------------------------------------
// Die beiden Improv-Instanzen sind unabhaengig, teilen sich aber die globale
// Arduino-WiFi-Klasse. Laufen zwei Provisioning-Sessions gleichzeitig, sehen
// beide denselben Scan: einer bekommt die AP-Liste, der andere eine LEERE
// (on-HW beobachtet 2026-07-25 — gleichzeitige Probe auf beiden Ports lieferte
// dem zweiten nur den leeren Listen-Endmarker). Deshalb: der erste Port, auf
// dem ein VOLLSTAENDIGES "IMPROV"-Magic ankommt, gewinnt das Window; der andere
// wird bis Window-Ende nur noch gedraint.
//
// Bewusst NICHT "erstes Byte gewinnt": auf UART0 haengt im Lab (und bei Usern
// mit offenem Terminal) leicht Muell an, der den Port sonst faelschlich claimt
// und die echte Provisionierung ueber USB aussperrt. Das Magic ist das
// verlaessliche Signal, dass dort wirklich ein Improv-Host spricht.
constexpr char IMPROV_MAGIC[6] = {'I','M','P','R','O','V'};

struct ImprovLink {
    Stream*     io;
    ImprovWiFi* impl;
    const char* name;
    uint8_t     magicPos;   // Fortschritt im rollenden Magic-Match
};

ImprovLink improvLinks[] = {
    { &Serial, &improvSerial, "uart0",    0 },
    { &usbCdc, &improvUsb,    "usb-cdc",  0 },
};
constexpr size_t IMPROV_LINK_COUNT = sizeof(improvLinks) / sizeof(improvLinks[0]);

int improvClaimedBy = -1;   // Index des Gewinner-Links, -1 = noch offen

// Rollender Match auf "IMPROV". Gibt true zurueck, sobald das Magic in diesem
// Byte-Block vollstaendig gesehen wurde.
bool improvScanForMagic(ImprovLink& link, const uint8_t* buf, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = buf[i];
        if (b == static_cast<uint8_t>(IMPROV_MAGIC[link.magicPos])) {
            if (++link.magicPos == sizeof(IMPROV_MAGIC)) {
                link.magicPos = 0;
                return true;
            }
        } else {
            // Fehlschlag: ggf. direkt wieder auf dem ersten Zeichen aufsetzen.
            link.magicPos = (b == static_cast<uint8_t>(IMPROV_MAGIC[0])) ? 1 : 0;
        }
    }
    return false;
}

// Ein einziger improvActive/idle-Timer bleibt bestehen: das Window ist eine
// Eigenschaft des Geraets, nicht des Kabels.
void improvPumpAll() {
    for (size_t i = 0; i < IMPROV_LINK_COUNT; ++i) {
        ImprovLink& link = improvLinks[i];

        // Verlierer-Port: RX wegwerfen, damit Muell dort weder das Idle-Fenster
        // haelt noch spaeter als halber Frame in den Parser laeuft.
        if (improvClaimedBy >= 0 && static_cast<int>(i) != improvClaimedBy) {
            while (link.io->available() > 0) link.io->read();
            continue;
        }

        uint8_t buf[64];
        size_t  n = 0;
        while (link.io->available() > 0 && n < sizeof(buf)) {
            const int b = link.io->read();
            if (b < 0) break;
            buf[n++] = static_cast<uint8_t>(b);
        }

        if (n == 0) {
            // Kein Byte — trotzdem ticken, damit das Wall-Clock-Fenster des
            // Cores weiterlaeuft (handleBuffer(nullptr,0) macht genau das).
            link.impl->handleBuffer(nullptr, 0);
            continue;
        }

        if (improvClaimedBy < 0 && improvScanForMagic(link, buf, n)) {
            improvClaimedBy = static_cast<int>(i);
            Serial.printf("[improv] transport claimed by %s — other port muted\n",
                          link.name);
        }
        link.impl->handleBuffer(buf, n);
    }
}

inline bool improvAnyAvailable() {
    for (size_t i = 0; i < IMPROV_LINK_COUNT; ++i) {
        if (improvLinks[i].io->available() > 0) return true;
    }
    return false;
}

inline void improvDrainAll() {
    for (size_t i = 0; i < IMPROV_LINK_COUNT; ++i) {
        while (improvLinks[i].io->available() > 0) improvLinks[i].io->read();
    }
}

// Window zu -> naechster Boot/naechstes Window darf wieder frei vergeben werden.
inline void improvResetClaim() { improvClaimedBy = -1; }

#else   // Single-Transport: Verhalten exakt wie bisher, kein neuer Code-Pfad.

inline void improvPumpAll()      { improvSerial.handleSerial(); }
inline bool improvAnyAvailable() { return Serial.available(); }
inline void improvDrainAll()     { while (Serial.available()) Serial.read(); }
inline void improvResetClaim()   {}

#endif  // SIXBACK_IMPROV_DUAL

bool        improvActive          = false;
uint32_t    improvStartMs         = 0;
uint32_t    improvLastActivityMs  = 0;
uint32_t    improvIdleMs          = IMPROV_IDLE_FRESH;

// 2026-05-21 22:50 — gratuitous-ARP-Versuch via etharp_gratuitous() hat
// C6 in einen Reboot-Loop gestuerzt (lwIP-Stack-Frames im PanicHandler).
// Vermutlich weil esp_netif_get_netif_impl() bei direkt-nach-Connect-Aufruf
// nicht voll initialisiert ist, oder der lwIP-Netif-Lock falsch gehalten
// wird. Code entfernt — Retry-on-ARP-Race in addByIp (speaker_inventory.cpp)
// reicht als Fix fuer P0b. Wenn das nicht hilft, alternative Wege:
//  - esp_event-Listener auf IP_EVENT_STA_GOT_IP + send_gratuitous_arp_request
//    via raw socket (per Hand, ohne lwIP-Direktzugriff)
//  - UDP-broadcast auf Port 6 ohne payload — Switches lernen die MAC trotzdem
void sendGratuitousArpBroadcast() {
    // intentionally no-op for now — siehe Kommentar oben.
}

void onImprovConnected(const char* ssid, const char* pw) {
    Serial.printf("[improv] connected -> ssid=%s\n", ssid);
    persistCreds(ssid ? ssid : "", pw ? pw : "");
    improvActive = false;
    wifiOptimizeForReliability();   // Power-Save aus, sobald STA up ist
    sendGratuitousArpBroadcast();   // Mesh-ARP-Race-Mitigation (P0b)
}

void onImprovError(ImprovTypes::Error err) {
    Serial.printf("[improv] error 0x%02X\n", static_cast<unsigned>(err));
}

bool tryConnectFromNVS() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return false;
    String ssid = prefs.getString(KEY_SSID, "");
    String psk  = prefs.getString(KEY_PSK,  "");
    prefs.end();
    if (ssid.length() == 0) {
        Serial.println("[wifi] no NVS credentials");
        return false;
    }
    Serial.printf("[wifi] NVS credentials present, trying ssid=%s\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    // C6 4WAY_HANDSHAKE-Fix (siehe Memo reference_esp32_c6_wifi_ps_handshake).
    // Arduino-Wrapper WiFi.setSleep + direkter IDF-Call esp_wifi_set_ps —
    // bei Framework 3.3.7 reicht der Wrapper allein nicht mehr aus, der
    // IDF-Default WIFI_PS_MIN_MODEM ueberlebt sonst den Mode-Switch und
    // killt das 4-way-handshake-Timing auf WPA2-Mixed-APs.
    WiFi.setSleep(WIFI_PS_NONE);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.begin(ssid.c_str(), psk.c_str());
    uint32_t t0 = millis();
    // WICHTIG: improvSerial waehrend des STA-connect-waits weiter pumpen.
    // tryConnectFromNVS blockt sonst bis zu 20 s und ESP Web Tools
    // bekommt keine Antwort auf seine Improv-Setup-Frames — Symptom:
    // "Initializing Improv Serial → SCHEDULE RETRY 0,1,2" + Abbruch.
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        improvPumpAll();
        delay(50);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] connected, IP=%s\n", WiFi.localIP().toString().c_str());
        wifiOptimizeForReliability();
        sendGratuitousArpBroadcast();
        improvLastActivityMs = millis();
        Serial.printf("[improv] reset window after AP connect — improvLastActivityMs=%lu improvActive=%d\n",
                      (unsigned long)improvLastActivityMs, (int)improvActive);
        return true;
    }
    Serial.println("[wifi] NVS-credential connect timed out");
    WiFi.disconnect(true);
    return false;
}

bool nvsHasCreds() {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) return false;
    String s = prefs.getString(KEY_SSID, "");
    prefs.end();
    return s.length() > 0;
}


void startImprovMode() {
    improvResetClaim();   // frisches Window -> Transport wieder frei vergebbar
    improvIdleMs = nvsHasCreds() ? IMPROV_IDLE_HASCREDS : IMPROV_IDLE_FRESH;
    Serial.printf("[improv] starting (idle-window %us, %sNVS-creds present)\n",
                  improvIdleMs / 1000, nvsHasCreds() ? "" : "no ");
    // WiFi-STA-Mode GARANTIEREN: Improv ruft WiFi.scanNetworks() wenn
    // ESP Web Tools "Scan WiFi" triggert.  Im no-creds-Boot ist Mode
    // sonst noch WIFI_MODE_NULL (tryConnectFromNVS skipt vor uns), und
    // scan returnt WIFI_SCAN_FAILED → leere Liste im UI.  Auch falls
    // captive spaeter AP_STA setzt: STA ist bereits angefordert.
    if (WiFi.getMode() == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_STA);
    }
    // Country-Code / Scan-Range EXPLIZIT auf EU-2.4GHz (Ch 1-13) setzen.
    // Default ist `CN` o.ae. mit policy=AUTO, was bei ESP32-C6 + WiFi-6-
    // Stack in einen NO_AP_FOUND-Pfad fuehrt wenn der AP auf Ch 12/13
    // sitzt (typisch nach Fritzbox-Auto-Channel-Selection). S3/C3 sind
    // toleranter, C6 strikter — daher symptomatisch nur C6-Boards.
    // POLICY_MANUAL stellt sicher dass der Connect-Pfad NICHT per
    // Beacon-Override auf einen restriktiveren Channel-Set zurueckfaellt.
    // 2026-06-17: KEIN C5-Sonderfall hier. Vermutet wurde, die 2.4-GHz-MANUAL-
    // Config (schan=1/nchan=13) breche auf dem C5 den 5-GHz-Reconnect — das war
    // falsch: der C5 verbindet sich damit problemlos initial auf 5 GHz (ch40),
    // und der beobachtete Reconnect-Fehler ist `Reason 210 =
    // NO_AP_FOUND_W_COMPATIBLE_SECURITY` (esp-idf station-scenarios), d.h. der
    // AP WIRD gefunden, aber wegen Security/PMF-Mismatch abgelehnt — KEIN
    // Band-/Scan-Problem. POLICY_AUTO half daher nicht und wurde zurueckgenommen.
    {
        wifi_country_t c = {
            .cc      = "DE",
            .schan   = 1,
            .nchan   = 13,
            .max_tx_power = 78,    // 19.5 dBm — Default Arduino-Core
            .policy  = WIFI_COUNTRY_POLICY_MANUAL,
        };
        esp_wifi_set_country(&c);
    }
    // PS-Mode AUS *VOR* irgendeinem WiFi.begin() — gilt sowohl fuer
    // tryConnectFromNVS als auch fuer den begin(), den die improv-Lib
    // bei Send-WiFi-Settings selbst ausfuehrt. C6+WiFi-6-Defaults
    // (WIFI_PS_MIN_MODEM, DTIM-basiert) kappen sonst das 4-way-
    // handshake-Timing → reproduzierbarer 4WAY_HANDSHAKE_TIMEOUT auf
    // WPA2-Mixed-APs. EULFW32 / ip4knx setzen das gleiche vor begin();
    // SixBack hat es bis v0.5.450 erst NACH Connect via
    // wifiOptimizeForReliability() gesetzt — zu spaet wenn der Connect
    // selbst schon scheitert.
    //
    // 2026-05-27: Arduino-Wrapper WiFi.setSleep reicht bei Framework
    // 3.3.7 nicht mehr; der IDF-Default WIFI_PS_MIN_MODEM ueberlebt den
    // Mode-Switch. Zusaetzlich esp_wifi_set_ps direkt aufrufen.
    WiFi.setSleep(WIFI_PS_NONE);
    esp_wifi_set_ps(WIFI_PS_NONE);
    WiFi.setAutoReconnect(true);
    // Chip-Family per IDF-Target, damit ESP Web Tools die korrekte
    // chipFamily-String fuer ihren UI-flow sieht.
    ImprovTypes::ChipFamily cf =
#if CONFIG_IDF_TARGET_ESP32S3
        ImprovTypes::CF_ESP32_S3
#elif CONFIG_IDF_TARGET_ESP32C3
        ImprovTypes::CF_ESP32_C3
#elif CONFIG_IDF_TARGET_ESP32C6
        ImprovTypes::CF_ESP32_C6
#elif CONFIG_IDF_TARGET_ESP32S2
        ImprovTypes::CF_ESP32_S2
#else
        ImprovTypes::CF_ESP32
#endif
        ;
    improvSerial.setDeviceInfo(
        cf,
        FW_NAME,
        FW_VERSION_STRING,
        "SixBack"
    );
    improvSerial.onImprovConnected(onImprovConnected);
    improvSerial.onImprovError(onImprovError);
#if SIXBACK_IMPROV_DUAL
    // Zweiter Transport (nativer USB-Serial-JTAG). begin() ist Pflicht, sonst
    // liefert available() dauerhaft 0. Baud ist bei CDC bedeutungslos.
    usbCdc.begin();
    improvUsb.setDeviceInfo(cf, FW_NAME, FW_VERSION_STRING, "SixBack");
    improvUsb.onImprovConnected(onImprovConnected);
    improvUsb.onImprovError(onImprovError);
    Serial.println("[improv] dual transport active: UART0 + native USB-CDC");
#endif
    improvActive          = true;
    improvStartMs         = millis();
    improvLastActivityMs  = improvStartMs;
}

// Tick-Helper: jedes UART-Byte gilt als Aktivitaet → idle-Timer zuruecksetzen.
// Konservativ: auch User-Tipperei (= keine valide Improv-Frame) verlaengert
// das Window, damit niemand mitten im Provisioning rausgekickt wird.
void improvTickInternal() {
    if (!improvActive) return;
    // Wedge-Guard (Fix 2026-07-02): der Improv-Core hat ein HARTES 120-s-
    // Wall-Clock-Fenster (IMPROV_RUN_FOR; "once false, stays false until
    // reboot") und drained nach dem Disarm den RX nicht mehr. Jedes weitere
    // Host-Byte hielte dann via Serial.available() unser Idle-Fenster ewig
    // offen -> die provisionWifi()-Warteschleife erreichte den Restart-
    // Fallback nie und das unprovisionierte Geraet hinge dunkel (kein SoftAP,
    // kein Reboot). Also: Core-Fenster zu -> RX verwerfen, Fenster schliessen.
    if (millis() - improvStartMs > IMPROV_RUN_FOR) {
        improvDrainAll();
        Serial.println("[improv] core wall-clock window closed — closing improv window");
        improvActive = false;
        return;
    }
    if (improvAnyAvailable()) improvLastActivityMs = millis();
    improvPumpAll();
    const uint32_t idle = millis() - improvLastActivityMs;
    if (idle > improvIdleMs) {
        Serial.printf("[improv] idle %us — window closed\n", idle / 1000);
        improvActive = false;
    }
}

} // anon

void provisionWifi() {
    // DHCP-Hostname = mDNS-Hostname (NVS-Override oder Default). MUSS vor
    // dem ersten WiFi.mode(WIFI_STA)/begin() stehen (Arduino-Core), deshalb
    // ganz am Anfang — deckt Improv-, Captive- und NVS-Reconnect-Pfad.
    WiFi.setHostname(hostname().c_str());

    // Improv-Window IMMER ab Boot oeffnen (120 s), egal ob NVS-Creds da
    // sind. Damit kann der User auch ohne Factory-Reset jederzeit nach
    // einem Boot neue WLAN-Credentials einspeisen — z.B. weil sich der
    // Router/das WLAN geaendert hat oder der Speaker an einen anderen
    // Standort wandert. Pattern uebernommen aus RFNETHM (improv_glue.cpp):
    // Improv ist NICHT Fallback, sondern Standard-Service waehrend der
    // Boot-Phase; der NVS-Reconnect-Versuch laeuft parallel dazu.
    startImprovMode();

    if (tryConnectFromNVS()) {
        // WiFi up. Improv-Window laeuft via wifiProvisioningTick() noch
        // bis zum 120 s-Ende weiter — User kann jederzeit re-provisionieren.
        return;
    }

    // Kein Connect aus NVS (oder keine Creds vorhanden).
    //
    // 2026-05-22: ESP32-C6 zeigt einen reproduzierbaren AP-STA-Konflikt
    // wenn captive softAP gleichzeitig mit improv-WiFi.begin() laeuft —
    // WiFi 6 PHY auf C6 toleriert die Radio-Teilung schlechter als
    // S3/Classic. Symptom: improv liefert "Unable to connect to WiFi",
    // ARP-Resolve auf softAP-Gateway scheitert.
    // Mitigation: improv kriegt 10s exklusive Radio-Zeit. Nur wenn dort
    // nichts ankommt, oeffnet captive als 2.-Pfad.
    constexpr uint32_t IMPROV_EXCLUSIVE_MS = 10 * 1000;
    const uint32_t improvOnlyStart = millis();
    Serial.println("[provision] improv-exclusive window 10s (no captive yet)");
    while (millis() - improvOnlyStart < IMPROV_EXCLUSIVE_MS && improvActive) {
        improvTickInternal();
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("[provision] improv-exclusive: STA up — done");
            return;
        }
        delay(10);
    }
    // Post-loop re-check: onImprovConnected setzt improvActive=false NACHDEM
    // WiFi.begin() bereits WL_CONNECTED gemeldet hat. Die Loop-Bedingung
    // sieht improvActive=false und exit'd, OHNE die WL_CONNECTED-Pruefung
    // im Loop-Body ein letztes Mal zu durchlaufen. Wenn wir hier nicht
    // re-checken, oeffnen wir captive ueber einer aktiven STA — softAP
    // belegt port 80, captiveStop()->end() haelt das port-80-binding noch
    // ein paar Sekunden in TIME_WAIT, uiServer.begin() schlaegt fehl.
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[provision] improv-exclusive: STA up at loop-exit — skipping captive");
        return;
    }
    captiveStart();

    Serial.println("[provision] no NVS connect — waiting on improv OR captive");
    uint32_t connectedAtMs = 0;
    constexpr uint32_t POST_CONNECT_GRACE_MS = 15 * 1000;
    while (improvActive || captiveIsActive()) {
        improvTickInternal();
        captiveTick();
        if (WiFi.status() == WL_CONNECTED && connectedAtMs == 0) {
            connectedAtMs = millis();
            improvLastActivityMs = connectedAtMs;
            sendGratuitousArpBroadcast();   // P0b: Mesh-ARP-Cache priming
            Serial.println("[provision] STA up — keeping captive alive for 15s grace "
                           "(browser still polling /save_status)");
        }
        if (connectedAtMs && millis() - connectedAtMs > POST_CONNECT_GRACE_MS) {
            Serial.println("[provision] grace expired — tearing down captive AP");
            break;
        }
        delay(10);
    }
    captiveStop();

    if (WiFi.status() == WL_CONNECTED) {
        // Fix Port-80-Bind-Race (2026-07-02, HW-verifiziert am C5): der
        // Captive-AsyncWebServer haelt das Port-80-Binding nach end() noch
        // Sekunden im lwIP-Close-Lag — uiServer.begin() im weiteren setup()
        // bindet dann ins Leere (AsyncWebServer meldet Bind-Fail nicht) und
        // die Web-UI bleibt bis zum naechsten Reboot tot, waehrend :8000 und
        // ICMP normal laufen. Der User haelt das Provisioning fuer
        // gescheitert, obwohl es geklappt hat. Creds sind hier bereits
        // persistiert (captiveTick-Success-Zweig bzw. onImprovConnected) →
        // kontrollierter Neustart in den sauberen, getesteten NVS-Boot-Pfad
        // statt Port-80-Roulette. successHtml kuendigt den Restart an
        // (Redirect nach ~35 s).
        Serial.println("[provision] captive-phase connect OK — restart into clean NVS boot");
        delay(500);   // Serial-Flush
        ESP.restart();
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[provision] all windows expired, no provisioning — restart in 10s");
        delay(10000);
        ESP.restart();
    }
}

void factoryResetWifi() {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    prefs.clear();
    prefs.end();
    Serial.println("[wifi] NVS credentials erased");
}

void wifiProvisioningTick() {
    improvTickInternal();
}

bool improvIsActive() { return improvActive; }

uint32_t improvWindowRemainingS() {
    if (!improvActive) return 0;
    const uint32_t idle = millis() - improvLastActivityMs;
    return idle >= improvIdleMs ? 0 : (improvIdleMs - idle) / 1000;
}

// Public-API-Variante (Header-deklariert) — delegiert auf die anonyme
// Helper-Variante oben, damit captive_portal.cpp denselben NVS-Pfad nutzt.
void persistCreds(const String& ssid, const String& psk) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putString(KEY_SSID, ssid);
    p.putString(KEY_PSK,  psk);
    p.end();
    Serial.printf("[wifi] credentials persisted (ssid=%s)\n", ssid.c_str());
}

void wifiOptimizeForReliability() {
    // 1) WiFi-Modem-Sleep KOMPLETT AUS. Default ist WIFI_PS_MIN_MODEM
    //    (DTIM-basiert), das auf C3/C6 mit WiFi 6 zu Ping-Latenzen
    //    > 1 s fuehrt — und damit zu spuerbaren Verzoegerungen bzw
    //    Hangs beim Speaker, wenn er die Cloud-Endpoints abfragt.
    WiFi.setSleep(WIFI_PS_NONE);

    // 2) TX-Power auf Maximum.
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    // 3) Auto-Reconnect explizit ein.
    WiFi.setAutoReconnect(true);

    // 4) Keine WiFi-Creds in den internen ESP-WiFi-NVS spiegeln (wir
    //    persistieren selbst). Spart Flash-Schreibzyklen + verhindert
    //    konkurrierende Quellen.
    WiFi.persistent(false);

    // 5) CPU auf das Chip-Maximum pinnen. arduino-esp32 cappt NICHT still —
    //    setCpuFrequencyMhz(240) loggt auf C3/C6 einen Fehler ("could not be
    //    set to 240 MHz", Discussion #28). Daher explizit den Chip-Max waehlen:
    //    S3/Classic 240 MHz, C3/C6 160 MHz.
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32)
    setCpuFrequencyMhz(240);
#else
    setCpuFrequencyMhz(160);  // C3/C6 Maximum
#endif

    Serial.printf("[wifi] PS=NONE TX=max CPU=%lu MHz — optimized for reliability\n",
                  (unsigned long)getCpuFrequencyMhz());
}

} // namespace sixback

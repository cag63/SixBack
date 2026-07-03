// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "captive_portal.h"
#include "wifi_provisioning.h"
#include "config.h"
#include "version.h"

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

namespace sixback {

namespace {

// Idle-Fenster: jeder HTTP-Request des Portals touched startMs (siehe
// captiveTouch_). Bis 2026-07-02 war das Fenster ABSOLUT ab captiveStart —
// das Portal starb nach 5 min auch mitten in der User-Eingabe (danach
// ESP.restart via provisionWifi = "Reboot-Schleife waehrend der Eingabe").
// Das Hard-Cap verhindert, dass periodische OS-Captive-Detection-Pings
// eines haengengebliebenen Clients das Fenster ewig offen halten.
constexpr uint32_t  CAPTIVE_WINDOW_MS  =  5 * 60 * 1000;   // 5 min idle
constexpr uint32_t  CAPTIVE_HARDCAP_MS = 30 * 60 * 1000;   // absolutes Cap
constexpr uint16_t  CAPTIVE_PORT      = 80;
constexpr uint16_t  DNS_PORT          = 53;
const     IPAddress AP_IP(192, 168, 4, 1);
const     IPAddress AP_NETMASK(255, 255, 255, 0);
// Offener AP — kein PSK. Schwester-Pattern aus ip4knx / TUL-KNX-Gateway:
// dort laeuft der Captive-AP auf ESP32-C3 und C6 ohne PSK zuverlaessig.
// Vorteil fuer User: AP-Beitritt ist Ein-Klick, Captive-Popup geht auf
// allen Handys automatisch auf (iOS/Android erkennen Open-Netze als
// "Hotspot ohne Internet" und triggern den Detect-Endpoint).

DNSServer       dnsServer;
AsyncWebServer* captiveServer = nullptr;
bool            active        = false;
uint32_t        startMs       = 0;   // idle-Fenster-Anker, per captiveTouch_ verlaengert
uint32_t        hardStartMs   = 0;   // absoluter Fenster-Anker (Hard-Cap)
String          apSsid;
String          provisionedSta;   // empty = pending, "x.x.x.x" once STA up

// Async save-state-machine. handleSave kickt einen non-blocking
// WiFi.begin() an und kehrt sofort mit einer Progress-HTML zurueck.
// captiveTick() pollt WiFi.status() und transitioniert.
// /save_status liefert JSON fuer das JS-Polling im Browser.
enum class SaveState : uint8_t { Idle, Connecting, Success, Failed };
SaveState   saveState      = SaveState::Idle;
String      saveSsid;
String      savePsk;
uint32_t    saveStartMs    = 0;
constexpr uint32_t SAVE_TIMEOUT_MS = 20 * 1000;

// Jeder Portal-HTTP-Request verlaengert das Idle-Fenster.
void captiveTouch_() { startMs = millis(); }

// Async-Scan-State-Machine (Fix 2026-07-02). Der fruehere handleScan pollte
// scanComplete() bis 15 s IM async_tcp-Task. Der ist per Default am Task-WDT
// subscribed (CONFIG_ASYNC_TCP_USE_WDT, 5 s, trigger_panic) und feedet nur
// ZWISCHEN Events — delay() im Handler yieldet, fuettert aber nicht. Auf dem
// C5 dauert der Dual-Band-Scan ~9 s -> jeder Portal-Seitenaufruf (formHtml
// feuert fetch('/scan') automatisch) panic-rebootete das Geraet nach exakt
// 5 s (HW-verifiziert am C5, UART0: "task_wdt: - async_tcp" -> rst:0xc).
// 2,4-GHz-Chips blieben mit ~4 s Scan knapp unter der Grenze. Daher jetzt:
// Handler kickt nur an und antwortet sofort (rc:-1), captiveTick() pollt im
// loop-Task, das Formular-JS pollt /scan bis rc >= 0.
//
// Ergebnis-Puffer bewusst festes char[] statt Arduino-String: geschrieben im
// loop-Task (captiveTick), gelesen im async_tcp-Handler (Cross-Task-Regel).
// State-Uebergaenge: Idle->Running nur im Handler, Running->Ready nur im
// Tick, Ready->Idle nur im Handler — der Puffer wird nie beschrieben,
// waehrend ein Handler ihn lesen kann.
enum class ScanState : uint8_t { Idle, Running, Ready };
volatile ScanState scanState = ScanState::Idle;
char scanJsonBuf[3072] = "{\"networks\":[],\"rc\":0}";

void buildScanJson_(int n) {
    size_t off = (size_t)snprintf(scanJsonBuf, sizeof(scanJsonBuf), "{\"networks\":[");
    for (int i = 0; i < n && off + 100 < sizeof(scanJsonBuf); ++i) {
        char ssid[68]; size_t k = 0;
        String s = WiFi.SSID(i);
        for (size_t j = 0; j < s.length() && k < sizeof(ssid) - 2; ++j) {
            char ch = s[j];
            if (ch == '"' || ch == '\\') ssid[k++] = '\\';
            ssid[k++] = ch;
        }
        ssid[k] = 0;
        off += (size_t)snprintf(scanJsonBuf + off, sizeof(scanJsonBuf) - off,
                                "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                                i ? "," : "", ssid, (int)WiFi.RSSI(i),
                                WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false");
    }
    snprintf(scanJsonBuf + off, sizeof(scanJsonBuf) - off, "],\"rc\":%d}", n);
}

// Minimal-HTML Form (~1.2 KB), keine externen Assets — funktioniert ohne
// Internet, was im Captive-Portal Pflicht ist.
String formHtml() {
    return F(
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<title>SixBack \xe2\x80\x94 WiFi Setup</title>"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<style>body{font:-apple-system,Segoe UI,sans-serif;max-width:28em;margin:1.5em auto;padding:0 1em;color:#222;background:#f7f6f3}"
"h1{color:#7a3e00;margin:0 0 .2em}label{display:block;margin:.9em 0 .25em;font-weight:600;font-size:.9em}"
"input,select{width:100%;padding:.45em;font:inherit;border:1px solid #e1ddd2;border-radius:4px;background:#fff}"
"button{margin-top:1.2em;width:100%;padding:.7em;background:#7a3e00;color:#fff;border:0;border-radius:5px;font:inherit;font-weight:600;cursor:pointer}"
"button:hover{background:#5a2d00}p{color:#777;font-size:.9em}</style></head><body>"
"<h1>SixBack \xe2\x80\x94 WiFi Setup</h1>"
"<p>Pick your home network and enter its password.</p>"
"<form method=\"post\" action=\"/save\">"
"<label>Network</label>"
"<select id=\"pick\" onchange=\"document.getElementById('ssid').value=this.value\">"
"<option value=\"\">\xe2\x80\x94 scanning \xe2\x80\xa6 \xe2\x80\x94</option></select>"
"<label>SSID</label><input type=\"text\" id=\"ssid\" name=\"ssid\" required>"
"<label>Password</label><input type=\"password\" name=\"psk\" placeholder=\"(empty for open networks)\">"
"<button type=\"submit\">Save &amp; connect</button></form>"
"<script>function ld(t){fetch('/scan').then(r=>r.json()).then(d=>{"
"if(d.rc===-1){if(t<30)setTimeout(()=>ld(t+1),1000);return;}"
"const s=document.getElementById('pick');"
"s.innerHTML='<option value=\"\">\xe2\x80\x94 pick one \xe2\x80\x94</option>'+"
"(d.networks||[]).map(n=>`<option value=\"${n.ssid}\">${n.ssid} (${n.rssi} dBm${n.open?', open':''})</option>`).join('');"
"}).catch(()=>{if(t<30)setTimeout(()=>ld(t+1),1500);});}ld(0);</script></body></html>");
}

String successHtml(const String& staIp) {
    String h = F(
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<title>SixBack \xe2\x80\x94 Connected</title>"
// 35 s statt 20 s: nach dem Captive-Success rebootet das Geraet kontrolliert
// in den NVS-Boot-Pfad (Port-80-Bind-Fix, siehe provisionWifi) — Reconnect +
// uiServer brauchen zusammen ~10-15 s, erst dann ist das Redirect-Ziel da.
"<meta http-equiv=\"refresh\" content=\"35;url=http://");
    h += staIp;
    h += F("/\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<style>body{font:-apple-system,Segoe UI,sans-serif;max-width:28em;margin:1.5em auto;padding:0 1em;color:#222;background:#f7f6f3}"
"h1{color:#1f7a3a;margin:0 0 .3em}p{color:#444}code{background:#fee;padding:.1em .35em;border-radius:3px}"
"a{color:#7a3e00}</style></head><body><h1>Connected!</h1>"
"<p>SixBack is now on your LAN at <a href=\"http://");
    h += staIp;
    h += F("/\">http://");
    h += staIp;
    h += F("/</a> (or <code>http://sixback.local/</code>).</p>"
"<p><b>Switch your phone back to your normal Wi-Fi.</b> SixBack restarts "
"once to finish setup; this page will redirect automatically in about half "
"a minute \xe2\x80\x94 which of course only works once you are on the right "
"network.</p>"
"<p style=\"color:#777;font-size:.85em\">SixBack " FW_VERSION_STRING "</p>"
"</body></html>");
    return h;
}

void handleRoot(AsyncWebServerRequest* req) {
    captiveTouch_();
    if (provisionedSta.length() > 0) {
        req->send(200, "text/html; charset=utf-8", successHtml(provisionedSta));
        return;
    }
    req->send(200, "text/html; charset=utf-8", formHtml());
}

void handleScan(AsyncWebServerRequest* req) {
    // NIE im Handler auf den Scan warten (Task-WDT-Panic, siehe Kommentar an
    // der ScanState-Machine oben). Sync-scanNetworks() waere noch schlimmer:
    // blockiert im AP_STA-Mode > 60 s (live verifiziert).
    captiveTouch_();
    if (scanState == ScanState::Ready) {
        req->send(200, "application/json", scanJsonBuf);
        scanState = ScanState::Idle;   // one-shot: naechster Aufruf scannt frisch
        return;
    }
    if (scanState == ScanState::Idle) {
        WiFi.scanDelete();
        WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
        scanState = ScanState::Running;
    }
    req->send(200, "application/json", "{\"networks\":[],\"rc\":-1}");
}

String progressHtml(const String& ssid) {
    String h = F(
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<title>SixBack \xe2\x80\x94 Connecting</title>"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<style>body{font:-apple-system,Segoe UI,sans-serif;max-width:28em;margin:1.5em auto;padding:0 1em;color:#222;background:#f7f6f3}"
"h1{color:#7a3e00;margin:0 0 .3em}p{color:#444}"
"#status{font-size:1.05em;margin:1em 0;padding:.7em;border-radius:5px;background:#fff;border:1px solid #e1ddd2}"
".ok{color:#1f7a3a}.err{color:#a32525}.busy{color:#7a3e00}"
"a{color:#7a3e00}</style></head><body>"
"<h1>Connecting\xe2\x80\xa6</h1>"
"<p>SixBack is associating with <b>");
    h += ssid;
    h += F("</b>. This may take up to 20 seconds.</p>"
"<div id=\"status\" class=\"busy\">\xe2\x8f\xb3 connecting\xe2\x80\xa6</div>"
"<p><a href=\"/\">Back to setup form</a></p>"
"<script>"
"async function poll(){"
" try{const r=await fetch('/save_status',{cache:'no-store'});const d=await r.json();"
"   const s=document.getElementById('status');"
"   if(d.state==='success'){s.className='ok';s.textContent='\xe2\x9c\x93 connected as '+d.sta_ip+' \xe2\x80\x94 loading\xe2\x80\xa6';setTimeout(()=>location.href='/',800);return;}"
"   if(d.state==='failed'){s.className='err';s.textContent='\xe2\x9c\x97 connect failed \xe2\x80\x94 wrong password? Use Back to retry.';return;}"
"   s.textContent='\xe2\x8f\xb3 connecting ('+d.elapsed_s+'s)\xe2\x80\xa6';"
" }catch(e){/* AP-channel-hop \xe2\x80\x94 retry */}"
" setTimeout(poll,1500);"
"}"
"poll();"
"</script></body></html>");
    return h;
}

void handleSave(AsyncWebServerRequest* req) {
    captiveTouch_();
    if (!req->hasParam("ssid", true)) {
        req->send(400, "text/plain", "Missing ssid");
        return;
    }
    String ssid = req->getParam("ssid", true)->value();
    String psk  = req->hasParam("psk", true) ? req->getParam("psk", true)->value() : "";

    // Re-Submit waehrend bereits laufender Connect-Versuch: nur Progress-Page.
    // Bei Success/Failed/Idle starten wir einen neuen Versuch.
    if (saveState != SaveState::Connecting) {
        Serial.printf("[captive] async save ssid=%s\n", ssid.c_str());
        saveSsid    = ssid;
        savePsk     = psk;
        saveStartMs = millis();
        saveState   = SaveState::Connecting;
        // Non-blocking — captiveTick pollt WiFi.status() ab jetzt.
        WiFi.begin(ssid.c_str(), psk.c_str());
    } else {
        Serial.printf("[captive] save while already connecting (existing ssid=%s, new=%s)\n",
                      saveSsid.c_str(), ssid.c_str());
    }
    req->send(200, "text/html; charset=utf-8", progressHtml(saveSsid));
}

void handleSaveStatus(AsyncWebServerRequest* req) {
    captiveTouch_();
    const char* st = "idle";
    switch (saveState) {
        case SaveState::Idle:       st = "idle";       break;
        case SaveState::Connecting: st = "connecting"; break;
        case SaveState::Success:    st = "success";    break;
        case SaveState::Failed:     st = "failed";     break;
    }
    String body = "{\"state\":\"";
    body += st;
    body += "\",\"ssid\":\"";
    body += saveSsid;
    body += "\",\"sta_ip\":\"";
    body += provisionedSta;
    body += "\",\"elapsed_s\":";
    body += String(saveState == SaveState::Connecting ? (millis() - saveStartMs) / 1000 : 0);
    body += "}";
    AsyncWebServerResponse* r = req->beginResponse(200, "application/json", body);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
}

void handleCaptiveRedirect(AsyncWebServerRequest* req) {
    captiveTouch_();
    req->redirect("http://" + AP_IP.toString() + "/");
}

}  // namespace

void captiveStart() {
    if (active) return;

    uint8_t mac[6]; WiFi.macAddress(mac);
    char buf[24];
    snprintf(buf, sizeof(buf), "SixBack-%02X%02X%02X", mac[3], mac[4], mac[5]);
    apSsid = buf;

    Serial.printf("[captive] starting open AP '%s' + DNS + HTTP on %s\n",
                  apSsid.c_str(), AP_IP.toString().c_str());

    WiFi.mode(WIFI_AP_STA);   // STA bleibt parallel, damit improv weiter
    WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
    WiFi.softAP(apSsid.c_str());   // kein PSK — offener AP

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", AP_IP);

    captiveServer = new AsyncWebServer(CAPTIVE_PORT);
    // Literaler Pfad "/" — captiveServer->on() ist KEIN Regex-Router (Regex
    // macht routeT in web_router.h). Ein Pattern wie "^/$" wird hier LITERAL
    // genommen -> ein echter "/"-Request matcht nie -> faellt auf onNotFound ->
    // handleCaptiveRedirect redirected "/" wieder auf "/" -> Endlosschleife
    // (ERR_TOO_MANY_REDIRECTS, Issue #12). Daher literaler Pfad.
    captiveServer->on("/",                         HTTP_GET,  handleRoot);
    captiveServer->on("/scan",                     HTTP_GET,  handleScan);
    captiveServer->on("/save",                     HTTP_POST, handleSave);
    captiveServer->on("/save_status",              HTTP_GET,  handleSaveStatus);
    // Captive-detection endpoints von Apple/Google/Microsoft — alle auf
    // unsere Form umleiten damit der Popup aufgeht.
    captiveServer->on("/hotspot-detect.html",      HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/library/test/success.html",HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/generate_204",             HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/gen_204",                  HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/connecttest.txt",          HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/ncsi.txt",                 HTTP_GET,  handleCaptiveRedirect);
    captiveServer->on("/redirect",                 HTTP_GET,  handleCaptiveRedirect);
    captiveServer->onNotFound(handleCaptiveRedirect);
    captiveServer->begin();

    active  = true;
    startMs     = millis();
    hardStartMs = startMs;
    provisionedSta = "";
    saveState = SaveState::Idle;
    saveSsid  = "";
    savePsk   = "";
    saveStartMs = 0;
    scanState = ScanState::Idle;
}

void captiveStop() {
    if (!active) return;
    Serial.println("[captive] stopping");
    if (captiveServer) {
        captiveServer->end();
        delete captiveServer;
        captiveServer = nullptr;
    }
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    active = false;
}

void captiveTick() {
    if (!active) return;
    dnsServer.processNextRequest();

    // Async-Scan antreiben (laeuft im loop-Task, NIE im async_tcp-Handler —
    // Task-WDT, siehe ScanState-Kommentar).
    if (scanState == ScanState::Running) {
        const int n = WiFi.scanComplete();
        if (n >= 0 || n == WIFI_SCAN_FAILED) {
            buildScanJson_(n);
            WiFi.scanDelete();
            scanState = ScanState::Ready;
            Serial.printf("[captive] scan done, rc=%d\n", n);
        }
    }

    // Async-Save-State-Machine antreiben.
    if (saveState == SaveState::Connecting) {
        if (WiFi.status() == WL_CONNECTED) {
            provisionedSta = WiFi.localIP().toString();
            persistCreds(saveSsid, savePsk);
            wifiOptimizeForReliability();
            saveState = SaveState::Success;
            Serial.printf("[captive] async connect OK, sta-IP=%s\n", provisionedSta.c_str());
        } else if (millis() - saveStartMs > SAVE_TIMEOUT_MS) {
            Serial.println("[captive] async connect timeout");
            WiFi.disconnect(true);
            saveState = SaveState::Failed;
        }
    }

    // Fenster-Expiry: idle-basiert + Hard-Cap. Ein laufender Connect-Versuch
    // blockiert das Expiry — sonst risse der Fenster-Ablauf einen /save kurz
    // vor Schluss ab, BEVOR persistCreds je gelaufen ist (Creds-Loss-Race:
    // korrektes Passwort, aber nie gespeichert -> Endlosschleife).
    if (saveState != SaveState::Connecting) {
        const bool hardExpired = millis() - hardStartMs > CAPTIVE_HARDCAP_MS;
        if (hardExpired || millis() - startMs > CAPTIVE_WINDOW_MS) {
            Serial.printf("[captive] window expired (%s)\n", hardExpired ? "hard-cap" : "idle");
            captiveStop();
        }
    }
}

bool captiveIsActive() { return active; }

uint32_t captiveWindowRemainingS() {
    if (!active) return 0;
    uint32_t e = millis() - startMs;
    return e >= CAPTIVE_WINDOW_MS ? 0 : (CAPTIVE_WINDOW_MS - e) / 1000;
}

}  // namespace sixback

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "zone_manager.h"
#include "config.h"
#include "speaker_inventory.h"

#include <HTTPClient.h>

namespace sixback {
namespace {

// Liefert das Attribut `attr` aus dem ersten <tag ...>-Header in `xml`.
String attrIn_(const String& xml, const String& tag, const String& attr) {
    int t = xml.indexOf("<" + tag);
    if (t < 0) return String();
    int e = xml.indexOf('>', t);
    if (e < 0) return String();
    String hdr = xml.substring(t, e);
    String needle = attr + "=\"";
    int s = hdr.indexOf(needle);
    if (s < 0) return String();
    s += needle.length();
    int q = hdr.indexOf('"', s);
    return q > s ? hdr.substring(s, q) : String();
}

// Baut einen <member>-Block: <member ipaddress="<ip>"><deviceId></member>.
String memberXml_(const String& ip, const String& deviceId) {
    String m = "<member ipaddress=\"";
    m += ip;
    m += "\">";
    m += deviceId;
    m += "</member>";
    return m;
}

// Resolve deviceId -> (ip, deviceId, name). Liefert false bei unbekannter id.
// Caller haelt KEINEN LockGuard — diese Funktion nimmt ihn selbst und kopiert
// die Felder raus (der Speaker*-Pointer dangelt sonst bei vector-realloc).
bool resolveMember_(const String& id, ZoneMember& out) {
    auto& inv = SpeakerInventory::instance();
    SpeakerInventory::LockGuard g(inv);
    auto* sp = inv.findById(id);
    if (!sp) return false;
    out.deviceId = sp->deviceId;
    out.ip       = sp->ip;
    out.name     = sp->name;
    return true;
}

// Setzt member.name aus dem Inventory (fuer Member die nur via deviceId/ip
// aus /getZone bekannt sind). Laesst name leer wenn nicht gefunden.
void fillName_(ZoneMember& m) {
    auto& inv = SpeakerInventory::instance();
    SpeakerInventory::LockGuard g(inv);
    auto* sp = inv.findById(m.deviceId);
    if (sp) m.name = sp->name;
}

// Parst die <member>-Bloecke aus einem /getZone-XML in `out`.
void parseMembers_(const String& xml, std::vector<ZoneMember>& out) {
    int pos = 0;
    while (true) {
        int t = xml.indexOf("<member", pos);
        if (t < 0) break;
        int hdrEnd = xml.indexOf('>', t);
        if (hdrEnd < 0) break;
        String hdr = xml.substring(t, hdrEnd);
        String ip;
        String needle = "ipaddress=\"";
        int s = hdr.indexOf(needle);
        if (s >= 0) {
            s += needle.length();
            int q = hdr.indexOf('"', s);
            if (q > s) ip = hdr.substring(s, q);
        }
        int close = xml.indexOf("</member>", hdrEnd);
        if (close < 0) break;
        String devId = xml.substring(hdrEnd + 1, close);
        devId.trim();
        if (devId.length()) {
            ZoneMember m;
            m.deviceId = devId;
            m.ip       = ip;
            fillName_(m);
            out.push_back(m);
        }
        pos = close + 9;  // len("</member>")
    }
}

} // anon

int bmxPost(const String& ip, const char* path, const String& body) {
    HTTPClient h;
    h.setReuse(false);
    // Kurze, explizite Timeouts: diese POSTs laufen synchron im AsyncWebServer-
    // Handler-Thread; ohne Timeout blockt ein offline-Speaker den async_tcp-Task
    // den HTTPClient-Default (5 s) lang. Werte gespiegelt von den bestehenden
    // :8090-Callern (api_endpoints.cpp /presets, /listMediaServers).
    h.setConnectTimeout(1500);
    h.setTimeout(3000);
    String u = "http://" + ip + ":" + String(BOSE_BMX_PORT) + path;
    if (!h.begin(u)) return -1;
    h.addHeader("Content-Type", "text/xml");
    int rc = h.POST(body);
    h.end();
    return rc;
}

int zoneCreate(const String& masterId, const std::vector<String>& slaveIds) {
    ZoneMember master;
    if (!resolveMember_(masterId, master)) return -1;

    std::vector<ZoneMember> slaves;
    slaves.reserve(slaveIds.size());
    for (const auto& sid : slaveIds) {
        ZoneMember m;
        if (!resolveMember_(sid, m)) return -1;
        slaves.push_back(m);
    }

    // Master ist der ERSTE <member>; Attr-Namen exakt master / senderIPAddress.
    String body = "<zone master=\"";
    body += master.deviceId;
    body += "\" senderIPAddress=\"";
    body += master.ip;
    body += "\">";
    body += memberXml_(master.ip, master.deviceId);
    for (const auto& s : slaves) body += memberXml_(s.ip, s.deviceId);
    body += "</zone>";

    int rc = bmxPost(master.ip, "/setZone", body);
    return (rc == 200) ? 0 : -2;
}

int zoneAdd(const String& masterId, const String& slaveId) {
    ZoneMember master, slave;
    if (!resolveMember_(masterId, master)) return -1;
    if (!resolveMember_(slaveId, slave))  return -1;

    String body = "<zone master=\"";
    body += master.deviceId;
    body += "\">";
    body += memberXml_(slave.ip, slave.deviceId);
    body += "</zone>";

    int rc = bmxPost(master.ip, "/addZoneSlave", body);
    return (rc == 200) ? 0 : -2;
}

int zoneRemove(const String& masterId, const String& slaveId) {
    ZoneMember master, slave;
    if (!resolveMember_(masterId, master)) return -1;
    if (!resolveMember_(slaveId, slave))  return -1;

    String body = "<zone master=\"";
    body += master.deviceId;
    body += "\">";
    body += memberXml_(slave.ip, slave.deviceId);
    body += "</zone>";

    int rc = bmxPost(master.ip, "/removeZoneSlave", body);
    return (rc == 200) ? 0 : -2;
}

int zoneDissolve(const String& masterId) {
    ZoneView zv = zoneStatus(masterId);
    if (zv.masterId.length() == 0) return -1;  // unbekannte id
    if (!zv.inZone) return 0;                   // nichts zu tun

    // Sequenziell EINEN Slave pro removeZoneSlave entfernen (multi-member
    // teardown unverifiziert). Member[0] ist der Master selbst -> skippen.
    // Removing the last slave loest die Zone auf.
    bool anyFail = false;
    for (size_t i = 1; i < zv.members.size(); ++i) {
        int rc = zoneRemove(zv.masterId, zv.members[i].deviceId);
        if (rc != 0) anyFail = true;
    }
    return anyFail ? -2 : 0;
}

ZoneView zoneStatus(const String& anyDeviceId) {
    ZoneView view;

    ZoneMember any;
    if (!resolveMember_(anyDeviceId, any)) return view;  // masterId bleibt leer

    HTTPClient h;
    h.setReuse(false);
    h.setConnectTimeout(1500);
    h.setTimeout(3000);
    String u = "http://" + any.ip + ":" + String(BOSE_BMX_PORT) + "/getZone";
    if (!h.begin(u)) { view.masterId = any.deviceId; return view; }
    int code = h.GET();
    if (code != 200) { h.end(); view.masterId = any.deviceId; return view; }
    String xml = h.getString();
    h.end();

    // Leere Zone: <zone /> (kein master-Attribut, keine <member>).
    String masterAttr = attrIn_(xml, "zone", "master");
    if (masterAttr.length() == 0) {
        // Nicht in einer Zone.
        view.masterId = any.deviceId;
        view.masterIp = any.ip;
        view.inZone   = false;
        return view;
    }

    // /getZone ist master/slave-asymmetrisch: das master-Attribut ist beim
    // Master die eigene deviceId, beim Slave die des Masters. `any` ist also
    // genau dann ein Slave, wenn das master-Attribut auf ein ANDERES Geraet
    // zeigt (senderIsMaster="true" traegt der Slave redundant zusaetzlich).
    bool anyIsSlave = (masterAttr != any.deviceId);

    if (anyIsSlave) {
        // Slave-View: master= traegt die Master-deviceId, senderIPAddress die
        // erreichbare Master-IP (am Draht verifiziert). Wir re-GETten den Master
        // bewusst NICHT — dessen volle Member-Liste rendert sein eigener Poll,
        // und ein Re-GET pro Slave-Tick waere reine Amplifikation. Die Slave-
        // Karte zeigt ohnehin nur "part of <master>", braucht also keine Member.
        view.inZone   = true;
        view.masterId = masterAttr;
        String si = attrIn_(xml, "zone", "senderIPAddress");
        if (si.length()) {
            view.masterIp = si;
        } else {
            ZoneMember m;
            if (resolveMember_(masterAttr, m)) view.masterIp = m.ip;
        }
        return view;
    }

    // Master- (oder Standalone-) View: volle Member-Liste aus der eigenen
    // Antwort parsen (Master zuerst, keine Inventory-Abhaengigkeit).
    view.inZone   = true;
    view.masterId = masterAttr;
    view.masterIp = any.ip;
    parseMembers_(xml, view.members);
    return view;
}

} // namespace sixback

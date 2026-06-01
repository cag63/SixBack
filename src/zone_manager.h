// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Device-direct Multiroom (ZoneManager)
//
// Orchestriert SoundTouch-Multiroom-Zonen ueber die speaker-eigene BMX-API
// auf Port 8090 (/setZone, /addZoneSlave, /removeZoneSlave, /getZone).
//
// Stateless: eine Zone ist Runtime-Geraete-State, gehalten vom Master, und
// ueberlebt keinen Speaker-Reboot. SixBack persistiert NICHTS und liest die
// Live-Wahrheit aus dem /getZone des Masters. Dies ist eine ANDERE Schicht
// als der cloud-seitige Group-Store (PUT /api/speaker/{id}/group +
// /api/group/sync + Speaker.groupId) — die bleibt unangetastet.
//
// Wire-Contract (verifiziert ST10/ST30, FW 27.0.x): alle POSTs gehen an den
// MASTER, Content-Type text/xml; der Master ist der ERSTE <member>; das
// <member>-Attribut ist lowercase `ipaddress`; der Element-Text ist die
// SCM-deviceId (/info deviceID), NICHT die Wi-Fi-MAC.
#ifndef BOSEFIX32_ZONE_MANAGER_H
#define BOSEFIX32_ZONE_MANAGER_H

#include <Arduino.h>
#include <vector>

namespace sixback {

struct ZoneMember {
    String deviceId;
    String ip;
    String name;
};

struct ZoneView {
    bool inZone = false;
    String masterId;
    String masterIp;
    std::vector<ZoneMember> members;  // Master zuerst (so wie /getZone es liefert)
};

// Geteilter HTTP-Helper: POST nach http://<ip>:BOSE_BMX_PORT<path> mit
// text/xml-Body. Liefert den HTTP-Statuscode, oder negativ wenn begin()
// scheitert (mirror der handleSpeakerKey-Konvention: -1 bei begin-Fehler).
int bmxPost(const String& ip, const char* path, const String& body);

// Zone-Operationen. IPs werden via SpeakerInventory::findById() aufgeloest.
// Rueckgabe: 0 ok / -1 unbekannte deviceId / -2 HTTP-Fehler.
int zoneCreate  (const String& masterId, const std::vector<String>& slaveIds);
int zoneAdd     (const String& masterId, const String& slaveId);
int zoneRemove  (const String& masterId, const String& slaveId);
int zoneDissolve(const String& masterId);   // liest /getZone, removeZoneSlave pro Member

// GET /getZone am Master (Resolve anyDeviceId -> Master), Namen aus Inventory.
// Bei unbekannter id liefert ZoneView mit leerem masterId zurueck.
ZoneView zoneStatus(const String& anyDeviceId);

} // namespace sixback

#endif // BOSEFIX32_ZONE_MANAGER_H

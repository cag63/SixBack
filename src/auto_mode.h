// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Zero-Touch Auto-Migration
//
// Wenn aktiviert (NVS-Flag), laeuft beim Boot eine Pipeline:
//   1. Wait bootDelayMs nach WiFi-Connect, damit Server up sind.
//   2. SpeakerInventory::discover() (SSDP + known-IP-Probe + /24-Scan).
//   3. Pro Speaker mit (cloudUrl != myBase && !ownedByUs && Modell/FW-Whitelist):
//      a) GET /presets → Source-Normalizer → PresetStore (Snapshot vor Migration).
//      b) Telnet migrateSpeaker() → 6 sys-configuration-Kommandos + reboot.
//      c) Warten bis /info wieder antwortet (<= 180 s).
//      d) ownedByUs=true, NVS-persistent.
//   4. handleAccountFull embedded jetzt die normalisierten Presets im
//      account/full-XML → Speaker sync't sie an, kein Long-Press noetig
//      (Lessons-Item 7).
//
// Gated durch AutoModeConfig::enabled (default false). Hard-Limit
// AutoModeConfig::maxPerBoot (default 1) als Foot-Gun-Guard.
#ifndef BOSEFIX32_AUTO_MODE_H
#define BOSEFIX32_AUTO_MODE_H

#include <Arduino.h>

namespace bosefix {

struct AutoModeConfig {
    bool      enabled     = true;    // Image-Default: an, damit „flash → provision → migrate" zero-touch laeuft
    bool      dryRun      = false;
    uint32_t  bootDelayMs = 10000;
    uint32_t  maxPerBoot  = 4;       // typischer Haushalt 1-4 SoundTouch — alle in einem Boot durch
};

struct AutoModeStatus {
    bool      ran                = false;
    bool      running            = false;
    String    state              = "idle";  // "idle"/"discovering"/"import-presets"/"migrate-telnet"/"wait-reboot"/"done"
    String    currentDeviceId    = "";
    int       speakersSeen       = 0;
    int       speakersEligible   = 0;
    int       speakersMigrated   = 0;
    int       slotsNormalized    = 0;
    int       slotsConverted     = 0;
    int       slotsAbandoned     = 0;
    String    lastError          = "";
    uint32_t  startedMs          = 0;
    uint32_t  finishedMs         = 0;
};

AutoModeConfig loadAutoModeConfig();
void           saveAutoModeConfig(const AutoModeConfig& cfg);

// Spawn der FreeRTOS-Task, die die Pipeline ausfuehrt. Idempotent —
// mehrfache Aufrufe innerhalb eines Boots starten die Pipeline nur einmal.
// Wenn config.enabled == false: Task wird gestartet, prueft, beendet sich.
void           startAutoModeTask();

// Snapshot fuer /api/auto-mode.
AutoModeStatus getAutoModeStatus();

} // namespace bosefix

#endif

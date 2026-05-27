// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Diagnostic Sharing Settings
//
// Steuert ob/wohin Pre-Migrate-Diagnostic-Snapshots automatisch zum
// Maintainer-Receiver gepushed werden. Default OFF — siehe GitHub Issue
// #4: Auto-Migrate laeuft default-on und triggert Snapshot-Capture BEVOR
// der User die WebUI sieht, deshalb muss der Upload-Schalter auf
// Code-Ebene default-off sein, nicht nur im UI.
#ifndef SIXBACK_DIAG_SETTINGS_H
#define SIXBACK_DIAG_SETTINGS_H

#include <Arduino.h>

namespace sixback {

struct DiagShareConfig {
    bool uploadEnabled = false;   // Default OFF, opt-in via WebUI.
};

DiagShareConfig loadDiagShareConfig();
void             saveDiagShareConfig(const DiagShareConfig& cfg);

} // namespace sixback

#endif

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "diag_settings.h"
#include "nvs_helper.h"
#include <ArduinoJson.h>

namespace sixback {

namespace {
constexpr const char* NS  = "sixback-diag";
constexpr const char* KEY = "share";
}

DiagShareConfig loadDiagShareConfig() {
    DiagShareConfig cfg;
    JsonDocument doc;
    if (nvsLoadJson(NS, KEY, doc)) {
        cfg.uploadEnabled = doc["upload_enabled"] | false;
    }
    return cfg;
}

void saveDiagShareConfig(const DiagShareConfig& cfg) {
    JsonDocument doc;
    doc["upload_enabled"] = cfg.uploadEnabled;
    nvsSaveJson(NS, KEY, doc);
}

} // namespace sixback

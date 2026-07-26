// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "host_settings.h"
#include "config.h"
#include "nvs_helper.h"
#include <ArduinoJson.h>

namespace sixback {

namespace {
constexpr const char* NS  = "sixback-host";
constexpr const char* KEY = "name";
}

const String& hostname() {
    static String cached;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        cached = persistedHostname();
    }
    return cached;
}

String persistedHostname() {
    JsonDocument doc;
    if (nvsLoadJson(NS, KEY, doc)) {
        String v = doc["name"] | "";
        String err;
        if (v.length() && isValidHostname(v, err)) return v;
    }
    return String(MDNS_HOSTNAME);
}

bool isValidHostname(String& candidate, String& err) {
    candidate.trim();
    candidate.toLowerCase();
    if (candidate.isEmpty())     { err = "hostname empty"; return false; }
    if (candidate.length() > 24) { err = "max 24 characters"; return false; }
    if (candidate[0] == '-' || candidate[candidate.length() - 1] == '-') {
        err = "must not start or end with '-'";
        return false;
    }
    for (size_t i = 0; i < candidate.length(); i++) {
        char c = candidate[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            err = "allowed characters: a-z 0-9 -";
            return false;
        }
    }
    return true;
}

bool saveHostname(const String& candidate) {
    JsonDocument doc;
    doc["name"] = candidate.isEmpty() ? String(MDNS_HOSTNAME) : candidate;
    return nvsSaveJson(NS, KEY, doc);
}

} // namespace sixback

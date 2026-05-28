#pragma once
// ESP-side UPnP ContentDirectory browse — replaces the LAN-bound sixback-dlna
// proxy (sixback-backend/dlna/server.py) so the WebUI talks same-origin to the
// firmware instead of an external host that cannot reach the user's LAN.
//
// Two operations mirror the proxy:
//   dlnaDescribe(location)            — fetch MediaServerDevDesc.xml, return the
//                                       ContentDirectory:1 controlURL + name.
//   dlnaBrowse(controlUrl,objectId,…) — SOAP Browse(BrowseDirectChildren), parse
//                                       the doubly-encoded DIDL-Lite, return JSON.
//
// The DLNA leg is plain HTTP on the LAN (no TLS) so there is no mbedtls heap
// cost. The browse response is parsed with a single-pass STREAMING parser: the
// SOAP body is consumed in small reads, entity-unescaped on the fly, and each
// DIDL entry is serialised to a bounded output buffer as it is seen. Peak heap
// is a function of `count` (the page size) + per-entry scratch, NOT the total
// folder size — a folder with 10k tracks pages through `count` at a time without
// ever buffering the full response. This is what makes browse viable on the
// no-PSRAM targets (C3/C6/classic WROOM).
//
// Output JSON shape (matches what web-src/index.html consumes):
//   describe: {"ok":true,"name":"…","control_url":"http://…"}
//   browse:   {"ok":true,"object_id":"0","start":0,"returned":N,"total":M,
//              "containers":[{"id","title"[,"child_count"]},…],
//              "items":[{"id","title"[,"artist"][,"album"]},…]}
//   error:    {"ok":false,"detail":"…"}  (UI reads dj.detail / j.detail)

#include <Arduino.h>

// Compile-time gate. Default ON. RAM-tight targets that cannot afford the
// browse path set -DSIXBACK_DLNA_BROWSE=0; the endpoints then answer 501 with
// {"ok":false,"detail":"DLNA browse not available on this device"} and the UI
// shows "Feature not available".
#ifndef SIXBACK_DLNA_BROWSE
#define SIXBACK_DLNA_BROWSE 1
#endif

namespace sixback {

// Fetch + parse a MediaServer device descriptor. On success fills `out` with the
// describe JSON and returns 200. On failure fills `out` with an error JSON
// ({"ok":false,"detail":…}) and returns the HTTP-ish status to send (4xx/5xx).
int dlnaDescribe(const String& location, String& out);

// SOAP Browse(BrowseDirectChildren) against a ContentDirectory controlURL.
// Streams + parses the response into `out` (browse JSON) and returns 200, or
// fills `out` with an error JSON and returns 4xx/5xx.
int dlnaBrowse(const String& controlUrl, const String& objectId,
               long start, long count, String& out);

}  // namespace sixback

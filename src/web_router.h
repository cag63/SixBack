#pragma once
// Non-regex HTTP route matching for ESPAsyncWebServer.
//
// std::regex route matching (ASYNCWEBSERVER_REGEX) compiles one std::regex
// per "^…$" route; on RAM-tight targets (C3/C6/classic ESP32, no PSRAM) the
// ~60 NFA objects exhaust the heap. routeT() registers a handler that matches
// the same path templates by splitting on '/' (literal segments, "(…)" capture
// groups become single-segment wildcards, ".+"/".*" match the rest), with zero
// std::regex. Captured segments are read in-handler via pathParam(i), the
// drop-in replacement for AsyncWebServerRequest::pathArg(i).
#include <ESPAsyncWebServer.h>

namespace sixback {

void routeT(AsyncWebServer& server, const char* pattern,
            WebRequestMethodComposite method,
            ArRequestHandlerFunction onRequest,
            ArUploadHandlerFunction onUpload = nullptr,
            ArBodyHandlerFunction onBody = nullptr);

// i-th captured path segment of the route currently being handled.
const String& pathParam(size_t i);

}  // namespace sixback

using sixback::pathParam;
using sixback::routeT;

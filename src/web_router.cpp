#include "web_router.h"
#include <vector>

namespace sixback {
namespace {

// Captures of the route currently being matched/handled. Safe as a single
// shared buffer because AsyncTCP runs all handler callbacks sequentially on
// one task: canHandle() (which fills this) runs immediately before the body/
// request callbacks of the same request, with no other request interleaving.
std::vector<String> g_params;
const String EMPTY_PARAM;

struct Seg {
  uint8_t kind;   // 0=literal, 1=capture(single segment), 2=rest(.+/.*)
  String lit;
};

std::vector<String> splitPath(const String& s) {
  std::vector<String> out;
  int start = 0;
  while (true) {
    int slash = s.indexOf('/', start);
    out.push_back(slash < 0 ? s.substring(start) : s.substring(start, slash));
    if (slash < 0) break;
    start = slash + 1;
  }
  return out;
}

class TemplateRouteHandler : public AsyncWebHandler {
  WebRequestMethodComposite _method;
  std::vector<Seg> _segs;
  bool _restPlus = false;   // true for ".+" (>=1 remaining), false for ".*"
  ArRequestHandlerFunction _onRequest;
  ArUploadHandlerFunction _onUpload;
  ArBodyHandlerFunction _onBody;

 public:
  TemplateRouteHandler(const char* pattern, WebRequestMethodComposite method,
                       ArRequestHandlerFunction onRequest,
                       ArUploadHandlerFunction onUpload,
                       ArBodyHandlerFunction onBody)
      : _method(method), _onRequest(onRequest), _onUpload(onUpload), _onBody(onBody) {
    String p(pattern);
    if (p.startsWith("^")) p = p.substring(1);
    if (p.endsWith("$")) p = p.substring(0, p.length() - 1);
    // Split on '/' only at paren-depth 0 — capture groups like ([^/]+) contain
    // a literal '/' inside the char class and must stay a single token.
    std::vector<String> toks;
    {
      String tok;
      int depth = 0;
      for (size_t i = 0; i < p.length(); ++i) {
        char c = p[i];
        if (c == '/' && depth == 0) {
          toks.push_back(tok);
          tok = "";
          continue;
        }
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        tok += c;
      }
      toks.push_back(tok);
    }
    for (const String& tok : toks) {
      Seg s;
      if (tok == ".+" || tok == ".*") {
        s.kind = 2;
        _restPlus = (tok == ".+");
      } else if (tok.indexOf('(') >= 0) {
        s.kind = 1;
      } else {
        s.kind = 0;
        s.lit = tok;
      }
      _segs.push_back(s);
    }
  }

  bool matchInto(const String& url, std::vector<String>& caps) const {
    std::vector<String> u = splitPath(url);
    size_t ui = 0;
    for (size_t si = 0; si < _segs.size(); ++si) {
      const Seg& s = _segs[si];
      if (s.kind == 2) {  // rest: consume all remaining segments
        String rest;
        for (size_t k = ui; k < u.size(); ++k) {
          if (k > ui) rest += '/';
          rest += u[k];
        }
        if (_restPlus && rest.length() == 0) return false;
        caps.push_back(rest);
        return true;  // rest is always the final template segment
      }
      if (ui >= u.size()) return false;
      if (s.kind == 0) {
        if (u[ui] != s.lit) return false;
      } else {  // capture: any single non-empty segment
        if (u[ui].length() == 0) return false;
        caps.push_back(u[ui]);
      }
      ++ui;
    }
    return ui == u.size();
  }

  bool canHandle(AsyncWebServerRequest* request) const override {
    if (!_onRequest || !request->isHTTP() || !_method.matches(request->method())) {
      return false;
    }
    std::vector<String> caps;
    if (!matchInto(request->url(), caps)) return false;
    g_params = std::move(caps);
    return true;
  }

  void handleRequest(AsyncWebServerRequest* request) override {
    if (_onRequest) _onRequest(request);
    else request->send(404);
  }
  void handleUpload(AsyncWebServerRequest* request, const String& filename, size_t index,
                    uint8_t* data, size_t len, bool final) override {
    if (_onUpload) _onUpload(request, filename, index, data, len, final);
  }
  void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index,
                  size_t total) override {
    if (_onBody) _onBody(request, data, len, index, total);
  }
  bool isRequestHandlerTrivial() const override { return !_onRequest; }
};

}  // namespace

void routeT(AsyncWebServer& server, const char* pattern, WebRequestMethodComposite method,
            ArRequestHandlerFunction onRequest, ArUploadHandlerFunction onUpload,
            ArBodyHandlerFunction onBody) {
  server.addHandler(new TemplateRouteHandler(pattern, method, onRequest, onUpload, onBody));
}

const String& pathParam(size_t i) {
  return i < g_params.size() ? g_params[i] : EMPTY_PARAM;
}

}  // namespace sixback

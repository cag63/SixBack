#include "dlna_browse.h"

#include <WiFiClient.h>

namespace sixback {

namespace {

constexpr const char* CDS_TYPE =
    "urn:schemas-upnp-org:service:ContentDirectory:1";

// ---- limits (peak-heap guards) --------------------------------------------
constexpr uint32_t kConnectMs    = 4000;
constexpr uint32_t kIdleMs       = 8000;   // stall timeout on a slow server
constexpr size_t   kMaxDescBytes = 96 * 1024;   // device descriptor is small
constexpr size_t   kMaxOutBytes  = 48 * 1024;   // cap browse JSON per array
constexpr size_t   kMaxField     = 512;    // truncate a single title/artist/…
constexpr size_t   kMaxTag       = 2048;   // DIDL <res …> tags can be long

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
String errJson(const String& detail) {
    String s = "{\"ok\":false,\"detail\":\"";
    for (size_t i = 0; i < detail.length(); ++i) {
        char c = detail[i];
        if (c == '"' || c == '\\') { s += '\\'; s += c; }
        else if (c >= 0x20) s += c;
    }
    s += "\"}";
    return s;
}

void appendJsonEscaped(String& out, const String& s) {
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
                    out += buf;
                } else {
                    out += c;  // UTF-8 bytes pass through verbatim
                }
        }
    }
}

// localname of a tag token: strip leading '/', strip namespace prefix.
String localName(const String& token) {
    int s = 0;
    if (s < (int)token.length() && token[s] == '/') ++s;
    int colon = token.indexOf(':', s);
    int sp = token.indexOf(' ', s);
    int end = token.length();
    if (sp >= 0) end = sp;
    if (colon >= 0 && colon < end) s = colon + 1;
    int e = s;
    while (e < end) {
        char c = token[e];
        if (c == ' ' || c == '/' || c == '>' || c == '\t' ||
            c == '\r' || c == '\n')
            break;
        ++e;
    }
    return token.substring(s, e);
}

// value of attribute `name` in a tag string; tolerates ' or " quoting and
// requires a preceding space so `id` does not match `parentID`.
String tagAttr(const String& tag, const char* name) {
    const char quotes[2] = {'"', '\''};
    for (int qi = 0; qi < 2; ++qi) {
        char q = quotes[qi];
        String needle = " ";
        needle += name;
        needle += "=";
        needle += q;
        int s = tag.indexOf(needle);
        if (s < 0) continue;
        s += needle.length();
        int e = tag.indexOf(q, s);
        if (e > s) return tag.substring(s, e);
        if (e == s) return String();
    }
    return String();
}

// decode the 5 predefined entities + basic numeric refs (single char).
// Used for layer-2 unescaping of DIDL field text/attrs.
void unescapeInPlace(String& s) {
    if (s.indexOf('&') < 0) return;
    String out;
    out.reserve(s.length());
    size_t i = 0;
    while (i < s.length()) {
        char c = s[i];
        if (c != '&') { out += c; ++i; continue; }
        int semi = s.indexOf(';', i);
        if (semi < 0 || semi - (int)i > 10) { out += c; ++i; continue; }
        String ent = s.substring(i + 1, semi);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (ent.length() > 1 && ent[0] == '#') {
            long code = (ent[1] == 'x' || ent[1] == 'X')
                            ? strtol(ent.c_str() + 2, nullptr, 16)
                            : strtol(ent.c_str() + 1, nullptr, 10);
            if (code > 0 && code < 0x80) out += (char)code;
            else out += '?';  // multibyte numeric refs are rare in DIDL
        } else {
            out += s.substring(i, semi + 1);  // unknown entity: keep verbatim
        }
        i = semi + 1;
    }
    s = out;
}

// ---------------------------------------------------------------------------
// URL parsing + RFC1918 guard (browser, speaker and DLNA server are all LAN)
// ---------------------------------------------------------------------------
struct Url {
    String   scheme;
    String   host;
    uint16_t port = 0;
    String   path;
    bool     ok = false;
};

Url parseUrl(const String& u) {
    Url r;
    int s = u.indexOf("://");
    if (s < 0) return r;
    r.scheme = u.substring(0, s);
    r.scheme.toLowerCase();
    int hs = s + 3;
    int pe = u.indexOf('/', hs);
    String hostport = (pe < 0) ? u.substring(hs) : u.substring(hs, pe);
    r.path = (pe < 0) ? "/" : u.substring(pe);
    int colon = hostport.indexOf(':');
    if (colon < 0) {
        r.host = hostport;
        r.port = (r.scheme == "https") ? 443 : 80;
    } else {
        r.host = hostport.substring(0, colon);
        r.port = (uint16_t)hostport.substring(colon + 1).toInt();
    }
    if (r.host.length() == 0 || r.port == 0) return r;
    r.ok = true;
    return r;
}

bool isPrivateIPv4(const String& h) {
    int parts[4];
    int idx = 0, val = 0;
    bool any = false;
    for (size_t i = 0; i <= h.length(); ++i) {
        char c = (i < h.length()) ? h[i] : '.';
        if (c == '.') {
            if (!any || idx > 3 || val > 255) return false;
            parts[idx++] = val;
            val = 0;
            any = false;
        } else if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            any = true;
            if (val > 255) return false;
        } else {
            return false;
        }
    }
    if (idx != 4) return false;
    int a = parts[0], b = parts[1];
    if (a == 10) return true;
    if (a == 192 && b == 168) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 169 && b == 254) return true;
    return false;
}

// only http:// into RFC1918 is allowed
bool guardUrl(const Url& u) {
    return u.ok && u.scheme == "http" && isPrivateIPv4(u.host);
}

String urlJoin(const String& base, const String& ref) {
    if (ref.startsWith("http://") || ref.startsWith("https://")) return ref;
    Url b = parseUrl(base);
    if (!b.ok) return ref;
    String origin = b.scheme + "://" + b.host + ":" + String(b.port);
    if (ref.startsWith("/")) return origin + ref;
    String dir = b.path;
    int slash = dir.lastIndexOf('/');
    dir = (slash >= 0) ? dir.substring(0, slash + 1) : "/";
    return origin + dir + ref;
}

// ---------------------------------------------------------------------------
// byte pump over WiFiClient with an idle deadline (no reliance on setTimeout)
// ---------------------------------------------------------------------------
struct Pump {
    WiFiClient& c;
    uint32_t    last;
    explicit Pump(WiFiClient& cl) : c(cl), last(millis()) {}
    int get() {
        for (;;) {
            int a = c.available();
            if (a > 0) {
                int b = c.read();
                if (b < 0) continue;
                last = millis();
                return b;
            }
            if (!c.connected()) return -1;       // closed, drained
            if (millis() - last > kIdleMs) return -1;
            delay(1);
        }
    }
};

String readLine(Pump& p, size_t cap) {
    String l;
    for (;;) {
        int b = p.get();
        if (b < 0) break;
        if (b == '\n') break;
        if (b == '\r') continue;
        if (l.length() < cap) l += (char)b;
    }
    return l;
}

// HTTP body reader: transparently handles chunked, content-length, or
// read-to-close. get() returns the next body byte or -1 at end.
struct BodyReader {
    Pump& p;
    bool  chunked;
    long  clen;       // -1 unknown
    long  remaining;
    long  chunkLeft = 0;
    bool  done = false;
    BodyReader(Pump& pp, bool ch, long cl)
        : p(pp), chunked(ch), clen(cl), remaining(cl) {}
    int get() {
        if (done) return -1;
        if (chunked) {
            if (chunkLeft == 0) {
                String l = readLine(p, 16);
                long sz = strtol(l.c_str(), nullptr, 16);
                if (sz <= 0) { done = true; return -1; }
                chunkLeft = sz;
            }
            int b = p.get();
            if (b < 0) { done = true; return -1; }
            if (--chunkLeft == 0) readLine(p, 2);  // trailing CRLF
            return b;
        }
        if (clen >= 0) {
            if (remaining <= 0) { done = true; return -1; }
            int b = p.get();
            if (b < 0) { done = true; return -1; }
            --remaining;
            return b;
        }
        int b = p.get();           // read-to-close
        if (b < 0) { done = true; return -1; }
        return b;
    }
};

// ---------------------------------------------------------------------------
// DIDL-Lite tokenizer (layer-2): fed already layer-1-unescaped DIDL chars.
// Emits container/item objects to the two output buffers as they complete.
// ---------------------------------------------------------------------------
enum Field { F_NONE, F_TITLE, F_ARTIST, F_ALBUM };

struct Didl {
    String& containers;
    String& items;
    int     nC = 0;
    int     nI = 0;
    bool    truncated = false;

    int    st = 0;        // 0 = text, 1 = tag
    String tag;
    bool   inC = false, inI = false;
    Field  cap = F_NONE;
    String id, parent, title, artist, album, childCount;

    Didl(String& c, String& i) : containers(c), items(i) {}

    void resetRecord() {
        id = parent = title = artist = album = childCount = "";
        cap = F_NONE;
    }

    String* fieldPtr() {
        switch (cap) {
            case F_TITLE:  return &title;
            case F_ARTIST: return &artist;
            case F_ALBUM:  return &album;
            default:       return nullptr;
        }
    }

    void emitContainer() {
        if (containers.length() > kMaxOutBytes) { truncated = true; inC = false; return; }
        unescapeInPlace(id);
        unescapeInPlace(title);
        if (nC++) containers += ',';
        containers += "{\"id\":\"";
        appendJsonEscaped(containers, id);
        containers += "\",\"title\":\"";
        appendJsonEscaped(containers, title);
        containers += "\"";
        if (childCount.length()) {
            long cc = strtol(childCount.c_str(), nullptr, 10);
            char buf[16];
            snprintf(buf, sizeof(buf), "%ld", cc);
            containers += ",\"child_count\":";
            containers += buf;
        }
        containers += "}";
        inC = false;
    }

    void emitItem() {
        if (items.length() > kMaxOutBytes) { truncated = true; inI = false; return; }
        unescapeInPlace(id);
        unescapeInPlace(title);
        unescapeInPlace(artist);
        unescapeInPlace(album);
        if (nI++) items += ',';
        items += "{\"id\":\"";
        appendJsonEscaped(items, id);
        items += "\",\"title\":\"";
        appendJsonEscaped(items, title);
        items += "\"";
        if (artist.length()) {
            items += ",\"artist\":\"";
            appendJsonEscaped(items, artist);
            items += "\"";
        }
        if (album.length()) {
            items += ",\"album\":\"";
            appendJsonEscaped(items, album);
            items += "\"";
        }
        items += "}";
        inI = false;
    }

    void handleTag() {
        // trim leading whitespace
        int b = 0;
        while (b < (int)tag.length() &&
               (tag[b] == ' ' || tag[b] == '\t' || tag[b] == '\r' ||
                tag[b] == '\n'))
            ++b;
        String t = (b > 0) ? tag.substring(b) : tag;
        if (t.length() == 0) return;
        if (t[0] == '?' || t[0] == '!') return;  // <?xml…?>, comments, CDATA

        bool endTag = (t[0] == '/');
        bool selfClose = t.endsWith("/");
        String ln = localName(t);

        if (endTag) {
            if (ln == "container" && inC) emitContainer();
            else if (ln == "item" && inI) emitItem();
            else cap = F_NONE;  // leaf field closed
            return;
        }

        if (ln == "container") {
            inC = true; inI = false; resetRecord();
            id = tagAttr(t, "id");
            parent = tagAttr(t, "parentID");
            childCount = tagAttr(t, "childCount");
            if (selfClose) emitContainer();
            return;
        }
        if (ln == "item") {
            inI = true; inC = false; resetRecord();
            id = tagAttr(t, "id");
            parent = tagAttr(t, "parentID");
            if (selfClose) emitItem();
            return;
        }
        if (inC || inI) {
            if (ln == "title") { cap = F_TITLE; title = ""; }
            else if (ln == "artist" || ln == "creator") { cap = F_ARTIST; artist = ""; }
            else if (ln == "album") { cap = F_ALBUM; album = ""; }
            else cap = F_NONE;
            if (selfClose) cap = F_NONE;
        }
    }

    void feed(char c) {
        if (st == 0) {  // text
            if (c == '<') { tag = ""; st = 1; return; }
            if ((inC || inI) && cap != F_NONE) {
                String* f = fieldPtr();
                if (f && f->length() < kMaxField) *f += c;
            }
        } else {  // tag
            if (c == '>') { handleTag(); st = 0; return; }
            if (tag.length() < kMaxTag) tag += c;
        }
    }
};

// ---------------------------------------------------------------------------
// outer SOAP scan: find <Result> (escaped DIDL → layer-1 unescape → Didl),
// plus <NumberReturned> / <TotalMatches>. The Result content is escaped so it
// contains no raw '<' until the </Result> close tag — we stream until the
// first raw '<'.
// ---------------------------------------------------------------------------
struct SoapScan {
    Didl&  didl;
    long   num = 0, tot = 0;

    int    st = 0;          // 0 text, 1 tag
    String tag;
    bool   inResult = false;
    int    capInt = 0;      // 0 none, 1 NumberReturned, 2 TotalMatches
    String intbuf;

    // layer-1 entity unescaper state
    bool   inEnt = false;
    String ent;

    explicit SoapScan(Didl& d) : didl(d) {}

    void l1(char c) {  // layer-1 unescape, feed result of decode into Didl
        if (inEnt) {
            if (c == ';') {
                if (ent == "lt") didl.feed('<');
                else if (ent == "gt") didl.feed('>');
                else if (ent == "amp") didl.feed('&');
                else if (ent == "quot") didl.feed('"');
                else if (ent == "apos") didl.feed('\'');
                else if (ent.length() > 1 && ent[0] == '#') {
                    long code = (ent[1] == 'x' || ent[1] == 'X')
                                    ? strtol(ent.c_str() + 2, nullptr, 16)
                                    : strtol(ent.c_str() + 1, nullptr, 10);
                    didl.feed((code > 0 && code < 0x80) ? (char)code : '?');
                } else {
                    didl.feed('&');
                    for (size_t i = 0; i < ent.length(); ++i) didl.feed(ent[i]);
                    didl.feed(';');
                }
                inEnt = false;
                ent = "";
            } else if (ent.length() < 10) {
                ent += c;
            } else {  // runaway, flush literally
                didl.feed('&');
                for (size_t i = 0; i < ent.length(); ++i) didl.feed(ent[i]);
                didl.feed(c);
                inEnt = false;
                ent = "";
            }
        } else if (c == '&') {
            inEnt = true;
            ent = "";
        } else {
            didl.feed(c);
        }
    }

    void onTag() {
        int b = 0;
        while (b < (int)tag.length() &&
               (tag[b] == ' ' || tag[b] == '\t'))
            ++b;
        String t = (b > 0) ? tag.substring(b) : tag;
        if (t.length() == 0 || t[0] == '?' || t[0] == '!') return;
        bool endTag = (t[0] == '/');
        bool selfClose = t.endsWith("/");
        String ln = localName(t);
        if (endTag) {
            if (capInt == 1 && ln == "NumberReturned") { num = strtol(intbuf.c_str(), nullptr, 10); }
            else if (capInt == 2 && ln == "TotalMatches") { tot = strtol(intbuf.c_str(), nullptr, 10); }
            capInt = 0;
            return;
        }
        if (ln == "Result" && !selfClose) {
            inResult = true;
            inEnt = false;
            ent = "";
        } else if (ln == "NumberReturned") {
            capInt = 1; intbuf = "";
        } else if (ln == "TotalMatches") {
            capInt = 2; intbuf = "";
        }
    }

    void feed(char c) {
        if (st == 0) {  // text
            if (c == '<') {
                if (inResult) inResult = false;  // raw '<' ends Result content
                tag = "";
                st = 1;
                return;
            }
            if (inResult) { l1(c); return; }
            if (capInt && intbuf.length() < 12) intbuf += c;
        } else {  // tag
            if (c == '>') { onTag(); st = 0; return; }
            if (tag.length() < 256) tag += c;
        }
    }
};

// ---------------------------------------------------------------------------
// shared: connect + send request line/headers, parse response status+headers,
// leave `chunked`/`clen` set and pump positioned at body start.
// ---------------------------------------------------------------------------
bool sendAndHeaders(WiFiClient& c, const Url& u, const String& method,
                    const String& extraHeaders, const String& body,
                    Pump& pump, int& status, bool& chunked, long& clen) {
    if (!c.connect(u.host.c_str(), u.port, kConnectMs)) return false;
    c.setNoDelay(true);
    String req = method + " " + u.path + " HTTP/1.1\r\n";
    req += "Host: " + u.host + ":" + String(u.port) + "\r\n";
    req += extraHeaders;
    req += "Connection: close\r\n";
    if (body.length()) req += "Content-Length: " + String(body.length()) + "\r\n";
    req += "\r\n";
    c.print(req);
    if (body.length()) c.print(body);

    String line = readLine(pump, 256);   // HTTP/1.1 200 OK
    int sp = line.indexOf(' ');
    status = (sp >= 0) ? line.substring(sp + 1).toInt() : 0;
    chunked = false;
    clen = -1;
    for (;;) {
        String h = readLine(pump, 1024);
        if (h.length() == 0) break;
        String low = h;
        low.toLowerCase();
        if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0)
            chunked = true;
        else if (low.startsWith("content-length:"))
            clen = h.substring(h.indexOf(':') + 1).toInt();
    }
    return true;
}

}  // namespace

// ===========================================================================
// public API
// ===========================================================================
#if SIXBACK_DLNA_BROWSE

int dlnaDescribe(const String& location, String& out) {
    Url u = parseUrl(location);
    if (!guardUrl(u)) { out = errJson("location: only http:// into RFC1918 allowed"); return 400; }

    WiFiClient c;
    Pump pump(c);
    int status; bool chunked; long clen;
    if (!sendAndHeaders(c, u, "GET", "", "", pump, status, chunked, clen)) {
        out = errJson("connect failed"); return 502;
    }
    if (status != 200) { c.stop(); out = errJson("descriptor HTTP " + String(status)); return 502; }

    // bounded buffer — device descriptors are small
    BodyReader br(pump, chunked, clen);
    String xml;
    for (int b; (b = br.get()) >= 0; ) {
        if (xml.length() >= kMaxDescBytes) break;
        xml += (char)b;
    }
    c.stop();
    if (xml.length() == 0) { out = errJson("empty descriptor"); return 502; }

    // ContentDirectory:1 service → controlURL
    String control;
    int pos = 0;
    while (true) {
        int s = xml.indexOf("<service>", pos);   // exact: not <serviceList>/<serviceId>
        if (s < 0) break;
        int e = xml.indexOf("</service>", s);
        if (e < 0) break;
        String blk = xml.substring(s, e);
        pos = e + 1;
        if (blk.indexOf(CDS_TYPE) < 0) continue;
        int cs = blk.indexOf("<controlURL>");
        if (cs < 0) continue;
        cs += strlen("<controlURL>");
        int ce = blk.indexOf("</controlURL>", cs);
        if (ce < 0) continue;
        String cu = blk.substring(cs, ce);
        cu.trim();
        unescapeInPlace(cu);
        control = urlJoin(location, cu);
        break;
    }
    if (control.length() == 0) { out = errJson("no ContentDirectory service"); return 502; }

    auto field = [&](const char* tag) -> String {
        String open = String("<") + tag + ">";
        int s = xml.indexOf(open);
        if (s < 0) return String();
        s += open.length();
        int e = xml.indexOf(String("</") + tag + ">", s);
        if (e < 0) return String();
        String v = xml.substring(s, e);
        v.trim();
        unescapeInPlace(v);
        return v;
    };
    String name = field("friendlyName");
    if (name.length() == 0) name = field("modelName");
    if (name.length() == 0) name = "DLNA Server";

    out = "{\"ok\":true,\"name\":\"";
    appendJsonEscaped(out, name);
    out += "\",\"control_url\":\"";
    appendJsonEscaped(out, control);
    out += "\"}";
    return 200;
}

int dlnaBrowse(const String& controlUrl, const String& objectId,
               long start, long count, String& out) {
    Url u = parseUrl(controlUrl);
    if (!guardUrl(u)) { out = errJson("control_url: only http:// into RFC1918 allowed"); return 400; }
    if (start < 0) start = 0;
    if (count < 1) count = 1;
    if (count > 500) count = 500;

    // SOAP envelope (ObjectID is the only caller-supplied value → escape it)
    String oid;
    for (size_t i = 0; i < objectId.length(); ++i) {
        char ch = objectId[i];
        if (ch == '&') oid += "&amp;";
        else if (ch == '<') oid += "&lt;";
        else if (ch == '>') oid += "&gt;";
        else oid += ch;
    }
    String soap =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
        "<ObjectID>" + oid + "</ObjectID>"
        "<BrowseFlag>BrowseDirectChildren</BrowseFlag>"
        "<Filter>*</Filter>"
        "<StartingIndex>" + String(start) + "</StartingIndex>"
        "<RequestedCount>" + String(count) + "</RequestedCount>"
        "<SortCriteria></SortCriteria>"
        "</u:Browse>"
        "</s:Body>"
        "</s:Envelope>";

    String headers =
        "Content-Type: text/xml; charset=\"utf-8\"\r\n"
        "SOAPAction: \"" + String(CDS_TYPE) + "#Browse\"\r\n"
        "User-Agent: SixBack-DLNA/1.0 UPnP/1.0\r\n";

    WiFiClient c;
    Pump pump(c);
    int status; bool chunked; long clen;
    if (!sendAndHeaders(c, u, "POST", headers, soap, pump, status, chunked, clen)) {
        out = errJson("connect failed"); return 502;
    }
    if (status != 200) { c.stop(); out = errJson("Browse HTTP " + String(status)); return 502; }

    String containers, items;
    Didl didl(containers, items);
    SoapScan scan(didl);
    BodyReader br(pump, chunked, clen);
    for (int b; (b = br.get()) >= 0; ) scan.feed((char)b);
    c.stop();

    out = "{\"ok\":true,\"object_id\":\"";
    appendJsonEscaped(out, objectId);
    out += "\",\"start\":";
    out += String(start);
    out += ",\"returned\":";
    out += String(scan.num);
    out += ",\"total\":";
    out += String(scan.tot);
    out += ",\"containers\":[";
    out += containers;
    out += "],\"items\":[";
    out += items;
    out += "]";
    if (didl.truncated) out += ",\"truncated\":true";
    out += "}";
    return 200;
}

#else  // SIXBACK_DLNA_BROWSE == 0  → feature gated off on this target

int dlnaDescribe(const String&, String& out) {
    out = errJson("DLNA browse not available on this device");
    return 501;
}
int dlnaBrowse(const String&, const String&, long, long, String& out) {
    out = errJson("DLNA browse not available on this device");
    return 501;
}

#endif  // SIXBACK_DLNA_BROWSE

}  // namespace sixback

// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gabbo_ws.h"

#ifdef SIXBACK_GABBO_WATCHER_ENABLED

#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace sixback {
namespace {

String base64_(const uint8_t* d, size_t n) {
    size_t olen = 0;
    uint8_t out[64];
    if (mbedtls_base64_encode(out, sizeof(out), &olen, d, n) != 0) return String();
    out[olen] = 0;
    return String((const char*)out);
}

// Sec-WebSocket-Accept = base64( SHA1( clientKey + RFC6455-GUID ) )
String wsAccept_(const String& keyB64) {
    String s = keyB64 + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t sha[20];
    mbedtls_sha1((const uint8_t*)s.c_str(), s.length(), sha);
    return base64_(sha, 20);
}

} // anon

bool GabboWsClient::readN_(uint8_t* buf, size_t n, uint32_t toMs) {
    size_t got = 0;
    uint32_t start = millis();
    while (got < n) {
        int avail = cli_.available();
        if (avail > 0) {
            int r = cli_.read(buf + got, n - got);
            if (r > 0) { got += (size_t)r; start = millis(); }
        } else {
            if (!cli_.connected()) return false;
            if (millis() - start > toMs) return false;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return true;
}

bool GabboWsClient::sendFrame_(uint8_t opcode, const uint8_t* data, size_t len) {
    if (len > 125) return false;            // wir senden nur winzige Control-Frames (Pong)
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(esp_random() & 0xFF);
    uint8_t hdr[6];
    hdr[0] = 0x80 | (opcode & 0x0F);        // FIN + opcode
    hdr[1] = 0x80 | (uint8_t)len;           // MASK + len (client->server MUSS maskiert sein)
    hdr[2] = mask[0]; hdr[3] = mask[1]; hdr[4] = mask[2]; hdr[5] = mask[3];
    if (cli_.write(hdr, 6) != 6) return false;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i] ^ mask[i & 3];
        if (cli_.write(&b, 1) != 1) return false;
    }
    return true;
}

bool GabboWsClient::connect(const String& ip, uint16_t port, uint32_t timeoutMs) {
    up_ = false;
    cli_.stop();
    if (!cli_.connect(ip.c_str(), port, (int32_t)timeoutMs)) return false;

    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(esp_random() & 0xFF);
    String keyB64 = base64_(key, 16);

    String req;
    req.reserve(220);
    req  = "GET / HTTP/1.1\r\n";
    req += "Host: " + ip + ":" + String(port) + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + keyB64 + "\r\n";
    req += "Sec-WebSocket-Protocol: gabbo\r\n";
    req += "Sec-WebSocket-Version: 13\r\n\r\n";
    if (cli_.print(req) != req.length()) { cli_.stop(); return false; }

    // Antwort-Header bis CRLFCRLF lesen (bounded)
    String resp;
    resp.reserve(512);
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        while (cli_.available()) {
            char c = (char)cli_.read();
            resp += c;
            if (resp.endsWith("\r\n\r\n")) break;
            if (resp.length() > 2048) { cli_.stop(); return false; }
        }
        if (resp.endsWith("\r\n\r\n")) break;
        if (!cli_.connected() && cli_.available() <= 0) break;
        vTaskDelay(pdMS_TO_TICKS(3));
    }
    if (resp.indexOf(" 101") < 0) { cli_.stop(); return false; }
    // Sec-WebSocket-Accept exakt validieren
    String accept = wsAccept_(keyB64);
    if (accept.length() == 0 || resp.indexOf(accept) < 0) { cli_.stop(); return false; }

    up_ = true;
    return true;
}

bool GabboWsClient::connected() {
    if (up_ && !cli_.connected()) up_ = false;
    return up_;
}

void GabboWsClient::close() {
    if (cli_.connected()) {
        uint8_t dummy = 0;
        sendFrame_(0x8, &dummy, 0);         // Close-Frame, best effort
    }
    cli_.stop();
    up_ = false;
}

bool GabboWsClient::ping() {
    if (!connected()) return false;
    // 0-Byte-Ping (client->server MUSS maskiert sein, sendFrame_ maskiert).
    // false bei Schreibfehler -> Socket tot -> Caller reconnectet.
    if (!sendFrame_(0x9, nullptr, 0)) { up_ = false; return false; }
    return true;
}

bool GabboWsClient::poll(String& out) {
    if (!connected()) return false;
    if (cli_.available() <= 0) return false;

    uint8_t h[2];
    if (!readN_(h, 2, 3000)) { close(); return false; }
    uint8_t opcode = h[0] & 0x0F;
    bool masked = (h[1] & 0x80) != 0;       // Server maskiert NICHT
    uint64_t len = h[1] & 0x7F;
    if (len == 126) {
        uint8_t e[2]; if (!readN_(e, 2, 3000)) { close(); return false; }
        len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        uint8_t e[8]; if (!readN_(e, 8, 3000)) { close(); return false; }
        len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) { if (!readN_(mask, 4, 3000)) { close(); return false; } }

    if (len > 16384) { close(); return false; }   // gabbo-Frames sind klein; Schutz

    String payload;
    payload.reserve((size_t)len);
    uint8_t buf[256];
    uint64_t rem = len;
    size_t idx = 0;
    while (rem > 0) {
        size_t want = rem > sizeof(buf) ? sizeof(buf) : (size_t)rem;
        if (!readN_(buf, want, 5000)) { close(); return false; }
        for (size_t i = 0; i < want; i++) {
            uint8_t b = buf[i];
            if (masked) b ^= mask[(idx + i) & 3];
            payload += (char)b;
        }
        idx += want;
        rem -= want;
    }

    switch (opcode) {
        case 0x1:   // text (gabbo sendet single-frame, FIN=1)
            out = payload;
            return true;
        case 0x0:   // continuation: Fragmentierung erwarten wir nicht. Payload wurde
                    // vom Draht gelesen, aber NICHT als Text ausgeliefert (kein Mis-Parse).
            return false;
        case 0x9:   // ping -> pong
            sendFrame_(0xA, (const uint8_t*)payload.c_str(), payload.length());
            return false;
        case 0x8:   // close
            close();
            return false;
        default:    // binary / pong -> ignorieren
            return false;
    }
}

} // namespace sixback

#endif // SIXBACK_GABBO_WATCHER_ENABLED

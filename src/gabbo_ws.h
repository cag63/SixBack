// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — gabbo WebSocket client (hand-rolled RFC6455, zero external deps)
//
// Minimaler Client fuer den Bose SoundTouch gabbo-Notification-Bus
// (ws://<speaker>:8080/, subprotocol "gabbo"). Wir EMPFANGEN nur Text-Frames
// (nowSelectionUpdated / nowPlayingUpdated / errorUpdate ...) und beantworten
// Ping mit Pong; sonst wird nichts gesendet. Zweck: Hardware-Tastendruck am
// Speaker in Echtzeit mitlesen (#15 Re-Arm kalter LIR/ORION-Slots).
//
// Bewusst hand-rolled statt esp_websocket_client (im Arduino-SDK NICHT
// vorhanden) oder links2004 (externe Dependency) -> zero deps, voll
// reproduzierbar ueber alle Build-Hosts.

#ifndef SIXBACK_GABBO_WS_H
#define SIXBACK_GABBO_WS_H

#include <Arduino.h>
#include <WiFiClient.h>

namespace sixback {

class GabboWsClient {
public:
    // TCP-Connect + WebSocket-Upgrade-Handshake (subprotocol "gabbo").
    // true = "101 Switching Protocols" + gueltiger Sec-WebSocket-Accept.
    bool connect(const String& ip, uint16_t port, uint32_t timeoutMs = 5000);
    bool connected();
    // Verarbeitet wartende Bytes. Bei vollstaendigem Text-Frame: out gesetzt + true.
    // Ping wird intern mit Pong beantwortet (out leer, false). Close/Fehler: trennt.
    bool poll(String& out);
    void close();
    bool ping();   // WS-Ping (0-Byte, maskiert) -> false bei Schreibfehler (Liveness)

private:
    WiFiClient cli_;
    bool up_ = false;
    bool readN_(uint8_t* buf, size_t n, uint32_t toMs);
    bool sendFrame_(uint8_t opcode, const uint8_t* data, size_t len);
};

} // namespace sixback

#endif

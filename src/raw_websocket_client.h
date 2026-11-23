#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include <vector>

class RawWebSocketClient {
 public:
  explicit RawWebSocketClient(const char* tag);

  bool connect(const char* url, const String& bearer);
  bool sendText(const String& text);
  bool pollTextMessage(String& payloadOut);
  bool isConnected();
  void close();

 private:
  bool parseUrl(const char* url);
  bool performHandshake(const String& bearer);
  bool readFrame(uint8_t& opcodeOut, bool& finOut, std::vector<uint8_t>& payloadOut);
  bool readExact(uint8_t* buffer, size_t length, uint32_t timeoutMs);
  bool sendFrame(uint8_t opcode, const uint8_t* payload, size_t length, bool fin);
  bool sendPong(const uint8_t* payload, size_t length);
  String summarize(const String& text, size_t maxLen = 160) const;

  const char* tag_;
  WiFiClientSecure socket_;
  String host_;
  String path_;
  uint16_t port_ = 443;
  bool secure_ = true;
  bool connected_ = false;
  String fragmentedText_;
};

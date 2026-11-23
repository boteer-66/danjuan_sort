#include "raw_websocket_client.h"

#include <mbedtls/base64.h>
#include <esp_system.h>

#include "app_config.h"

namespace {

String base64EncodeBytes(const uint8_t* data, size_t length) {
  size_t outputLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outputLen, data, length);
  std::vector<uint8_t> buffer(outputLen + 1, 0);
  if (mbedtls_base64_encode(buffer.data(), buffer.size(), &outputLen, data, length) != 0) {
    return "";
  }
  return String(reinterpret_cast<const char*>(buffer.data()));
}

String bytesToString(const std::vector<uint8_t>& bytes, size_t offset = 0) {
  String out;
  if (offset >= bytes.size()) {
    return out;
  }

  out.reserve(static_cast<unsigned>(bytes.size() - offset));
  for (size_t i = offset; i < bytes.size(); ++i) {
    out += static_cast<char>(bytes[i]);
  }
  return out;
}

}  // namespace

RawWebSocketClient::RawWebSocketClient(const char* tag)
    : tag_(tag ? tag : "WS") {
  socket_.setInsecure();
  socket_.setTimeout(AppConfig::kWsConnectTimeoutMs / 1000);
}

bool RawWebSocketClient::connect(const char* url, const String& bearer) {
  connected_ = false;
  fragmentedText_ = "";

  if (!parseUrl(url)) {
    Serial.printf("[%s][WS2] URL parse failed: %s\n", tag_, url ? url : "<null>");
    return false;
  }

  Serial.printf("[%s][WS2] connect host=%s port=%u secure=%d path=%s\n",
                tag_, host_.c_str(), port_, secure_ ? 1 : 0, path_.c_str());

  const uint32_t tcpStartMs = millis();
  if (!socket_.connect(host_.c_str(), port_)) {
    Serial.printf("[%s][WS2] TCP/TLS connect failed after %lu ms\n",
                  tag_, static_cast<unsigned long>(millis() - tcpStartMs));
    return false;
  }

  Serial.printf("[%s][WS2] TCP/TLS connected in %lu ms\n",
                tag_, static_cast<unsigned long>(millis() - tcpStartMs));

  if (!performHandshake(bearer)) {
    socket_.stop();
    connected_ = false;
    return false;
  }

  connected_ = true;
  Serial.printf("[%s][WS2] handshake OK\n", tag_);
  return true;
}

bool RawWebSocketClient::sendText(const String& text) {
  return sendFrame(0x1, reinterpret_cast<const uint8_t*>(text.c_str()), text.length(), true);
}

bool RawWebSocketClient::pollTextMessage(String& payloadOut) {
  payloadOut = "";
  if (!connected_) {
    return false;
  }

  while (socket_.connected() && socket_.available() > 0) {
    uint8_t opcode = 0;
    bool fin = false;
    std::vector<uint8_t> payload;
    if (!readFrame(opcode, fin, payload)) {
      connected_ = false;
      return false;
    }

    if (opcode == 0x9) {
      Serial.printf("[%s][WS2] ping len=%u\n", tag_, static_cast<unsigned>(payload.size()));
      sendPong(payload.data(), payload.size());
      continue;
    }

    if (opcode == 0xA) {
      Serial.printf("[%s][WS2] pong len=%u\n", tag_, static_cast<unsigned>(payload.size()));
      continue;
    }

    if (opcode == 0x8) {
      int closeCode = 0;
      String reason;
      if (payload.size() >= 2) {
        closeCode = (payload[0] << 8) | payload[1];
        if (payload.size() > 2) {
          reason = bytesToString(payload, 2);
        }
      }
      Serial.printf("[%s][WS2] close frame code=%d reason=%s\n",
                    tag_, closeCode, summarize(reason).c_str());
      connected_ = false;
      socket_.stop();
      return false;
    }

    if (opcode == 0x1) {
      String chunk = bytesToString(payload);
      if (fin) {
        payloadOut = chunk;
        return true;
      }
      fragmentedText_ = chunk;
      continue;
    }

    if (opcode == 0x0) {
      String chunk = bytesToString(payload);
      fragmentedText_ += chunk;
      if (fin) {
        payloadOut = fragmentedText_;
        fragmentedText_ = "";
        return true;
      }
      continue;
    }

    Serial.printf("[%s][WS2] unhandled opcode=0x%02X len=%u\n",
                  tag_, opcode, static_cast<unsigned>(payload.size()));
  }

  if (!socket_.connected()) {
    connected_ = false;
  }
  return false;
}

bool RawWebSocketClient::isConnected() {
  return connected_ && socket_.connected();
}

void RawWebSocketClient::close() {
  if (socket_.connected()) {
    sendFrame(0x8, nullptr, 0, true);
  }
  socket_.stop();
  connected_ = false;
}

bool RawWebSocketClient::parseUrl(const char* url) {
  host_ = "";
  path_ = "/";
  port_ = 443;
  secure_ = true;

  String raw(url ? url : "");
  if (raw.startsWith("wss://")) {
    secure_ = true;
    raw.remove(0, 6);
    port_ = 443;
  } else if (raw.startsWith("ws://")) {
    secure_ = false;
    raw.remove(0, 5);
    port_ = 80;
  } else {
    return false;
  }

  const int slashIndex = raw.indexOf('/');
  String hostPort = slashIndex >= 0 ? raw.substring(0, slashIndex) : raw;
  path_ = slashIndex >= 0 ? raw.substring(slashIndex) : "/";

  const int colonIndex = hostPort.indexOf(':');
  if (colonIndex >= 0) {
    host_ = hostPort.substring(0, colonIndex);
    const long parsedPort = hostPort.substring(colonIndex + 1).toInt();
    if (parsedPort <= 0 || parsedPort > 65535) {
      return false;
    }
    port_ = static_cast<uint16_t>(parsedPort);
  } else {
    host_ = hostPort;
  }

  return !host_.isEmpty();
}

bool RawWebSocketClient::performHandshake(const String& bearer) {
  uint8_t nonce[16];
  for (uint8_t& value : nonce) {
    value = static_cast<uint8_t>(esp_random() & 0xFF);
  }
  const String secKey = base64EncodeBytes(nonce, sizeof(nonce));

  String request;
  request += "GET " + path_ + " HTTP/1.1\r\n";
  request += "Host: " + host_ + "\r\n";
  request += "Authorization: " + bearer + "\r\n";
  request += "Upgrade: websocket\r\n";
  request += "Connection: Upgrade\r\n";
  request += "Sec-WebSocket-Version: 13\r\n";
  request += "Sec-WebSocket-Key: " + secKey + "\r\n";
  request += "User-Agent: ESP32 RawWS\r\n";
  request += "Origin: https://esp32.local\r\n";
  request += "\r\n";

  socket_.print(request);

  const String statusLine = socket_.readStringUntil('\n');
  if (statusLine.isEmpty()) {
    Serial.printf("[%s][WS2] handshake timeout waiting status line\n", tag_);
    return false;
  }

  const bool accepted = statusLine.startsWith("HTTP/1.1 101");
  Serial.printf("[%s][WS2] status: %s\n", tag_, summarize(statusLine).c_str());

  int contentLength = -1;
  int headerCount = 0;
  while (socket_.connected() && headerCount < 32) {
    String headerLine = socket_.readStringUntil('\n');
    if (headerLine.isEmpty()) {
      Serial.printf("[%s][WS2] handshake header timeout\n", tag_);
      break;
    }

    if (headerLine == "\r") {
      break;
    }

    headerLine.trim();
    Serial.printf("[%s][WS2] header: %s\n", tag_, summarize(headerLine).c_str());

    const int colonIndex = headerLine.indexOf(':');
    if (colonIndex > 0) {
      const String key = headerLine.substring(0, colonIndex);
      String value = headerLine.substring(colonIndex + 1);
      value.trim();
      if (key.equalsIgnoreCase("Content-Length")) {
        contentLength = value.toInt();
      }
    }
    ++headerCount;
  }

  if (!accepted) {
    String body;
    const uint32_t bodyStartMs = millis();
    if (contentLength > 0) {
      body.reserve(static_cast<unsigned>(contentLength));
      while (body.length() < static_cast<unsigned>(contentLength) &&
             millis() - bodyStartMs < AppConfig::kWsConnectTimeoutMs) {
        while (socket_.available() > 0 &&
               body.length() < static_cast<unsigned>(contentLength)) {
          body += static_cast<char>(socket_.read());
        }
        if (body.length() >= static_cast<unsigned>(contentLength)) {
          break;
        }
        delay(5);
      }
    } else {
      while (millis() - bodyStartMs < AppConfig::kWsConnectTimeoutMs) {
        while (socket_.available() > 0) {
          body += static_cast<char>(socket_.read());
        }
        if (!socket_.connected()) {
          break;
        }
        delay(5);
      }
    }

    Serial.printf("[%s][WS2] body: %s\n", tag_, body.isEmpty() ? "<empty>" : summarize(body, 400).c_str());
    return false;
  }

  return true;
}

bool RawWebSocketClient::readExact(uint8_t* buffer, size_t length, uint32_t timeoutMs) {
  size_t offset = 0;
  const uint32_t startMs = millis();
  while (offset < length && millis() - startMs < timeoutMs) {
    const int available = socket_.available();
    if (available > 0) {
      const int readLen = socket_.read(buffer + offset, length - offset);
      if (readLen > 0) {
        offset += static_cast<size_t>(readLen);
        continue;
      }
    }
    if (!socket_.connected()) {
      break;
    }
    delay(2);
  }
  return offset == length;
}

bool RawWebSocketClient::readFrame(uint8_t& opcodeOut, bool& finOut, std::vector<uint8_t>& payloadOut) {
  uint8_t header[2];
  if (!readExact(header, sizeof(header), AppConfig::kWsReadTimeoutMs)) {
    Serial.printf("[%s][WS2] frame header timeout\n", tag_);
    return false;
  }

  finOut = (header[0] & 0x80) != 0;
  opcodeOut = header[0] & 0x0F;
  const bool masked = (header[1] & 0x80) != 0;
  uint64_t payloadLen = header[1] & 0x7F;

  if (payloadLen == 126) {
    uint8_t ext[2];
    if (!readExact(ext, sizeof(ext), AppConfig::kWsReadTimeoutMs)) {
      Serial.printf("[%s][WS2] extended payload16 timeout\n", tag_);
      return false;
    }
    payloadLen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
  } else if (payloadLen == 127) {
    uint8_t ext[8];
    if (!readExact(ext, sizeof(ext), AppConfig::kWsReadTimeoutMs)) {
      Serial.printf("[%s][WS2] extended payload64 timeout\n", tag_);
      return false;
    }
    payloadLen = 0;
    for (uint8_t value : ext) {
      payloadLen = (payloadLen << 8) | value;
    }
  }

  uint8_t maskKey[4] = {0, 0, 0, 0};
  if (masked && !readExact(maskKey, sizeof(maskKey), AppConfig::kWsReadTimeoutMs)) {
    Serial.printf("[%s][WS2] mask key timeout\n", tag_);
    return false;
  }

  payloadOut.assign(static_cast<size_t>(payloadLen), 0);
  if (payloadLen > 0 &&
      !readExact(payloadOut.data(), static_cast<size_t>(payloadLen), AppConfig::kWsReadTimeoutMs)) {
    Serial.printf("[%s][WS2] payload timeout len=%u\n", tag_, static_cast<unsigned>(payloadLen));
    return false;
  }

  if (masked) {
    for (size_t i = 0; i < payloadOut.size(); ++i) {
      payloadOut[i] ^= maskKey[i % 4];
    }
  }

  return true;
}

bool RawWebSocketClient::sendFrame(uint8_t opcode, const uint8_t* payload, size_t length, bool fin) {
  if (!socket_.connected()) {
    Serial.printf("[%s][WS2] send blocked: socket not connected\n", tag_);
    connected_ = false;
    return false;
  }

  std::vector<uint8_t> frame;
  frame.reserve(length + 14);
  frame.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) | (opcode & 0x0F)));

  if (length <= 125) {
    frame.push_back(static_cast<uint8_t>(0x80 | length));
  } else if (length <= 0xFFFF) {
    frame.push_back(0x80 | 126);
    frame.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(length & 0xFF));
  } else {
    frame.push_back(0x80 | 127);
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(length) >> shift) & 0xFF));
    }
  }

  uint8_t maskKey[4];
  for (uint8_t& value : maskKey) {
    value = static_cast<uint8_t>(esp_random() & 0xFF);
    frame.push_back(value);
  }

  for (size_t i = 0; i < length; ++i) {
    const uint8_t source = payload ? payload[i] : 0;
    frame.push_back(source ^ maskKey[i % 4]);
  }

  const size_t written = socket_.write(frame.data(), frame.size());
  if (written != frame.size()) {
    Serial.printf("[%s][WS2] frame write failed: wrote=%u expected=%u\n",
                  tag_, static_cast<unsigned>(written), static_cast<unsigned>(frame.size()));
    connected_ = false;
    return false;
  }

  return true;
}

bool RawWebSocketClient::sendPong(const uint8_t* payload, size_t length) {
  return sendFrame(0xA, payload, length, true);
}

String RawWebSocketClient::summarize(const String& text, size_t maxLen) const {
  String summary = text;
  summary.replace("\r", "\\r");
  summary.replace("\n", "\\n");
  if (summary.length() > maxLen) {
    summary.remove(maxLen);
    summary += "...";
  }
  return summary;
}

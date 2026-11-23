#include "su03t_uart.h"

#include "app_config.h"

void Su03tUart::begin() {
  if (AppConfig::kSu03tRxPin < 0 || AppConfig::kSu03tTxPin < 0) {
    Serial.println("[INFO] SU-03T UART disabled (pins not configured)");
    return;
  }
  serial_.begin(AppConfig::kSu03tBaud, SERIAL_8N1, AppConfig::kSu03tRxPin, AppConfig::kSu03tTxPin);
  Serial.printf("[OK] SU-03T UART ready: RX=%d TX=%d baud=%u\n",
                AppConfig::kSu03tRxPin, AppConfig::kSu03tTxPin, AppConfig::kSu03tBaud);
}

bool Su03tUart::pollWakeSignal(String& reason) {
  while (serial_.available() > 0) {
    const uint8_t byteIn = static_cast<uint8_t>(serial_.read());

    if (byteIn == '\r' || byteIn == '\n') {
      if (!lineBuffer_.isEmpty()) {
        reason = String("SU-03T line: ") + lineBuffer_;
        lineBuffer_ = "";
        return true;
      }
      continue;
    }

    if (byteIn >= 32 && byteIn <= 126) {
      lineBuffer_ += static_cast<char>(byteIn);
      if (lineBuffer_.length() > 96) {
        lineBuffer_.remove(96);
      }
    }

    if (frameLen_ < sizeof(frameBuffer_)) {
      frameBuffer_[frameLen_++] = byteIn;
    } else {
      frameLen_ = 0;
    }

    // 帧协议: AA 55 [msgno] [可选参数...] 55 AA
    // 最小5字节(AA 55 XX 55 AA), 带参数可达N字节
    if (frameLen_ >= 5) {
      for (size_t start = 0; start + 4 < frameLen_; ++start) {
        if (frameBuffer_[start] == 0xAA && frameBuffer_[start + 1] == 0x55) {
          // 从尾部找 55 AA
          size_t tail = start + 3;  // AA 55 XX 55 AA → tail 在 start+3
          while (tail + 1 < frameLen_) {
            if (frameBuffer_[tail] == 0x55 && frameBuffer_[tail + 1] == 0xAA) {
              size_t frameLen = tail + 2 - start;
              if (parseWakeFrame(frameBuffer_ + start, frameLen, reason)) {
                frameLen_ = 0;
                return true;
              }
              break;
            }
            tail++;
          }
        }
      }
    }
  }

  return false;
}

bool Su03tUart::parseWakeFrame(const uint8_t* data, size_t length, String& reason) {
  if (length < 5) return false;

  const uint8_t commandId = data[2];  // msgno 永远在位置2
  reason = String("SU-03T wake id=0x") + String(commandId, HEX);
  return true;
}

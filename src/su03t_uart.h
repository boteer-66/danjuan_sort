#pragma once

#include <Arduino.h>

class Su03tUart {
 public:
  explicit Su03tUart(HardwareSerial& serial) : serial_(serial) {}

  void begin();
  bool pollWakeSignal(String& reason);

 private:
  bool parseWakeFrame(const uint8_t* data, size_t length, String& reason);

  HardwareSerial& serial_;
  String lineBuffer_;
  uint8_t frameBuffer_[64] = {0};
  size_t frameLen_ = 0;
};

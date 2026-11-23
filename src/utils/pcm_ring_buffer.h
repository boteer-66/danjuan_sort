#pragma once
#include <Arduino.h>
#include <cstring>

// SPSC 无锁环形缓冲 —— 主 task 写, 播放 task 读
// 64KB ≈ 4s @16kHz mono, WiFi 抖动有足够余量

class PcmRingBuffer {
  static constexpr size_t kCap = 65536;
  uint8_t  buf_[kCap];
  volatile size_t head_ = 0;
  volatile size_t tail_ = 0;
  volatile size_t used_ = 0;

public:
  // producer (主 task), 返回实际写入量
  size_t push(const uint8_t* data, size_t len) {
    size_t free = kCap - used_;
    size_t n = (len < free) ? len : free;
    if (n == 0) return 0;
    size_t first = kCap - head_;
    if (n <= first) {
      memcpy(buf_ + head_, data, n);
    } else {
      memcpy(buf_ + head_, data, first);
      memcpy(buf_, data + first, n - first);
    }
    head_ = (head_ + n) % kCap;
    used_ += n;
    return n;
  }

  // consumer (播放 task), 返回实际读出量
  size_t pop(uint8_t* data, size_t maxLen) {
    if (used_ == 0) return 0;
    size_t n = (maxLen < used_) ? maxLen : used_;
    size_t first = kCap - tail_;
    if (n <= first) {
      memcpy(data, buf_ + tail_, n);
    } else {
      memcpy(data, buf_ + tail_, first);
      memcpy(data + first, buf_, n - first);
    }
    tail_ = (tail_ + n) % kCap;
    used_ -= n;
    return n;
  }

  size_t available() const { return used_; }
  void reset() { head_ = tail_ = used_ = 0; }
};

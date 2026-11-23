#pragma once

#include <Arduino.h>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/module_interfaces.h"

class PcmRingBuffer;

class AudioI2S : public ISpeaker, public IMic {
 public:
  // ---- ISpeaker / IMic 共享 ----
  bool begin() override;

  // ---- ISpeaker ----
  bool play(const std::vector<uint8_t>& pcm) override;
  void stop() override;

  // ---- IMic ----
  bool readChunk(std::vector<uint8_t>& buf) override;

  // ---- 内部方法 ----
  bool readMicChunk(uint8_t* buffer, size_t capacity, size_t& bytesRead);
  void discardMicChunks(int n);
  bool playPcm(const uint8_t* data, size_t dataSize);
  void clearTx();
  bool ready() const { return ready_; }

  // ---- V3 FreeRTOS 播放 task ----
  void startPlaybackTask(PcmRingBuffer* ring);
  void stopPlaybackTask();

 private:
  bool ready_ = false;
  TaskHandle_t playTask_ = nullptr;
};

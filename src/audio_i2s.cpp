#include "audio_i2s.h"

#include <driver/i2s.h>
#include <esp_task_wdt.h>
#include <vector>
#include <cstring>

#include "app_config.h"
#include "utils/pcm_ring_buffer.h"

namespace {
constexpr i2s_port_t kMicPort  = I2S_NUM_0;  // PDM RX only
constexpr i2s_port_t kSpkPort  = I2S_NUM_1;  // I2S TX only
}

bool AudioI2S::begin() {
  // NS4168 CTRL
  if (AppConfig::kNs4168CtrlPin >= 0) {
    pinMode(AppConfig::kNs4168CtrlPin, OUTPUT);
    digitalWrite(AppConfig::kNs4168CtrlPin, HIGH);
  }

  // ---- 麦克风: I2S_NUM_0 PDM RX ----
  i2s_config_t micCfg;
  memset(&micCfg, 0, sizeof(micCfg));
  micCfg.mode                 = (i2s_mode_t)(I2S_MODE_PDM | I2S_MODE_RX | I2S_MODE_MASTER);
  micCfg.sample_rate          = AppConfig::kAudioSampleRate;
  micCfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  micCfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;    // 单声道
  micCfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  micCfg.dma_buf_count        = 4;
  micCfg.dma_buf_len          = 256;

  i2s_driver_uninstall(kMicPort);
  esp_err_t err = i2s_driver_install(kMicPort, &micCfg, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[ERR] mic i2s_driver_install: %d\n", err);
    return false;
  }

  i2s_pin_config_t micPins;
  memset(&micPins, 0, sizeof(micPins));
  micPins.bck_io_num   = I2S_PIN_NO_CHANGE;
  micPins.ws_io_num     = AppConfig::kI2SMicClkPin;   // GPIO2 = PDM CLK (走WS脚!)
  micPins.data_out_num  = I2S_PIN_NO_CHANGE;
  micPins.data_in_num   = AppConfig::kI2SMicDataPin;  // GPIO3 = PDM DATA

  err = i2s_set_pin(kMicPort, &micPins);
  if (err != ESP_OK) {
    Serial.printf("[ERR] mic i2s_set_pin: %d\n", err);
    i2s_driver_uninstall(kMicPort);
    return false;
  }
  Serial.printf("[OK] Mic PDM RX  I2S0  CLK=%d DATA=%d\n",
                AppConfig::kI2SMicClkPin, AppConfig::kI2SMicDataPin);

  // ---- 喇叭: I2S_NUM_1 标准 I2S TX ----
  i2s_config_t spkCfg;
  memset(&spkCfg, 0, sizeof(spkCfg));
  spkCfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  spkCfg.sample_rate          = AppConfig::kAudioSampleRate;
  spkCfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  spkCfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  spkCfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  spkCfg.dma_buf_count        = 4;
  spkCfg.dma_buf_len          = 256;

  i2s_driver_uninstall(kSpkPort);
  err = i2s_driver_install(kSpkPort, &spkCfg, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[ERR] spk i2s_driver_install: %d\n", err);
    i2s_driver_uninstall(kMicPort);
    return false;
  }

  i2s_pin_config_t spkPins;
  memset(&spkPins, 0, sizeof(spkPins));
  spkPins.bck_io_num   = AppConfig::kI2SBclkPin;     // GPIO7
  spkPins.ws_io_num     = AppConfig::kI2SLrclkPin;    // GPIO21
  spkPins.data_out_num  = AppConfig::kI2SSpkDataPin;  // GPIO46
  spkPins.data_in_num   = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin(kSpkPort, &spkPins);
  if (err != ESP_OK) {
    Serial.printf("[ERR] spk i2s_set_pin: %d\n", err);
    i2s_driver_uninstall(kSpkPort);
    i2s_driver_uninstall(kMicPort);
    return false;
  }
  i2s_zero_dma_buffer(kSpkPort);
  Serial.printf("[OK] Spk I2S TX  I2S1  BCLK=%d LRCLK=%d DOUT=%d\n",
                AppConfig::kI2SBclkPin, AppConfig::kI2SLrclkPin, AppConfig::kI2SSpkDataPin);

  ready_ = true;
  return true;
}

bool AudioI2S::readMicChunk(uint8_t* buffer, size_t capacity, size_t& bytesRead) {
  bytesRead = 0;
  if (!ready_ || !buffer || !capacity) return false;
  if (AppConfig::kI2SMicDataPin < 0) return false;

  const TickType_t timeout = pdMS_TO_TICKS(AppConfig::kAsrChunkMs + 40);
  esp_err_t err = i2s_read(kMicPort, buffer, capacity, &bytesRead, timeout);
  if (err != ESP_OK) return false;
  return bytesRead > 0;
}

void AudioI2S::discardMicChunks(int n) {
  if (!ready_ || n <= 0) return;
  size_t chunkSize = AppConfig::kAsrChunkBytes;
  uint8_t* trash = static_cast<uint8_t*>(malloc(chunkSize));
  if (!trash) return;
  for (int i = 0; i < n; ++i) {
    size_t bytesRead = 0;
    i2s_read(kMicPort, trash, chunkSize, &bytesRead, pdMS_TO_TICKS(AppConfig::kAsrChunkMs + 20));
  }
  free(trash);
  Serial.printf("[AUDIO] discarded %d mic chunks\n", n);
}

bool AudioI2S::playPcm(const uint8_t* data, size_t dataSize) {
  if (!ready_ || !data || !dataSize) return false;

  const int16_t* samples = reinterpret_cast<const int16_t*>(data);
  const size_t sampleCount = dataSize / sizeof(int16_t);
  constexpr size_t kChunkSamples = 128;
  std::vector<int16_t> stereoChunk(kChunkSamples * 2);

  for (size_t base = 0; base < sampleCount; base += kChunkSamples) {
    const size_t n = (base + kChunkSamples <= sampleCount) ? kChunkSamples : sampleCount - base;
    for (size_t i = 0; i < n; ++i) {
      stereoChunk[i * 2]     = samples[base + i];
      stereoChunk[i * 2 + 1] = samples[base + i];
    }
    for (size_t i = n; i < kChunkSamples; ++i) {
      stereoChunk[i * 2]     = 0;
      stereoChunk[i * 2 + 1] = 0;
    }
    const size_t byteCount = kChunkSamples * 2 * sizeof(int16_t);
    size_t written = 0;
    const uint8_t* cursor = reinterpret_cast<const uint8_t*>(stereoChunk.data());
    while (written < byteCount) {
      size_t w = 0;
      // M0：有界超时 + 喂狗，禁止 portMAX_DELAY 永久阻塞主任务
      esp_task_wdt_reset();
      esp_err_t err = i2s_write(kSpkPort, cursor + written, byteCount - written, &w,
                                pdMS_TO_TICKS(500));
      if (err != ESP_OK || w == 0) {
        Serial.printf("[I2S] write timeout/err=%d w=%u\n", err, (unsigned)w);
        return false;
      }
      written += w;
    }
  }
  delay(100);
  clearTx();
  return true;
}

void AudioI2S::clearTx() {
  if (ready_) i2s_zero_dma_buffer(kSpkPort);
}

// ISpeaker
bool AudioI2S::play(const std::vector<uint8_t>& pcm) {
  return playPcm(pcm.data(), pcm.size());
}
void AudioI2S::stop() { clearTx(); }

// IMic
bool AudioI2S::readChunk(std::vector<uint8_t>& buf) {
  size_t bytesRead = 0;
  bool ok = readMicChunk(buf.data(), buf.size(), bytesRead);
  buf.resize(bytesRead);
  return ok;
}

// ===== V3 FreeRTOS 播放 task ==========================================

static PcmRingBuffer* g_playRing = nullptr;

static void playTaskLoop(void*) {
  while (true) {
    if (g_playRing && g_playRing->available() > 0) {
      uint8_t buf[2048];
      size_t n = g_playRing->pop(buf, sizeof(buf));
      if (n > 0) {
        const int16_t* samples = (const int16_t*)buf;
        const size_t sampleCount = n / sizeof(int16_t);
        constexpr size_t kChunk = 128;
        int16_t stereo[256];  // 128*2, stack alloc, no heap frag

        for (size_t base = 0; base < sampleCount; base += kChunk) {
          size_t m = (base + kChunk <= sampleCount) ? kChunk : sampleCount - base;
          for (size_t i = 0; i < m; ++i) {
            stereo[i * 2]     = samples[base + i];
            stereo[i * 2 + 1] = samples[base + i];
          }
          size_t byteCount = m * 2 * sizeof(int16_t);
          size_t written = 0;
          const uint8_t* cur = (const uint8_t*)stereo;
          while (written < byteCount) {
            size_t w = 0;
            esp_task_wdt_reset();
            if (i2s_write(kSpkPort, cur + written, byteCount - written, &w,
                          pdMS_TO_TICKS(500)) != ESP_OK || w == 0) {
              break;
            }
            written += w;
          }
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

void AudioI2S::startPlaybackTask(PcmRingBuffer* ring) {
  g_playRing = ring;
  if (playTask_) return;
  xTaskCreatePinnedToCore(
    playTaskLoop, "i2sPlay", 4096, nullptr, 1, &playTask_, 1);  // Core1, 远离I2S
  Serial.println("[I2S] playback task started on Core0");
}

void AudioI2S::stopPlaybackTask() {
  if (playTask_) { vTaskDelete(playTask_); playTask_ = nullptr; }
  g_playRing = nullptr;
  Serial.println("[I2S] playback task stopped");
}

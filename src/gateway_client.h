#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <vector>
#include "app_config.h"

class AudioI2S;
class RawWebSocketClient;
class PcmRingBuffer;

class GatewayClient {
 public:
  bool isConfigReady() const;
  String chatCompletion(const String& userPrompt);

  // V1: single-turn API (backward compat)
  bool transcribeRealtime(AudioI2S& audio, String& transcriptOut);
  bool synthesizeRealtime(const String& text, std::vector<uint8_t>& pcmOut);

  // V2: conversation session (WS keep-alive)
  bool beginConversation();
  void endConversation();
  bool synthesizeAndPlay(const String& text, AudioI2S& audio);
  void setTtsConfig(const char* voice, float speed = 1.0f, float volume = 1.0f);
  void resetAsrSession();
  void clearAsrBuffer();   // 清音频缓冲区，连续对话复用 WS

  // V3: set ring buffer → synthesizeAndPlay 自动流式播
  void setRingBuffer(PcmRingBuffer* ring) { ring_ = ring; }

  // V3: LLM 对话记忆
  void addToHistory(const String& role, const String& content);
  void clearHistory();

  // V3: LLM 回复去重
  bool isReplyDuplicate(const String& reply);

 private:
  String base64Encode(const uint8_t* data, size_t length);
  bool base64DecodeAppend(const String& input, std::vector<uint8_t>& out);

  // V2 persistent WS
  RawWebSocketClient* asrWs_ = nullptr;
  RawWebSocketClient* ttsWs_ = nullptr;
  bool asrReady_ = false;
  bool ttsReady_ = false;

  // LLM HTTPS 持久连接
  WiFiClientSecure* chatClient_ = nullptr;
  uint32_t chatClientMs_ = 0;     // 上次使用时间

  bool ensureAsrSession();
  bool ensureTtsSession();
  bool rebuildAsrSession();
  bool rebuildTtsSession();

  // V3 ring buffer 流式播
  PcmRingBuffer* ring_ = nullptr;

  // V3 去重
  String lastReply_;

  // V3 LLM 对话记忆 (最近 5 轮 = 10 条)
  struct ChatMessage { String role; String content; };
  std::vector<ChatMessage> history_;
  static constexpr int kMaxHistory = 10;

  // TTS 可调参数
  String ttsVoice_  = AppConfig::kTtsVoice;
  float  ttsSpeed_  = 1.0f;
  float  ttsVolume_ = 1.0f;
};

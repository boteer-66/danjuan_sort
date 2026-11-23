#include "gateway_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <mbedtls/base64.h>
#include <functional>

#include "app_config.h"
#include "audio_i2s.h"
#include "raw_websocket_client.h"
#include "utils/pcm_ring_buffer.h"

namespace {

String makeBearer() {
  return String("Bearer ") + AppConfig::kApiKey;
}

String summarizeData(const String& data, size_t maxLen = 120) {
  String summary = data;
  summary.replace("\r", "\\r");
  summary.replace("\n", "\\n");
  if (summary.length() > maxLen) {
    summary.remove(maxLen);
    summary += "...";
  }
  return summary;
}

bool waitForCondition(uint32_t timeoutMs,
                      const std::function<void()>& pump,
                      const std::function<bool()>& predicate) {
  const uint32_t startMs = millis();
  while (millis() - startMs < timeoutMs) {
    esp_task_wdt_reset();
    pump();
    if (predicate()) return true;
    delay(5);
  }
  return predicate();
}

}  // namespace

// ===== V1 / Shared =====================================================

bool GatewayClient::isConfigReady() const {
  return strlen(AppConfig::kApiKey) > 0 &&
         strlen(AppConfig::kChatApiUrl) > 0 &&
         strlen(AppConfig::kAsrRealtimeUrl) > 0 &&
         strlen(AppConfig::kTtsRealtimeUrl) > 0;
}

String GatewayClient::chatCompletion(const String& userPrompt) {
  // 持久 TLS 复用：从 URL 提取 host，30s 内不重复握手
  const char* chatHost = "ai-gateway.vei.volces.com";  // AppConfig::kChatApiUrl 的 host
  if (!chatClient_ || millis() - chatClientMs_ > 120000) {  // 不检查connected(), http.end()会关socket
    if (chatClient_) { delete chatClient_; chatClient_ = nullptr; }
    chatClient_ = new WiFiClientSecure();
    chatClient_->setInsecure();
    uint32_t t0 = millis();
    if (!chatClient_->connect(chatHost, 443)) {
      Serial.printf("[ERR] chat TLS failed %lu ms\n", (unsigned long)(millis() - t0));
      delete chatClient_; chatClient_ = nullptr;
      return "";
    }
    Serial.printf("[LLM] TLS handshake %lu ms\n", (unsigned long)(millis() - t0));
  }
  chatClientMs_ = millis();

  HTTPClient httpClient;
  httpClient.setTimeout(AppConfig::kHttpTimeoutMs);
  httpClient.setReuse(true);

  if (!httpClient.begin(*chatClient_, AppConfig::kChatApiUrl)) {
    // socket 已死 → 重建重试一次
    Serial.println("[ERR] chat begin failed, 重建重试...");
    delete chatClient_; chatClient_ = new WiFiClientSecure();
    chatClient_->setInsecure();
    if (!chatClient_->connect(chatHost, 443)) {
      Serial.println("[ERR] chat TLS retry failed");
      delete chatClient_; chatClient_ = nullptr;
      return "";
    }
    chatClientMs_ = millis();
    if (!httpClient.begin(*chatClient_, AppConfig::kChatApiUrl)) {
      delete chatClient_; chatClient_ = nullptr;
      return "";
    }
  }

  httpClient.addHeader("Content-Type", "application/json");
  httpClient.addHeader("Authorization", makeBearer());

  JsonDocument payloadDoc;
  payloadDoc["model"] = AppConfig::kChatModel;
  payloadDoc["max_tokens"] = 80;
  payloadDoc["temperature"] = 0.7;

  JsonArray messages = payloadDoc["messages"].to<JsonArray>();
  JsonObject sys = messages.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = AppConfig::kAssistantSystemPrompt;

  for (const auto& msg : history_) {
    JsonObject m = messages.add<JsonObject>();
    m["role"] = msg.role;
    m["content"] = msg.content;
  }

  JsonObject usr = messages.add<JsonObject>();
  usr["role"] = "user";
  usr["content"] = userPrompt;

  String payload;
  serializeJson(payloadDoc, payload);

  const int httpCode = httpClient.POST(payload);
  if (httpCode <= 0) {
    Serial.printf("[ERR] chat POST failed: %d %s\n",
                  httpCode, httpClient.errorToString(httpCode).c_str());
    httpClient.end();
    delete chatClient_; chatClient_ = nullptr;
    return "";
  }

  const String response = httpClient.getString();
  httpClient.end();  // 只关 HTTPClient，不关底层 socket

  if (httpCode < 200 || httpCode >= 300) {
    Serial.printf("[ERR] chat status=%d\n%s\n", httpCode, response.c_str());
    return "";
  }

  JsonDocument responseDoc;
  if (deserializeJson(responseDoc, response)) {
    Serial.printf("[ERR] chat JSON parse failed\n");
    return "";
  }

  const char* reply = responseDoc["choices"][0]["message"]["content"];
  if (!reply) return "";

  String result(reply);
  result.trim();
  return result;
}

// ===== V1 ASR (supports persistent WS) =================================

bool GatewayClient::transcribeRealtime(AudioI2S& audio, String& transcriptOut) {
  transcriptOut = "";

  const String bearer = makeBearer();
  RawWebSocketClient* ws = nullptr;
  bool ownWs = false;  // true = we created it, must close

  // V2: try persistent WS（不复build——火山同一WS不接受二次session.update）
  if (asrWs_ && asrWs_->isConnected()) {
    ws = asrWs_;
    Serial.println("[ASR] reuse persistent WS");
  } else if (asrWs_) {
    delete asrWs_; asrWs_ = nullptr; asrReady_ = false;
  }

  if (!ws) {
    ws = new RawWebSocketClient("ASR");
    ownWs = true;
    asrReady_ = false;

    Serial.printf("[ASR][WS] connecting to %s\n", AppConfig::kAsrRealtimeUrl);
    uint32_t t0 = millis();
    if (!ws->connect(AppConfig::kAsrRealtimeUrl, bearer)) {
      Serial.printf("[ERR] ASR WS connect failed %lu ms\n", (unsigned long)(millis() - t0));
      delete ws;
      return false;
    }
    Serial.printf("[ASR][WS] connected %lu ms\n", (unsigned long)(millis() - t0));

    JsonDocument doc;
    doc["type"] = "transcription_session.update";
    JsonObject s = doc["session"].to<JsonObject>();
    s["input_audio_format"]       = "pcm";
    s["input_audio_codec"]        = "raw";
    s["input_audio_sample_rate"]  = AppConfig::kAudioSampleRate;
    s["input_audio_bits"]         = AppConfig::kAudioBitsPerSample;
    s["input_audio_channel"]      = AppConfig::kAudioChannels;
    JsonObject t = s["input_audio_transcription"].to<JsonObject>();
    t["model"] = "volcano_asr";

    String json;
    serializeJson(doc, json);
    if (!ws->sendText(json)) {
      Serial.println("[ERR] ASR session update send failed");
      delete ws;
      return false;
    }

    bool ready = false;
    auto pump = [&]() {
      String payload;
      while (ws->pollTextMessage(payload)) {
        JsonDocument ev;
        if (!deserializeJson(ev, payload.c_str()) && ev["type"] == "transcription_session.updated")
          ready = true;
      }
    };
    if (!waitForCondition(AppConfig::kWsConnectTimeoutMs, pump, [&]() { return ready; })) {
      Serial.println("[ERR] ASR session update timeout");
      delete ws;
      return false;
    }

    asrWs_ = ws;
    asrReady_ = true;
    ownWs = false;  // 移交持久成员，不再负责释放
  }

  // ---- record & stream ----
  bool sessionReady = asrReady_;
  bool commitSent   = false;
  bool finalReady   = false;
  bool sawPartial   = false;
  bool wsError      = false;
  String finalTranscript;
  uint32_t lastPartialMs = 0;

  auto pumpMessages = [&]() {
    String payload;
    while (ws->pollTextMessage(payload)) {
      JsonDocument event;
      if (deserializeJson(event, payload.c_str()) != DeserializationError::Ok) continue;

      const String type = event["type"] | "";
      if (type == "error") {
        wsError = true;
        Serial.printf("[ASR] server error: %s\n", (event["message"] | "unknown"));
        continue;
      }
      if (type == "transcription_session.updated") { sessionReady = true; continue; }
      if (type == "conversation.item.input_audio_transcription.result") {
        const String p = event["transcript"] | "";
        if (!p.isEmpty()) { sawPartial = true; lastPartialMs = millis();
          Serial.printf("[ASR] partial: %s\n", p.c_str()); }
        continue;
      }
      if (type == "conversation.item.input_audio_transcription.completed") {
        finalTranscript = event["transcript"] | "";
        finalReady = true;
        Serial.printf("[ASR] final: %s\n", finalTranscript.c_str());
        continue;
      }
      if (!type.isEmpty()) Serial.printf("[ASR] event=%s\n", type.c_str());
    }
  };

  std::vector<uint8_t> chunk(AppConfig::kAsrChunkBytes);
  const uint32_t startMs = millis();

  while (millis() - startMs < AppConfig::kAsrHardLimitMs) {
    size_t bytesRead = 0;
    if (!audio.readMicChunk(chunk.data(), chunk.size(), bytesRead)) {
      if (ownWs) { ws->close(); delete ws; }
      return false;
    }

    String audioB64 = base64Encode(chunk.data(), bytesRead);
    JsonDocument appendDoc;
    appendDoc["type"] = "input_audio_buffer.append";
    appendDoc["audio"] = audioB64;
    String appendJson;
    serializeJson(appendDoc, appendJson);
    if (!ws->sendText(appendJson)) {
      Serial.println("[ERR] ASR audio append send failed");
      if (ownWs) { ws->close(); delete ws; }
      return false;
    }

    pumpMessages();

    if (sawPartial && millis() - lastPartialMs > AppConfig::kAsrResultIdleCommitMs) break;
  }

  if (!commitSent) {
    JsonDocument commitDoc;
    commitDoc["type"] = "input_audio_buffer.commit";
    String commitJson;
    serializeJson(commitDoc, commitJson);
    ws->sendText(commitJson);
    commitSent = true;
  }

  bool gotFinal = waitForCondition(AppConfig::kAsrFinalWaitMs, pumpMessages,
                                   [&]() { return finalReady; });

  if (ownWs) {
    ws->close(); delete ws;
  }
  // persistent WS: 出错就销毁，下次自动重建
  if (wsError && !ownWs) {
    delete asrWs_; asrWs_ = nullptr; asrReady_ = false;
    Serial.println("[ASR] 会话过期，已销毁（下次重建）");
  }

  if (!gotFinal || finalTranscript.isEmpty()) {
    Serial.println("[ERR] ASR final timeout/empty");
    return false;
  }

  transcriptOut = finalTranscript;
  return true;
}

// ===== V1 TTS (backward compat, one-shot) ==============================

bool GatewayClient::synthesizeRealtime(const String& text, std::vector<uint8_t>& pcmOut) {
  pcmOut.clear();

  const String bearer = makeBearer();
  RawWebSocketClient client("TTS");

  bool sessionReady = false;
  bool audioDone    = false;

  auto pump = [&]() {
    String payload;
    while (client.pollTextMessage(payload)) {
      JsonDocument event;
      if (deserializeJson(event, payload.c_str()) != DeserializationError::Ok) continue;
      const String type = event["type"] | "";
      if (type == "tts_session.updated") { sessionReady = true; continue; }
      if (type == "response.audio.delta") {
        const String delta = event["delta"] | "";
        if (!delta.isEmpty()) base64DecodeAppend(delta, pcmOut);
        continue;
      }
      if (type == "response.audio.done") { audioDone = true; continue; }
      if (!type.isEmpty()) Serial.printf("[TTS] event=%s\n", type.c_str());
    }
  };

  Serial.printf("[TTS][WS] connecting to %s\n", AppConfig::kTtsRealtimeUrl);
  uint32_t t0 = millis();
  if (!client.connect(AppConfig::kTtsRealtimeUrl, bearer)) {
    Serial.printf("[ERR] TTS WS connect failed %lu ms\n", (unsigned long)(millis() - t0));
    return false;
  }
  Serial.printf("[TTS][WS] connected %lu ms\n", (unsigned long)(millis() - t0));

  JsonDocument doc;
  doc["type"] = "tts_session.update";
  JsonObject s = doc["session"].to<JsonObject>();
  s["voice"] = ttsVoice_;
  s["output_audio_format"] = "pcm";
  s["output_audio_sample_rate"] = AppConfig::kAudioSampleRate;
  if (ttsSpeed_ != 1.0f)  s["speed_ratio"] = ttsSpeed_;
  if (ttsVolume_ != 1.0f) s["volume_ratio"] = ttsVolume_;
  String json;
  serializeJson(doc, json);
  if (!client.sendText(json)) { client.close(); return false; }
  if (!waitForCondition(AppConfig::kWsConnectTimeoutMs, pump, [&]() { return sessionReady; })) {
    client.close(); return false;
  }

  JsonDocument inputDoc;
  inputDoc["type"] = "input_text.append";
  inputDoc["delta"] = text;
  serializeJson(inputDoc, json);
  client.sendText(json);

  JsonDocument doneDoc;
  doneDoc["type"] = "input_text.done";
  serializeJson(doneDoc, json);
  client.sendText(json);

  bool gotDone = waitForCondition(AppConfig::kTtsFinalWaitMs, pump, [&]() { return audioDone; });
  client.close();

  if (!gotDone || pcmOut.empty()) return false;
  Serial.printf("[TTS] pcm=%u B\n", (unsigned)pcmOut.size());
  return true;
}

// ===== V2: Conversation session ========================================

bool GatewayClient::beginConversation() {
  endConversation();  // clean slate

  const String bearer = makeBearer();

  // -- ASR WS only（TTS 懒加载，省 1.3s 前端等待）--
  asrWs_ = new RawWebSocketClient("ASR");
  Serial.printf("[V2] ASR WS connecting...\n");
  uint32_t t0 = millis();
  if (!asrWs_->connect(AppConfig::kAsrRealtimeUrl, bearer)) {
    Serial.printf("[ERR] V2 ASR connect failed %lu ms\n", (unsigned long)(millis() - t0));
    delete asrWs_; asrWs_ = nullptr;
    return false;
  }
  Serial.printf("[V2] ASR WS connected %lu ms\n", (unsigned long)(millis() - t0));

  if (!rebuildAsrSession()) {
    endConversation();
    return false;
  }

  // TTS WS 不在唤醒时建立——第一时刻只需 ASR 开始听。
  // synthesizeAndPlay() 首次调用时懒创建 TTS WS，TLS 藏于 LLM 等待期。

  Serial.println("[V2] session ready (ASR only)");
  return true;
}

void GatewayClient::endConversation() {
  if (asrWs_) { asrWs_->close(); delete asrWs_; asrWs_ = nullptr; asrReady_ = false; }
  if (ttsWs_) { ttsWs_->close(); delete ttsWs_; ttsWs_ = nullptr; ttsReady_ = false; }
  Serial.println("[V2] conversation session closed");
}

// ===== V2: Streaming TTS + playback ====================================

void GatewayClient::setTtsConfig(const char* voice, float speed, float volume) {
  if (voice && voice[0]) ttsVoice_ = voice;
  ttsSpeed_  = speed;
  ttsVolume_ = volume;
}

bool GatewayClient::synthesizeAndPlay(const String& text, AudioI2S& audio) {
  RawWebSocketClient* ws = nullptr;
  bool ownWs = false;
  const String bearer = makeBearer();
  uint32_t tConnect = 0;

  // 每次新建 WS，音色/语速/音量变化自动生效
  if (ttsWs_) {
    delete ttsWs_; ttsWs_ = nullptr; ttsReady_ = false;
  }

  {
    ws = new RawWebSocketClient("TTS");
    ownWs = true;
    ttsReady_ = false;
    tConnect = millis();
    if (!ws->connect(AppConfig::kTtsRealtimeUrl, bearer)) {
      Serial.printf("[ERR] TTS connect failed %lu ms\n", (unsigned long)(millis() - tConnect));
      delete ws;
      return false;
    }

    JsonDocument doc;
    doc["type"] = "tts_session.update";
    JsonObject s = doc["session"].to<JsonObject>();
    s["voice"] = ttsVoice_;
    s["output_audio_format"] = "pcm";
    s["output_audio_sample_rate"] = AppConfig::kAudioSampleRate;
    if (ttsSpeed_ != 1.0f)  s["speed_ratio"] = ttsSpeed_;
    if (ttsVolume_ != 1.0f) s["volume_ratio"] = ttsVolume_;
    String json;
    serializeJson(doc, json);
    if (!ws->sendText(json)) { delete ws; return false; }

    bool ready = false;
    auto pump = [&]() {
      String p;
      while (ws->pollTextMessage(p)) {
        JsonDocument ev;
        if (!deserializeJson(ev, p.c_str()) && ev["type"] == "tts_session.updated") ready = true;
      }
    };
    if (!waitForCondition(AppConfig::kWsConnectTimeoutMs, pump, [&]() { return ready; })) {
      delete ws; return false;
    }
    Serial.printf("[TTS] WS connected %lu ms\n", (unsigned long)(millis() - tConnect));

    ttsWs_ = ws; ttsReady_ = true;
    ownWs = false;  // 移交持久成员
  }

  // Send text (check return)
  bool sendOk = false;
  {
    JsonDocument doc;
    doc["type"] = "input_text.append";
    doc["delta"] = text;
    String json;
    serializeJson(doc, json);
    sendOk = ws->sendText(json);
  }
  if (sendOk) {
    JsonDocument doc;
    doc["type"] = "input_text.done";
    String json;
    serializeJson(doc, json);
    sendOk = ws->sendText(json);
  }
  if (!sendOk) {
    Serial.println("[ERR] TTS send text failed");
    if (ownWs) { ws->close(); delete ws; }
    else { delete ttsWs_; ttsWs_ = nullptr; ttsReady_ = false; }
    return false;
  }

  // --- V3 分阶段: 攒够 32KB(1s)就灌ring, 边收边灌 ---
  if (ring_) {
    std::vector<uint8_t> buffer;
    buffer.reserve(131072);
    bool audioDone   = false;
    bool streaming   = false;  // 到阈值后开始流式灌
    constexpr size_t        kStreamThreshold = 32768;  // 32KB = 1s @16kHz
    constexpr uint32_t      kTimeoutMs = 15000;
    const uint32_t          tStart     = millis();

    auto pumpTts = [&]() {
      String payload;
      while (ws->pollTextMessage(payload)) {
        JsonDocument event;
        if (deserializeJson(event, payload.c_str()) != DeserializationError::Ok) continue;
        const String type = event["type"] | "";
        if (type == "response.audio.delta") {
          const String delta = event["delta"] | "";
          if (!delta.isEmpty()) {
            base64DecodeAppend(delta, buffer);
            // 攒够阈值就开始灌 ring
            if (!streaming && buffer.size() >= kStreamThreshold) {
              streaming = true;
              Serial.printf("[TTS] 32KB ready, start streaming\n");
            }
            if (streaming) {
              while (buffer.size() > 0) {
                size_t n = ring_->push(buffer.data(), buffer.size());
                if (n == 0) { delay(2); break; }
                buffer.erase(buffer.begin(), buffer.begin() + n);
              }
            }
          }
          continue;
        }
        if (type == "response.audio.done") { audioDone = true; continue; }
        if (type == "response.audio_subtitle.delta") continue;
        if (!type.isEmpty()) Serial.printf("[TTS] event=%s\n", type.c_str());
      }
    };

    while (!audioDone) {
      if (!ws->isConnected()) { Serial.println("[ERR] TTS WS lost"); break; }
      if (millis() - tStart > kTimeoutMs) {
        Serial.printf("[ERR] TTS timeout\n"); break;
      }
      pumpTts();
      if (!audioDone) delay(10);
    }
    if (ownWs) { ws->close(); delete ws; }
    // 灌完剩余
    while (buffer.size() > 0) {
      esp_task_wdt_reset();
      size_t n = ring_->push(buffer.data(), buffer.size());
      if (n == 0) delay(2);
      buffer.erase(buffer.begin(), buffer.begin() + n);
    }
    while (ring_->available() > 0) {
      esp_task_wdt_reset();
      delay(10);
    }
    delay(100);
    audio.stop();
    return audioDone;

  } else {
    // === V2 fallback: 先收完再播 ===
    std::vector<uint8_t> buffer;
    buffer.reserve(131072);
    bool audioDone = false;
    constexpr uint32_t kTtsStreamTimeoutMs = 15000;
    const uint32_t tStart = millis();

    auto pumpTts = [&]() {
      String payload;
      while (ws->pollTextMessage(payload)) {
        JsonDocument event;
        if (deserializeJson(event, payload.c_str()) != DeserializationError::Ok) continue;
        const String type = event["type"] | "";
        if (type == "response.audio.delta") {
          const String delta = event["delta"] | "";
          if (!delta.isEmpty()) base64DecodeAppend(delta, buffer);
          continue;
        }
        if (type == "response.audio.done") { audioDone = true; continue; }
        if (type == "response.audio_subtitle.delta") continue;
        if (!type.isEmpty()) Serial.printf("[TTS] event=%s\n", type.c_str());
      }
    };

    while (!audioDone) {
      if (!ws->isConnected()) { Serial.println("[ERR] TTS WS lost"); break; }
      if (millis() - tStart > kTtsStreamTimeoutMs) {
        Serial.printf("[ERR] TTS timeout %u ms\n", (unsigned)(millis() - tStart));
        break;
      }
      pumpTts();
      if (!audioDone) delay(10);
    }

    if (ownWs) { ws->close(); delete ws; }

    if (buffer.empty()) return false;
    Serial.printf("[TTS] %u B collected, playing...\n", (unsigned)buffer.size());
    audio.playPcm(buffer.data(), buffer.size());
    audio.stop();
    return audioDone;
  }
}

// ===== V2: Session rebuild helpers =====================================

void GatewayClient::resetAsrSession() {
  if (asrWs_) { delete asrWs_; asrWs_ = nullptr; asrReady_ = false; }
}

void GatewayClient::clearAsrBuffer() {
  if (!asrWs_ || !asrWs_->isConnected()) return;
  JsonDocument doc;
  doc["type"] = "input_audio_buffer.clear";
  String json;
  serializeJson(doc, json);
  asrWs_->sendText(json);
  Serial.println("[ASR] buffer cleared");
}

bool GatewayClient::rebuildAsrSession() {
  if (!asrWs_ || !asrWs_->isConnected()) return false;

  JsonDocument doc;
  doc["type"] = "transcription_session.update";
  JsonObject s = doc["session"].to<JsonObject>();
  s["input_audio_format"]       = "pcm";
  s["input_audio_codec"]        = "raw";
  s["input_audio_sample_rate"]  = AppConfig::kAudioSampleRate;
  s["input_audio_bits"]         = AppConfig::kAudioBitsPerSample;
  s["input_audio_channel"]      = AppConfig::kAudioChannels;
  JsonObject t = s["input_audio_transcription"].to<JsonObject>();
  t["model"] = "volcano_asr";

  String json;
  serializeJson(doc, json);
  if (!asrWs_->sendText(json)) return false;

  bool ready = false;
  auto pump = [&]() {
    String p;
    while (asrWs_->pollTextMessage(p)) {
      JsonDocument ev;
      if (!deserializeJson(ev, p.c_str()) && ev["type"] == "transcription_session.updated")
        ready = true;
    }
  };
  return waitForCondition(AppConfig::kWsConnectTimeoutMs, pump, [&]() { return ready; });
}

bool GatewayClient::rebuildTtsSession() {
  if (!ttsWs_ || !ttsWs_->isConnected()) return false;

  JsonDocument doc;
  doc["type"] = "tts_session.update";
  JsonObject s = doc["session"].to<JsonObject>();
  s["voice"] = ttsVoice_;
  s["output_audio_format"] = "pcm";
  s["output_audio_sample_rate"] = AppConfig::kAudioSampleRate;
  if (ttsSpeed_ != 1.0f)  s["speed_ratio"] = ttsSpeed_;
  if (ttsVolume_ != 1.0f) s["volume_ratio"] = ttsVolume_;

  String json;
  serializeJson(doc, json);
  if (!ttsWs_->sendText(json)) return false;

  bool ready = false;
  auto pump = [&]() {
    String p;
    while (ttsWs_->pollTextMessage(p)) {
      JsonDocument ev;
      if (!deserializeJson(ev, p.c_str()) && ev["type"] == "tts_session.updated")
        ready = true;
    }
  };
  return waitForCondition(AppConfig::kWsConnectTimeoutMs, pump, [&]() { return ready; });
}

bool GatewayClient::ensureAsrSession() {
  if (asrWs_ && asrWs_->isConnected() && asrReady_) return true;
  // fallback: first call to transcribeRealtime will create it
  return true;
}

bool GatewayClient::ensureTtsSession() {
  if (ttsWs_ && ttsWs_->isConnected() && ttsReady_) return true;
  return true;
}

// ===== V3 LLM 对话记忆 =================================================

void GatewayClient::addToHistory(const String& role, const String& content) {
  history_.push_back({role, content});
  while ((int)history_.size() > kMaxHistory)
    history_.erase(history_.begin());
}

void GatewayClient::clearHistory() {
  history_.clear();
}

// ===== V3 LLM 回复去重 =================================================

bool GatewayClient::isReplyDuplicate(const String& reply) {
  if (reply == lastReply_) return true;
  lastReply_ = reply;
  return false;
}

// ===== Base64 helpers ==================================================

String GatewayClient::base64Encode(const uint8_t* data, size_t length) {
  size_t outputLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outputLen, data, length);
  std::vector<uint8_t> buffer(outputLen + 1, 0);
  if (mbedtls_base64_encode(buffer.data(), buffer.size(), &outputLen, data, length) != 0)
    return "";
  return String(reinterpret_cast<const char*>(buffer.data()));
}

bool GatewayClient::base64DecodeAppend(const String& input, std::vector<uint8_t>& out) {
  size_t decodedLen = 0;
  mbedtls_base64_decode(nullptr, 0, &decodedLen,
                        reinterpret_cast<const unsigned char*>(input.c_str()), input.length());
  if (decodedLen == 0) return false;
  const size_t oldSize = out.size();
  out.resize(oldSize + decodedLen);
  if (mbedtls_base64_decode(out.data() + oldSize, decodedLen, &decodedLen,
                            reinterpret_cast<const unsigned char*>(input.c_str()),
                            input.length()) != 0) {
    out.resize(oldSize);
    return false;
  }
  out.resize(oldSize + decodedLen);
  return true;
}

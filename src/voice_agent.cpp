#include "voice_agent.h"
#include <Arduino.h>
#include <vector>
#include "app_config.h"
#include "globals.h"

// 历史摘要函数（定义在 main.cpp）
String getAlertHistorySummary();

// ===== 语音：本地提示音（100ms 短 beep） =====
void playLocalBeep() {
  std::vector<uint8_t> beep;
  constexpr int kSamples = 16000 * 100 / 1000;  // 100ms @16kHz
  beep.reserve(kSamples * 2);
  for (int i = 0; i < kSamples; i++) {
    int16_t v = (int16_t)(sin(2 * PI * 800 * i / 16000.0f) * 4000);
    beep.push_back(v & 0xFF);
    beep.push_back((v >> 8) & 0xFF);
  }
  g_audio.play(beep);
  g_audio.stop();
  g_audio.discardMicChunks(2);
}

// ===== 语音：单轮对话（ASR → LLM → TTS → 播放） =====
void runConversationOnce() {
  g_voiceActive = true;
  Serial.println("[VOICE] === 对话开始 ===");

  playLocalBeep();

  // 首次调用无旧 WS，clear 无意义；连续对话才需要 reset
  static int asrFailStreak = 0;
  String transcript;
  bool asrOk = g_cloud.transcribeRealtime(g_audio, transcript);
  if (!asrOk || transcript.isEmpty()) {
    Serial.println("[VOICE] ASR 失败，清缓冲+重建重试...");
    g_cloud.clearAsrBuffer();
    g_cloud.resetAsrSession();
    delay(200);  // 让 mic 稳定
    asrOk = g_cloud.transcribeRealtime(g_audio, transcript);
  }
  if (!asrOk || transcript.isEmpty()) {
    asrFailStreak++;
    Serial.printf("[VOICE] ASR 超时/空 (连败%d次)\n", asrFailStreak);
    if (asrFailStreak >= 2) {
      // 连续失败 2 次 → 退出连续对话
      asrFailStreak = 0;
      g_continuousTalk = false;
      if (g_mqtt.connected()) g_mqtt.publishStatus("MONITORING", g_scene, true);
      Serial.println("[VOICE] 连续失败，退出对话模式");
    }
    g_voiceActive = false;
    return;
  }
  asrFailStreak = 0;  // 成功则清零
  Serial.printf("[ASR]  %s\n", transcript.c_str());

  // LLM 推理（注入告警历史上下文）
  String histSummary = getAlertHistorySummary();
  String prompt = transcript;
  if (histSummary.length() > 0) {
    prompt = "[设备最近巡检告警记录]\n" + histSummary + "\n[用户问题] " + transcript;
  }
  String reply = g_cloud.chatCompletion(prompt);
  if (reply.isEmpty()) {
    Serial.println("[VOICE] LLM 空回复");
    g_voiceActive = false;
    return;
  }
  Serial.printf("[LLM]  %s\n", reply.c_str());

  // 检查退出词 → 告别 → 退出
  if (reply.indexOf("拜拜") >= 0 || reply.indexOf("退出") >= 0 ||
      reply.indexOf("再见") >= 0 || reply.indexOf("睡觉") >= 0) {
    g_continuousTalk = false;
    if (g_mqtt.connected()) g_mqtt.publishStatus("MONITORING", g_scene, true);
    g_cloud.synthesizeAndPlay("好的，有需要再叫我呀～拜拜", g_audio);
    g_voiceActive = false;
    return;
  }

  // TTS + 播放
  g_cloud.synthesizeAndPlay(reply, g_audio);

  // 播放后洗 mic（TTS 长音频对麦直灌，多洗几轮）
  g_audio.stop();
  g_audio.discardMicChunks(12);  // ~2.4s 清干净

  g_voiceActive = false;
  Serial.println("[VOICE] === 对话结束 ===");
}

// ===== SU-03T 轮询 =====
void pollSU03T() {
  if (g_voiceActive) return;
  if (millis() < g_wakeCooldownEnd) return;

  String reason;
  if (g_su03t.pollWakeSignal(reason)) {
    Serial.printf("[WAKE] %s\n", reason.c_str());
    g_wakeCooldownEnd = millis() + AppConfig::kWakeCooldownMs;
    gFsm.feed(Event::Kind::WAKE_WORD);
    runConversationOnce();
    gFsm.feed(Event::Kind::FRAME_DIFF_NORMAL);
  }
}

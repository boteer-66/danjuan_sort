#pragma once

#include <Arduino.h>
#include <cstdint>

namespace Event {

enum class Kind : uint8_t {
  NONE,
  BOOT_DONE,
  // ---- 巡检事件 ----
  FRAME_DIFF_HIGH,       // 帧差分异常（运动）
  FRAME_DIFF_NORMAL,     // 帧差分正常
  FRAME_DIFF_STILL,      // 长时间静止 → 触发关注
  CAPTURE_DONE,          // 拍照完成
  // ---- 云端事件 ----
  UPLOAD_DONE,           // TOS 上传成功
  UPLOAD_ERROR,          // 上传失败（存队列，稍后重试）
  ANALYZE_DONE,          // 云端确认异常 → 进告警
  ANALYZE_NORMAL,        // 云端判定正常 → 回巡检（猫动了，不是跌倒）
  ANALYZE_ERROR,         // 云端分析失败
  // ---- 语音事件 ----
  WAKE_WORD,             // SU03T 唤醒词
  ASR_DONE,              // 云端 ASR 返回转写
  ASR_TIMEOUT,           // ASR 超时
  LLM_DONE,              // LLM 推理完成
  LLM_ERROR,             // LLM 失败
  TTS_DONE,              // TTS 播放完毕
  TTS_ERROR,             // TTS 失败
  // ---- 系统事件 ----
  WIFI_LOST,
  WIFI_RECOVERED,
  ALERT_RESOLVED,        // 告警解除（用户确认/云端判断正常）
  MODE_SWITCH,           // 老人看护 ↔ 工位健康
  ERROR_RECOVERED,       // 模块故障自恢复成功
  SD_ERROR,
  APP_ERROR,
};

struct FrameDiffEvent {
  float   score;
  bool    alert;
};

struct AsrEvent {
  String  transcript;
  bool    timeout;
};

struct LlmEvent {
  String  reply;
  bool    error;
};

struct CloudEvent {
  String  eventType;     // "long_stillness" / "fall" / "posture" / "fatigue"
  float   confidence;
  String  summary;
};

}  // namespace Event

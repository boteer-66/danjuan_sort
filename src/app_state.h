#pragma once

// ===== FSM 最终版：12 状态 + 优先级仲裁 =====
// 2026-05-24 定版
//
// 状态分为 3 个优先级（数值越小越高）:
//   P0(红色) = 安全告警、断网、硬件故障 — 不可打断
//   P1(黄色) = 语音交互 — 打断巡检，但打不断告警
//   P2(绿色) = 巡检+上传 — 被语音和告警中断

enum class AppState : uint8_t {
  // ---- P2 巡检链 (绿色) ----
  Idle = 0,          // 待机
  Monitoring,        // 摄像头巡检中（帧差法）
  Capturing,         // 拍照中
  Uploading,         // 上传 TOS 中
  Analyzing,         // 等待云端分析结果

  // ---- P1 语音链 (黄色) ----
  Listening,         // 麦克风收音 → ASR
  Thinking,          // LLM 推理中
  Speaking,          // TTS 播放中

  // ---- P0 安全与异常 (红色) ----
  Alerting,          // 确认异常，推送告警（最高优先级）
  OfflineFallback,   // WiFi 断网，本地降级
  Error,             // 模块故障，尝试自恢复

  // ---- 过渡 ----
  Booting,           // 上电初始化
};

inline const char* AppStateName(AppState state) {
  switch (state) {
    case AppState::Booting:         return "BOOTING";
    case AppState::Idle:            return "IDLE";
    case AppState::Monitoring:      return "MONITORING";
    case AppState::Capturing:       return "CAPTURING";
    case AppState::Uploading:       return "UPLOADING";
    case AppState::Analyzing:       return "ANALYZING";
    case AppState::Listening:       return "LISTENING";
    case AppState::Thinking:        return "THINKING";
    case AppState::Speaking:        return "SPEAKING";
    case AppState::Alerting:        return "ALERTING";
    case AppState::OfflineFallback: return "OFFLINE";
    case AppState::Error:           return "ERROR";
    default:                        return "UNKNOWN";
  }
}

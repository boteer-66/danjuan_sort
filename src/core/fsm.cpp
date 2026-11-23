#include <Arduino.h>
#include "fsm.h"

Fsm gFsm;

Fsm::Fsm() {}

bool Fsm::feed(Event::Kind event, const void* /*payload*/) {
  AppState to = nextState(event);
  if (to == state_) return false;
  transit(to);
  return true;
}

AppState Fsm::nextState(Event::Kind event) const {
  switch (event) {
    // ---- 引导 ----
    case Event::Kind::BOOT_DONE:
      return AppState::Idle;

    // ---- 巡检链 (Idle → Monitoring → Capturing → Uploading → Analyzing) ----
    case Event::Kind::FRAME_DIFF_NORMAL:
      return AppState::Monitoring;

    case Event::Kind::FRAME_DIFF_HIGH:   // 异常运动 → 拍照取证
    case Event::Kind::FRAME_DIFF_STILL:  // 长时间静止 → 拍照
      if (state_ == AppState::Alerting) return AppState::Alerting;
      return AppState::Capturing;

    case Event::Kind::CAPTURE_DONE:
      return AppState::Uploading;

    case Event::Kind::UPLOAD_DONE:
      return AppState::Analyzing;

    case Event::Kind::ANALYZE_DONE:
      return AppState::Alerting;   // 云端确认异常 → 告警

    case Event::Kind::ANALYZE_NORMAL:
      return AppState::Monitoring; // 云端说没事（猫/窗帘）→ 回巡检

    case Event::Kind::ANALYZE_ERROR:
      return AppState::Monitoring; // 分析失败 → 继续巡检

    case Event::Kind::UPLOAD_ERROR:
      return AppState::Monitoring; // 上传失败存队列 → 继续巡检

    // ---- 语音链 (多轮对话: Listening ↔ Thinking ↔ Speaking, 超时3次退出) ----
    case Event::Kind::WAKE_WORD:
      if (state_ == AppState::Alerting) return AppState::Alerting;
      return AppState::Listening;  // 唤醒 → 开始听

    case Event::Kind::ASR_DONE:
      return AppState::Thinking;   // 听到了 → 去想

    case Event::Kind::ASR_TIMEOUT:
      return AppState::Listening;  // 没听到 → 继续听（超时计数在onEnter里）

    case Event::Kind::LLM_DONE:
      return AppState::Speaking;   // 想好了 → 去说

    case Event::Kind::LLM_ERROR:
      return AppState::Listening;  // LLM挂了 → 继续听下一句

    case Event::Kind::TTS_DONE:
      return AppState::Listening;  // 说完了 → 继续听下一句

    case Event::Kind::TTS_ERROR:
      return AppState::Listening;  // TTS挂了 → 继续听

    // ---- 告警恢复 ----
    case Event::Kind::ALERT_RESOLVED:
      return AppState::Monitoring;

    // ---- 模式切换 ----
    case Event::Kind::MODE_SWITCH:
      return AppState::Idle;  // 切模式 → 重置背景模型 → 回待机

    // ---- 系统异常 ----
    case Event::Kind::WIFI_LOST:
      if (state_ == AppState::Alerting || state_ == AppState::OfflineFallback)
        return state_;
      return AppState::OfflineFallback;

    case Event::Kind::WIFI_RECOVERED:
      return AppState::Monitoring;  // 网络恢复 → 直接巡检

    case Event::Kind::ERROR_RECOVERED:
      return AppState::Idle;

    case Event::Kind::SD_ERROR:
    case Event::Kind::APP_ERROR:
      return AppState::Error;

    default:
      return state_;
  }
}

void Fsm::transit(AppState to) {
  AppState from = state_;
  int idxFrom = static_cast<int>(from);
  int idxTo   = static_cast<int>(to);

  if (exit_[idxFrom]) exit_[idxFrom]();
  state_ = to;
  Serial.printf("[FSM] %s → %s\n", AppStateName(from), AppStateName(to));
  if (enter_[idxTo]) enter_[idxTo]();
}

void Fsm::forceState(AppState st) {
  AppState from = state_;
  if (exit_[static_cast<int>(from)]) exit_[static_cast<int>(from)]();
  state_ = st;
  Serial.printf("[FSM] force → %s\n", AppStateName(st));
  if (enter_[static_cast<int>(st)]) enter_[static_cast<int>(st)]();
}

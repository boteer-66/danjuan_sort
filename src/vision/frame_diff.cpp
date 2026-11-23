#include "frame_diff.h"

#include <cstring>

FrameDiff::FrameDiff() {
  memset(bgAvg_, 0, sizeof(bgAvg_));
}

FrameDiff::Config FrameDiff::elderCareConfig() {
  Config c;
  c.diffThreshold    = 0.15f;   // 灵敏——老人不动要立刻知道
  c.stillAlertMs     = 300000;  // 5 分钟
  c.confirmFrames    = 3;       // 3 帧确认，过滤猫/窗帘
  c.patrolIntervalMs = 2000;    // 2 秒一帧
  c.bgUpdateFrames   = 15;      // 每 30 秒更新背景
  c.bgBlendFactor    = 0.03f;   // 慢融合
  return c;
}

FrameDiff::Config FrameDiff::workspaceConfig() {
  Config c;
  c.diffThreshold    = 0.25f;   // 宽松——专注工作不是异常
  c.stillAlertMs     = 5400000; // 90 分钟
  c.confirmFrames    = 5;       // 5 帧确认，更不骚扰
  c.patrolIntervalMs = 5000;    // 5 秒一帧
  c.bgUpdateFrames   = 6;       // 每 30 秒更新背景
  c.bgBlendFactor    = 0.05f;   // 稍快融合
  return c;
}

bool FrameDiff::begin(const Config& cfg) {
  cfg_ = cfg;
  anomalyStreak_ = 0;
  lastMotionMs_  = 0;
  stillStartMs_  = 0;
  bgFrameCount_  = 0;
  bgReady_       = false;
  return true;
}

FrameDiff::Result FrameDiff::processFrame(const uint8_t* grayFrame, size_t len,
                                          uint32_t nowMs) {
  Result r{};
  const size_t expectedLen = kCols * kRows * kBlock * kBlock;  // 320×240
  if (!grayFrame || len < expectedLen) return r;

  // ---- 计算每块平均亮度 ----
  uint8_t curAvg[kBlocks];
  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kCols; ++col) {
      uint32_t sum = 0;
      for (int y = 0; y < kBlock; ++y) {
        const uint8_t* line = grayFrame + ((row * kBlock + y) * kCols * kBlock) + col * kBlock;
        for (int x = 0; x < kBlock; ++x) {
          sum += line[x];
        }
      }
      curAvg[row * kCols + col] = static_cast<uint8_t>(sum / (kBlock * kBlock));
    }
  }

  // ---- 首帧 → 初始化背景 ----
  if (!bgReady_) {
    memcpy(bgAvg_, curAvg, kBlocks);
    bgReady_ = true;
    lastMotionMs_ = nowMs;
    stillStartMs_ = nowMs;
    return r;
  }

  // ---- 分块比较 ----
  int diffBlocks    = 0;
  int lowerDiff     = 0;  // 下半屏差异块数（下半 7 行：row 8~14）
  int upperDiff     = 0;  // 上半屏差异块数（row 0~7）
  int lowerTotal    = 0;
  int upperTotal    = 0;

  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kCols; ++col) {
      int idx = row * kCols + col;
      int diff = abs(static_cast<int>(curAvg[idx]) - static_cast<int>(bgAvg_[idx]));

      if (row >= 8) {
        lowerTotal++;
        if (diff > 18) lowerDiff++;     // 亮度差异 >18（0~255 尺度）
      } else {
        upperTotal++;
        if (diff > 18) upperDiff++;
      }

      if (diff > 18) diffBlocks++;
    }
  }

  r.diffScore      = static_cast<float>(diffBlocks) / kBlocks;
  r.lowerHalfScore = lowerTotal > 0 ? static_cast<float>(lowerDiff) / lowerTotal : 0.0f;
  r.upperHalfScore = upperTotal > 0 ? static_cast<float>(upperDiff) / upperTotal : 0.0f;

  // ---- 运动判定 ----
  r.motionDetected = r.diffScore > cfg_.diffThreshold;

  // ---- 静止计时 ----
  if (!r.motionDetected) {
    r.stillDurationMs = nowMs - lastMotionMs_;
  } else {
    lastMotionMs_ = nowMs;
    r.stillDurationMs = 0;
  }

  // ---- 异常确认（多次连续触发）----
  if (r.motionDetected) {
    anomalyStreak_++;
  } else {
    anomalyStreak_ = 0;
  }
  r.anomalyStreak    = anomalyStreak_;
  r.anomalyConfirmed = anomalyStreak_ >= cfg_.confirmFrames;

  // ---- 长时间静止 ----
  r.longStill = (cfg_.stillAlertMs > 0) &&
                (r.stillDurationMs > static_cast<uint32_t>(cfg_.stillAlertMs));

  // ---- 背景自适应更新 ----
  // 静止超 1/3 告警阈值 → 暂停更新，防止人"融进"背景
  bool freezeBg = cfg_.stillAlertMs > 0 &&
                  r.stillDurationMs > static_cast<uint32_t>(cfg_.stillAlertMs / 3);
  // 有持续运动 → 也不更新（可能是场景变化，等稳定后重置）
  bool sceneChanged = anomalyStreak_ > cfg_.confirmFrames * 3;

  bgFrameCount_++;
  if (!freezeBg && !sceneChanged &&
      bgFrameCount_ >= static_cast<uint32_t>(cfg_.bgUpdateFrames)) {
    bgFrameCount_ = 0;
    for (int i = 0; i < kBlocks; ++i) {
      float blended = bgAvg_[i] * (1.0f - cfg_.bgBlendFactor) +
                      curAvg[i] * cfg_.bgBlendFactor;
      bgAvg_[i] = static_cast<uint8_t>(blended);
    }
  }

  // 长时间静止结束后 → 重置背景（场景可能变了）
  if (prevStill_ && !r.motionDetected && r.stillDurationMs < 1000) {
    resetBackground();  // 静止→运动过渡，重置
  }
  prevStill_ = r.longStill;

  return r;
}

void FrameDiff::resetBackground() {
  bgReady_ = false;
  anomalyStreak_ = 0;
  bgFrameCount_ = 0;
}

#pragma once

#include <Arduino.h>
#include <cstdint>

// ===== 帧差法检测器 =====
// 本地只做第一层筛选，回答三个问题：
//   画面变了多少？哪个区域变了？持续多久了？
// 不判断"这是什么"——交给云端多模态

class FrameDiff {
 public:
  struct Config {
    float diffThreshold;     // 差异敏感度：越低越容易触发（0.0~1.0）
    int   stillAlertMs;      // 连续静止多久触发告警（ms）
    int   confirmFrames;     // 连续多少帧超阈值才确认异常
    int   patrolIntervalMs;  // 巡检间隔（ms）
    int   bgUpdateFrames;    // 每 N 帧更新一次背景
    float bgBlendFactor;     // 背景融合比例（0.01 ~ 0.10）
  };

  struct Result {
    float   diffScore;         // 整帧差异分数 0.0~1.0
    float   lowerHalfScore;    // 下半屏差异（跌倒特征）
    float   upperHalfScore;    // 上半屏差异
    bool    motionDetected;    // 当前帧有运动
    bool    anomalyConfirmed;  // 连续多帧异常 → 触发告警
    bool    longStill;         // 长时间静止 → 触发关注
    uint32_t stillDurationMs;  // 已静止多久
    int     anomalyStreak;     // 当前连续异常帧数
  };

  FrameDiff();

  bool begin(const Config& cfg);
  Result processFrame(const uint8_t* grayFrame, size_t len,
                      uint32_t nowMs);
  void resetBackground();     // 强制重置背景（切换场景时）

  // 预设双模态配置
  static Config elderCareConfig();
  static Config workspaceConfig();

 private:
  static constexpr int kBlock    = 16;   // 每块 16×16 像素
  static constexpr int kCols     = 20;   // 320/16
  static constexpr int kRows     = 15;   // 240/16
  static constexpr int kBlocks   = 300;  // 20×15

  uint8_t bgAvg_[kBlocks];       // 背景模型：每块平均亮度
  Config  cfg_;
  uint32_t bgFrameCount_  = 0;
  int      anomalyStreak_  = 0;
  uint32_t lastMotionMs_   = 0;
  uint32_t stillStartMs_   = 0;
  bool     bgReady_        = false;
  bool     prevStill_      = false;
};

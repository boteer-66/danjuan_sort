#pragma once

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_task_wdt.h>
#include <vector>

// SD 卡安全层 —— 防崩五原则:
// 1. 打开→写→关，不持握文件句柄
// 2. 每步喂狗
// 3. 脏标记检测上次是否干净退出
// 4. 图片原子写入(临时文件→改名)
// 5. 队列只追加、日志按天、图片设上限

class SdManager {
public:
  bool begin();   // 初始化 + 脏标记检测 + 建目录
  void end();     // 安全卸载

  // 写操作 — 全部原子化
  bool writeLog(const char* line);                          // 追加日志
  bool saveJpeg(const char* dir, const uint8_t* buf, size_t len); // 存图片
  bool appendQueue(const char* jsonLine);                   // 追加上传队列
  bool markQueueDone(const char* eventId);                  // 标记已上传

  // 清理
  int  purgeOldest(const char* dir, int keep);              // 保留最近 N 个
  void rotateLogs(int keepDays);                            // 日志保留 N 天

  bool isMounted() const { return mounted_; }

private:
  bool mounted_ = false;

  void feed();                              // 喂狗
  bool mkdirIf(const char* path);
  bool writeAtomic(const char* path, const uint8_t* buf, size_t len);
  bool appendLine(const char* path, const char* line);
  String todayStr();

  // 脏标记
  static constexpr const char* kDirtyFlag = "/dirty.flag";
  void setDirty();
  void clearDirty();
  bool wasDirty();

  // 上限
  static constexpr int kMaxImagesPerDir = 100;
  static constexpr int kMaxLogDays      = 7;
};

#pragma once

#include <Arduino.h>
#include <cstdint>
#include <vector>

// ===== 抽象接口层 v2 =====
// FSM 只依赖这些接口名，不关心底层实现
// v2 改动: ICloud 拆成 IChatCloud + IMessaging

// --- 喇叭 ---
class ISpeaker {
public:
  virtual ~ISpeaker() = default;
  virtual bool begin() = 0;
  virtual bool play(const std::vector<uint8_t>& pcm) = 0;
  virtual void stop() = 0;
};

// --- 麦克风 ---
class IMic {
public:
  virtual ~IMic() = default;
  virtual bool begin() = 0;
  virtual bool readChunk(std::vector<uint8_t>& buf) = 0;
};

// --- 摄像头 ---
class ICamera {
public:
  virtual ~ICamera() = default;
  virtual bool begin() = 0;
  virtual bool capture(uint8_t* buf, size_t& len) = 0;
};

// --- 存储（SD 卡）---
class IStorage {
public:
  virtual ~IStorage() = default;
  virtual bool begin() = 0;
  virtual bool writeLog(const char* msg) = 0;
  virtual bool saveJpeg(const char* path, const uint8_t* buf, size_t len) = 0;
  virtual bool appendQueue(const char* entry) = 0;
};

// --- 云端对话（HTTP Chat + WebSocket ASR/TTS）---
class IChatCloud {
public:
  virtual ~IChatCloud() = default;
  virtual String chatCompletion(const String& prompt) = 0;
  virtual bool synthesizeTts(const String& text, std::vector<uint8_t>& pcm) = 0;
  virtual bool transcribeAsr(std::vector<uint8_t>& audioBuf, String& transcript) = 0;
};

// --- 消息推送（MQTT 告警/状态）---
class IMessaging {
public:
  virtual ~IMessaging() = default;
  virtual bool publishAlert(const char* eventType, const char* summary) = 0;
  virtual bool publishStatus() = 0;
};

// --- 唤醒词 ---
class IWakeWord {
public:
  virtual ~IWakeWord() = default;
  virtual bool begin() = 0;
  virtual bool poll(String& reason) = 0;
};


/*
现象	含义
上电亮 0.5 秒然后灭	启动中
3 快闪 循环不断	SD 卡挂了
5 快闪 循环不断	摄像头挂了
每 2 秒闪一下（50ms）	心跳——正常运行中
常亮约 1 秒	检测到异常，正在拍图存 SD



*/
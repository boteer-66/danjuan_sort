#pragma once

#include <Arduino.h>

namespace AppConfig {

// ===== Network =====
static const char* const kWifiSsid     = "YOUR_WIFI_SSID";      // 替换为实际WiFi名
static const char* const kWifiPassword  = "YOUR_WIFI_PASSWORD";  // 替换为实际WiFi密码
static const char* const kApiKey        = "YOUR_VOLCANO_API_KEY"; // 替换为火山引擎API Key

// ===== Gateway =====
static const char* const kChatApiUrl = "https://ai-gateway.vei.volces.com/v1/chat/completions";
static const char* const kAsrRealtimeUrl = "wss://ai-gateway.vei.volces.com/v1/realtime?model=volcano_asr";
static const char* const kTtsRealtimeUrl = "wss://ai-gateway.vei.volces.com/v1/realtime?model=doubao-tts";
static const char* const kChatModel = "doubao-seed-1.6-flash";  // 当前稳定, 读图可切vision
static const char* const kTtsVoice = "zh_female_linjianvhai_moon_bigtts";  // 邻家女孩: 自然亲切

// ===== Pins =====
// ⚠️ N16R8 Octal PSRAM 占用 33~42,47,48，所有引脚已避开
inline constexpr int kLedPin = -1;   // 未用（GPIO 2 已给 PDM 麦克风）

// I2S audio module — DevKitC-1 实物板安全引脚 (2026-05-10 锁定)
inline constexpr int kI2SBclkPin = 7;   // 原45让给SD CLK，7远离PCLK(8)安全
inline constexpr int kI2SLrclkPin = 21;
inline constexpr int kI2SMicClkPin  = 2;  // PDM CLK
inline constexpr int kI2SMicDataPin = 3;  // PDM DATA
inline constexpr int kI2SSpkDataPin = 46;
inline constexpr int kNs4168CtrlPin = -1;   // CTRL 板载电阻固定电平，不外接

// SU-03T UART（模块在途，2026-05-10 锁定）
inline constexpr int kSu03tRxPin = 44;
inline constexpr int kSu03tTxPin = 43;

// ===== Serial =====
inline constexpr uint32_t kSerialBaud = 115200;
inline constexpr uint32_t kSu03tBaud = 9600;  // SDK 生成的是 9600，不是 115200

// ===== Audio =====
inline constexpr uint32_t kAudioSampleRate = 16000;
inline constexpr uint8_t kAudioBitsPerSample = 16;
inline constexpr uint8_t kAudioChannels = 1;
inline constexpr size_t kAudioBytesPerSample = 2;
inline constexpr uint32_t kAsrChunkMs = 100;
inline constexpr size_t kAsrChunkBytes =
    (kAudioSampleRate * kAudioBytesPerSample * kAsrChunkMs) / 1000;

// ===== Timeouts =====
inline constexpr uint32_t kWifiConnectTimeoutMs = 20000;
inline constexpr uint32_t kHttpTimeoutMs = 20000;
inline constexpr uint32_t kWsConnectTimeoutMs = 10000;
inline constexpr uint32_t kWsReadTimeoutMs = 3000;
inline constexpr uint32_t kAsrHardLimitMs = 8000;     // 8s 最长录音窗口
inline constexpr uint32_t kAsrResultIdleCommitMs = 400; // 400ms 停顿就截断
inline constexpr uint32_t kAsrFinalWaitMs = 2000;  // 2s 足够火山返final
inline constexpr uint32_t kTtsFinalWaitMs = 8000;
inline constexpr uint32_t kWakeCooldownMs = 2500;
inline constexpr uint32_t kUsbPromptMaxLen = 240;

// ===== Self test =====
static const char* const kStartupPrompt = "请用一句自然中文回复：豆包语音链路文本自检成功。";

// ===== Persona =====
static const char* const kAssistantSystemPrompt =
    "你是蛋卷，不是AI，就是一个陪在身边的小搭子。"
    "说话像跟朋友闲聊——加语气词（呀～、嘛、诶、嘿嘿），句句短，50字以内。"
    "活泼、软萌、有点俏皮。不要用Markdown，不要反问，不要'您好'，不要'我是AI助手'。";

}  // namespace AppConfig

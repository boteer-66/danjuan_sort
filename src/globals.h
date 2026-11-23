#pragma once
#include <Arduino.h>
#include "core/fsm.h"
#include "mqtt_client.h"
#include "gateway_client.h"
#include "audio_i2s.h"
#include "su03t_uart.h"
#include "vision/frame_diff.h"

// 模块实例
extern MqttClient    g_mqtt;
extern FrameDiff     g_fd;
extern GatewayClient g_cloud;
extern AudioI2S      g_audio;
extern Su03tUart     g_su03t;
extern Fsm           gFsm;

// 场景 & 配置
extern const char*      g_scene;
extern FrameDiff::Config g_fdCfg;

// TOS
extern const char* TOS_URLS[];
extern int g_tosIdx;
extern int g_snapIdx;

// 告警状态
extern const char* g_tosStatus;
extern int        g_alertLevel2;
extern uint32_t   g_lastAlertMs;
extern int        g_lastAlertDay;
extern const uint32_t kAlertCooldownMs;

// 系统状态
extern bool g_ntpSynced;
extern bool g_bgNeedReset;

// 语音状态
extern bool     g_voiceActive;
extern bool     g_continuousTalk;
extern uint32_t g_wakeCooldownEnd;

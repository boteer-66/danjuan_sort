// ============================================================
// 蛋卷 V4 统一版 — 帧差巡检 + MQTT + 告警三级 + LED + NTP + OTA
// 硬件: ESP32-S3 N16R8 | WS2812 GPIO43 | EMQX Cloud | 火山引擎
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cstring>
#include <time.h>
#include <Update.h>
#include <Preferences.h>
#include <FastLED.h>
#include <esp_task_wdt.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include "esp_camera.h"
#include "camera_pins.h"
#include "app_config.h"
#include "app_state.h"
#include "core/fsm.h"
#include "core/event_types.h"
#include "vision/frame_diff.h"
#include "mqtt_client.h"
#include "tos_upload.h"
#include "gateway_client.h"
#include "audio_i2s.h"
#include "su03t_uart.h"
#include "camera_setup.h"
#include "voice_agent.h"

// ===== 比赛演示模式 =====
#define DEMO_MODE

// ===== WS2812 LED (GPIO 43, 原 SD_D0) =====
#define LED_PIN 1
#define LED_NUM 12
static CRGB g_led[12];

// ===== LED 颜色 =====
static const CRGB LED_GREEN(0, 80, 0);
static const CRGB LED_YELLOW(80, 60, 0);
static const CRGB LED_RED(80, 0, 0);
static const CRGB LED_OFF(0, 0, 0);

// ===== TOS 预签名 URL（gen_20_urls.py 生成，2026-05-29） =====
// ===== TOS 预签名 URL（请本地生成后填写，勿把真实 AK/SK 提交到 Git）=====
// 使用 gen_20_urls.py 生成 20 条有效期 24h 的 PUT 预签名 URL，粘贴到下方数组。
static const char* TOS_URLS[] = {
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-01.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-02.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-03.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-04.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-05.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-06.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-07.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-08.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-09.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-10.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-11.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-12.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-13.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-14.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-15.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-16.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-17.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-18.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-19.jpg?YOUR_PRESIGNED_QUERY",
  "https://YOUR_TOS_BUCKET.tos-cn-beijing.volces.com/esp32/patrol-20.jpg?YOUR_PRESIGNED_QUERY",
};
static constexpr int kTosUrlCount = 20;
static int g_tosIdx = 0;
static int g_snapIdx = 15;  // 抓拍独立索引 (15~19)，不与巡逻 0~14 冲突

String tosPublicUrl(int idx);

// ===== 模块实例 =====
MqttClient     g_mqtt;
FrameDiff      g_fd;
GatewayClient  g_cloud;
AudioI2S       g_audio;
Su03tUart      g_su03t(Serial1);

// ===== 语音状态 =====
uint32_t g_wakeCooldownEnd = 0;
bool     g_voiceActive     = false;
bool     g_continuousTalk  = false;  // 连续对话模式

// ===== 告警状态 =====
static uint32_t g_lastAlertMs   = 0;
static int      g_alertLevel2   = 0;  // 仅 Level 2（推送小程序）计数
// 告警历史（Preferences 持久化，断电不丢，最多 5 条）
static String   g_alertHistory[5];
static int      g_alertHistCnt  = 0;
static void saveAlertHistory() {
  Preferences prefs;
  prefs.begin("danjuan", false);
  for (int i = 0; i < g_alertHistCnt; i++) {
    char key[8]; snprintf(key, sizeof(key), "ah%d", i);
    prefs.putString(key, g_alertHistory[i].substring(0, 120));
  }
  prefs.putInt("ah_cnt", g_alertHistCnt);
  prefs.end();
}
static void loadAlertHistory() {
  Preferences prefs;
  prefs.begin("danjuan", true);
  g_alertHistCnt = prefs.getInt("ah_cnt", 0);
  if (g_alertHistCnt > 5) g_alertHistCnt = 5;
  for (int i = 0; i < g_alertHistCnt; i++) {
    char key[8]; snprintf(key, sizeof(key), "ah%d", i);
    g_alertHistory[i] = prefs.getString(key, "");
  }
  prefs.end();
}
static void addAlertHistory(const char* entry) {
  // 移动旧条目，最新放 [0]
  for (int i = 4; i > 0; i--) g_alertHistory[i] = g_alertHistory[i - 1];
  g_alertHistory[0] = String(entry).substring(0, 120);
  if (g_alertHistCnt < 5) g_alertHistCnt++;
  saveAlertHistory();
}
String getAlertHistorySummary() {
  String s;
  for (int i = 0; i < g_alertHistCnt; i++) {
    if (g_alertHistory[i].length() > 0) {
      s += String(i + 1) + ". " + g_alertHistory[i] + "\n";
    }
  }
  return s;
}
#ifdef DEMO_MODE
static constexpr uint32_t kAlertCooldownMs  = 30000;  // 演示30s冷却，避免告警轰炸
#else
static constexpr uint32_t kAlertCooldownMs  = 10000;
#endif
static int g_lastAlertDay = -1;  // NTP 日序号，用于每日重置

// ===== 场景 =====
const char*      g_scene  = "eldercare";
FrameDiff::Config g_fdCfg;

// ===== 遥测 & 状态 =====
static uint32_t g_lastTelemetryMs = 0;
static uint32_t g_lastStatusMs    = 0;
static constexpr uint32_t kTelemetryIntervalMs = 60000;
static constexpr uint32_t kStatusIntervalMs    = 30000;
const char* g_tosStatus = "idle";

// ===== 预声明 =====
static void checkWiFi();
static void checkHeap();
static void doMonitoring();
static void publishTelemetryIfNeeded();
static void publishStatusIfNeeded();
static void doOTA(const char* url);
static void updateLed(AppState st);
static bool g_ntpSynced = false;
static bool g_bgNeedReset = false;  // WiFi 恢复后标记需重置背景

// ===== 拦截 cam_task 栈大小（绕过预编译库的 2048 限制）=====
extern "C" {
BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*, BaseType_t);
BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t f, const char* name, uint32_t stack, void* arg, UBaseType_t prio, TaskHandle_t* handle, BaseType_t core) {
  if (strcmp(name, "cam_task") == 0) stack = 8192;
  return __real_xTaskCreatePinnedToCore(f, name, stack, arg, prio, handle, core);
}
BaseType_t __real_xTaskCreate(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*);
BaseType_t __wrap_xTaskCreate(TaskFunction_t f, const char* name, uint32_t stack, void* arg, UBaseType_t prio, TaskHandle_t* handle) {
  if (strcmp(name, "cam_task") == 0) stack = 8192;
  return __real_xTaskCreate(f, name, stack, arg, prio, handle);
}
}

String analyzeWithDoubao(const char* publicUrl);

// ===== MQTT 控制回调 =====
static void onMqttControl(const char* cmd) {
  String c(cmd);
  Serial.printf("[CTRL] %s\n", cmd);

  if (c == "capture") {
    // 手动抓拍：拍一张发到小程序，不触发告警
    Serial.println("[SNAP] 手动抓拍");
    int sIdx = g_snapIdx;
    const char* url = TOS_URLS[g_snapIdx];
    g_snapIdx = 15 + ((g_snapIdx - 15 + 1) % 5);  // 15~19 循环
    fill_solid(g_led, LED_NUM, LED_YELLOW); FastLED.show();
    esp_task_wdt_reset();  // 拍照前喂狗
    g_tosStatus = "uploading";
    bool ok = snapAndUpload(url);
    g_tosStatus = ok ? "ok" : "fail";
    if (!camInitGray()) {
      Serial.println("[WARN] reinit 失败，硬复位重试...");
      cameraHardReset();
      if (!camInitGray()) { Serial.println("[FATAL] reinit 再次失败"); ESP.restart(); }
    }
    g_fd.resetBackground();
    if (ok && g_mqtt.connected()) {
      g_mqtt.publishImageRef("snap", tosPublicUrl(sIdx).c_str(), "snap");
    }
    g_tosStatus = "idle";
    if (g_mqtt.connected()) g_mqtt.publishTelemetry(WiFi.RSSI(), ESP.getFreeHeap(), false, true, 0, g_tosStatus, g_alertLevel2);
    // M0：抓拍后不再整机重启；灰度 reinit 失败才 cameraHardReset（见上）
    Serial.println("[SNAP] 抓拍完成，继续巡检");
  }
  else if (c.startsWith("switch:")) {
    String mode = c.substring(7);
    g_scene = (mode == "workspace") ? "workspace" : "eldercare";
    g_fdCfg = (strcmp(g_scene, "workspace") == 0)
              ? FrameDiff::workspaceConfig()
              : FrameDiff::elderCareConfig();
    g_fd.begin(g_fdCfg);
    g_fd.resetBackground();
    gFsm.feed(Event::Kind::MODE_SWITCH);        // → Idle
    gFsm.feed(Event::Kind::FRAME_DIFF_NORMAL);  // → Monitoring
    Serial.printf("[MODE] → %s\n", g_scene);
    // 立即推送状态，小程序秒切
    if (g_mqtt.connected()) {
      g_mqtt.publishStatus("MONITORING", g_scene, true);
    }
    // 云端 TTS → 功放喇叭播报
    String ttsText = (mode == "workspace")
        ? "已切换至工位健康模式，久坐监测中"
        : "已切换至老人看护模式，巡检守护中";
    g_cloud.synthesizeAndPlay(ttsText, g_audio);
  }
  else if (c == "talk") {
    g_continuousTalk = !g_continuousTalk;
    Serial.printf("[TALK] 连续对话: %s\n", g_continuousTalk ? "开" : "关");
    if (g_continuousTalk) {
      // 直接进对话，不走 FSM Listning（主循环那边不调 runConversationOnce）
      runConversationOnce();
      gFsm.feed(Event::Kind::FRAME_DIFF_NORMAL);
    }
  }
  else if (c == "demo") {
    static bool busy = false;
    if (busy) { Serial.println("[DEMO] 忙，忽略"); return; }
    busy = true;
    // 演示用：拍照 → 豆包实时分析 → 播报分析结果
    Serial.println("[DEMO] ═══ 演示告警触发 ═══");
    fill_solid(g_led, LED_NUM, LED_YELLOW); FastLED.show();  // 绕过50ms节流直设黄灯
    g_tosStatus = "uploading";
    int dIdx = g_tosIdx;
    const char* dUrl = TOS_URLS[g_tosIdx];
    g_tosIdx = (g_tosIdx + 1) % 15;
    bool dOk = snapAndUpload(dUrl);
    g_tosStatus = dOk ? "ok" : "fail";
    if (!camInitGray()) { cameraHardReset(); camInitGray(); }
    g_fd.resetBackground();
    String dPubUrl = tosPublicUrl(dIdx);
    if (dOk && g_mqtt.connected()) {
      g_mqtt.publishImageRef("demo", dPubUrl.c_str(), "event");
      esp_task_wdt_reset();
      // 豆包实时分析当前画面
      String dConclusion = analyzeWithDoubao(dPubUrl.c_str());
      // 根据分析结论定级
      bool aiAbnormal = dConclusion.indexOf("正常") < 0
                     && dConclusion.indexOf("无异常") < 0
                     && dConclusion.indexOf("日常") < 0;
      const char* dSev = aiAbnormal ? "high" : "low";
      // 推送到小程序
      g_mqtt.publishAlert(
        ("demo_" + String(millis())).c_str(),
        "fall_suspected", dSev, aiAbnormal ? 0.85f : 0.1f,
        dConclusion.c_str(), dPubUrl.c_str(),
        g_scene, dConclusion.c_str()
      );
      Serial.printf("[DEMO] 分析结论: %s\n", dConclusion.c_str());
      addAlertHistory(dConclusion.c_str());
      fill_solid(g_led, LED_NUM, LED_RED); FastLED.show();
      // 喇叭播报：无条件播报，不管优先级策略
      String demoAnnounce = "模拟告警：" + dConclusion;
      esp_task_wdt_reset();
      g_cloud.synthesizeAndPlay(demoAnnounce, g_audio);
    }
    g_tosStatus = "idle";
    if (g_mqtt.connected()) g_mqtt.publishTelemetry(WiFi.RSSI(), ESP.getFreeHeap(), false, true, 0, g_tosStatus, g_alertLevel2);
    busy = false;
    Serial.println("[DEMO] ═══ 演示完成 ═══");
  }
  else if (c == "alert_ack") {
    gFsm.feed(Event::Kind::ALERT_RESOLVED);
    Serial.println("[ALERT] 子女已确认");
  }
  else if (c == "status") {
    g_lastStatusMs = 0;
  }
  else if (c.startsWith("voice:")) {
    String voiceId = c.substring(6);
    Serial.printf("[VOICE] 切换音色: %s\n", voiceId.c_str());
    g_cloud.setTtsConfig(voiceId.c_str(), 1.0f, 1.0f);
    // 持久化，下次开机自动恢复
    Preferences prefs; prefs.begin("danjuan", false);
    prefs.putString("voice", voiceId); prefs.end();
  }
  else if (c.startsWith("speed:")) {
    float spd = c.substring(6).toFloat();
    if (spd < 0.5f) spd = 0.5f;
    if (spd > 2.0f) spd = 2.0f;
    Serial.printf("[SPEED] 语速: %.1f\n", spd);
    g_cloud.setTtsConfig(nullptr, spd, 1.0f);
    Preferences prefs; prefs.begin("danjuan", false);
    prefs.putFloat("speed", spd); prefs.end();
  }
  else if (c.startsWith("volume:")) {
    float vol = c.substring(7).toFloat() / 100.0f;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    Serial.printf("[VOL]  音量: %.0f%%\n", vol * 100);
    g_cloud.setTtsConfig(nullptr, 1.0f, vol);
    Preferences prefs; prefs.begin("danjuan", false);
    prefs.putFloat("volume", vol); prefs.end();
  }
  else if (c.startsWith("ota:")) {
    String url = c.substring(4);
    Serial.printf("[OTA]  开始远程升级: %s\n", url.c_str());
    if (g_mqtt.connected()) {
      g_mqtt.publishStatus("OTA_UPDATING", g_scene, true);
    }
    doOTA(url.c_str());
  }
  else if (c.startsWith("wifi:")) {
    String payload = c.substring(5);
    int sep = payload.indexOf('|');
    if (sep > 0) {
      Preferences prefs;
      prefs.begin("danjuan", false);
      prefs.putString("ssid", payload.substring(0, sep));
      prefs.putString("pwd",  payload.substring(sep + 1));
      prefs.end();
      Serial.printf("[WIFI] 已保存配网，重启中...\n");
      delay(500);
      ESP.restart();
    }
  }
}

bool camInitGray(); void cameraHardReset(); bool snapAndUpload(const char*);

// ===== WiFi 监控 =====
static void checkWiFi() {
  static bool wasLost = false;
  if (WiFi.status() != WL_CONNECTED) {
    if (!wasLost) { wasLost = true; gFsm.feed(Event::Kind::WIFI_LOST); }
    // 退避：每 3 秒重连一次，不每 cycle 都调
    static uint32_t lastReconnect = 0;
    if (millis() - lastReconnect > 3000) {
      lastReconnect = millis();
      WiFi.reconnect();
    }
  } else if (wasLost) {
    wasLost = false;
    g_bgNeedReset = true;
    Serial.println("[WIFI] 已恢复");
    gFsm.feed(Event::Kind::WIFI_RECOVERED);
  }
}

// ===== 内存监控 =====
static void checkHeap() {
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < 10000) return;
  lastCheck = millis();
  if (ESP.getFreeHeap() < 100000) {
    Serial.printf("[HEAP] ⚠️  %u B\n", ESP.getFreeHeap());
  }
}

// ===== 每日告警重置 =====
static void dailyResetAlert() {
  int today = -1;
  if (g_ntpSynced) {
    time_t now; time(&now);
    struct tm* t = localtime(&now);
    today = t->tm_yday;
  } else {
    // 无 NTP 时用 millis 兜底：86400000ms = 1 天
    today = millis() / 86400000;
  }
  if (g_lastAlertDay < 0) g_lastAlertDay = today;
  if (today != g_lastAlertDay) {
    g_alertLevel2 = 0;
    g_lastAlertDay = today;
    Serial.println("[DAILY] 告警计数已重置");
  }
}

// ===== LED 状态更新 =====
static void updateLed(AppState st) {
  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate < 50) return;
  lastUpdate = millis();

  switch (st) {
    case AppState::Monitoring: {
      // 绿色呼吸（sin 波形 0→40）
      float t = millis() / 2000.0f * PI * 2;
      uint8_t v = (uint8_t)((sin(t) + 1) * 40);
      fill_solid(g_led, LED_NUM, CRGB(0, v, 0));
      break;
    }
    case AppState::Capturing:
    case AppState::Uploading:
    case AppState::Analyzing:
      // 黄色快闪
      fill_solid(g_led, LED_NUM, ((millis() / 200) % 2) ? LED_YELLOW : LED_OFF);
      break;
    case AppState::Alerting:
      fill_solid(g_led, LED_NUM, LED_RED);  // 红色常亮
      break;
    case AppState::OfflineFallback:
    case AppState::Error:
      fill_solid(g_led, LED_NUM, ((millis() / 800) % 2) ? LED_RED : LED_OFF);  // 红色慢闪
      break;
    default:
      fill_solid(g_led, LED_NUM, LED_OFF);
      break;
  }
  FastLED.show();
}

// ===== 巡检（Monitoring） =====
static void doMonitoring() {
  // WiFi 恢复后重置背景
  if (g_bgNeedReset) {
    g_bgNeedReset = false;
    g_fd.resetBackground();
    Serial.println("[BG]   背景已重置（离线恢复）");
  }

  // 每日告警计数重置
  dailyResetAlert();

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb || !fb->buf) { delay(100); return; }

  uint32_t now = millis();
  FrameDiff::Result r = g_fd.processFrame(fb->buf, fb->len, now);
  esp_camera_fb_return(fb);

  // 串口心跳（每 5 秒）
  static uint32_t lastBeat = 0;
  if (now - lastBeat > 10000) {
    lastBeat = now;
    Serial.printf("[OK]   巡检中  score=%.2f  motion=%d  still=%lums\n",
                  r.diffScore, r.motionDetected, r.stillDurationMs);
  }

  // ── 触发判定 ──
  bool isElder = (strcmp(g_scene, "eldercare") == 0);
  bool shouldAlert = false;
  const char* eventType  = nullptr;
  const char* severity   = nullptr;
  const char* summary    = nullptr;
  bool isStillAlert      = false;

  // 长时间静止 → 独立触发（静止场景没有连续运动帧，不能靠 anomalyConfirmed）
  if (r.longStill) {
    if (isElder) {
      // 老人看护: 长时间静止 → P1 异常 → 拍照+豆包+MQTT
      shouldAlert = true;
      isStillAlert = true;
      eventType = "long_stillness";
      severity  = "medium";
      summary   = "检测到长时间静止";
    } else {
      // 工位健康: 久坐 → P2 关怀 → 仅语音，不推小程序
      Serial.printf("[LEVEL1] 久坐提醒  still=%lums\n", r.stillDurationMs);
      // TODO: 语音播报 "起来走走呀～"
      { uint32_t d = now + g_fdCfg.patrolIntervalMs;
        while (millis() < d) { camera_fb_t* x = esp_camera_fb_get(); if (x) esp_camera_fb_return(x); esp_task_wdt_reset(); g_mqtt.loop(); delay(100); } }
      return;
    }
  }
  // 运动异常 → 连续多帧超阈值确认
  else if (r.anomalyConfirmed) {
    shouldAlert = true;
    isStillAlert = false;
    eventType = "fall_suspected";
    severity  = r.diffScore > 0.7 ? "high" : "medium";
    summary   = "检测到异常运动特征";
  }

  // 无异常 → 消费帧，等待下一次巡检
  if (!shouldAlert) {
    uint32_t end = now + g_fdCfg.patrolIntervalMs;
    while (millis() < end) {
      camera_fb_t* drain = esp_camera_fb_get();
      if (drain) esp_camera_fb_return(drain);
      esp_task_wdt_reset();
      g_mqtt.loop();
      delay(100);
    }
    return;
  }

  // ── 冷却期 ──
  if (now - g_lastAlertMs < kAlertCooldownMs) {
    { uint32_t _d = millis() + g_fdCfg.patrolIntervalMs;
      while (millis() < _d) { esp_task_wdt_reset(); g_mqtt.loop(); delay(100); } }
    return;
  }
  g_lastAlertMs = now;

  // Level 2: 拍照→TOS→MQTT
  g_alertLevel2++;
  Serial.printf("[LEVEL2] #%d  score=%.2f  %s\n",
                g_alertLevel2, r.diffScore,
                isStillAlert ? "long_still" : "motion");

  g_tosStatus = "uploading";
  fill_solid(g_led, LED_NUM, LED_YELLOW); FastLED.show();
  esp_task_wdt_reset();  // 拍照+上传+TOS阻塞前喂狗
  gFsm.feed(Event::Kind::FRAME_DIFF_HIGH);

  int snapIdx = g_tosIdx;
  const char* url = TOS_URLS[g_tosIdx];
  g_tosIdx = (g_tosIdx + 1) % 15;  // 巡逻只循环 0~14，不踩抓拍 15~19
  bool ok = snapAndUpload(url);
  g_tosStatus = ok ? "ok" : "fail";
  String pubUrl = tosPublicUrl(snapIdx);

  if (ok && g_mqtt.connected()) {
    gFsm.feed(Event::Kind::ANALYZE_DONE);
    g_mqtt.publishImageRef("patrol", pubUrl.c_str(), "event");
    esp_task_wdt_reset();  // 豆包阻塞前喂狗
    String conclusion = analyzeWithDoubao(pubUrl.c_str());
    g_mqtt.publishAlert(
      ("evt_" + String(millis())).c_str(),
      eventType,
      severity,
      r.diffScore,
      summary,
      pubUrl.c_str(),
      g_scene,
      conclusion.c_str()
    );
    Serial.println("[MQTT]  告警已推送");
    addAlertHistory(conclusion.c_str());
    fill_solid(g_led, LED_NUM, LED_RED); FastLED.show();  // 红灯常亮, 绕过FSM节流
    // 只有豆包确认非正常 + 高危才语音叫唤
    bool aiConfirmed = conclusion.indexOf("正常") < 0
                    && conclusion.indexOf("无异常") < 0
                    && conclusion.indexOf("日常") < 0;
    if (strcmp(severity, "high") == 0 && aiConfirmed) {
      esp_task_wdt_reset();  // TTS 阻塞前喂狗
      g_cloud.synthesizeAndPlay("检测到异常情况，已发送提醒，请确认是否需要帮助", g_audio);
    }
  } else if (ok) {
    Serial.println("[MQTT]  未连接，告警未推送");
  } else {
    gFsm.feed(Event::Kind::UPLOAD_ERROR);
  }

  // 回到巡检 — reinit + 重试
  delay(200);
  if (!camInitGray()) {
    Serial.println("[WARN] 相机 reinit 失败，硬复位重试...");
    cameraHardReset();
    if (!camInitGray()) {
      Serial.println("[FATAL] 相机 reinit 再次失败，重启");
      delay(500);
      ESP.restart();
    }
  }
  g_fd.resetBackground();
  gFsm.feed(Event::Kind::ALERT_RESOLVED);
  g_tosStatus = "idle";
  if (g_mqtt.connected()) g_mqtt.publishTelemetry(WiFi.RSSI(), ESP.getFreeHeap(), false, true, 0, g_tosStatus, g_alertLevel2);
  delay(500);
}

// ===== MQTT 定时发布 =====
static void publishTelemetryIfNeeded() {
  if (!g_mqtt.connected()) return;
  uint32_t now = millis();
  if (now - g_lastTelemetryMs < kTelemetryIntervalMs) return;
  g_lastTelemetryMs = now;
  g_mqtt.publishTelemetry(
    WiFi.RSSI(), ESP.getFreeHeap(),
    false, true,
    0, g_tosStatus, g_alertLevel2  // 只推 Level 2 计数
  );
}

static void publishStatusIfNeeded() {
  if (!g_mqtt.connected()) return;
  uint32_t now = millis();
  if (now - g_lastStatusMs < kStatusIntervalMs) return;
  g_lastStatusMs = now;
  g_mqtt.publishStatus(gFsm.stateName(), g_scene, true);
}

// ===== 远程 OTA =====
static void doOTA(const char* url) {
  fill_solid(g_led, LED_NUM, LED_YELLOW); FastLED.show();

  // OTA：writeStream 单次阻塞无法分段喂狗，仍临时解绑；失败路径会加回
  esp_task_wdt_delete(NULL);

  WiFiClientSecure ssl; ssl.setInsecure();
  HTTPClient http; http.setTimeout(120000); http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(ssl, url)) {
    Serial.println("[OTA]   HTTP begin 失败");
    esp_task_wdt_add(NULL);
    return;
  }
  int code = http.GET();
  if (code < 200 || code >= 400) {
    Serial.printf("[OTA]   HTTP %d\n", code);
    http.end();
    esp_task_wdt_add(NULL);
    return;
  }
  int len = http.getSize();
  if (len <= 0) {
    Serial.println("[OTA]   无法获取文件大小");
    http.end();
    esp_task_wdt_add(NULL);
    return;
  }
  Serial.printf("[OTA]   下载 %d 字节...\n", len);

  if (!Update.begin(len)) {
    Serial.printf("[OTA]   Update.begin: %s\n", Update.errorString());
    http.end();
    esp_task_wdt_add(NULL);
    return;
  }
  size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();

  if (written != (size_t)len) {
    Serial.printf("[OTA]   写入不完整 %d/%d\n", written, len);
    esp_task_wdt_add(NULL);
    return;
  }
  if (!Update.end() || !Update.isFinished()) {
    Serial.printf("[OTA]   Update.end: %s\n", Update.errorString());
    esp_task_wdt_add(NULL);
    return;
  }
  fill_solid(g_led, LED_NUM, LED_GREEN); FastLED.show();
  Serial.println("[OTA]   升级完成，3 秒后重启...");
  delay(3000);
  ESP.restart();
}

void playLocalBeep(); void runConversationOnce(); void pollSU03T();

// ===== BLE 配网回调 =====
#define BLE_SERVICE_UUID    "0000ff00-0000-1000-8000-00805f9b34fb"
#define BLE_CHAR_SSID_UUID  "0000ff01-0000-1000-8000-00805f9b34fb"
#define BLE_CHAR_PWD_UUID   "0000ff02-0000-1000-8000-00805f9b34fb"
static String g_bleSsid, g_blePwd;
static bool   g_bleConfigDone = false;

class WifiConfigCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String val = String(c->getValue().c_str());
    if (c->getUUID().toString() == BLE_CHAR_SSID_UUID) g_bleSsid = val;
    else if (c->getUUID().toString() == BLE_CHAR_PWD_UUID)  g_blePwd  = val;
    if (g_bleSsid.length() > 0 && g_blePwd.length() > 0) {
      Serial.printf("[BLE]  配网: %s\n", g_bleSsid.c_str());
      Preferences p; p.begin("danjuan", false);
      p.putString("ssid", g_bleSsid); p.putString("pwd", g_blePwd);
      p.end();
      g_bleConfigDone = true;
    }
  }
};

static void initBLE() {
  BLEDevice::init("Danjuan-ESP32");
  BLEServer* server = BLEDevice::createServer();
  BLEService* svc = server->createService(BLE_SERVICE_UUID);
  BLECharacteristic* chrSsid = svc->createCharacteristic(BLE_CHAR_SSID_UUID, BLECharacteristic::PROPERTY_WRITE);
  BLECharacteristic* chrPwd  = svc->createCharacteristic(BLE_CHAR_PWD_UUID,  BLECharacteristic::PROPERTY_WRITE);
  chrSsid->setCallbacks(new WifiConfigCallback());
  chrPwd->setCallbacks(new WifiConfigCallback());
  svc->start();
  server->getAdvertising()->start();
  Serial.println("[OK]   BLE 配网广播中 (Danjuan-ESP32)");
}

// ================================================================
void setup() {
  Serial.begin(AppConfig::kSerialBaud);
  delay(2000);
  Serial.println("\n╔══════════════════════════════╗");
  Serial.println(  "║   蛋卷 V4 统一版             ║");
#ifdef DEMO_MODE
  Serial.println(  "║   DEMO MODE                  ║");
#endif
  Serial.println(  "╚══════════════════════════════╝\n");

  // 0. 看门狗（30s 超时，TTS 播放最长约 25s）
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);
  Serial.println("[OK]   看门狗 (30s)");

  // 0. LED
  FastLED.addLeds<WS2812, LED_PIN, GRB>(g_led, LED_NUM);
  fill_solid(g_led, LED_NUM, LED_OFF); FastLED.show();
  Serial.printf("[OK]   LED (GPIO %d)\n", LED_PIN);

  // 1. 相机
  if (!camInitGray()) {
    Serial.println("[FATAL] 相机初始化失败");
    while (1) { fill_solid(g_led, LED_NUM, LED_RED); FastLED.show(); delay(1000); }
  }
  Serial.println("[OK]   相机 GRAYSCALE");

  // 2. WiFi
  String wifiSsid = AppConfig::kWifiSsid;
  String wifiPwd  = AppConfig::kWifiPassword;
  Preferences prefs;
  prefs.begin("danjuan", true);
  String savedSsid = prefs.getString("ssid", "");
  String savedPwd  = prefs.getString("pwd", "");
  prefs.end();
  if (savedSsid.length() > 0) {
    wifiSsid = savedSsid; wifiPwd = savedPwd;
    Serial.printf("[WIFI] 使用已保存配网: %s\n", wifiSsid.c_str());
  }
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPwd.c_str());
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > AppConfig::kWifiConnectTimeoutMs) break;
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\n[OK]   WiFi %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\n[WARN] WiFi 失败，离线降级");

  // 3. NTP 对时（北京时间 UTC+8）
  configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org", "time.nist.gov");
  time_t now; struct tm tmInfo;
  for (int retry = 0; retry < 30; retry++) {
    time(&now); localtime_r(&now, &tmInfo);
    if (tmInfo.tm_year >= 2026 - 1900) { g_ntpSynced = true; break; }
    delay(500);
  }
  if (g_ntpSynced)
    Serial.printf("[OK]   NTP %04d-%02d-%02d %02d:%02d:%02d CST\n",
                  tmInfo.tm_year+1900, tmInfo.tm_mon+1, tmInfo.tm_mday,
                  tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);
  else
    Serial.println("[NTP]  未同步，继续运行");

  // 4. 音频（I2S 全双工：PDM 麦克风 + I2S 喇叭）
  if (g_audio.begin()) {
    g_audio.stop();  // 初始化后静音，避免 I2S TX 空闲输出杂讯
    Serial.println("[OK]   音频 I2S");
  } else {
    Serial.println("[WARN] 音频未初始化");
  }

  // 5. SU-03T 唤醒词（预留接口，模块在途）
  g_su03t.begin();

  // 5b. BLE 配网（PCB 后启用，当前 Bluedroid+WiFi 内存冲突）
  // if (savedSsid.length() == 0) initBLE();

  // 6. MQTT → EMQX Cloud
  g_mqtt.onControl(onMqttControl);
  bool mqttOk = g_mqtt.begin(
    "YOUR_EMQX_HOST", 8883,
    "YOUR_DEVICE_ID",
    "YOUR_EMQX_USERNAME", "YOUR_EMQX_PASSWORD",
    30  // keepalive 30s，配合 LWT；主循环需保证间隔内 loop()
  );
  Serial.printf("[MQTT] %s\n", mqttOk ? "已连接" : "失败，继续运行");

  // 5. 帧差法
  g_fdCfg = FrameDiff::elderCareConfig();
  g_fd.begin(g_fdCfg);
  Serial.println("[OK]   帧差法就绪");

  // 5b. 告警历史 + TTS 参数恢复
  loadAlertHistory();
  Serial.printf("[HIST] 已加载 %d 条告警历史\n", g_alertHistCnt);
  {
    Preferences prefs; prefs.begin("danjuan", true);
    String savedVoice = prefs.getString("voice", "");
    float savedSpeed  = prefs.getFloat("speed", 1.0f);
    float savedVolume = prefs.getFloat("volume", 1.0f);
    prefs.end();
    if (savedVoice.length() > 0) g_cloud.setTtsConfig(savedVoice.c_str(), savedSpeed, savedVolume);
  }

  // 6. FSM
  gFsm.onEnter(AppState::Monitoring, []() {
    Serial.println("[FSM]  → Monitoring");
  });
  gFsm.onEnter(AppState::Alerting, []() {
    Serial.println("[FSM]  → Alerting");
  });
  gFsm.onEnter(AppState::OfflineFallback, []() {
    Serial.println("[FSM]  → OfflineFallback (本地降级)");
  });
  gFsm.feed(Event::Kind::BOOT_DONE);
  gFsm.feed(Event::Kind::FRAME_DIFF_NORMAL);

  if (mqttOk) g_mqtt.publishStatus("MONITORING", g_scene, true);
  Serial.println("[READY] 开始巡检\n");
}

void loop() {
  if (g_bleConfigDone) { delay(500); ESP.restart(); }
  esp_task_wdt_reset();  // 喂狗
  g_mqtt.loop();
  checkWiFi();
  checkHeap();

  AppState st = gFsm.state();
  updateLed(st);

  // SU-03T 唤醒词（模块未就绪，暂用小程序"对话"按钮替代）
  // if (st == AppState::Idle || st == AppState::Monitoring) {
  //   pollSU03T();
  // }

  switch (st) {
    case AppState::Idle:
      g_mqtt.loop();
      delay(100);
      break;

    case AppState::Monitoring:
      if (g_continuousTalk && !g_voiceActive) {
        // 连续对话：跳过巡检，直接听
        runConversationOnce();
        gFsm.feed(Event::Kind::FRAME_DIFF_NORMAL);
      } else if (!g_voiceActive) {
        doMonitoring();
      }
      break;

    case AppState::Listening:
    case AppState::Thinking:
    case AppState::Speaking:
      // 语音链阻塞执行，这里作为异常兜底
      g_mqtt.loop();
      checkWiFi();
      delay(100);
      break;

    case AppState::Alerting:
      g_mqtt.loop();
      checkWiFi();
      delay(100);
      break;

    case AppState::OfflineFallback: {
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb && fb->buf) {
        FrameDiff::Result r = g_fd.processFrame(fb->buf, fb->len, millis());
        esp_camera_fb_return(fb);
        static uint32_t lastOffBeat = 0;
        if (millis() - lastOffBeat > 10000) {
          lastOffBeat = millis();
          Serial.printf("[OFF]  离线巡检  score=%.2f  anomaly=%d\n",
                        r.diffScore, r.anomalyConfirmed);
        }
      }
      { uint32_t _d = millis() + g_fdCfg.patrolIntervalMs;
        while (millis() < _d) { esp_task_wdt_reset(); g_mqtt.loop(); delay(100); } }
      break;
    }

    default:
      g_mqtt.loop();
      checkWiFi();
      delay(100);
      break;
  }

  publishTelemetryIfNeeded();
  publishStatusIfNeeded();
}

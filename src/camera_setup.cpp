#include "camera_setup.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "camera_pins.h"
#include "app_config.h"
#include "globals.h"
#include "tos_upload.h"

// 公开读 URL
String tosPublicUrl(int idx) {
  char buf[80];
  snprintf(buf, sizeof(buf), "https://the-test-tos.tos-cn-beijing.volces.com/esp32/patrol-%02d.jpg", idx + 1);
  return String(buf);
}

// 豆包视觉分析
String analyzeWithDoubao(const char* publicUrl) {
  WiFiClientSecure ssl; ssl.setInsecure();
  HTTPClient http; http.setTimeout(20000); http.setReuse(false);
  if (!http.begin(ssl, AppConfig::kChatApiUrl)) return "分析失败";

  String bearer = "Bearer " + String(AppConfig::kApiKey);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", bearer);

  String body = "{\"model\":\"" + String(AppConfig::kChatModel) + "\","
    "\"max_tokens\":60,"
    "\"messages\":["
      "{\"role\":\"system\",\"content\":\"你是安全巡检助手。用一句话描述图中发生了什么(跌倒/摔倒/久坐/长时间静止/异常姿势/正常)。\"},"
      "{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"分析这张监控图\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" + String(publicUrl) + "\"}}"
      "]}"
    "]}";

  int code = http.POST(body);
  String result = (code == 200) ? "云端分析完成" : "分析失败";
  if (code == 200) {
    String resp = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, resp)) {
      const char* answer = doc["choices"][0]["message"]["content"];
      if (answer) result = String(answer);
    }
  }
  http.end();
  Serial.printf("[VISION] %s\n", result.c_str());
  return result;
}

// 相机：GRAYSCALE 巡检
bool camInitGray() {
  camera_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ledc_channel = LEDC_CHANNEL_0; cfg.ledc_timer = LEDC_TIMER_0;
  cfg.pin_d0 = Y2_GPIO_NUM; cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2 = Y4_GPIO_NUM; cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4 = Y6_GPIO_NUM; cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6 = Y8_GPIO_NUM; cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk = XCLK_GPIO_NUM; cfg.pin_pclk = PCLK_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM; cfg.pin_href = HREF_GPIO_NUM;
  cfg.pin_sscb_sda = SIOD_GPIO_NUM; cfg.pin_sscb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn = PWDN_GPIO_NUM; cfg.pin_reset = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format  = PIXFORMAT_GRAYSCALE;
  cfg.frame_size    = FRAMESIZE_QVGA;
  cfg.fb_count      = 1;
  cfg.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
  if (esp_camera_init(&cfg) != ESP_OK) {
    esp_camera_deinit();
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_whitebal(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_ae_level(s, -1);
    s->set_gain_ctrl(s, 1);
    s->set_gainceiling(s, GAINCEILING_4X);
  }
  return true;
}

// 相机硬复位
void cameraHardReset() {
  esp_camera_deinit();
  delay(100);
  if (PWDN_GPIO_NUM >= 0) {
    pinMode(PWDN_GPIO_NUM, OUTPUT);
    digitalWrite(PWDN_GPIO_NUM, HIGH);
    delay(100);
    digitalWrite(PWDN_GPIO_NUM, LOW);
    delay(200);
  }
}

// 拍照 → TOS
bool snapAndUpload(const char* url) {
  cameraHardReset();
  delay(1000);  // 给传感器 1s 彻底掉电复位

  camera_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ledc_channel = LEDC_CHANNEL_0; cfg.ledc_timer = LEDC_TIMER_0;
  cfg.pin_d0 = Y2_GPIO_NUM; cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2 = Y4_GPIO_NUM; cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4 = Y6_GPIO_NUM; cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6 = Y8_GPIO_NUM; cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk = XCLK_GPIO_NUM; cfg.pin_pclk = PCLK_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM; cfg.pin_href = HREF_GPIO_NUM;
  cfg.pin_sscb_sda = SIOD_GPIO_NUM; cfg.pin_sscb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn = PWDN_GPIO_NUM; cfg.pin_reset = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format  = PIXFORMAT_JPEG;
  cfg.frame_size    = FRAMESIZE_SVGA;
  cfg.jpeg_quality  = 10;
  cfg.fb_count      = 1;
  cfg.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
  if (esp_camera_init(&cfg) != ESP_OK) {
    esp_camera_deinit();
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, -1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_ae_level(s, 0);
    s->set_gain_ctrl(s, 1);
    s->set_gainceiling(s, GAINCEILING_4X);
  }
  delay(800);
  for (int i = 0; i < 5; i++) {
    camera_fb_t* discard = esp_camera_fb_get();
    if (discard) esp_camera_fb_return(discard);
    delay(100);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb || !fb->buf) { esp_camera_deinit(); return false; }

  bool ok = tosUploadJpeg(url, fb->buf, fb->len);
  Serial.printf("       TOS %s  %u B\n", ok ? "OK" : "FAIL", (unsigned)fb->len);
  esp_camera_fb_return(fb);
  esp_camera_deinit();
  return ok;
}

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// MQTT 消息推送——对接微信小程序
// EMQX Cloud Serverless, TLS + 用户名密码认证
class MqttClient {
 public:
  // keepalive 秒；LWT 发到 {deviceId}/device/status，payload online=false
  bool begin(const char* broker, uint16_t port,
             const char* deviceId,
             const char* username, const char* password,
             uint16_t keepAliveSec = 30);
  void loop();  // 必须每 loop 调用一次

  // IMessaging 接口
  bool publishStatus(const char* fsmState, const char* scene, bool online);
  bool publishAlert(const char* eventId, const char* eventType,
                    const char* severity, float confidence,
                    const char* summary, const char* imageRef,
                    const char* scene, const char* analysis = nullptr);
  bool publishTelemetry(int rssi, uint32_t freeHeap,
                        bool sdReady, bool camReady,
                        int imgQueue,
                        const char* tosStatus, int alertCount);
  bool publishImageRef(const char* eventId, const char* imgUrl,
                       const char* type);

  void onControl(void (*cb)(const char* cmd));
  bool connected() { return client_.connected(); }

 private:
  WiFiClientSecure espClient_;
  PubSubClient client_{espClient_};
  String deviceId_;
  String username_;
  String password_;
  void (*controlCb_)(const char*) = nullptr;

  bool publishJson(const char* topic, const String& json);
  void reconnect();
};

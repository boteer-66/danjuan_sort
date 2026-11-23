#include "mqtt_client.h"

#include <ArduinoJson.h>

static const char* kStatus    = "status";
static const char* kAlert     = "alert";
static const char* kTelemetry = "telemetry";
static const char* kImageRef  = "image_ref";
static const char* kControl   = "control";

bool MqttClient::begin(const char* broker, uint16_t port,
                       const char* deviceId,
                       const char* username, const char* password,
                       uint16_t keepAliveSec) {
  deviceId_ = deviceId;
  username_  = username;
  password_  = password;
  // EMQX Cloud 需 TLS，跳过证书验证（比赛环境可接受）
  espClient_.setInsecure();
  client_.setServer(broker, port);
  client_.setKeepAlive(keepAliveSec > 0 ? keepAliveSec : 30);

  using std::placeholders::_1;
  using std::placeholders::_2;
  using std::placeholders::_3;
  client_.setCallback([this](char* topic, byte* payload, unsigned length) {
    String t(topic);
    String msg;
    msg.concat((char*)payload, length);
    if (t.endsWith(kControl) && controlCb_) {
      JsonDocument doc;
      if (!deserializeJson(doc, msg)) {
        controlCb_(doc["cmd"] | "");
      }
    }
  });

  reconnect();
  return client_.connected();
}

void MqttClient::loop() {
  if (!client_.connected()) reconnect();
  client_.loop();
}

void MqttClient::reconnect() {
  if (client_.connected()) return;
  String clientId = deviceId_ + "_" + String(random(0xffff), HEX);
  // LWT：异常掉线时 broker 代发 offline，小程序据此置灰
  String willTopic = deviceId_ + "/device/" + kStatus;
  static const char* kWillPayload =
      "{\"online\":false,\"fsm_state\":\"OFFLINE\",\"reason\":\"lwt\"}";
  if (client_.connect(clientId.c_str(), username_.c_str(), password_.c_str(),
                      willTopic.c_str(), 1, true, kWillPayload)) {
    client_.subscribe((deviceId_ + "/device/" + kControl).c_str(), 1);
  }
}

// ---- IMessaging 实现 ----

bool MqttClient::publishStatus(const char* fsmState, const char* scene, bool online) {
  JsonDocument doc;
  doc["device_id"]  = deviceId_;
  doc["fsm_state"]  = fsmState;
  doc["scene"]      = scene;
  doc["online"]     = online;
  doc["msg_id"]     = "st_" + String(millis());
  String json;
  serializeJson(doc, json);
  return publishJson(kStatus, json);
}

bool MqttClient::publishAlert(const char* eventId, const char* eventType,
                              const char* severity, float confidence,
                              const char* summary, const char* imageRef,
                              const char* scene, const char* analysis) {
  JsonDocument doc;
  doc["event_id"]   = eventId;
  doc["device_id"]  = deviceId_;
  doc["scene"]      = scene ? scene : "eldercare";
  doc["event_type"] = eventType;
  doc["severity"]   = severity;
  doc["confidence"] = confidence;
  doc["summary"]    = summary;
  doc["image_ref"]  = imageRef ? imageRef : "";
  if (analysis && analysis[0]) doc["analysis"] = analysis;
  time_t now; time(&now);
  struct tm* t = localtime(&now);
  char timeBuf[25];
  if (now > 1700000000) {
    snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             t->tm_year+1900, t->tm_mon+1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
  } else {
    snprintf(timeBuf, sizeof(timeBuf), "NTP未同步");
  }
  doc["time"]       = timeBuf;
  String json;
  serializeJson(doc, json);
  return publishJson(kAlert, json);
}

bool MqttClient::publishTelemetry(int rssi, uint32_t freeHeap,
                                  bool sdReady, bool camReady,
                                  int imgQueue,
                                  const char* tosStatus, int alertCount) {
  JsonDocument doc;
  doc["device_id"]   = deviceId_;
  doc["rssi"]        = rssi;
  doc["free_heap"]   = freeHeap;
  doc["sd_ready"]    = sdReady;
  doc["cam_ready"]   = camReady;
  doc["uptime"]      = millis() / 1000;
  doc["img_queue"]   = imgQueue;
  doc["tos_status"]  = tosStatus;
  doc["alert_count"] = alertCount;
  String json;
  serializeJson(doc, json);
  return publishJson(kTelemetry, json);
}

bool MqttClient::publishImageRef(const char* eventId, const char* imgUrl, const char* type) {
  JsonDocument doc;
  doc["event_id"]  = eventId;
  doc["device_id"] = deviceId_;
  doc["img_url"]   = imgUrl;
  doc["timestamp"] = "2026-05-24T00:00:00";  // FIXME: NTP 时间
  doc["type"]      = type;
  String json;
  serializeJson(doc, json);
  return publishJson(kImageRef, json);
}

void MqttClient::onControl(void (*cb)(const char*)) { controlCb_ = cb; }

// ---- private ----

bool MqttClient::publishJson(const char* subTopic, const String& json) {
  if (!client_.connected()) return false;
  String full = deviceId_ + "/device/" + subTopic;
  return client_.publish(full.c_str(), json.c_str());
}

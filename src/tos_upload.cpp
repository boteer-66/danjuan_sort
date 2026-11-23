#include "tos_upload.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ---- 命名 ----

String tosMakeFilename(const char* scene, int seq) {
  time_t now; time(&now);
  struct tm* t = localtime(&now);
  char ts[20];
  if (now > 1700000000) {
    // NTP 已同步 → 真实时间戳
    snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
  } else {
    // 无 NTP → uptime 秒数
    snprintf(ts, sizeof(ts), "uptime%lu", (unsigned long)(millis() / 1000));
  }
  char name[80];
  snprintf(name, sizeof(name), "%s_%s_%04d.jpg", scene, ts, seq);
  return String(name);
}

// ---- 上传 ----

bool tosUploadJpeg(const char* url, const uint8_t* data, size_t len) {
  if (WiFi.status() != WL_CONNECTED || !url || !data || !len) return false;

  WiFiClientSecure ssl;
  ssl.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  http.setReuse(false);

  if (!http.begin(ssl, url)) {
    Serial.println("[TOS] HTTP begin FAIL");
    return false;
  }

  http.addHeader("Content-Type", "image/jpeg");
  int code = http.PUT(String((const char*)data, len));

  bool ok = (code == 200 || code == 201 || code == 204);
  if (!ok) {
    Serial.printf("[TOS] http=%d  %s\n", code,
                  code > 0 ? http.getString().c_str() : http.errorToString(code).c_str());
  }
  http.end();
  return ok;
}

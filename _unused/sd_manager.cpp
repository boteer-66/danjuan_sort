#include "sd_manager.h"
#include <time.h>

// ===== helpers =====

void SdManager::feed() {
  esp_task_wdt_reset();
}

String SdManager::todayStr() {
  time_t now; time(&now);
  struct tm t; localtime_r(&now, &t);
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year+1900, t.tm_mon+1, t.tm_mday);
  return String(buf);
}

bool SdManager::mkdirIf(const char* path) {
  if (!SD_MMC.exists(path)) {
    if (!SD_MMC.mkdir(path)) {
      Serial.printf("[SD] mkdir FAIL: %s\n", path);
      return false;
    }
    Serial.printf("[SD] mkdir %s\n", path);
  }
  return true;
}

// ===== init / shutdown =====

bool SdManager::begin() {
  SD_MMC.setPins(45, 1, 43);  // CLK=45 远离PCLK(8), CMD=1, D0=43  // D0=43 永久方案：CH343 RXD高阻输入，永远不抢线
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[SD] mount FAIL");
    return false;
  }
  feed();  // 在 SD_MMC.begin() 之后喂，不在之前

  Serial.printf("[SD] %s %llu MB\n",
    SD_MMC.cardType() == CARD_SDHC ? "SDHC" : "SD",
    SD_MMC.cardSize() / (1024*1024));

  // 脏标记检测
  if (wasDirty()) {
    Serial.println("[SD] WARNING: dirty flag found — last shutdown was unsafe");
  }
  setDirty();
  feed();

  // 建目录（父目录先建，SD.mkdir() 不递归）
  const char* dirs[] = {
    "/logs", "/chat", "/queue",
    "/audio", "/audio/in", "/audio/out",
    "/image", "/image/event", "/image/patrol"
  };
  for (auto d : dirs) mkdirIf(d);
  feed();

  mounted_ = true;
  Serial.println("[SD] ready");
  return true;
}

void SdManager::end() {
  if (!mounted_) return;
  feed();
  clearDirty();
  SD_MMC.end();
  mounted_ = false;
  Serial.println("[SD] unmounted safely");
}

// ===== dirty flag =====

void SdManager::setDirty() {
  File f = SD_MMC.open(kDirtyFlag, FILE_WRITE);
  if (f) { f.print("1"); f.close(); }
}

void SdManager::clearDirty() {
  SD_MMC.remove(kDirtyFlag);
}

bool SdManager::wasDirty() {
  return SD_MMC.exists(kDirtyFlag);
}

// ===== atomic write =====

bool SdManager::writeAtomic(const char* path, const uint8_t* buf, size_t len) {
  if (!mounted_ || !buf || !len) return false;
  feed();

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) { Serial.printf("[SD] open FAIL: %s\n", path); return false; }
  size_t w = f.write(buf, len);
  f.close();

  if (w != len) { Serial.printf("[SD] write short: %s (%u/%u)\n", path, w, len); return false; }
  feed();
  return true;
}

bool SdManager::appendLine(const char* path, const char* line) {
  if (!mounted_ || !line) return false;
  feed();

  File f = SD_MMC.open(path, FILE_APPEND);
  if (!f) { Serial.printf("[SD] append FAIL: %s\n", path); return false; }
  f.println(line);
  f.close();

  feed();
  return true;
}

// ===== public API =====

bool SdManager::writeLog(const char* line) {
  String path = String("/logs/sys_") + todayStr() + ".log";
  return appendLine(path.c_str(), line);
}

bool SdManager::saveJpeg(const char* dir, const uint8_t* buf, size_t len) {
  // 先写临时文件，写完改名（原子化）
  String tmp  = String(dir) + "/.tmp";
  if (!writeAtomic(tmp.c_str(), buf, len)) return false;

  // ESP32 无 RTC，用启动毫秒数做文件名（唯一且递增）
  static int seq = 0;
  char name[64];
  snprintf(name, sizeof(name), "%s/%lu_%04d.jpg",
           dir, static_cast<unsigned long>(millis() / 1000), ++seq);

  if (!SD_MMC.rename(tmp.c_str(), name)) {
    Serial.printf("[SD] rename FAIL: %s -> %s\n", tmp.c_str(), name);
    SD_MMC.remove(tmp.c_str());
    return false;
  }
  Serial.printf("[SD] JPEG saved: %s (%u bytes)\n", name, len);

  // 超上限清理
  purgeOldest(dir, kMaxImagesPerDir);
  feed();
  return true;
}

bool SdManager::appendQueue(const char* jsonLine) {
  return appendLine("/queue/upload.jsonl", jsonLine);
}

bool SdManager::markQueueDone(const char* eventId) {
  String donePath = "/queue/uploaded.jsonl";
  return appendLine(donePath.c_str(), eventId);
}

// ===== cleanup =====

int SdManager::purgeOldest(const char* dir, int keep) {
  feed();
  std::vector<String> files;
  File root = SD_MMC.open(dir);
  if (!root) return -1;
  File f = root.openNextFile();
  while (f) {
    if (!f.isDirectory()) files.push_back(String(f.name()));
    f = root.openNextFile();
  }
  root.close();

  int removed = 0;
  while ((int)files.size() > keep) {
    // 删字典序最前的（最旧）
    int oldest = 0;
    for (int i = 1; i < (int)files.size(); i++)
      if (files[i] < files[oldest]) oldest = i;
    String path = String(dir) + "/" + files[oldest];
    if (SD_MMC.remove(path.c_str())) { removed++; }
    files.erase(files.begin() + oldest);
    feed();
  }
  return removed;
}

void SdManager::rotateLogs(int keepDays) {
  feed();
  for (int age = keepDays; age <= 30; age++) {
    time_t then = time(nullptr) - age * 86400;
    struct tm t; localtime_r(&then, &t);
    char name[32];
    snprintf(name, sizeof(name), "/logs/sys_%04d-%02d-%02d.log",
             t.tm_year+1900, t.tm_mon+1, t.tm_mday);
    if (SD_MMC.exists(name)) {
      SD_MMC.remove(name);
      Serial.printf("[SD] rotated: %s\n", name);
    }
    feed();
  }
}

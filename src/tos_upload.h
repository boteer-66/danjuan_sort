#pragma once
#include <Arduino.h>

// TOS 上传工具：预签名 URL PUT + 文件命名

// 生成文件名: {scene}_{YYYYMMDD}_{HHMMSS}_{seq}.jpg
// 无 NTP 时用 uptime 秒数替代时间戳
String tosMakeFilename(const char* scene, int seq);

// 上传 JPEG buffer 到 TOS, 返回 true 表示 HTTP 200/201/204
bool tosUploadJpeg(const char* url, const uint8_t* data, size_t len);
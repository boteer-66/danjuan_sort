#pragma once
#include <Arduino.h>
#include "esp_camera.h"

// 相机工具：初始化 + 预热 + 拍照
// 命名规范：{scene}_{YYYYMMDD}_{HHMMSS}_{seq}.jpg

struct CameraFrame {
  uint8_t* buf  = nullptr;
  size_t   len  = 0;
  uint16_t w    = 0;
  uint16_t h    = 0;
};

bool cameraInit();
void cameraWarmup(int frames);

// 拍照，返回 heap 拷贝（调用方 free），不会跟 camera FB 池冲突
CameraFrame cameraCapture();
#pragma once
#include <Arduino.h>

bool camInitGray();
void cameraHardReset();
bool snapAndUpload(const char* url);
String tosPublicUrl(int idx);
String analyzeWithDoubao(const char* publicUrl);

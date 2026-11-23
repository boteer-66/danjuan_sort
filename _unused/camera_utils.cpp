#include "camera_utils.h"
#include "camera_pins.h"

bool cameraInit() {
  camera_config_t cfg;
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0  = Y2_GPIO_NUM;  cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2  = Y4_GPIO_NUM;  cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4  = Y6_GPIO_NUM;  cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6  = Y8_GPIO_NUM;  cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk  = XCLK_GPIO_NUM;   cfg.pin_pclk = PCLK_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM;  cfg.pin_href = HREF_GPIO_NUM;
  cfg.pin_sscb_sda = SIOD_GPIO_NUM; cfg.pin_sscb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn  = PWDN_GPIO_NUM;   cfg.pin_reset = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format  = PIXFORMAT_JPEG;
  cfg.frame_size    = FRAMESIZE_SVGA;
  cfg.jpeg_quality  = 6;
  cfg.fb_count      = 3;
  cfg.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;

  if (esp_camera_init(&cfg) != ESP_OK) return false;

  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_brightness(s, 1);
  s->set_contrast(s, 1);
  s->set_sharpness(s, 1);
  s->set_whitebal(s, 1);
  s->set_gain_ctrl(s, 1);
  s->set_exposure_ctrl(s, 1);
  return true;
}

void cameraWarmup(int frames) {
  for (int i = 0; i < frames; i++) {
    camera_fb_t* tmp = esp_camera_fb_get();
    if (tmp) esp_camera_fb_return(tmp);
    delay(300);
  }
}

CameraFrame cameraCapture() {
  CameraFrame f;
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb || !fb->buf) return f;
  f.buf = (uint8_t*)malloc(fb->len);
  if (f.buf) {
    memcpy(f.buf, fb->buf, fb->len);
    f.len = fb->len;
    f.w   = fb->width;
    f.h   = fb->height;
  }
  esp_camera_fb_return(fb);
  return f;
}

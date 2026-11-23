#ifndef CAMERA_PINS_H
#define CAMERA_PINS_H

#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      9
#define SIOD_GPIO_NUM      5   // SDA
#define SIOC_GPIO_NUM      4   // SCL

#define Y9_GPIO_NUM        18  // D7
#define Y8_GPIO_NUM        17  // D6
#define Y7_GPIO_NUM        16  // D5
#define Y6_GPIO_NUM        15  // D4
#define Y5_GPIO_NUM        14  // D3
#define Y4_GPIO_NUM        13  // D2
#define Y3_GPIO_NUM        12  // D1
#define Y2_GPIO_NUM        11  // D0

#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      10  // 从 7 移出，7 已给 SD CLK
#define PCLK_GPIO_NUM      8

#endif
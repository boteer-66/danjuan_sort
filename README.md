# 蛋卷 Danjuan — 云边协同 AIoT 智能监测与交互终端

基于 **ESP32-S3** 的云边协同 AIoT 终端：帧差法巡检、MQTT 双向通信、火山引擎豆包视觉/语音链路、微信小程序面板。

> **说明：** 原仓库 `danjuan` 因部分内容与配置需整理，已转为私有；**本仓为整理后的公开版本**（`danjuan_sort`），代码与文档来自嵌赛交付工程，并已脱敏（密钥、预签名 URL 等改为占位符）。

**竞赛：** 第九届（2026）全国大学生嵌入式芯片与系统设计竞赛 · 芯片应用赛道 · **北部赛区三等奖**（中国电子教育学会）

**配套小程序（整理公开版）：** [danjuan-app_sort](https://github.com/boteer-66/danjuan-app_sort)（链接以实际仓库为准）

---

## 功能

- 摄像头帧差法巡检（运动 / 长时间静止双路触发）
- 异常抓拍上传 TOS + 豆包视觉一句话分析
- MQTT 三级告警推送到微信小程序
- 云端语音对话（ASR → LLM → TTS）
- 双模态一键切换（老人看护 / 工位健康）
- OTA 远程升级、LWT 离线检测等工程化能力

## 硬件

| 模块 | 型号 |
|------|------|
| MCU | ESP32-S3 N16R8 |
| 摄像头 | OV2640 |
| 麦克风 | PDM MEMS |
| 喇叭 | NS4168 I2S 功放 |
| LED | WS2812B ×12 |
| 唤醒 | SU03T（接口预留） |

## 快速开始

1. 克隆本仓库，用 **PlatformIO** 打开  
2. 配置 `src/app_config.h`（WiFi / 火山引擎 API Key 占位符）  
3. 配置 `src/main.cpp` 中 EMQX 连接信息占位符  
4. 用 `src/gen_20_urls.py` 生成 TOS 预签名 URL，填入 `TOS_URLS`（**勿提交真实 AK/SK**）  
5. `pio run -t upload` 烧录  

小程序侧见 `danjuan-app_sort`，替换同一套 MQTT 连接配置。

## 项目结构（摘要）

```text
src/           固件：main / camera / mqtt / gateway / audio / fsm / vision
include/       栈覆盖等编译辅助
docs/          设计与诊断文档
嵌赛提交技术文档.md
修改日志与踩坑记录.md
```

## 安全提示

- 仓库内所有密钥、TOS 签名均为**占位符**  
- 请勿将真实 AK/SK、WiFi 密码、EMQX 口令提交到 Git  
- TOS 预签名 URL 请在本地生成并注意有效期  

## License

仅供学习交流；二次使用请自行承担配置与安全责任。

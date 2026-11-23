# 蛋卷 V3 完成状态 & 接口对齐文档

> 2026-05-29 | 供小 Claude 对齐小程序和 ESP32 下位机

---

## 一、V3 已完成功能

### 语音链路
- WS 长连接复用（ASR/TTS WS 跨轮保持）
- 多轮对话（Listening↔Thinking↔Speaking 循环）
- LLM 5 轮对话记忆（内存 vector）
- LLM 回复去重
- 动态告别（LLM 生成，非死句子）
- Persona 精调（软萌小女孩语气）
- 音色: `zh_female_linjianvhai_moon_bigtts`（邻家女孩）
- 退出现: 说"拜拜/退出/再见/睡觉" + 2 轮静默超时（16s）

### 双模态
- 帧差法（16×16 块，自适应背景，上下半区分割）
- 摄像头 JPEG 拍照（SVGA 800×600）
- TOS 上传（预签名 URL PUT）
- **豆包读图闭环**: 告警→TOS上传→公开URL→doubao视觉分析→串口输出

### 系统
- FreeRTOS 双核骨架（PcmRingBuffer + playback task，代码到位等 PCB）
- WiFi 断线自动重连
- 内存水位告警（<100KB 串口飙红）
- WiFi.setSleep(false)
- 延迟优化: idle 停顿 400ms, max_tokens 80, ASR final 2s

### 模型配置
- LLM: `doubao-seed-1.6-flash`（已确认支持读图）
- ASR: `volcano_asr`
- TTS: `doubao-tts`
- API Key: `YOUR_API_KEY`
- 网关: `https://ai-gateway.vei.volces.com/v1/chat/completions`

---

## 二、MQTT 接口（小程序对齐）

### 连接
- 平台: **EMQX Cloud Serverless**（免费版）
- 部署: `oaf8afff.ala.cn-hangzhou.emqxsl.cn`
- MQTT TLS 端口: `8883`
- WebSocket TLS 端口: `8084`
- 协议: `wxs://` (WebSocket over TLS)
- 认证: 用户名 `YOUR_EMQX_USERNAME` / 密码 `YOUR_EMQX_PASSWORD`
- 主题前缀: `danjuan_1129_66/device/`
- 替代原有的公共 Broker `broker.emqx.io`（无认证，不再使用）

### 5 个主题

| 主题 | 方向 | QoS | 说明 |
|------|:--:|:--:|------|
| `status` | ESP32→小程序 | 1 | 设备状态 |
| `alert` | ESP32→小程序 | 1 | 告警事件 |
| `telemetry` | ESP32→小程序 | 0 | 设备遥测 |
| `image_ref` | ESP32→小程序 | 1 | TOS 图片链接 |
| `control` | 小程序→ESP32 | 1 | 控制命令 |

### telemetry JSON 新增字段（V3）

```json
{
  "device_id": "danjuan_1129_66",
  "rssi": -55,
  "free_heap": 220000,
  "sd_ready": false,
  "cam_ready": true,
  "uptime": 3600,
  "img_queue": 0,
  "tos_status": "ok",
  "alert_count": 3
}
```

| 新字段 | 类型 | 值 |
|------|------|------|
| `tos_status` | String | `"idle"` / `"uploading"` / `"ok"` / `"fail"` |
| `alert_count` | int | 当日累计异常次数 |

### 小程序面板建议

| 指标位置 | 现在 | 改后 |
|------|------|------|
| 第一行 | 📡 WiFi 信号 | 保留不动 |
| 第二行 | 🧠 空闲内存 | 📸 TOS 上传状态（`tos_status`） |
| 第三行 | 📦 待传队列 | ⚠️ 今日告警（`alert_count`） |

---

## 三、V4 待做

| 项 | 说明 |
|------|------|
| BLE 配网 | 小程序发 SSID+密码→ESP32 BLE 收→存 NVS→连 WiFi |
| 音色切换接口 | 小程序设置页→MQTT `voice:xxx`→ESP32 改 `kTtsVoice` |
| STS Token TOS | ESP32 动态命名上传（替代预签名） |
| barge-in 打断 | 播放中唤醒词停播 |
| FSM 全状态集成 | 语音+巡检+告警三线并发 |
| SU-03T 唤醒 | 固件重烧+UART 联调 |
| 告警分级 | P0 语音播报+小程序, P1 仅小程序, P2 仅日志 |
| 告警记忆查询 | "最近有什么异常"→LLM 上下文 |
| PCB + 外壳 | 六月 |

---

## 四、ESP32 当前引脚

| 功能 | GPIO |
|------|:--:|
| SD CLK | 45 |
| SD CMD | 1 |
| SD D0 | 43 |
| Camera D0-D7 | 11-18 |
| Camera PCLK | 8 |
| Camera XCLK | 9 |
| Camera VSYNC | 6 |
| Camera HREF | 10 |
| Camera SDA/SCL | 5/4 |
| PDM Mic CLK | 2 |
| PDM Mic DATA | 3 |
| I2S Spk BCLK | 7 |
| I2S Spk LRCLK | 21 |
| I2S Spk DOUT | 46 |
| SU-03T RX | 44 |
| SU-03T TX | 0 |

---

## 五、小程序待改清单

1. **面板对齐**: 替换那两个字段（见第二节）
2. **BLE 配网页**: 新增一页，扫描 BLE→连接→发 WiFi 配置
3. **音色选择器**: 设置页下拉菜单，发 MQTT `{cmd:"voice:xxx"}`
4. **HomeKit 风格**: iOS 毛玻璃+大圆角白卡（已在改）

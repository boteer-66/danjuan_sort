# 蛋卷 — 深度诊断清单：栈溢出 / 看门狗 / OTA / 隐藏 Bug

> 2026-07 纯代码静态审阅（不改业务逻辑）
> 范围：`src/` 固件 + 对照小程序契约；结合《修改日志与踩坑记录》与运行时现象
> 用途：明年前打磨前的「问题地图」；按优先级修，避免只抬参数不治病

---

## 0. 如何用这份清单

### 0.1 现象对号入座（串口特征）

| 串口/复位现象 | 更可能的原因 | 本文条目 |
|---|---|---|
| `Stack canary watchpoint triggered (cam_task)` | 相机任务栈 / 排线信号完整性 | §1.1 |
| `Guru Meditation Error` + `Stack overflow` + 任务名是 `loopTask`/`main` | 主任务 TLS + 阻塞云调用叠峰 | §1.2 |
| `task_wdt: Task watchdog got triggered` | 长阻塞中途不喂狗 | §2 |
| 抓拍后约 1s 重启，且 log 有 `[SNAP] 重启清传感器状态` | **主动** `ESP.restart()`，不是崩溃 | §3.1 |
| 相机 reinit 失败后 `[FATAL] ... 重启` | 相机状态残留/初始化顺序 | §3.2 |
| MQTT 在对话/播报后 `failed` 再 `已连接` | 阻塞期 keepalive 超时 | §2.3 |
| TOS `http=403 Request has expired` | 预签名 URL 过期 | §4.1 |
| 小程序一直显示「设备在线」但设备已拔电 | 无 LWT + App 无在线超时 | §5.1 / §8.1 |
| ASR 第二轮空、第三轮退出对话 | TTS 灌麦 + 连败退出策略 | §2.4 / §6 |
| 升级后不重启或半砖 | OTA 无校验/无回滚 | §7 |

### 0.2 严重度

- **P0**：导致崩溃/失联/演示翻车
- **P1**：功能名不副实或安全风险
- **P2**：体验/可维护性/真实性（文档写到了但没做到）
- **P3**：清理项、死代码、测试债

---

## 1. 栈溢出：为什么频繁？

ESP32 上「栈溢出」「堆分配失败」「TLS 内存不够」三者表象接近。本项目是**结构性多峰叠加**，不是单一 bug。

### 1.1 P0 — `cam_task` 栈（已部分修复，根因仍在）

**机制**

- 预编译 `esp32-camera` 把 `CONFIG_CAMERA_TASK_STACK_SIZE` 钉死为 **2048**
- 对预编译 `.a` 写 `-D` 宏**无效**；`include/camera_stack_override.h` 只影响你自己的编译单元

**现有缓解**（`main.cpp:160-171`）

```cpp
// linker wrap: 仅当 task 名字 == "cam_task" 时强制 stack = 8192
if (strcmp(name, "cam_task") == 0) stack = 8192;
```

**为何仍会炸**

| # | 原因 | 说明 |
|---|---|---|
| 1.1.1 | 物理层 | 30cm FPC、20MHz DVP 衰减 → 驱动错误路径更深 → 8KB 仍可能不够 |
| 1.1.2 | 双像素格式反复 init | 巡检 GRAY QVGA ↔ 抓拍 JPEG SVGA，`deinit/init` 往返，失败恢复栈更深 |
| 1.1.3 | wrap 匹配脆弱 | 只认字面量 `"cam_task"`；库改名/内部子任务会漏 |
| 1.1.4 | 未订阅 WDT | `cam_task` 卡死不会直接 WDT，可能表现为无响应后主循环 WDT |

**验证方法**

- 抓一次完整 crash backtrace，确认 PC 是否落在 camera 驱动 / `cam_task`
- 串口连续跑 2h 帧差巡检 + 每 5 分钟强制 `capture`，统计 canary 次数 vs 排线长度

**修复方向（按性价比）**

1. 硬件：摄像头直焊或 ≤15cm 排线（文档已验证有效）
2. 降低 `xclk_freq_hz`（20→16MHz）做对照实验
3. 减少 JPEG↔GRAY 全量 reinit（见 §3.2）
4. wrap 同时匹配 `cam_task` 前缀，并打日志确认命中
5. 长期：换支持更大栈的 camera 库配置或自编 framework

---

### 1.2 P0 — 主任务 `loopTask` 32KB 对「多路 TLS + 阻塞」过紧

`platformio.ini`：`board_build.stack_size = 32768`。

**同屏可能共存的资源**

| 资源 | 位置 | 压力 |
|---|---|---|
| MQTT TLS 常驻 | `mqtt_client.h` `WiFiClientSecure espClient_` | mbedTLS 握手/读写，栈 8~15KB 级 |
| LLM HTTPS 持久连接 | `gateway_client.h` `chatClient_` | 额外 `WiFiClientSecure` |
| ASR WebSocket TLS | `RawWebSocketClient` | 会话可跨轮复用 |
| TTS WebSocket TLS | 每次 `synthesizeAndPlay` **新建** | 峰值时再 +1 路 TLS |
| TOS PUT | `tos_upload.cpp` 栈上 `WiFiClientSecure` | 叠在拍照路径 |
| OTA HTTPS | `doOTA` | 又一路 TLS |
| ArduinoJson / `String` | 全工程 | 临时对象碎片化堆 |
| TTS PCM 缓冲 | `buffer.reserve(131072)` | **堆** 128KB，挤占内部 SRAM |
| ASR 每 100ms base64 | `transcribeRealtime` | 高频小块堆分配 |

**结论**：8MB PSRAM **不能当栈用**；堆若未强制进 PSRAM，内部 SRAM 会被 TLS + PCM 打穿。栈溢出、`malloc` 失败、TLS 握手失败会轮换出现。

**验证方法**

- 对话/告警全链路期间打印：`ESP.getFreeHeap()`、`ESP.getMinFreeHeap()`、`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`、`MALLOC_CAP_SPIRAM`
- 开 `CONFIG_HEAP_POISONING` / 抓 `mem_manage` 故障地址

**修复方向**

1. 大缓冲显式进 PSRAM（`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`）
2. 任意时刻最多保留 1~2 路业务 TLS；TTS 结束立刻 `delete`
3. 把云调用挪到独立 FreeRTOS 任务，栈单独给 12~16KB，主循环只做状态与 MQTT
4. 减少 `String` 拼接，改用固定缓冲 / `std::string` reserve

---

### 1.3 P1 — TTS / 播放路径的大块内存与阻塞

`gateway_client.cpp` `synthesizeAndPlay`：

```cpp
std::vector<uint8_t> buffer;
buffer.reserve(131072);  // 128KB
```

- 非 ring 回退路径：收完整段 PCM 再 `playPcm`，期间主循环停摆
- ring 路径：`while (ring_->available() > 0) delay(10);` **不喂狗**
- `audio_i2s.cpp` `playPcm`：`i2s_write(..., portMAX_DELAY)` 可无限阻塞

`playTaskLoop`（`startPlaybackTask`）栈 4096，内有 `uint8_t buf[2048] + int16_t stereo[256]`，偏紧但通常够；当前主路径似乎仍用同步 `playPcm`，play task 可能未成为主路径。

---

### 1.4 P2 — `bytesToString` / base64 的堆碎片

`raw_websocket_client.cpp`：

```cpp
out += static_cast<char>(bytes[i]);  // 逐字节 +=，易碎片
```

ASR/TTS 大 JSON 帧会放大该问题。

---

### 1.5 P2 — 帧差 `processFrame` 栈上数组

`frame_diff.cpp`：`uint8_t curAvg[kBlocks]`（300 字节）尚可；嵌套循环本身不深。相对 TLS 不是主因。

---

## 2. 看门狗：为什么会重启？

当前策略（`main.cpp:700-701`）：

```cpp
esp_task_wdt_init(30, true);  // 30s，超时 panic
esp_task_wdt_add(NULL);       // 只订阅 loopTask
```

### 2.1 P0 — 阻塞结构与 30s 硬顶

已记录：TTS 可 15~25s。把超时从 15s 抬到 30s **只是抬阈值**，没有消掉「主循环被云 I/O 占死」。

典型危险串行（告警全链路）：

```
cam hardReset + delay(1000) + AEC delay(800) + 丢帧
→ TOS TLS PUT（timeout 15s）
→ 豆包 HTTP（timeout 20s）
→ MQTT publish
→（高危）TTS TLS + 合成 + 阻塞播放（15s+）
```

只在拍照前/豆包前/TTS 前各 `esp_task_wdt_reset()` 一次，**中途不喂**。网络一抖就顶满。

### 2.2 P0 — 等待环里不喂狗的路径

| 路径 | 是否喂狗 |
|---|---|
| `waitForCondition`（gateway） | 有 |
| `doMonitoring` 无异常 drain（`patrolIntervalMs`） | **无**（一般 2~5s，尚可） |
| 冷却期 drain | **无**（短） |
| TTS ring 排空 `while (available()) delay(10)` | **无** |
| `playPcm` 阻塞写 | **无** |
| `i2s_write(portMAX_DELAY)` | 可永久卡死 |

### 2.3 P1 — MQTT keepalive 与阻塞期

踩坑记录 #19：PubSubClient 三参数 `connect` 无 keepalive，用默认 **15s**。

TTS/LLM 阻塞期间不 `g_mqtt.loop()` → broker 踢会话 → 恢复后重连风暴。表现为「对话完 MQTT 闪断」。

### 2.4 P1 — ASR 连败退出 vs 麦克风残留

`voice_agent.cpp`：TTS 后 `discardMicChunks(12)`；ASR 连败 2 次退出连续对话。  
若 discard 不足或环境噪声大，第二轮仍空——不是栈/WDT，但是「用着用着对话死了」的主因之一。

### 2.5 P2 — WDT 只挂主任务

`cam_task`、WiFi、play task 均未 `esp_task_wdt_add`。相机驱动死等时，只能靠主循环后续逻辑或整机复位。

### 2.6 P0（OTA 期间）— WDT 被主动摘掉

见 §7.2。

---

## 3. 「频繁重启」里混着的设计性重启

### 3.1 P1 — 手动抓拍成功后整机 `ESP.restart()`

`main.cpp:204-206`：

```cpp
Serial.println("[SNAP] 重启清传感器状态...");
delay(1000);
ESP.restart();
```

- 动机：规避 JPEG 模式后回灰度 init 死等（踩坑合理）
- 代价：每次抓拍都「重启设备」；MQTT 会话、语音、LED、在线状态全断；用户/演示观感像崩溃
- 小程序 `capture` 只等 `image_ref`，不感知即将重启

### 3.2 P0 — 相机 init/deinit 策略脆弱

`snapAndUpload`：`cameraHardReset` → `esp_camera_init(JPEG)` → 拍 → `esp_camera_deinit`；之后主流程再 `camInitGray`。

- 失败路径：`ESP.restart()` 兜底
- `fb_count = 1`：更容易丢帧/阻塞
- 无「init 超时/次数上限」以外的优雅降级

### 3.3 P2 — BLE / 配网完成后 `ESP.restart()`

`loop()`：`if (g_bleConfigDone) ESP.restart();`  
当前 BLE init 被注释，属休眠逻辑；若启用需防误触发。

---

## 4. 云边链路与协议层 Bug

### 4.1 P0 — TOS 预签名 URL 硬编码且会过期

- `main.cpp` 顶部 20 条 URL，`X-Tos-Expires=86400`，生成日写死
- 过期后全部上传 403 → 告警链路静默失败（仅串口 `TOS http=403`）
- 无自动刷新；依赖人手跑 `gen_20_urls.py`

**影响**：超过 24h 的连续运行/答辩彩排必炸。这是**演示可用性**最大暗雷之一。

### 4.2 P0 — `tosUploadJpeg` 用 `String` 装整张 JPEG

```cpp
http.PUT(String((const char*)data, len));
```

- SVGA Q=10 可达数十至上百 KB，再拷一份进 `String` + TLS 发送
- 堆峰值高，是栈/堆连环炸的放大器

### 4.3 P1 — MQTT 真实 QoS 与文档不符

- 订阅：`subscribe(..., 1)` QoS1
- **发布**：`PubSubClient::publish` 固定 QoS0  
  文档写 alert/status QoS1，实际**不会**保证到达
- 接口规范 `msg_id` 幂等：设备端**不解析**也不去重；小程序只生成不校验回执

### 4.4 P1 — 无 MQTT LWT（遗嘱）

设备掉电后 broker 不会自动发布 offline。

### 4.5 P1 — `online` 永远 `true`

所有 `publishStatus(..., true)` 写死在线；配合无 LWT，离线检测完全依赖「不再发 status」。

### 4.6 P2 — `publishImageRef` 时间戳写死

```cpp
doc["timestamp"] = "2026-05-24T00:00:00";  // FIXME
```

NTP 已同步也不更新。

### 4.7 P1 — 告警 JSON 可能顶破 `MQTT_MAX_PACKET_SIZE=1024`

`analysis` 为豆包一句话时通常 OK；若模型偶发长回复或 URL 很长，**整包 publish 失败且无重试/降级截断**。

### 4.8 P2 — 异常判定用字符串包含

```cpp
conclusion.indexOf("正常") < 0 && conclusion.indexOf("无异常") < 0 && ...
```

「不正常」「姿势正常但需关注」等话术会误判。演示模式 `demo` 与真实告警共用这套脆弱逻辑。

### 4.9 P2 — `FrameDiff` 的 `lowerHalfScore` / `upperHalfScore` 算了不用

跌倒相关「下半屏变化」指标存在但 `doMonitoring` 未消费；`fall_suspected` 实际由 `diffScore` + 字符串分析决定。

### 4.10 P2 — 光照/AEC 误触发

自动曝光变化会造成全局亮度漂移 → 大量 diff 块 → 假运动。背景融合与 `resetBackground` 能缓解，但无光照突变保护。

### 4.11 P3 — 工位健康长静止未实现语音

`main.cpp:472` `// TODO: 语音播报 "起来走走呀～"`，只忙等跳过。

### 4.12 P3 — SU03T / BLE 配网半成品

- `pollSU03T` 在 `loop` 中注释掉；唤醒靠小程序 `talk`
- `initBLE` 注释，原因写「Bluedroid+WiFi 内存冲突」——与 §1.2 同根
- 小程序侧 BLE UI 已写，固件侧不广播，**两侧不对称**

---

## 5. 隐藏 Bug（容易被当成「玄学」）

### 5.1 P0 — 小程序 `device_online` 无超时（跨仓库）

`app.js`：

```js
g.device_online = data.online === true;
g._lastStatusTs = Date.now();
```

**从未**用 `_lastStatusTs` 做失效判断。设备拔电后 UI 仍可能长期显示「设备在线」。

### 5.2 P1 — LLM「多轮对话记忆」是死代码

- `GatewayClient::addToHistory` **全工程零调用**
- `chatCompletion` 只读 `history_`，从不写入
- 技术文档写「对话记忆可注入巡检告警历史」——**告警历史**是拼进 prompt 的（真），**多轮 role 历史**是假的

```698:706:src/gateway_client.cpp
void GatewayClient::addToHistory(...) { ... }  // 无调用方
```

### 5.3 P1 — FSM 与真实执行流脱节

`feed()` 只做状态名迁移；**真正干活**的是同步函数：

| 文档声称 | 代码实际 |
|---|---|
| Capturing→Uploading→Analyzing 事件链 | `doMonitoring` 直接 `snapAndUpload` + `analyzeWithDoubao`，只喂了 `FRAME_DIFF_HIGH` 和 `ANALYZE_DONE` |
| Listening→Thinking→Speaking | `runConversationOnce` 同步跑完，几乎不喂 ASR_DONE/LLM_DONE/TTS_DONE |
| P0 打断 P1/P2 | `WAKE_WORD` 时若在 Alerting 会拒绝；但 `demo`/`talk` 不走完整优先级仲裁 |

FSM 目前更像**状态指示器**，不是调度器。改架构时不要假设「FSM 会保护你」。

### 5.4 P1 — BLE UUID 比较方式脆弱

```cpp
c->getUUID().toString() == BLE_CHAR_SSID_UUID
```

Bluedroid UUID 字符串格式（大小写、是否带短横）常与宏不一致 → 配网写特征值「看起来成功实则未保存」。启用 BLE 前必须实测。

### 5.5 P1 — 控制命令无鉴权、无去重、无速率限制

`device/control` 只要能连上 EMQX 就能：

- `ota:<任意URL>`
- `capture` / `demo`（触发拍照+外呼）
- `wifi:ssid|pwd`
- `voice/speed/volume`

无 `msg_id` 去重 → 重连重放会重复执行；无频率限制 → 可刷爆 TOS/Token。

### 5.6 P2 — `chatClient_` 复用策略不稳

注释写 30s，代码是 `millis() - chatClientMs_ > 120000`（120s）；且不检查 `connected()`（注释说 `http.end()` 会关 socket——与 `setReuse(true)` 的行为依赖框架版本）。弱网下可能静默失败一次再重建。

### 5.7 P2 — WiFi 重连无指数退避

`checkWiFi` 固定 3s `reconnect()`；与 MQTT 重连 2s（小程序）叠加，AP 风暴时行为差。

### 5.8 P2 — 每日告警计数与小程序「今日告警」语义

- 设备：`g_alertLevel2` 每日 NTP/yday 重置，仅 Level2
- 小程序：曾踩坑改用 `alerts.length`（本地缓存 50 条）
- 两边「今日」定义仍可能不一致（跨天未清 Storage）

### 5.9 P2 — `alert_ack` 只 feed FSM

设备侧不删除告警状态、不减计数、不通知 TTS；小程序本地 filter。多端不一致。

### 5.10 P2 — `DEMO_MODE` 常开

冷却 30s、部分行为偏演示；忘关会改变真实告警节奏。

### 5.11 P3 — 凭据与占位符

`YOUR_WIFI_*` / `YOUR_VOLCANO_API_KEY` / `YOUR_EMQX_*` 仍在源码约定位置。仓库已有脱敏提交，**若历史 commit 含真 Key，应轮换**（见 git log「脱敏」相关提交）。

### 5.12 P3 — 小程序 `utils/mqtt_files/` 像误提交资源

与 MQTT 无关的 css/图片，污染仓库体积。

### 5.13 P3 — `pages/logs` 脚手架残留

未接业务。

### 5.14 P2 — 状态上报粒度粗

- `publishStatusIfNeeded` 固定 30s，**FSM 变化不即时广播**（仅少数路径手动 publish）
- 小程序 1s 轮询 `globalData`，事件驱动本可 0 延迟

### 5.15 P2 — 连续对话与巡检互斥是「软开关」

`g_continuousTalk` / `g_voiceActive` 布尔量；`talk` 切换时若正处在 `doMonitoring` 中段，行为依赖调用时机。无会话 ID、无超时强退（仅 ASR 连败）。

---

## 6. 性能与体验（非崩溃，但伤「工程完成度」）

| # | 项 | 现状 | 打磨建议 |
|---|---|---|---|
| 6.1 | 全链路延迟 | ~12s | 拆 ASR/LLM/TTS 时间戳日志；LLM 换更低延迟模型；TTS 流式播放已部分做了 |
| 6.2 | 抓拍体验 | 拍完整机重启 | 去掉 restart，改 reinit 策略 |
| 6.3 | 照片 URL | 预签名过期后小程序图挂 | 设备侧上传成功后再推 URL；失败重签 |
| 6.4 | 告警分级 | 主要靠本地帧差 + 字符串 | 让豆包返回 JSON（severity/event_type） |
| 6.5 | 工位模式 | 久坐无语音 | 补 Level1 播报 |
| 6.6 | 小程序在线状态 | 无超时 | `_lastStatusTs > 90s` 显示离线 |
| 6.7 | OTA 反馈 | 无进度 | MQTT 上报 percent/result |
| 6.8 | 功耗 | ~500mA | 未做休眠；场景可谈「常电」 |

---

## 7. OTA：算真 OTA 吗？差距表

### 7.1 结论

**机制上是真 OTA**：MQTT `ota:<url>` → HTTPS 下载 → Arduino `Update` 写非活动分区 → `Update.end()` 切启动分区 → 重启。  
`default_16MB.csv` 常规为 `app0`/`app1` 双 OTA 分区，满足「远程换固件」定义。

**工程上是演示级 OTA**，不是产品级。

### 7.2 P0 — 升级期间 WDT 全摘

```cpp
esp_task_wdt_delete(NULL);
// ... 下载+写Flash ...
// 各失败路径 esp_task_wdt_add(NULL)；成功则 restart
```

网络卡死 → 无看门狗 → 只能断电。

### 7.3 安全差距

| 能力 | 现状 | 风险 |
|---|---|---|
| 传输 | HTTPS + `setInsecure()` | 可 MITM |
| 固件签名 | 无 | 任意镜像可刷入 |
| 版本/防降级 | 无 | 可被刷旧版 |
| URL 白名单 | 无 | 任意 HTTP(S) 源 |
| MQTT 控制鉴权 | 仅 EMQX 账密 | 账密泄露 = 完全控制 |

### 7.4 可靠性差距

| 能力 | 现状 |
|---|---|
| Content-Length 与实收一致性 | `written != len` 会失败，但不 `Update.abort()` 统一清理 |
| MD5/CRC | 无 |
| 进度回传小程序 | 仅开始 `OTA_UPDATING` |
| 成功/失败终态 | 成功直接 restart；失败只串口 |
| 升级前释放 MQTT/相机/TLS | 无，峰值内存最差 |
| 回滚 / 首次启动健康检查 | 无（Arduino 默认通常不启用 rollback） |
| 断点续传 | 无 |
| 双 App 分区实际校验 | 依赖 `default_16MB.csv`；本机未在仓库内嵌自定义表，换 platform 版本时应再确认 |

### 7.5 OTA 与「重启」的交叉

- OTA 失败若写坏分区状态 + 无回滚 → 启动循环
- OTA 成功但新固件栈问题依旧 → 仍 WDT/canary（OTA 不解决 §1/§2）

---

## 8. 修复难度评估与会踩的坑

### 8.1 好不好修？（总评）

| 类别 | 难度 | 工作量感 | 说明 |
|---|---|---|---|
| 参数类（WDT 时长、MQTT 包、冷却） | 低 | 数小时 | 已做过一轮，收益边际递减 |
| 在线超时、image_ref 时间戳、工位 TTS、去掉抓拍 restart | 低 | 1~2 天 | 风险小，立刻改善观感 |
| TOS 动态签名 / 上传不拷 `String` | 中 | 2~4 天 | 要设计密钥存放与轮转 |
| 喂狗补全、TLS 会话收敛、PSRAM 缓冲 | 中 | 3~7 天 | 需长时间 soak 测试 |
| 云 I/O 出主循环（真异步） | **高** | 1~3 周 | 触及架构；FSM 要真做或拆掉 |
| 相机 init 策略重构 | **高** | 1~2 周 | 与硬件强相关，可能仍要改板/排线 |
| 产品级 OTA（签名+回滚+进度） | 中高 | 1~2 周 | 需签名链与分区表吃透 |
| BLE 配网 + WiFi 共存 | **高** | 不定 | 内存冲突；可能要关摄像头再配网 |

**一句话**：  
「表层参数」好修且你们已经修过；「根因」集中在 **单线程阻塞架构 + 内部 SRAM 预算 + 相机物理层**——这三块不好修，返工像小型重构，不是改几行。

### 8.2 预估会遇到的问题

1. **一改异步就回归**：MQTT 在 TTS 期间仍会断，需要任务优先级 + loop 心跳分离  
2. **PSRAM 分配路径**：Arduino 默认堆策略、DMA 缓冲是否允许 PSRAM、相机 fb 位置，要实测不能假设  
3. **TLS 并发数**：mbedTLS 动态缓冲已开，仍可能 `mbedtls_ssl_handshake` 失败，需要「一时刻一握手」策略  
4. **去掉抓拍 restart 后 init 又死等**：要用状态机化 camera + 硬复位保留，而不是整机重启  
5. **TOS 服务端密钥**：设备持 AK/SK 不安全；预签名必须上云生成或用 STS  
6. **OTA 签名**：Arduino ESP32 的 secure boot / signed app 与 PlatformIO 流程要对齐，首次配容易把自己锁死  
7. **测试不可少**：没有 24~72h soak + 弱网（丢包/高延迟）矩阵，改完仍会在答辩现场炸  
8. **双仓库契约**：小程序与固件字段改动必须同步，建议先定 OpenAPI/JSON Schema  
9. **回归基线**：用 tag `v1.0-嵌赛完成版` 做对照，每项修复单独分支 + 可回滚

---

## 9. 若打磨为明年嵌赛作品：最该做什么

按「评委感知 × 工程可信度 × 你已有基础」排序。

### 9.1 第一优先级（先做，决定能不能稳定演示）

1. **稳定性地基（P0）**  
   - 消掉 WDT 主因：长 I/O 分段喂狗 + 禁止无限 `portMAX_DELAY`  
   - TOS 签名轮转（或后端签名 API）  
   - 抓拍不再整机重启  
   - 小程序设备离线超时 + MQTT LWT  
   - 24h soak 报告（重启次数、free heap 曲线）

2. **OTA 做到「可信」**  
   - 进度/结果 MQTT 上报  
   - 完整性校验 + 失败 abort  
   - 升级前释放相机/语音  
   - 有条件再加应用签名校验  

3. **密钥与仓库卫生**  
   - 全历史轮换 EMQX/火山/TOS Key  
   - 占位符 + `secrets` 不入库；README 写清配置步骤  

### 9.2 第二优先级（决定「创新点」是否站得住）

4. **把 FSM 做实，或勇敢删掉假 FSM**  
   - 评委若读代码，「12 状态优先级仲裁」必须与执行流一致  
   - 推荐：`loop` 只 `feed`；工作线程执行 Capturing/Uploading/ASR/TTS  

5. **云边协同数据闭环更硬**  
   - 本地：用上 `lowerHalfScore` 做跌倒倾向，而非只 `diffScore`  
   - 云端：豆包返回结构化 JSON（event_type/severity/confidence/summary）  
   - 端上统计：误报率、延迟分解、上传字节省了多少（量化「非必要不上传」）  

6. **双模态做成真产品语义**  
   - 工位久坐 TTS、离岗检测、静音时段  
   - 场景参数进 NVS，可小程序调  

### 9.3 第三优先级（拉开和「玩具项目」差距）

7. **可靠性叙事**：断网降级可演示（本地告警 LED + 恢复后补传）  
8. **安全叙事**：TLS 校验证书（或至少固定公钥）、control 命令 token、OTA 签名  
9. **可维护性**：`main.cpp` 继续拆；配置单点（`app_config` + NVS）；协议文档与代码同源  
10. **演示工程化**：一键 demo 不依赖手贴 TOS URL；预置「有剧本」的异常视频/动作  

### 9.4 不建议优先投入

- 再堆更多 UI 瓦片  
- 未调通的 BLE 全功能配网（先保证 WiFi 稳定）  
- 多设备 Mesh（除非稳定性已过关）  
- 盲目上更重的端侧模型（先解决 1.2 内存与延迟）

---

## 10. 建议的修复路线图（可当 issue 列表）

### Phase A — 止血（1 周内，低风险）

- [ ] A1 小程序：`device_online` 超时（建议 90~120s）
- [ ] A2 固件：MQTT LWT + `online` 真实化
- [ ] A3 `publishImageRef` 用 NTP 时间
- [ ] A4 工位模式长静止 TTS 播报
- [ ] A5 抓拍路径去掉 `ESP.restart()`（先保留失败硬复位）
- [ ] A6 TOS 上传避免整图 `String` 拷贝
- [ ] A7 告警 JSON 超限截断 `analysis`
- [ ] A8 关闭默认 `DEMO_MODE` 或编译开关分离

### Phase B — 稳定性（2~4 周）

- [ ] B1 全阻塞路径喂狗审计表（函数级 checklist）
- [ ] B2 `playPcm` 有界 timeout，去掉无限 `portMAX_DELAY`
- [ ] B3 大缓冲迁 PSRAM + 启动打印内外部堆
- [ ] B4 TTS/ASR/LLM 收敛为单会话槽位
- [ ] B5 TOS 预签名自动刷新或后端签发
- [ ] B6 控制命令 `msg_id` 去重 + 简单频率限制
- [ ] B7 72h soak 脚本与日志分析

### Phase C — 架构诚实度（1~2 月）

- [ ] C1 FSM 真调度或文档降级为「状态上报」
- [ ] C2 实现 `addToHistory` 或删除假 API
- [ ] C3 豆包结构化输出接入告警分级
- [ ] C4 OTA 进度/结果/abort/资源释放
- [ ] C5 （可选）应用签名校验
- [ ] C6 协议 JSON Schema + 双端生成/校验

### Phase D — 嵌赛叙事打磨

- [ ] D1 量化指标页：延迟分解、误报、带宽/Token 节省
- [ ] D2 断网/弱网演示剧本
- [ ] D3 技术文档补图（框图/原理图/PCB/截图）
- [ ] D4 演示视频与一键复现步骤

---

## 11. 参考：关键代码锚点

| 主题 | 位置 |
|---|---|
| cam_task wrap | `src/main.cpp:160-171` |
| 抓拍后重启 | `src/main.cpp:204-206` |
| 控制命令分发 | `src/main.cpp:177` |
| 巡检告警流水线 | `src/main.cpp:426` |
| WDT 初始化 | `src/main.cpp:700-701` |
| OTA | `src/main.cpp:596-649` |
| MQTT 连接/发布 | `src/mqtt_client.cpp` |
| TOS PUT | `src/tos_upload.cpp:29-53` |
| 豆包读图 | `src/camera_setup.cpp:21-53` |
| 相机 JPEG 抓拍 | `src/camera_setup.cpp:106-160` |
| 帧差双模态 | `src/vision/frame_diff.cpp` |
| FSM 迁移表 | `src/core/fsm.cpp` |
| 未调用的 addToHistory | `src/gateway_client.cpp:698` |
| TTS 阻塞播放 | `src/gateway_client.cpp:430+` / `src/audio_i2s.cpp:121` |
| 小程序 MQTT | `wx_miniprogram_project1/app.js`（对照仓库） |
| 接口规范 | `test/md合集/微信小程序MQTT接口规范.md` |
| 踩坑全记录 | `修改日志与踩坑记录.md` |

---

## 12. 总结（给决策用）

1. **栈溢出**：`cam_task` 是硬件+预编译库问题；主任务是**多 TLS + 阻塞云 I/O + 堆挤内部 RAM** 的结构性问题。  
2. **看门狗**：30s 是绷带；根因是单线程把「拍照-上传-分析-播报」串成一条可能 >30s 的链，且中途不喂狗。  
3. **OTA**：真能远程换固件，但是**演示级**——无签名、无回滚、无进度、WDT 摘除。  
4. **隐藏雷**：TOS URL 24h 过期、小程序假在线、LLM 多轮记忆死代码、MQTT 实际 QoS0、FSM 未真正调度、抓拍主动重启。  
5. **明年前打磨**：先 Phase A/B 把「能稳定跑满一天、断网可恢复、OTA 可反馈」做实，再谈 FSM 真调度与安全 OTA；创新点用**可测量的云边协同数据**说话，而不是再加功能面。

---

*本文档为审阅产物，不替代运行日志与硬件复测。修复时请逐项开 issue，并用 soak 数据关闭。*

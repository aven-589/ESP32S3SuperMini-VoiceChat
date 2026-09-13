# ESP32 AI 语音对话助手

基于 ESP32-S3 的语音对话系统：**说话 → 云端识别 → 大模型思考 → 语音合成 → 喇叭回答**。

支持两种形态：

| 形态 | 说明 | 依赖 |
|---|---|---|
| **全WiFi版**（推荐） | ESP32 独立联网，直连云端，插个充电头就能用 | 不需要电脑 |
| 串口版 | ESP32 通过 USB 连电脑，PC 跑全流程 | 电脑 |

---

## 功能特性

- **全WiFi独立运行**：ESP32 自己连网，脱离电脑
- **本地能量VAD**：说完停顿自动断句，无需按键
- **半双工防自问自答**：听时不播、播时不听，回答不会被麦克风二次拾取
- **TTS流式播放**：环形缓冲边下边播，DMA加大抗网络抖动，无断音无爆音
- **支持PSRAM**：录音缓冲放外部PSRAM，最长可录20秒
- **一个Key搞定**：云端识别/大模型/语音合成全部用硅基流动，国内直连
- **串口模式备用**：PC 可选用本地 faster-whisper 识别，离线也能跑

---

## 硬件清单

| 器件 | 型号 | 备注 |
|---|---|---|
| 主控 | ESP32-S3 SuperMini | 实测 4MB Flash + 2MB PSRAM（商品页虚标情况见FAQ） |
| 麦克风 | INMP441 | I2S 数字麦 |
| 功放 | MAX98357A | I2S 功放，直推喇叭 |
| 喇叭 | 8Ω 1~3W | 建议 8Ω，音量中档 |

### 接线

```
INMP441 麦克风:
  VDD → 3.3V        GND → GND
  SD  → GPIO6       WS  → GPIO5
  SCK → GPIO4       L/R → GND

MAX98357A 功放:
  VIN → 5V          GND → GND
  DIN → GPIO7       BCLK → GPIO4（与麦 SCK 并联）
  LRC → GPIO5（与麦 WS 并联）
  SD  → GPIO8       GAIN → GND（最小增益）

喇叭 → 功放输出 + / -
```

> 供电：USB 3.0 口或 5V/2A 充电头。功放务必接 5V 直供，不要接 3.3V。
> 喇叭与麦克风保持 30cm 以上距离，避免啸叫。

---

## 系统架构

### 全WiFi版（脱离电脑）

```
说话 ─→ INMP441 ─→ ESP32本地VAD切句 ─→ WiFi
                                        │
                    ┌───────────────────┼───────────────────┐
                    ↓                   ↓                   ↓
              云STT识别            DeepSeek-V3          CosyVoice2
           (TeleSpeechASR)         生成回复              语音合成
                    └───────────────────┼───────────────────┘
                                        ↓
                          PCM流式下载 → 环形缓冲 → I2S → 喇叭
```

### 串口版（PC辅助）

```
ESP32录音 ──USB串口──→ PC(python) ──→ 云STT → 大模型 → TTS合成
   ↑                                                    │
   └──────────── USB串口传PCM ──────────────────────────┘
```

---

## 目录结构

```
esp32_voice_loop/
├── esp32_voice_wifi/esp32_voice_wifi.ino   全WiFi版固件（独立运行）★
├── esp32_voice/esp32_voice.ino             串口版固件（接电脑）
├── pc_voice_assistant.py                   串口版全链路脚本（STT→LLM→喇叭）
├── stt_client.py                           云STT客户端（硅基流动）
├── llm_client.py                           大模型客户端（多平台预设）
├── pc_stt_cloud_test.py                    云STT连通性测试（不接硬件）
├── pc_llm_test.py                          大模型连通性测试（不接硬件）
├── stt_key.txt                             硅基流动 API Key（自己填，勿提交）*
├── stt_key.example.txt                     API Key 存放说明
├── test_zh.wav                             测试音频
├── 使用说明.txt                             详细操作手册（含排错）
├── README.md                               本文件
└── .gitignore
```

> \* `stt_key.txt` 已在 `.gitignore` 中忽略，克隆仓库后请自行创建并填入 Key。

---

## 快速开始（全WiFi版）

### 0. 前置要求

- Arduino IDE + esp32 开发板支持（3.3.x）
- 硅基流动 API Key：[cloud.siliconflow.cn](https://cloud.siliconflow.cn) 注册申请（个人使用免费额度足够）

### 1. Arduino IDE 设置

开发板选 **ESP32S3 Dev Module**，工具菜单：

| 设置项 | 值 |
|---|---|
| USB CDC On Boot | **Enabled** |
| PSRAM | **QSPI PSRAM** |
| Flash Size | **4MB (32Mb)** |
| Partition Scheme | Default 4MB with spiffs |

> 注意：若板子商品页标注"16MB Flash"多为虚标，按实测 4MB 设置；设错会导致无法启动。

### 2. 填写配置

打开 `esp32_voice_wifi/esp32_voice_wifi.ino`，改开头部分：

```cpp
const char* WIFI_SSID = "你的WiFi名";     // 必须是2.4G网络
const char* WIFI_PASS = "你的WiFi密码";
const char* API_KEY   = "你在硅基流动或者其他平台申请的API Key";
```

### 3. 烧录与使用

1. 上传固件
2. 可直接拔下插 5V/2A 充电头独立运行；插电脑时开串口监视器（115200）看日志
3. 等日志出现 `[就绪]` 后，对麦克风说一句话，停顿约1秒
4. 自动完成：识别 → 回答 → 喇叭念出来

---

## 串口版（PC辅助）

1. 烧录 `esp32_voice/esp32_voice.ino`（IDE 设置同上）
2. 电脑安装依赖：

```bash
pip install pyserial numpy edge-tts av sounddevice
# 可选：本地识别 pip install faster-whisper
```

3. 把 API Key 写入 `stt_key.txt`（整行只放 Key）
4. 运行：

```bash
# 全链路（麦→识别→大模型→喇叭）
python pc_voice_assistant.py --port COM23

# 文字测试（不接麦）
python pc_voice_assistant.py --say "你好" --port COM23

# 打字聊天
python pc_voice_assistant.py --text-chat --port COM23
```

PC 端大模型默认走 opencode-go 订阅，也可用 `--provider deepseek` 等切换到其他平台（见 `llm_client.py` 预设）。

---

## 配置项说明

| 配置 | 位置 | 说明 |
|---|---|---|
| WiFi 账号密码 | 固件开头 | 必须 2.4G 网络 |
| API Key | 固件开头 / `stt_key.txt` | 硅基流动一个 Key 通用 |
| 音色 | `TTS_VOICE` | 可选 `claire`(温柔女) `anna`(稳重女) `bella`(热情女) `diana`(活泼女) `alex`(稳重男) 等 |
| 语音门限 | `VAD_THRESH` | 误触发调高、识别不到调低 |
| 最长录音 | 见 `allocRecBuf()` | PSRAM 下约20秒 |

---

## 工作原理（关键实现）

- **半双工**：听的时候喇叭不出声，放音时停止采集麦克风，放完等1秒再听 → 不会自己听自己
- **VAD断句**：连续4帧能量超门限触发录音，静音0.7秒判停
- **TTS流式播放**：下载任务写环形缓冲，播放循环边取边放；512字节对齐避免奇数字节错位产生的噪音
- **内存策略**：录音缓冲（PSRAM，最长20秒）与播放缓冲分离；内部 SRAM 留给 WiFi/TLS
- **I2S**：麦克风 32bit 立体声读取、扬声器 32bit 双声道输出，16kHz 采样率

---

## 常见问题

| 现象 | 排查 |
|---|---|
| 板子无响应/串口无输出 | USB CDC On Boot 是否为 Enabled |
| 启动崩溃报 flash 错误 | Flash Size 误设16MB，改回 4MB |
| 日志显示 `[内存] 无PSRAM` | PSRAM 设为 QSPI PSRAM 后重新烧录 |
| 播放有噪音/咔咔声 | 确认使用最新固件（字节对齐修复）；舵机等大电流负载与音频电源分开 |
| 识别不到/误触发 | 调整 `VAD_THRESH`（安静rms与说话rms之间） |
| WiFi 连不上 | 必须 2.4G 网络，ESP32 不支持 5G |
| TTS 失败 | 看日志 HTTP 状态码：401=Key问题，429=限流 |

---

## Roadmap

- [x] 全WiFi独立运行
- [x] TTS流式播放（环形缓冲抗抖动）
- [ ] 流式语音识别（边说边出字）
- [ ] 大模型流式输出 + 按句朗读（开口更快）
- [ ] 唤醒词
- [ ] 云台人脸跟踪联动（视觉模块串口 + 舵机）

---

## License

MIT

---

## 说明

- 请勿将 `stt_key.txt` 或固件中的 API Key 提交到公开仓库（`.gitignore` 已处理）
- 本项目仅供学习交流

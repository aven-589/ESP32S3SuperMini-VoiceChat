/*
  ESP32-S3 SuperMini 全WiFi语音助手 (不依赖电脑)
  麦(INMP441) -> 云STT -> 云大模型 -> 云TTS -> 喇叭(MAX98357A)

  ============ 第一次用改这里 ============
  1. WiFi 必须是 2.4G 网络 (ESP32不支持5G)
  2. 把下面 API_KEY 换成你在硅基流动(cloud.siliconflow.cn)申请的Key
     (同一个key做 STT+大模型+TTS)
  ========================================

  接线 (和串口版一样):
  INMP441  VDD -> 3.3V, GND -> GND, SD -> GPIO6, WS -> GPIO5, SCK -> GPIO4, L/R -> GND
  MAX98357 VIN -> 5V, GND -> GND, DIN -> GPIO7, BCLK -> GPIO4, LRC -> GPIO5, SD -> GPIO8, GAIN -> GND

  流程: 听(本地VAD) -> 录一句 -> 云STT -> DeepSeek-V3 -> 云TTS(pcm) -> 喇叭放
  串口921600只用于打日志(可选, 不插电脑也能跑)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "driver/i2s_std.h"
#include "freertos/ringbuf.h"

// ================= 配置 =================
const char* WIFI_SSID = "你的WiFi名";     // 必须是2.4G网络
const char* WIFI_PASS = "你的WiFi密码";
// ↓↓↓ 换成你在硅基流动或其他平台申请的API Key ↓↓↓
const char* API_KEY   = "你在硅基流动或者其他平台申请的API Key";

const char* HOST = "api.siliconflow.cn";
const char* STT_PATH = "/v1/audio/transcriptions";
const char* LLM_PATH = "/v1/chat/completions";
const char* TTS_PATH = "/v1/audio/speech";

const char* STT_MODEL = "TeleAI/TeleSpeechASR";
const char* LLM_MODEL = "deepseek-ai/DeepSeek-V3";
const char* TTS_MODEL = "FunAudioLLM/CosyVoice2-0.5B";
// 音色可选: claire(温柔女) anna(稳重女) bella(热情女) diana(活泼女) alex(稳重男) benjamin...等
const char* TTS_VOICE = "FunAudioLLM/CosyVoice2-0.5B:claire";

const char* SYS_PROMPT = "你是一个语音助手，回答要口语化、简短，一般1到3句话，不超过80字，不要用markdown、列表、代码块或表情符号。";

// ================= 引脚/参数 =================
#define I2S_BCLK 4
#define I2S_WS   5
#define I2S_DIN  6
#define I2S_DOUT 7
#define SD_PIN   8

#define SAMPLE_RATE 16000
#define VAD_THRESH 700          // 能量门限; 误触发多调高(如1200), 说话识别不到调低(如400)
#define TRIG_FRAMES 4           // 连续超门限几帧才算开口(防噪音误触发)
#define FRAME_SAMPLES 256       // 16ms一帧
#define PRE_ROLL_FRAMES 16      // 预卷0.26秒(开口第一个字不丢)
#define MIN_SPEECH_FRAMES 30    // 至少0.48秒才算一句
#define SIL_FRAMES 44           // 静音0.7秒算说完
#define MAX_REC_SECONDS 8       // 单句最长8秒

i2s_chan_handle_t tx_handle = nullptr;
i2s_chan_handle_t rx_handle = nullptr;

static int32_t raw_buf[512];            // RX: 256帧 stereo
static int16_t frame_buf[FRAME_SAMPLES];
static int16_t ring_buf[PRE_ROLL_FRAMES * FRAME_SAMPLES];
static int32_t spk_buf[512 * 2];        // 播放转换缓冲

int16_t* rec_buf = nullptr;             // 录音大缓冲(动态分配)
size_t rec_cap_samples = 0;             // 最多能录多少样点

static int32_t g_last_x = 0, g_last_y = 0;  // 去直流滤波状态

WiFiClientSecure client;

// ------------- 工具 -------------
static void logf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

static inline int16_t mic_to_pcm16(int32_t s) {
  int32_t y = s - g_last_x + (int32_t)(0.995f * g_last_y);
  g_last_x = s;
  g_last_y = y;
  int32_t v = y >> 16;
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return (int16_t)v;
}

static float rms_of(const int16_t* x, int n) {
  double acc = 0;
  for (int i = 0; i < n; i++) acc += (double)x[i] * x[i];
  return (float)sqrt(acc / n);
}

static String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 16);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:   o += c;
    }
  }
  return o;
}

// 从JSON里取 "key":"..." 并解转义 (够用够稳)
static String jsonGetString(const String& src, const char* key) {
  String pat = String("\"") + key + "\":\"";
  int i = src.indexOf(pat);
  if (i < 0) return "";
  i += pat.length();
  String out;
  out.reserve(64);
  while (i < (int)src.length()) {
    char c = src.charAt(i++);
    if (c == '\\') {
      char e = src.charAt(i++);
      if (e == 'n') out += '\n';
      else if (e == 'r') out += '\r';
      else if (e == 't') out += '\t';
      else if (e == '"') out += '"';
      else if (e == '\\') out += '\\';
      else if (e == '/') out += '/';
      else if (e == 'u') {
        String hx = src.substring(i, i + 4);
        i += 4;
        uint32_t cp = strtoul(hx.c_str(), NULL, 16);
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
          out += (char)(0xC0 | (cp >> 6));
          out += (char)(0x80 | (cp & 0x3F));
        } else {
          out += (char)(0xE0 | (cp >> 12));
          out += (char)(0x80 | ((cp >> 6) & 0x3F));
          out += (char)(0x80 | (cp & 0x3F));
        }
      } else out += e;
    } else if (c == '"') break;
    else out += c;
  }
  return out;
}

static void buildWavHeader(uint8_t* h, uint32_t dataLen) {
  uint32_t chunk = dataLen + 36;
  uint32_t sr = SAMPLE_RATE, brate = SAMPLE_RATE * 2;
  uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
  uint32_t sub1 = 16;
  memcpy(h, "RIFF", 4); memcpy(h + 4, &chunk, 4); memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); memcpy(h + 16, &sub1, 4);
  memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &sr, 4); memcpy(h + 28, &brate, 4);
  memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &dataLen, 4);
}

// ------------- I2S -------------
static void setupI2S() {
  pinMode(SD_PIN, OUTPUT);
  digitalWrite(SD_PIN, LOW);

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  chan_cfg.dma_desc_num = 10;    // 加大DMA缓冲(防网络抖动断音), 默认6
  chan_cfg.dma_frame_num = 512;  // 默认240; 约320ms抗抖动
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_BCLK,
      .ws = (gpio_num_t)I2S_WS,
      .dout = (gpio_num_t)I2S_DOUT,
      .din = (gpio_num_t)I2S_DIN,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));

  // 时钟稳定前丢几包
  size_t dummy = 0;
  for (int k = 0; k < 10; k++) {
    i2s_channel_read(rx_handle, raw_buf, sizeof(raw_buf), &dummy, 200);
  }
  digitalWrite(SD_PIN, LOW); // 听的时候功放关着, 减少干扰
}

// 读一帧(256样点) 返回实际样点数
static int micReadFrame(int16_t* out, int n) {
  size_t bytes_read = 0;
  esp_err_t ret = i2s_channel_read(rx_handle, raw_buf, sizeof(raw_buf), &bytes_read, 1000);
  if (ret != ESP_OK || bytes_read == 0) return 0;
  int frames = bytes_read / (sizeof(int32_t) * 2);
  if (frames > n) frames = n;
  for (int f = 0; f < frames; f++) {
    out[f] = mic_to_pcm16(raw_buf[f * 2]);
  }
  return frames;
}

// PCM16 -> I2S (32bit stereo)
static void playPcm16(const uint8_t* data, size_t len) {
  size_t samples = len / 2;
  size_t i = 0;
  while (i < samples) {
    size_t n = samples - i;
    if (n > 512) n = 512;
    for (size_t k = 0; k < n; k++) {
      int16_t v;
      memcpy(&v, data + (i + k) * 2, 2);
      int32_t v32 = ((int32_t)v) << 16;
      spk_buf[k * 2] = v32;
      spk_buf[k * 2 + 1] = v32;
    }
    size_t bw = 0;
    i2s_channel_write(tx_handle, spk_buf, n * 2 * sizeof(int32_t), &bw, portMAX_DELAY);
    i += n;
  }
}

static void playSilenceMs(int ms) {
  static int32_t zbuf[256 * 2] = {0};
  int chunks = ms * 16 / 256;
  for (int i = 0; i <= chunks; i++) {
    size_t bw = 0;
    i2s_channel_write(tx_handle, zbuf, sizeof(zbuf), &bw, portMAX_DELAY);
  }
}

// ------------- WiFi -------------
static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  logf("[WiFi] 连接 %s ...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_11dBm);   // 降低发射峰值电流(之前电流账)
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    logf("\n[WiFi] 已连接, IP=%s", WiFi.localIP().toString().c_str());
  } else {
    logf("\n[WiFi] 连接失败, 10秒后重试 (SSID/密码/2.4G?)");
    delay(10000);
  }
}

// ------------- HTTP -------------
static bool apiConnect() {
  if (client.connected()) return true;
  client.setInsecure();          // 演示用: 跳过证书校验
  client.setTimeout(15000);
  logf("[HTTP] 连接 %s ...", HOST);
  if (!client.connect(HOST, 443)) {
    logf("[HTTP] TLS连接失败");
    return false;
  }
  return true;
}

static void sendHeaders(const char* path, size_t contentLen, const char* ctype) {
  client.print(String("POST ") + path + " HTTP/1.1\r\n");
  client.print(String("Host: ") + HOST + "\r\n");
  client.print(String("Authorization: Bearer ") + API_KEY + "\r\n");
  client.print(String("Content-Type: ") + ctype + "\r\n");
  client.print(String("Content-Length: ") + contentLen + "\r\n");
  client.print("User-Agent: esp32-voice-assistant/1.0\r\n");
  client.print("Connection: close\r\n\r\n");
}

static int readResponseHead(String& headers) {
  headers = "";
  String line = client.readStringUntil('\n');
  line.trim();
  int code = 0;
  int sp = line.indexOf(' ');
  if (sp > 0) code = line.substring(sp + 1).toInt();
  while (true) {
    line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    headers += line + "\n";
    if (headers.length() > 4096) break;
  }
  return code;
}

static bool isChunked(const String& headers) {
  String h = headers;
  h.toLowerCase();
  return h.indexOf("transfer-encoding: chunked") >= 0;
}

static long contentLenOf(const String& headers) {
  String h = headers;
  h.toLowerCase();
  int i = h.indexOf("content-length:");
  if (i < 0) return -1;
  return h.substring(i + 15).toInt();
}

// 读小body(STT/LLM响应用)
static void readBodyAll(String& out, bool chunked, long contentLen, size_t cap = 32768) {
  out = "";
  out.reserve(2048);
  uint8_t buf[512];
  if (chunked) {
    while (true) {
      String line = client.readStringUntil('\n');
      line.trim();
      long n = strtol(line.c_str(), NULL, 16);
      if (n <= 0) { client.readStringUntil('\n'); break; }
      size_t got = 0;
      while (got < (size_t)n) {
        size_t want = (size_t)n - got;
        if (want > sizeof(buf)) want = sizeof(buf);
        size_t rd = client.readBytes(buf, want);
        if (rd == 0) return;
        out.concat((const char*)buf, rd);
        got += rd;
        if (out.length() > cap) return;
      }
      client.readStringUntil('\n');
    }
  } else if (contentLen > 0) {
    size_t got = 0;
    while (got < (size_t)contentLen) {
      size_t want = (size_t)contentLen - got;
      if (want > sizeof(buf)) want = sizeof(buf);
      size_t rd = client.readBytes(buf, want);
      if (rd == 0) break;
      out.concat((const char*)buf, rd);
      got += rd;
      if (out.length() > cap) break;
    }
  } else {
    while (client.connected() || client.available()) {
      size_t rd = client.readBytes(buf, sizeof(buf));
      if (rd == 0) break;
      out.concat((const char*)buf, rd);
      if (out.length() > cap) break;
    }
  }
}

// 流式读body -> 环形缓冲 (TTS下载任务用; 播放由播放循环从环形缓冲取)
// 普通HAL读: 按毫秒超时等待, 返回0才表示真断
static size_t tlsReadSome(WiFiClientSecure& c, uint8_t* buf, size_t want, uint32_t timeout_ms) {
  uint32_t t0 = millis();
  size_t total = 0;
  while (total < want) {
    int avail = c.available();
    if (avail > 0) {
      int r = c.read(buf + total, want - total);
      if (r > 0) { total += r; t0 = millis(); continue; }
      if (r < 0) break;
    }
    if (millis() - t0 > timeout_ms) break;
    delay(5);
  }
  return total;
}

// ---------- TTS 后台下载任务 ----------
static RingbufHandle_t tts_rb = nullptr;
static volatile bool tts_done = false;
static volatile bool tts_ok = false;
static volatile size_t tts_total = 0;
static char tts_payload[768];
static volatile int tts_http_code = 0;

static void ttsDownloadTask(void* param) {
  WiFiClientSecure c;
  c.setInsecure();
  c.setTimeout(15000);
  bool ok = false;
  static uint8_t buf[2048];
  int code = 0;

  if (c.connect(HOST, 443)) {
    c.print(String("POST ") + TTS_PATH + " HTTP/1.1\r\n");
    c.print(String("Host: ") + HOST + "\r\n");
    c.print(String("Authorization: Bearer ") + API_KEY + "\r\n");
    c.print("Content-Type: application/json\r\n");
    c.print(String("Content-Length: ") + strlen(tts_payload) + "\r\n");
    c.print("User-Agent: esp32-voice-assistant/1.0\r\n");
    c.print("Connection: close\r\n\r\n");
    c.print(tts_payload);

    String line = c.readStringUntil('\n');
    line.trim();
    int sp = line.indexOf(' ');
    code = (sp > 0) ? line.substring(sp + 1).toInt() : 0;
    tts_http_code = code;
    String headers = "";
    while (true) {
      line = c.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) break;
      headers += line + "\n";
      if (headers.length() > 4096) break;
    }
    if (code == 200) {
      String h = headers;
      h.toLowerCase();
      bool chunked = h.indexOf("transfer-encoding: chunked") >= 0;
      long clen = -1;
      int ci = h.indexOf("content-length:");
      if (ci >= 0) clen = h.substring(ci + 15).toInt();

      ok = true;
      if (chunked) {
        while (true) {
          String cl = c.readStringUntil('\n');
          cl.trim();
          long n = strtol(cl.c_str(), NULL, 16);
          if (n <= 0) { c.readStringUntil('\n'); break; }
          size_t remaining = n;
          while (remaining > 0) {
            size_t want = remaining;
            if (want > sizeof(buf)) want = sizeof(buf);
            size_t rd = tlsReadSome(c, buf, want, 10000);
            if (rd == 0) { ok = false; break; }
            if (xRingbufferSend(tts_rb, buf, rd, pdMS_TO_TICKS(15000)) != pdTRUE) { ok = false; break; }
            tts_total += rd;
            remaining -= rd;
          }
          if (!ok) break;
          c.readStringUntil('\n');
        }
      } else if (clen > 0) {
        size_t remaining = clen;
        while (remaining > 0) {
          size_t want = remaining;
          if (want > sizeof(buf)) want = sizeof(buf);
          size_t rd = tlsReadSome(c, buf, want, 10000);
          if (rd == 0) { ok = false; break; }
          if (xRingbufferSend(tts_rb, buf, rd, pdMS_TO_TICKS(15000)) != pdTRUE) { ok = false; break; }
          tts_total += rd;
          remaining -= rd;
        }
      }
    }
    c.stop();
  }
  tts_ok = ok;
  tts_done = true;
  vTaskDelete(NULL);
}

// ------------- 三个云接口 -------------
static String cloudSTT(size_t samples) {
  if (!apiConnect()) return "";
  String boundary = "----esp32voice";
  String p1 = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" + String(STT_MODEL) + "\r\n";
  String p2 = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String p3 = "\r\n--" + boundary + "--\r\n";
  size_t pcmBytes = samples * 2;
  size_t total = p1.length() + p2.length() + 44 + pcmBytes + p3.length();

  sendHeaders(STT_PATH, total, (String("multipart/form-data; boundary=") + boundary).c_str());
  logf("[STT] 上传 %.1f秒音频(%u字节)...", samples / (float)SAMPLE_RATE, (unsigned)pcmBytes);
  client.print(p1);
  client.print(p2);
  uint8_t hdr[44];
  buildWavHeader(hdr, pcmBytes);
  client.write(hdr, 44);
  client.write((const uint8_t*)rec_buf, pcmBytes);
  client.print(p3);

  String headers;
  int code = readResponseHead(headers);
  String body;
  readBodyAll(body, isChunked(headers), contentLenOf(headers));
  client.stop();
  if (code != 200) {
    logf("[STT] HTTP %d: %s", code, body.substring(0, 120).c_str());
    return "";
  }
  String text = jsonGetString(body, "text");
  if (!text.length()) {
    logf("[STT] 返回内容: %s", body.substring(0, 160).c_str());
  }
  return text;
}

static String cloudLLM(const String& userText) {
  if (!apiConnect()) return "";
  String payload = "{\"model\":\"" + String(LLM_MODEL) + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" +
                   jsonEscape(SYS_PROMPT) + "\"},{\"role\":\"user\",\"content\":\"" + jsonEscape(userText) +
                   "\"}],\"max_tokens\":150,\"temperature\":0.8}";
  sendHeaders(LLM_PATH, payload.length(), "application/json");
  client.print(payload);
  String headers;
  int code = readResponseHead(headers);
  String body;
  readBodyAll(body, isChunked(headers), contentLenOf(headers));
  client.stop();
  if (code != 200) {
    logf("[LLM] HTTP %d: %s", code, body.substring(0, 120).c_str());
    return "";
  }
  return jsonGetString(body, "content");
}

static bool cloudTTS(const String& text) {
  String payload = "{\"model\":\"" + String(TTS_MODEL) + "\",\"input\":\"" + jsonEscape(text) +
                   "\",\"voice\":\"" + String(TTS_VOICE) +
                   "\",\"response_format\":\"pcm\",\"sample_rate\":16000,\"speed\":1.0}";
  if (payload.length() >= sizeof(tts_payload)) {
    logf("[TTS] 文本过长");
    return false;
  }
  strcpy(tts_payload, payload.c_str());

  // 环形缓冲: 下载任务往里灌, 播放循环从里取(解耦网络抖动)
  const size_t RB_SZ = 20 * 1024;
  if (tts_rb) { vRingbufferDelete(tts_rb); tts_rb = nullptr; }
  tts_rb = xRingbufferCreate(RB_SZ, RINGBUF_TYPE_BYTEBUF);
  if (!tts_rb) { logf("[TTS] 环形缓冲分配失败"); return false; }
  tts_done = false;
  tts_ok = false;
  tts_total = 0;
  tts_http_code = 0;

  if (xTaskCreatePinnedToCore(ttsDownloadTask, "tts_dl", 12288, NULL, 1, NULL, 1) != pdPASS) {
    logf("[TTS] 任务创建失败");
    vRingbufferDelete(tts_rb);
    tts_rb = nullptr;
    return false;
  }

  // 预缓冲: 攒够10KB(约0.3秒)再开口, 或下载已结束
  uint32_t t0 = millis();
  while (!tts_done) {
    if (RB_SZ - xRingbufferGetCurFreeSize(tts_rb) >= 10240) break;
    if (millis() - t0 > 15000) break;
    delay(10);
  }

  bool played_any = false;
  if (!tts_done || tts_ok) {
    digitalWrite(SD_PIN, HIGH);  // 开功放
    delay(30);
    static uint8_t acc[512];
    size_t acc_len = 0;
    uint32_t last_data = millis();
    while (true) {
      size_t sz = 0;
      void* item = xRingbufferReceive(tts_rb, &sz, pdMS_TO_TICKS(500));
      if (item) {
        last_data = millis();
        uint8_t* p = (uint8_t*)item;
        size_t off = 0;
        while (off < sz) {
          size_t take = sz - off;
          if (take > sizeof(acc) - acc_len) take = sizeof(acc) - acc_len;
          memcpy(acc + acc_len, p + off, take);
          acc_len += take;
          off += take;
          if (acc_len == sizeof(acc)) {
            playPcm16(acc, acc_len);  // 512字节整块=偶数对齐, 不会串位
            acc_len = 0;
            played_any = true;
          }
        }
        vRingbufferReturnItem(tts_rb, item);
      } else {
        if (tts_done) break;                       // 下载完且缓冲已取空
        if (millis() - last_data > 15000) break;   // 卡死保护
      }
    }
    if (acc_len >= 2) {
      playPcm16(acc, acc_len & ~(size_t)1);        // 收尾可能剩奇数, 丢掉尾字节
      played_any = true;
    }
    playSilenceMs(200);
    digitalWrite(SD_PIN, LOW);  // 关功放
  }

  logf("[TTS] %s(HTTP%d), 共%u字节(%.1f秒), 播放%s",
       tts_ok ? "下载完" : "中断", tts_http_code,
       (unsigned)tts_total, tts_total / 2 / (float)SAMPLE_RATE,
       played_any ? "完成" : "无");

  // 等任务退出再释放缓冲
  uint32_t t1 = millis();
  while (!tts_done && millis() - t1 < 5000) delay(10);
  vRingbufferDelete(tts_rb);
  tts_rb = nullptr;
  return played_any;
}

// ------------- 录音(VAD) -------------
// 听 -> 触发 -> 录 -> 静音结束, 返回录音样点数
static size_t recordWithVAD() {
  static int ringPos = 0;
  int ringCount = 0;
  size_t recLen = 0;
  int trig = 0, sil = 0;
  bool rec = false;
  unsigned long lastMeter = millis();
  float idleMax = 0;
  float recPeak = 0;

  while (true) {
    int n = micReadFrame(frame_buf, FRAME_SAMPLES);
    if (n <= 0) continue;
    float v = rms_of(frame_buf, n);

    if (!rec) {
      if (v > idleMax) idleMax = v;
      // 存预卷环
      memcpy(ring_buf + ringPos * FRAME_SAMPLES, frame_buf, n * 2);
      ringPos = (ringPos + 1) % PRE_ROLL_FRAMES;
      if (ringCount < PRE_ROLL_FRAMES) ringCount++;
      trig = (v > VAD_THRESH) ? trig + 1 : 0;
      if (trig >= TRIG_FRAMES) {
        // 把环里内容按时间顺序拷进录音缓冲
        recLen = 0;
        for (int i = 0; i < ringCount; i++) {
          int idx = (ringPos + PRE_ROLL_FRAMES - ringCount + i) % PRE_ROLL_FRAMES;
          size_t cnt = FRAME_SAMPLES;
          if (recLen + cnt > rec_cap_samples) cnt = rec_cap_samples - recLen;
          memcpy(rec_buf + recLen, ring_buf + idx * FRAME_SAMPLES, cnt * 2);
          recLen += cnt;
        }
        rec = true;
        trig = 0;
        sil = 0;
        recPeak = v;
        logf("[听] 检测到说话(rms=%.0f), 开始录音...", v);
      } else if (millis() - lastMeter > 3000) {
        logf("[电平] 安静rms~%.0f (thresh=%d)", idleMax, VAD_THRESH);
        idleMax = 0;
        lastMeter = millis();
      }
    } else {
      if (v > recPeak) recPeak = v;
      if (recLen + n <= rec_cap_samples) {
        memcpy(rec_buf + recLen, frame_buf, n * 2);
        recLen += n;
      }
      sil = (v > VAD_THRESH) ? 0 : sil + 1;
      if ((sil >= SIL_FRAMES && recLen >= MIN_SPEECH_FRAMES * FRAME_SAMPLES) ||
          recLen >= rec_cap_samples) {
        logf("[听] 录完 %.1f秒 peakrms=%.0f", recLen / (float)SAMPLE_RATE, recPeak);
        return recLen;
      }
    }
  }
}

// TTS前清一下文本
static String stripForTts(String t) {
  t.replace("*", "");
  t.replace("`", "");
  t.replace("#", "");
  t.replace("~", "");
  t.replace("\n", " ");
  t.replace("\r", " ");
  t.trim();
  if (t.length() > 150) t = t.substring(0, 150);
  return t;
}

// 录音缓冲按需分配(录之前要, 传给STT之后还)
static bool allocRecBuf() {
  if (rec_buf) return true;
  size_t wantSec = 0;
  if (psramFound()) {
    wantSec = 20;
    rec_buf = (int16_t*)ps_malloc(wantSec * SAMPLE_RATE * 2);
    if (rec_buf) logf("[内存] 有PSRAM, 可录%u秒", (unsigned)wantSec);
  }
  if (!rec_buf) {
    size_t freeH = ESP.getFreeHeap();
    size_t reserve = 80 * 1024;   // 给STT上传时的TLS/JSON留的余量
    size_t avail = (freeH > reserve + 48 * 1024) ? (freeH - reserve) : (48 * 1024);
    wantSec = avail / (SAMPLE_RATE * 2);
    if (wantSec > MAX_REC_SECONDS) wantSec = MAX_REC_SECONDS;
    if (wantSec < 2) wantSec = 2;
    while (wantSec >= 2) {
      rec_buf = (int16_t*)malloc(wantSec * SAMPLE_RATE * 2);
      if (rec_buf) break;
      wantSec--;
    }
    logf("[内存] 无PSRAM, 可录%u秒 (free heap %u)", (unsigned)wantSec, (unsigned)freeH);
  }
  if (!rec_buf) return false;
  rec_cap_samples = wantSec * SAMPLE_RATE;
  return true;
}

static void freeRecBuf() {
  if (rec_buf && !psramFound()) {
    free(rec_buf);
    rec_buf = nullptr;
    logf("[内存] 释放录音缓冲, free heap %u", (unsigned)ESP.getFreeHeap());
  }
}

// ------------- 主流程 -------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ESP32-S3 全WiFi语音助手 ===");

  setupI2S();
  connectWiFi();

  if (!allocRecBuf()) {
    logf("[错误] 分配录音缓冲失败, 重启");
    delay(3000);
    ESP.restart();
  }

  logf("[就绪] 对着麦说一句话, 停顿一下就回答你");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return;
  }

  if (!allocRecBuf()) {
    logf("[内存] 录音缓冲不足, 2秒后重试");
    delay(2000);
    return;
  }

  size_t samples = recordWithVAD();
  if (samples < MIN_SPEECH_FRAMES * FRAME_SAMPLES) return;

  unsigned long t0 = millis();
  String text = cloudSTT(samples);
  freeRecBuf();   // 录音数据已发完, 还内存给后面TTS
  if (!text.length()) {
    logf("[STT] 没识别到文字");
    return;
  }
  logf("[识别] %s (%.1fs)", text.c_str(), (millis() - t0) / 1000.0);

  t0 = millis();
  String reply = cloudLLM(text);
  if (!reply.length()) {
    logf("[LLM] 没有回复");
    return;
  }
  logf("[回复] %s (%.1fs)", reply.c_str(), (millis() - t0) / 1000.0);

  String ttsText = stripForTts(reply);
  if (ttsText.length()) {
    t0 = millis();
    bool ok = cloudTTS(ttsText);
    logf("[TTS] %s (%.1fs)", ok ? "播完" : "失败", (millis() - t0) / 1000.0);
  }

  delay(500); // 余音散掉再听
}

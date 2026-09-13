/*
  ESP32-S3 SuperMini 语音助手 二合一固件 (录音 + 播放)
  麦克风: INMP441   功放: MAX98357A
  烧录设置: 板子ESP32S3, 工具里 USB CDC On Boot 必须 = 启用

  接线:
  INMP441  VDD -> 3.3V, GND -> GND, SD -> GPIO6, WS -> GPIO5, SCK -> GPIO4, L/R -> GND
  MAX98357 VIN -> 5V, GND -> GND, DIN -> GPIO7, BCLK -> GPIO4(与麦SCK并联),
           LRC -> GPIO5(与麦WS并联), SD -> GPIO8, GAIN -> GND

  串口 921600, 兼容旧脚本 (pc_stt_demo / pc_stt_stream / pc_level_meter / pc_speak_clip)
  协议:
    R<秒>   录1~15秒, 回纯二进制 16k mono int16 LE, 完发 \nDONE len=xxx\n
    S       开始常开流(回 STREAM), 源源不断发PCM, 期间只认 Q
    Q       停止流(回 \nSTOP\n)
    A<len>  播放: 之后PC分块发 len 字节 16k mono int16 LE, 每块等1字节'K'流控, 完回 DONE
    M0/M1   功放硬静音(SD拉低) / 解除静音(SD拉高)
  说明: 听的时候不喂播放数据喇叭自然静音; 放之前固件自动解除M0静音, 不会因忘了M1而哑巴
*/

#include "driver/i2s_std.h"

#define I2S_BCLK 4
#define I2S_WS   5
#define I2S_DIN  6    // INMP441 SD
#define I2S_DOUT 7    // MAX98357 DIN
#define SD_PIN   8    // MAX98357 SD

#define SAMPLE_RATE 16000

i2s_chan_handle_t tx_handle = nullptr;
i2s_chan_handle_t rx_handle = nullptr;

static int32_t raw_buf[512];     // RX: 256帧 stereo (每帧2个int32)
static int16_t pcm_buf[256];     // 单声道输出
static uint8_t play_buf[4096];   // 播放: 收PC的16bit PCM
static int32_t tx_buf[4096];     // 播放: 转成 2048样点 x 2声道 的32bit

// 去直流滤波状态 (麦克风)
static int32_t g_last_x = 0, g_last_y = 0;

static inline int16_t mic_to_pcm16(int32_t s) {
  int32_t y = s - g_last_x + (int32_t)(0.995f * g_last_y);
  g_last_x = s;
  g_last_y = y;
  int32_t v = y >> 16;   // 32bit左对齐 -> 高16bit
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  return (int16_t)v;
}

void setup() {
  pinMode(SD_PIN, OUTPUT);
  digitalWrite(SD_PIN, LOW); // 上电先关功放防嗡嗡

  Serial.setRxBufferSize(8192);
  Serial.begin(921600);
  delay(500);
  Serial.println("\n=== ESP32-S3 语音助手固件 (录音R/S/Q + 播放A + 静音M) ===");
  Serial.printf("BCLK=%d WS=%d DIN=%d DOUT=%d SD=%d @%dHz\n",
                I2S_BCLK, I2S_WS, I2S_DIN, I2S_DOUT, SD_PIN, SAMPLE_RATE);

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

  // 32bit 立体声: 麦必须32bit读(24bit左对齐), 播放把16bit PCM <<16 放进左右两个槽
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

  delay(100);
  digitalWrite(SD_PIN, HIGH);
  Serial.println("READY voice fw. R<sec> | S/Q | A<len> | M0/M1");
}

// ---- 录音: 定长, 发完回DONE ----
void record_and_stream(int seconds) {
  const int total_samples = SAMPLE_RATE * seconds;
  int sent = 0;
  while (sent < total_samples) {
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_handle, raw_buf, sizeof(raw_buf), &bytes_read, 1000);
    if (ret != ESP_OK || bytes_read == 0) continue;
    int frames = bytes_read / (sizeof(int32_t) * 2);
    int out = 0;
    for (int f = 0; f < frames && sent < total_samples; f++) {
      pcm_buf[out++] = mic_to_pcm16(raw_buf[f * 2]); // 左声道是麦(L/R接地)
      sent++;
    }
    Serial.write((uint8_t*)pcm_buf, out * sizeof(int16_t));
  }
}

// ---- 常开流: 发一个chunk ----
void stream_one_chunk() {
  size_t bytes_read = 0;
  esp_err_t ret = i2s_channel_read(rx_handle, raw_buf, sizeof(raw_buf), &bytes_read, 1000);
  if (ret != ESP_OK || bytes_read == 0) return;
  int frames = bytes_read / (sizeof(int32_t) * 2);
  for (int f = 0; f < frames; f++) {
    pcm_buf[f] = mic_to_pcm16(raw_buf[f * 2]);
  }
  Serial.write((uint8_t*)pcm_buf, frames * sizeof(int16_t));
}

// ---- 播放: 收PC的16bit mono, 转32bit双声道写I2S ----
void play_from_serial(long total) {
  Serial.println("OK");
  long received = 0;
  bool ok = true;
  while (received < total) {
    long remain = total - received;
    size_t need = (remain < (long)sizeof(play_buf)) ? (size_t)remain : sizeof(play_buf);

    Serial.setTimeout(3000);
    size_t got = Serial.readBytes(play_buf, need);
    if (got == 0) { ok = false; break; }  // 3秒没数据, PC可能断了

    int n = got / 2; // 16bit样点数
    for (int i = 0; i < n; i++) {
      int32_t v = ((int32_t)((int16_t*)play_buf)[i]) << 16;
      tx_buf[i * 2] = v;      // 左
      tx_buf[i * 2 + 1] = v;  // 右
    }
    size_t written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, tx_buf, n * 2 * sizeof(int32_t), &written, portMAX_DELAY);
    if (ret != ESP_OK) { ok = false; break; }

    received += got;
    Serial.write('K'); // 流控: 让PC发下一块
  }
  Serial.println(ok ? "DONE" : "ERR");
}

void loop() {
  if (!Serial.available()) {
    delay(5);
    return;
  }

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  char c = line.charAt(0);
  if (c == 'S' || c == 's') {
    // 常开流: RX方向只会有命令字节, 非阻塞逐字节吃, Q即停
    Serial.println("STREAM");
    Serial.flush();
    while (true) {
      bool stop = false;
      while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == 'Q' || ch == 'q') stop = true;
      }
      if (stop) break;
      stream_one_chunk();
    }
    Serial.println("\nSTOP");
    Serial.flush();
  } else if (c == 'R' || c == 'r') {
    int sec = line.substring(1).toInt();
    if (sec < 1) sec = 5;
    if (sec > 15) sec = 15;
    delay(50); // 清一下残留命令
    record_and_stream(sec);
    Serial.printf("\nDONE len=%d\n", SAMPLE_RATE * sec * 2);
    Serial.flush();
  } else if (c == 'A' || c == 'a') {
    long total = line.substring(1).toInt();
    if (total <= 0) {
      Serial.println("ERR len");
      return;
    }
    digitalWrite(SD_PIN, HIGH); // 播放前自动解除静音
    delay(20);                  // 等功放起来
    play_from_serial(total);
  } else if (c == 'M' || c == 'm') {
    if (line.charAt(1) == '0') {
      digitalWrite(SD_PIN, LOW);
      Serial.println("MUTE0");
    } else {
      digitalWrite(SD_PIN, HIGH);
      Serial.println("MUTE1");
    }
  } else {
    Serial.println("ERR cmd, use R<sec>/S/Q/A<len>/M0/M1");
  }
}

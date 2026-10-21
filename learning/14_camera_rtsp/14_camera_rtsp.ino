/*
  HUB-8735 Ultra - 相机 + 声音 RTSP 串流（HD 1280x720、H.264 + AAC），IP 显示在 OLED

  做法：
    相机的 H.264 影像、麦克风的声音（先经 AAC 编码）经 StreamIO 混合后送到
    RTSP 伺服器，用 VLC、ffplay、手机的 RTSP 播放器等都能同时看到画面、
    听到声音。这是官方范例 Multimedia/examples/StreamRTSP/SingleVideoWithAudio
    的做法，影像/声音编码由晶片硬体完成，主迴圈几乎不用做事。

  和 00-3_camera_web_stream 的差别：
    00-3 是 MJPEG over HTTP（一张张 JPEG 连续送，浏览器直接看得到，没有声音）；
    这支是真正的 RTSP + H.264 + AAC，压缩率高得多、画面更顺，但浏览器不能
    直接开，要用支援 RTSP 的播放器。

  使用方式：
    1. 烧录后看 OLED：连线进度结束后会显示板子的 IP（序列埠也会印
       rtsp://<IP>:554）。
    2. 同一个网络底下，用 VLC（媒体 -> 开启网络串流）贴上
       rtsp://<OLED 上的 IP>:554 。命令列：ffplay rtsp://<IP>:554

  OLED：
    SDA -> IO3, SCL -> IO4, VCC -> 3.3V, GND -> GND
    IO3/IO4 是硬体 I2C0，会跟 JTAG pinmux 冲突，所以用 U8g2 软体 I2C +
    自订 GPIO callback（原因见 03/06）。IO0/IO1 是相机的 I2C1，这支不能
    再接别的装置。
    标题列左边是 WiFi 讯号格，右边闪烁的圆点代表串流中（LIVE）。

  声音：
    AudioSetting(3) = PDM 数位麦克风、16 kHz、单声道（官方范例的预设）。
    如果播放器有画面没声音，先确认板子上的麦克风类型，改用
    AudioSetting(1)（类比麦克风 16 kHz）试试。

  相机设定：
    VIDEO_HD（1280x720）、CAM_FPS、H.264。手机热点讯号不好、画面卡顿时
    可改成 VIDEO_VGA。

  WiFi：
    SSID / 密码放在同一个资料夹的 secrets.h（已 gitignore，不会被 commit），
    第一次使用请复制 secrets.h.example 成 secrets.h 再填入。

  画面文字使用简体中文（OLED 字型只保证涵盖 GB2312 字集）。
*/

#include <U8g2lib.h>
#include <WiFi.h>
#include "StreamIO.h"
#include "VideoStream.h"
#include "AudioStream.h"
#include "AudioEncoder.h"
#include "RTSP.h"
#include "secrets.h"

#define CHANNEL 0

const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;
const uint16_t RTSP_PORT = 554;

U8G2 display;

VideoSetting configV(VIDEO_HD, CAM_FPS, VIDEO_H264, 0);
AudioSetting configA(3);
Audio audio;
AAC aac;
RTSP rtsp;
StreamIO audioStreamer(1, 1);  // 1 个输入（麦克风）-> 1 个输出（AAC 编码）
StreamIO avMixStreamer(2, 1);  // 2 个输入（影像 + AAC 声音）-> 1 个输出（RTSP）

bool streaming = false;  // 相机、声音、RTSP 都启动完成

uint8_t u8x8_gpio_and_delay_fast(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
      pinMode(OLED_SDA, OUTPUT);
      pinMode(OLED_SCL, OUTPUT);
      digitalWrite(OLED_SDA, HIGH);
      digitalWrite(OLED_SCL, HIGH);
      break;
    case U8X8_MSG_GPIO_I2C_CLOCK:
      digitalWrite(OLED_SCL, arg_int);
      break;
    case U8X8_MSG_GPIO_I2C_DATA:
      digitalWrite(OLED_SDA, arg_int);
      break;
    case U8X8_MSG_DELAY_MILLI:
      delay(arg_int);
      break;
    case U8X8_MSG_DELAY_I2C:
      delayMicroseconds(arg_int <= 2 ? 5 : 2);
      break;
    default:
      return 0;
  }
  return 1;
}

// ---------------------------------------------------------------- 绘图

bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

// WiFi 讯号格数 0~4：依 RSSI 分级，没连线就是 0。
uint8_t wifiBars() {
  if (!wifiConnected()) return 0;
  const long rssi = WiFi.RSSI();
  if (rssi > -55) return 4;
  if (rssi > -65) return 3;
  if (rssi > -75) return 2;
  if (rssi > -85) return 1;
  return 0;
}

// 左上角讯号格：4 根由矮到高的长条；没讯号的只留底下一个点，完全没连线加小叉。
// 画在反白标题列上，所以用黑色画。
void drawWifiIcon(int16_t x, int16_t baseY) {
  const uint8_t bars = wifiBars();
  display.setDrawColor(0);
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t h = 3 + i * 2;
    if (i < bars) {
      display.drawBox(x + i * 4, baseY - h + 1, 3, h);
    } else {
      display.drawPixel(x + i * 4 + 1, baseY);
    }
  }
  if (!wifiConnected()) {
    display.drawLine(x + 9, baseY - 5, x + 13, baseY - 1);
    display.drawLine(x + 9, baseY - 1, x + 13, baseY - 5);
  }
  display.setDrawColor(1);
}

// 反白标题列：左边 WiFi 讯号格，中间标题，右边串流中会闪烁的圆点（LIVE）。
void drawTitleBar(const char *title, bool live) {
  display.drawBox(0, 0, 128, 14);
  drawWifiIcon(3, 11);

  display.setDrawColor(0);
  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8((128 - display.getUTF8Width(title)) / 2, 11, title);

  if (live) {
    if ((millis() / 500) % 2 == 0) {
      display.drawDisc(118, 7, 3);
    } else {
      display.drawCircle(118, 7, 3);
    }
  }
  display.setDrawColor(1);
}

// 一个小标签：圆角外框 + 文字，回传这个标签占的宽度（含右边间距）。
uint8_t drawChip(int16_t x, int16_t y, const char *text) {
  display.setFont(u8g2_font_6x10_tr);
  const uint8_t w = display.getStrWidth(text) + 7;
  display.drawRFrame(x, y, w, 13, 3);
  display.drawStr(x + 4, y + 10, text);
  return w + 3;
}

// 启动过程的状态画面：两行文字，置中。
void showStatus(const char *line1, const char *line2) {
  display.clearBuffer();
  drawTitleBar("RTSP 串流", false);

  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8((128 - display.getUTF8Width(line1)) / 2, 36, line1);
  if (line2) {
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr((128 - display.getStrWidth(line2)) / 2, 54, line2);
  }
  display.sendBuffer();
}

// 串流中的主画面：IP 大字 + 端口 + 规格标签。
void showStreamScreen() {
  IPAddress ip = WiFi.localIP();
  char ipText[20];
  snprintf(ipText, sizeof(ipText), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

  display.clearBuffer();
  drawTitleBar("RTSP 串流", streaming);

  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8(4, 29, "IP 地址");
  char portText[16];
  snprintf(portText, sizeof(portText), "端口 %u", RTSP_PORT);
  display.drawUTF8(124 - display.getUTF8Width(portText), 29, portText);

  display.setFont(u8g2_font_8x13B_tr);
  display.drawStr((128 - display.getStrWidth(ipText)) / 2, 45, ipText);

  display.drawHLine(4, 33, 120);

  int16_t x = 4;
  x += drawChip(x, 50, "720p");
  x += drawChip(x, 50, "H.264");
  x += drawChip(x, 50, "AAC");
  display.setFont(u8g2_font_6x10_tr);
  display.drawStr(x + 2, 60, "mic");

  display.sendBuffer();
}

// ---------------------------------------------------------------- 连线与串流

void connectWifi() {
  char footer[24];
  for (uint16_t attempt = 1; !wifiConnected(); attempt++) {
    Serial.print("[WiFi] 连线 ");
    Serial.print(WIFI_SSID);
    Serial.print("（第 ");
    Serial.print(attempt);
    Serial.println(" 次）");
    WiFi.begin(const_cast<char *>(WIFI_SSID), const_cast<char *>(WIFI_PASS));

    snprintf(footer, sizeof(footer), "%.12s  #%u", WIFI_SSID, attempt);
    const uint32_t startMs = millis();
    while (!wifiConnected() && millis() - startMs < 8000) {
      showStatus("连接 WiFi...", footer);
      delay(200);
    }
  }
  Serial.print("[WiFi] 已连线，IP = ");
  Serial.println(WiFi.localIP());
}

void printInfo() {
  Serial.println("------------------------------");
  Serial.println("- Summary of Streaming -");
  Serial.println("------------------------------");
  Camera.printInfo();

  Serial.println("- RTSP -");
  Serial.print("rtsp://");
  Serial.print(WiFi.localIP());
  Serial.print(":");
  rtsp.printInfo();

  Serial.println("- Audio -");
  audio.printInfo();
}

void startStreaming() {
  showStatus("启动相机与麦克风...", NULL);

  Camera.configVideoChannel(CHANNEL, configV);
  Camera.videoInit();

  audio.configAudio(configA);
  audio.begin();
  aac.configAudio(configA);
  aac.begin();

  rtsp.configVideo(configV);
  rtsp.configAudio(configA, CODEC_AAC);
  rtsp.begin();

  audioStreamer.registerInput(audio);
  audioStreamer.registerOutput(aac);
  if (audioStreamer.begin() != 0) {
    Serial.println("audio StreamIO link start failed");
  }

  avMixStreamer.registerInput1(Camera.getStream(CHANNEL));
  avMixStreamer.registerInput2(aac);
  avMixStreamer.registerOutput(rtsp);
  if (avMixStreamer.begin() != 0) {
    Serial.println("A/V StreamIO link start failed");
  }

  Camera.channelBegin(CHANNEL);

  delay(1000);
  streaming = true;
  printInfo();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("14 相机 + 声音 RTSP 串流启动");

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  showStatus("启动中", NULL);
  connectWifi();
  startStreaming();
}

void loop() {
  // WiFi 中途掉线就重连；串流本身由 StreamIO / RTSP 在背景执行。
  if (!wifiConnected()) {
    Serial.println("[WiFi] 断线，重新连线");
    connectWifi();
    printInfo();  // IP 可能变了，重印一次 RTSP 网址
  }

  showStreamScreen();
  delay(250);
}

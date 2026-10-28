/*
  HUB-8735 Ultra - 两指跑酷：食指 + 中指当两条腿，在桌上「走路」让角色跑起来（网页游戏）

  玩法（网页里）：
    - 镜头对着桌面前方，手放在桌上比「YA」：食指和中指就是角色的两条腿。
    - 跑步：两指轮流「抬起 -> 放下」像走路，越快跑越快；画面左下的两根长条是指尖离桌面的高度。
    - 跳跃：两指同时抬高，抬得越高跳得越高、滞空越久。
    - 冲刺：两指张成大 V，消耗能量（吃金币补充）；冲刺时无敌，还能撞碎障碍。
    - 后面的怪物一直追上来，停下来或撞到障碍就会被追上。
    - 没有摄像头也能玩：键盘 F = 食指、J = 中指（轮流按来跑）、F+J = 跳、空白键按住 = 张开冲刺。

  几何量（网页里用手部 21 个关键点算）：
    - 指尖离桌面的高度 = (桌面线 - 指尖y) / 手掌长度；桌面线自动学习（最近 3 秒指尖到过的最低位置），
      也可以按「校准桌面」，或按板子上的 FUNC 键；
    - 两指张开程度 = 食指尖到中指尖的距离 / 手掌长度。
    都用手掌长度（手腕到中指根）当单位，所以手离镜头远近不影响。

  分工：
    板子：相机 JPEG（网页向 /snap 一张一张要）+ NPU 手部关键点追踪（HandTracker），用 SSE 推给网页；
    网页：游戏逻辑、画面、音效全部在浏览器里做。

  传输：HTTP 80 port：/ 网页、/snap 快照、/events SSE。这个 SDK 没有 HTTPS 伺服器，区网内用 HTTP。
  OLED：SDA -> IO3, SCL -> IO4（U8g2 软体 I2C）。FUNC 键（IO12）= 网页校准桌面。
  IO0/IO1 是板载相机感光元件的 I2C1，不能接别的装置。

  为什么有 HandTracker.cpp / .h：这个 SDK 版本的 NNGestureDetection 拿不到手部关键点座标，
  所以把它的程序复制过来改成「把 21 个关键点存起来」，细节见 HandTracker.h。

  WiFi：SSID / 密码放在同一个资料夹的 secrets.h（已 gitignore，不会被 commit）。
  画面文字使用简体中文（OLED 字型只保证涵盖 GB2312 字集）；网页使用繁体中文。
*/

#include <stdarg.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include "StreamIO.h"
#include "VideoStream.h"
#include "HandTracker.h"
#include "game_page.h"
#include "secrets.h"

#define CHANNEL   0  // VGA JPEG，给网页的 MJPEG
#define CHANNELNN 3  // RGB，给手部追踪

#define NNWIDTH  192
#define NNHEIGHT 192

const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;
const uint8_t FUNC_PIN = 12;

const uint8_t NN_FPS = 15;
const uint32_t MAX_VIDEO_FPS = 10;  // 网页画面最高张数 / 秒

U8G2 display;
WiFiServer server(80);

VideoSetting configV(VIDEO_VGA, CAM_FPS, VIDEO_JPEG, 1);
VideoSetting configNN(NNWIDTH, NNHEIGHT, NN_FPS, VIDEO_RGB, 0);
HandTracker hands;
StreamIO videoStreamerNN(1, 1);  // 相机 channel 3 -> 手部追踪

// ---- 长连线：MJPEG 影像 / SSE 事件 ----
WiFiClient eventClient;
bool eventActive = false;

// ---- 手部追踪结果（给 OLED 显示用）----
bool handFound = false;
uint32_t handSeq = 0;
float handU[HAND_POINTS];
float handV[HAND_POINTS];
uint32_t handFrames = 0;     // 每秒处理几帧
uint32_t handRate = 0;
uint32_t lastRateMs = 0;

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

// 把格式化的文字接在 buf 后面。这个核心的 snprintf 遇到含中文的 %s 时回传值会少算，
// 所以不信回传值，每次都用 strlen() 重新量（15 的网页因此坏过一次）。
void appendf(char *buf, size_t cap, const char *fmt, ...) {
  const size_t len = strlen(buf);
  if (len + 1 >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
}

// ---------------------------------------------------------------- 网页伺服器

// 一定要带 Content-Length：浏览器靠它判断内容有没有收完整。
void sendHeader(WiFiClient &c, const char *type, size_t length) {
  c.print("HTTP/1.1 200 OK\r\nContent-Type: ");
  c.print(type);
  c.print("\r\nContent-Length: ");
  c.print(static_cast<unsigned long>(length));
  c.print("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
}

// 分段送出大字串，一次送太多可能被网路堆叠截断。
void sendBig(WiFiClient &c, const char *text) {
  const size_t total = strlen(text);
  size_t sent = 0;
  while (sent < total && c.connected()) {
    const size_t n = (total - sent) > 512 ? 512 : (total - sent);
    c.write(reinterpret_cast<const uint8_t *>(text + sent), n);
    sent += n;
  }
}

// 把 client 变成长连线（MJPEG 影像 / SSE 事件）保留下来。
// WiFiClient 的解构函式会把 socket 关掉，而且复制出来的物件跟原本的共用同一个
// socket，所以要先复制，再把「原本那个」改成非阻塞模式（解构时就不会关 socket）。
void adoptClient(WiFiClient &client, WiFiClient &slot, bool &active) {
  // 不要在这里 stop() 旧连线：旧连线多半已经被浏览器关掉，它的 socket 编号可能已经被
  // 刚接受的新连线重复使用，stop() 会把新连线一起关掉（之前画面送出去就中断就是这样）。
  // 旧的会在 pumpVideo / pumpEvents 写入失败时被清掉。
  slot = client;
  client.setNonBlockingMode();
  active = true;
}

// ---- 画面：一张一张的 JPEG 快照（/snap）----
// 之前用 multipart/x-mixed-replace 连续串流，浏览器只显示第一张就不动了（原因没查出来）；
// 改成最保险的做法：网页每收到一张就再要下一张，每张都是一个独立的、带 Content-Length 的
// 一般 HTTP 回应，跟 /api/status 同一条路，浏览器一定看得懂。
// Camera.getImage() 在 snapshot 模式下每次呼叫都会拍一张新的（等到画面送达才返回）。
uint32_t snapCount = 0;
uint32_t snapFail = 0;
uint32_t lastSnapLen = 0;
uint32_t lastSnapMs = 0;

// 一次 write 太大一包可能整个送不出去，所以切成小段送；
// 任何一段失败就回传 false。
bool writeAll(WiFiClient &client, const uint8_t *buf, uint32_t len) {
  const uint32_t SLICE = 1400;
  while (len > 0) {
    const uint32_t n = len > SLICE ? SLICE : len;
    if (client.write(buf, n) == 0) return false;
    buf += n;
    len -= n;
  }
  return true;
}

void serveSnapshot(WiFiClient &client) {
  uint32_t imageAddress = 0;
  uint32_t imageLength = 0;
  Camera.getImage(CHANNEL, &imageAddress, &imageLength);
  if (imageAddress == 0 || imageLength == 0) {
    snapFail++;
    client.print("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n");
    return;
  }
  char header[160];
  snprintf(header, sizeof(header),
           "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
           (unsigned long)imageLength);
  if (writeAll(client, (const uint8_t *)header, strlen(header)) && writeAll(client, (const uint8_t *)imageAddress, imageLength)) {
    snapCount++;
    lastSnapLen = imageLength;
    lastSnapMs = millis();
  } else {
    snapFail++;
  }
}

// 每 5 秒在序列埠印一次统计，网页没画面时靠这行判断是「相机没出图」还是「传输出问题」。
void printVideoStats() {
  static uint32_t lastReportMs = 0;
  if (millis() - lastReportMs < 5000) return;
  lastReportMs = millis();
  Serial.print("[VIDEO] 已送出 ");
  Serial.print(snapCount);
  Serial.print(" 张，最后一张 ");
  Serial.print(lastSnapLen);
  Serial.print(" bytes，失败 ");
  Serial.print(snapFail);
  Serial.print(" 次；手部NN 回呼 ");
  Serial.print(HandTracker::statCallbacks);
  Serial.print("，没找到手 ");
  Serial.print(HandTracker::statNotFound);
  Serial.print("，校正失败 ");
  Serial.println(HandTracker::statFixFail);
}

void startEventStream(WiFiClient &client) {
  client.print("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n");
  client.print("retry: 1000\n\n");
}

bool hadRequest = false;  // 这次 accept 到的连线有没有真的送来请求（没有人连时 accept 会回传空连线）

void handleClient(WiFiClient &client, bool &adopted) {
  adopted = false;
  hadRequest = false;
  // 读第一行（"GET /path?query HTTP/1.1"），再把剩下的 header 读掉直到空行；
  // 最多等 1.5 秒，避免连上来却不说话的客户端卡住主迴圈。
  char line[160];
  size_t len = 0;
  bool first = true;
  char request[160] = "";
  const uint32_t startMs = millis();
  while (client.connected() && millis() - startMs < 1500) {
    if (!client.available()) continue;
    const char ch = client.read();
    if (ch == '\n') {
      if (len == 0) break;  // 空行：header 结束
      line[len] = 0;
      if (first) {
        snprintf(request, sizeof(request), "%s", line);
        first = false;
      }
      len = 0;
    } else if (ch != '\r' && len < sizeof(line) - 1) {
      line[len++] = ch;
    }
  }

  if (request[0]) hadRequest = true;
  // request 形如 "GET /video?123 HTTP/1.1"
  char *path = strchr(request, ' ');
  if (!path) return;
  path++;
  char *end = strchr(path, ' ');
  if (end) *end = 0;
  char *query = strchr(path, '?');
  if (query) *query++ = 0;
  if (strcmp(path, "/snap") != 0 && strcmp(path, "/video") != 0) {
    Serial.print("[HTTP] ");
    Serial.println(request);
  }

  if (strcmp(path, "/") == 0) {
    sendHeader(client, "text/html; charset=utf-8", strlen(GAME_PAGE));
    sendBig(client, GAME_PAGE);
  } else if (strcmp(path, "/snap") == 0 || strcmp(path, "/video") == 0) {
    serveSnapshot(client);
  } else if (strcmp(path, "/events") == 0) {
    startEventStream(client);
    adoptClient(client, eventClient, eventActive);
    adopted = true;
  } else {
    client.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
  }
}

// 手部追踪每处理完一帧，就把 21 个关键点（0~1000 的整数）推给网页：
//   "data: 1,u0..u20,v0..v20"  有手
//   "data: 0"                  这一帧没看到手
// 没新结果时每 2 秒送一个心跳，让浏览器和路由器知道连线还活着。
void pumpEvents() {
  if (HandTracker::read(handSeq, handFound, handU, handV)) {
    handFrames++;
  }
  if (millis() - lastRateMs >= 1000) {
    lastRateMs = millis();
    handRate = handFrames;
    handFrames = 0;
  }

  if (!eventActive) return;
  if (!eventClient.connected()) {
    eventClient.stop();
    eventActive = false;
    return;
  }
  static uint32_t lastSeq = 0;
  static uint32_t lastSendMs = 0;

  if (handSeq != lastSeq) {
    lastSeq = handSeq;
    char msg[400];
    msg[0] = 0;
    if (handFound) {
      appendf(msg, sizeof(msg), "data: 1");
      for (uint8_t i = 0; i < HAND_POINTS; i++) appendf(msg, sizeof(msg), ",%d", static_cast<int>(handU[i] * 1000));
      for (uint8_t i = 0; i < HAND_POINTS; i++) appendf(msg, sizeof(msg), ",%d", static_cast<int>(handV[i] * 1000));
      appendf(msg, sizeof(msg), "\n\n");
    } else {
      appendf(msg, sizeof(msg), "data: 0\n\n");
    }
    eventClient.print(msg);
    lastSendMs = millis();
  } else if (millis() - lastSendMs > 2000) {
    eventClient.print(": hb\n\n");
    lastSendMs = millis();
  }
}

// ---------------------------------------------------------------- OLED

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

// 反白标题列：左边 WiFi 讯号格，中间标题，右边有网页在看画面时会闪烁的圆点（LIVE）。
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

void showStatus(const char *line1, const char *line2) {
  display.clearBuffer();
  drawTitleBar("两指跑酷", false);

  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8((128 - display.getUTF8Width(line1)) / 2, 36, line1);
  if (line2) {
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr((128 - display.getStrWidth(line2)) / 2, 54, line2);
  }
  display.sendBuffer();
}

// 小手图示：手掌方块 + 5 根手指；拇指和食指尖画成亮点，抓到手就把拇指食指连成取景框。
void drawHandIcon(int16_t x, int16_t y, bool lit) {
  display.drawRFrame(x + 6, y + 14, 20, 16, 3);
  const int16_t fx[5] = { 0, 8, 13, 18, 23 };
  const int16_t fh[5] = { 8, 14, 16, 14, 10 };
  for (uint8_t i = 0; i < 5; i++) {
    if (lit) {
      display.drawBox(x + 6 + (i > 0 ? fx[i] - 4 : -4), y + 14 - fh[i], 3, fh[i]);
    } else {
      display.drawFrame(x + 6 + (i > 0 ? fx[i] - 4 : -4), y + 14 - fh[i], 3, fh[i]);
    }
  }
}

// 主画面：左边小手图示，右边「有没有手」和每秒追踪帧数，底下是网址。
void showMainScreen() {
  const bool found = handFound && handRate > 0;

  display.clearBuffer();
  drawTitleBar("两指跑酷", (millis() - lastSnapMs < 2000));

  drawHandIcon(8, 20, found);

  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8(50, 30, found ? "侦测到手" : "请把手伸进镜头");
  char rate[24];
  snprintf(rate, sizeof(rate), "追踪 %u 帧/秒", static_cast<unsigned>(handRate));
  display.drawUTF8(50, 44, rate);

  display.drawHLine(4, 49, 120);
  IPAddress ip = WiFi.localIP();
  char ipText[28];
  snprintf(ipText, sizeof(ipText), "http://%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  display.setFont(u8g2_font_6x10_tr);
  display.drawStr((128 - display.getStrWidth(ipText)) / 2, 61, ipText);

  display.sendBuffer();
}

// ---------------------------------------------------------------- 连线与启动

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
  Serial.println("- 两指跑酷网页 -");
  Serial.print("http://");
  Serial.println(WiFi.localIP());
  Serial.println("------------------------------");
  Camera.printInfo();
}

void startCamera() {
  showStatus("启动相机与 AI...", NULL);

  Camera.configVideoChannel(CHANNEL, configV);
  Camera.configVideoChannel(CHANNELNN, configNN);
  Camera.videoInit();

  hands.configVideo(configNN);
  hands.configThreshold(0.35, 0.3);  // 手掌侦测信心度从预设 0.5 放宽，比较容易抓到手
  hands.modelSelect(GESTURE_DETECTION, NA_MODEL, NA_MODEL, NA_MODEL, NA_MODEL, NA_MODEL, DEFAULT_PALMDETECT, DEFAULT_HANDLANDMARK);
  hands.begin();

  Camera.channelBegin(CHANNEL);  // JPEG 通道：网页用 Camera.getImage() 取画面

  videoStreamerNN.registerInput(Camera.getStream(CHANNELNN));
  videoStreamerNN.setStackSize();
  videoStreamerNN.setTaskPriority();
  videoStreamerNN.registerOutput(hands);
  if (videoStreamerNN.begin() != 0) {
    Serial.println("NN StreamIO link start failed");
  }
  Camera.channelBegin(CHANNELNN);

  delay(500);
  printInfo();
}

// FUNC 键按下（高 -> 低的瞬间）回传 true，带简单防弹跳。
bool funcPressed() {
  static bool wasHigh = true;
  const bool high = (digitalRead(FUNC_PIN) == HIGH);
  const bool pressed = wasHigh && !high;
  wasHigh = high;
  if (pressed) delay(20);
  return pressed;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("15-2 两指跑酷 启动");

  pinMode(FUNC_PIN, INPUT_PULLUP);
  for (uint8_t i = 0; i < HAND_POINTS; i++) handU[i] = handV[i] = 0;

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  showStatus("启动中", NULL);
  connectWifi();
  startCamera();
  server.begin();
}

void loop() {
  if (!wifiConnected()) {
    Serial.println("[WiFi] 断线，重新连线");
    connectWifi();
    server.begin();
    printInfo();  // IP 可能变了
  }

  // 新的连线：网页请求处理完就关；影像、事件是长连线，留下来
  WiFiClient client = server.available();
  if (client) {
    bool adopted = false;
    handleClient(client, adopted);
    if (!adopted) {
      if (hadRequest) delay(20);  // 让最后的资料有时间送出去，再关连线
      client.stop();
    }
  }

  printVideoStats();
  pumpEvents();

  // FUNC 键 = 网页换下一个滤镜
  if (funcPressed()) {
    Serial.println("[FUNC] 校准桌面");
    if (eventActive && eventClient.connected()) eventClient.print("event: func\ndata: 1\n\n");
  }

  static uint32_t lastDrawMs = 0;
  if (millis() - lastDrawMs >= 250) {
    lastDrawMs = millis();
    showMainScreen();
  }
  delay(1);
}

/*
  HUB-8735 Ultra - YOLO 物体识别（YOLOv4-tiny，80 类 COCO）+ 网页即时画面叠框 + OLED

  参考内建范例 NeuralNetwork/ObjectDetectionCallback：同样用 NNObjectDetection + DEFAULT_YOLOV4TINY，
  差别是范例把框画在 RTSP 影像的 OSD 上，这里改成网页：
    板子：相机 JPEG（网页向 /snap 一张一张要）+ NPU 物体侦测，用 SSE 把框推给网页；
    网页：叠框、中文类别名、数量统计、信心度过滤、拍照存档。

  SSE 资料（/events）：
    "data: n,type,score,x1,y1,x2,y2,type,score,..."  n 个物体，座标为 0~1000 的整数（占画面比例）
    "data: 0"                                          这一帧没有物体
  FUNC 键（IO12）：清除累计统计（OLED 与网页同时归零）。
  伺服马达（IO21，逻辑沿用 17）：画面里出现「触发类别」（预设 person，网页可换）就转到 70 度，
  消失 1.5 秒后转回 -20 度；每次 6 度 / 15 ms。/api/status 回报角度，/api/cmd?cls=N 换类别。

  取舍：
    - NN 用 576x432（跟 JPEG 640x480 同为 4:3），这样框的 0~1 座标可以直接对到画面。
    - 范例的 NN 是 576x320（16:9）；要改回去的话，JPEG 也要换成 16:9，座标才会对齐。
    - 想换模型：DEFAULT_YOLOV3TINY / DEFAULT_YOLOV4TINY / DEFAULT_YOLOV7TINY 三选一（modelSelect）。

  传输：HTTP 80 port：/ 网页、/snap 快照、/events SSE（这个 SDK 没有 HTTPS 伺服器）。
  OLED：SDA -> IO3, SCL -> IO4（U8g2 软体 I2C）。IO0/IO1 是板载相机的 I2C1，不能接别的装置。
  WiFi：SSID / 密码放同资料夹的 secrets.h（已 gitignore），第一次请复制 secrets.h.example。
  OLED 文字使用简体中文（字型只保证 GB2312）；网页使用繁体中文。
*/

#include <stdarg.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <AmebaServo.h>
#include "StreamIO.h"
#include "VideoStream.h"
#include "NNObjectDetection.h"
#include "ObjectClassList.h"
#include "yolo_page.h"
#include "secrets.h"

#define CHANNEL   0  // VGA JPEG，给网页
#define CHANNELNN 3  // RGB，给 YOLO

#define NNWIDTH  576
#define NNHEIGHT 432

const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;
const uint8_t FUNC_PIN = 12;

const uint8_t NN_FPS = 10;

// ---- 伺服马达（IO21），逻辑沿用 17：看到目标物体转 ANGLE_FOUND，目标消失转 ANGLE_LOST ----
const uint8_t SERVO_PIN = 21;
const int ANGLE_FOUND = 70;
const int ANGLE_LOST = -20;
const int SERVO_US_AT_0 = 544;
const int SERVO_US_AT_180 = 2400;
const int SERVO_MIN_US = 340;      // 约 -20 度；马达嗡嗡响就调高
const uint8_t SERVO_STEP_DEG = 6;
const uint16_t SERVO_STEP_MS = 15;
const uint32_t SERVO_HOLD_MS = 300;    // 看到目标要稳定多久才转
const uint32_t SERVO_LOST_MS = 1500;   // 目标消失多久才转回去（YOLO 偶尔漏一帧，不要抖）

AmebaServo servo;
int servoAngle = 0;
int servoTarget = 0;
uint32_t lastServoStepMs = 0;
uint8_t targetClass = 0;          // 触发马达的类别，预设 person，网页可改
bool targetPresent = false;
int8_t appliedState = -1;         // -1 尚未决定，0 已转到 LOST，1 已转到 FOUND
uint32_t stateSinceMs = 0;

U8G2 display;
WiFiServer server(80);

VideoSetting configV(VIDEO_VGA, CAM_FPS, VIDEO_JPEG, 1);
VideoSetting configNN(NNWIDTH, NNHEIGHT, NN_FPS, VIDEO_RGB, 0);
NNObjectDetection ObjDet;
StreamIO videoStreamerNN(1, 1);

WiFiClient eventClient;
bool eventActive = false;

// ---- 侦测结果：NN 回呼写入，主迴圈读取 ----
struct Det {
  uint8_t type;
  uint8_t score;
  uint16_t x1, y1, x2, y2;  // 0~1000
};
volatile uint32_t detSeq = 0;
volatile uint8_t detCount = 0;
Det dets[MAX_OBJ_DET];

uint32_t detFrames = 0;
uint32_t detRate = 0;
uint32_t lastRateMs = 0;
uint32_t totalSeen = 0;      // 累计看到的物体次数（每帧每个物体算一次）
uint16_t classSeen[80];      // 各类累计次数

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

// 把格式化的文字接在 buf 后面（核心的 snprintf 回传值不可信，见 15）。
void appendf(char *buf, size_t cap, const char *fmt, ...) {
  const size_t len = strlen(buf);
  if (len + 1 >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
}

static uint16_t to1000(float v) {
  if (v < 0) v = 0;
  if (v > 1) v = 1;
  return static_cast<uint16_t>(v * 1000);
}

// NN 每处理完一帧就呼叫一次（在 NN 的 task 里，所以只存资料，不做网路和显示）。
void ODPostProcess(std::vector<ObjectDetectionResult> results) {
  uint8_t n = 0;
  const uint16_t cnt = ObjDet.getResultCount();
  for (uint16_t i = 0; i < cnt && n < MAX_OBJ_DET; i++) {
    ObjectDetectionResult item = results[i];
    const int type = item.type();
    if (type < 0 || type >= 80 || !itemList[type].filter) continue;
    Det &d = dets[n++];
    d.type = type;
    int s = item.score();
    d.score = s < 0 ? 0 : (s > 100 ? 100 : s);
    d.x1 = to1000(item.xMin());
    d.y1 = to1000(item.yMin());
    d.x2 = to1000(item.xMax());
    d.y2 = to1000(item.yMax());
  }
  detCount = n;
  detSeq = detSeq + 1;
}

// ---------------------------------------------------------------- 伺服马达

void servoWrite(int angle) {
  int us = SERVO_US_AT_0 + static_cast<int>((static_cast<long>(angle) * (SERVO_US_AT_180 - SERVO_US_AT_0)) / 180);
  us = constrain(us, SERVO_MIN_US, SERVO_US_AT_180);
  servo.writeMicroseconds(us);
}

void updateServo() {
  // 画面里有没有目标类别（只看最新一帧；detCount / dets 由 NN 回呼更新）
  bool present = false;
  const uint8_t n = detCount;
  for (uint8_t i = 0; i < n; i++) {
    if (dets[i].type == targetClass) {
      present = true;
      break;
    }
  }
  if (present != targetPresent) {
    targetPresent = present;
    stateSinceMs = millis();
  }
  const uint32_t stable = millis() - stateSinceMs;
  if (targetPresent && appliedState != 1 && stable >= SERVO_HOLD_MS) {
    appliedState = 1;
    servoTarget = ANGLE_FOUND;
    Serial.print("[SERVO] 看到 ");
    Serial.print(itemList[targetClass].objectName);
    Serial.println(" -> 70 度");
  } else if (!targetPresent && appliedState != 0 && stable >= SERVO_LOST_MS) {
    appliedState = 0;
    servoTarget = ANGLE_LOST;
    Serial.println("[SERVO] 目标消失 -> -20 度");
  }

  if (servoAngle != servoTarget && millis() - lastServoStepMs >= SERVO_STEP_MS) {
    lastServoStepMs = millis();
    // NN 标头会取消 min()/max() 宏，所以手动比较
    if (servoAngle < servoTarget) {
      servoAngle += SERVO_STEP_DEG;
      if (servoAngle > servoTarget) servoAngle = servoTarget;
    } else {
      servoAngle -= SERVO_STEP_DEG;
      if (servoAngle < servoTarget) servoAngle = servoTarget;
    }
    servoWrite(servoAngle);
  }
}

// ---------------------------------------------------------------- 网页伺服器

void sendHeader(WiFiClient &c, const char *type, size_t length) {
  c.print("HTTP/1.1 200 OK\r\nContent-Type: ");
  c.print(type);
  c.print("\r\nContent-Length: ");
  c.print(static_cast<unsigned long>(length));
  c.print("\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
}

void sendBig(WiFiClient &c, const char *text) {
  const size_t total = strlen(text);
  size_t sent = 0;
  while (sent < total && c.connected()) {
    const size_t n = (total - sent) > 512 ? 512 : (total - sent);
    c.write(reinterpret_cast<const uint8_t *>(text + sent), n);
    sent += n;
  }
}

// 长连线（SSE）要保留：复制 socket 后把原本的改成非阻塞，解构时才不会关掉（见 15）。
void adoptClient(WiFiClient &client, WiFiClient &slot, bool &active) {
  slot = client;
  client.setNonBlockingMode();
  active = true;
}

uint32_t snapCount = 0;
uint32_t snapFail = 0;
uint32_t lastSnapLen = 0;
uint32_t lastSnapMs = 0;

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
  Serial.print(" 次；YOLO ");
  Serial.print(detRate);
  Serial.println(" 帧/秒");
}

void startEventStream(WiFiClient &client) {
  client.print("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n");
  client.print("retry: 1000\n\n");
}

bool hadRequest = false;

void handleClient(WiFiClient &client, bool &adopted) {
  adopted = false;
  hadRequest = false;
  char line[160];
  size_t len = 0;
  bool first = true;
  char request[160] = "";
  const uint32_t startMs = millis();
  while (client.connected() && millis() - startMs < 1500) {
    if (!client.available()) continue;
    const char ch = client.read();
    if (ch == '\n') {
      if (len == 0) break;
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
  char *path = strchr(request, ' ');
  if (!path) return;
  path++;
  char *end = strchr(path, ' ');
  if (end) *end = 0;
  char *query = strchr(path, '?');
  if (query) *query++ = 0;
  if (strcmp(path, "/snap") != 0) {
    Serial.print("[HTTP] ");
    Serial.println(request);
  }

  if (strcmp(path, "/") == 0) {
    sendHeader(client, "text/html; charset=utf-8", strlen(YOLO_PAGE));
    sendBig(client, YOLO_PAGE);
  } else if (strcmp(path, "/snap") == 0) {
    serveSnapshot(client);
  } else if (strcmp(path, "/api/status") == 0) {
    char json[160];
    snprintf(json, sizeof(json), "{\"servo\":%d,\"stgt\":%d,\"cls\":%u,\"hit\":%d,\"fps\":%u}",
             servoAngle, servoTarget, static_cast<unsigned>(targetClass), targetPresent ? 1 : 0, static_cast<unsigned>(detRate));
    sendHeader(client, "application/json", strlen(json));
    client.print(json);
  } else if (strcmp(path, "/api/cmd") == 0) {
    // cls=<0~79> 换触发类别；sv=1/0 手动测试马达（直接转到 FOUND / LOST 角度）
    if (query) {
      const char *p = strstr(query, "cls=");
      if (p) {
        const int v = atoi(p + 4);
        if (v >= 0 && v < 80) {
          targetClass = v;
          appliedState = -1;
          Serial.print("[CMD] 触发类别 = ");
          Serial.println(itemList[v].objectName);
        }
      }
      p = strstr(query, "sv=");
      if (p) servoTarget = atoi(p + 3) ? ANGLE_FOUND : ANGLE_LOST;
    }
    sendHeader(client, "text/plain", 2);
    client.print("ok");
  } else if (strcmp(path, "/events") == 0) {
    startEventStream(client);
    adoptClient(client, eventClient, eventActive);
    adopted = true;
  } else {
    client.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
  }
}

// 每处理完一帧就推一次；没新结果时每 2 秒送心跳。
void pumpEvents() {
  static uint32_t lastSeq = 0;
  static uint32_t lastSendMs = 0;

  const bool fresh = (detSeq != lastSeq);
  if (fresh) {
    lastSeq = detSeq;
    detFrames++;
    const uint8_t n = detCount;
    totalSeen += n;
    for (uint8_t i = 0; i < n; i++) classSeen[dets[i].type]++;
  }
  if (millis() - lastRateMs >= 1000) {
    lastRateMs = millis();
    detRate = detFrames;
    detFrames = 0;
  }

  if (!eventActive) return;
  if (!eventClient.connected()) {
    eventClient.stop();
    eventActive = false;
    return;
  }

  if (fresh) {
    char msg[480];
    msg[0] = 0;
    const uint8_t n = detCount;
    appendf(msg, sizeof(msg), "data: %u", n);
    for (uint8_t i = 0; i < n; i++) {
      const Det &d = dets[i];
      appendf(msg, sizeof(msg), ",%u,%u,%u,%u,%u,%u", d.type, d.score, d.x1, d.y1, d.x2, d.y2);
    }
    appendf(msg, sizeof(msg), "\n\n");
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

uint8_t wifiBars() {
  if (!wifiConnected()) return 0;
  const long rssi = WiFi.RSSI();
  if (rssi > -55) return 4;
  if (rssi > -65) return 3;
  if (rssi > -75) return 2;
  if (rssi > -85) return 1;
  return 0;
}

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
  drawTitleBar("物体识别", false);
  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8((128 - display.getUTF8Width(line1)) / 2, 36, line1);
  if (line2) {
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr((128 - display.getStrWidth(line2)) / 2, 54, line2);
  }
  display.sendBuffer();
}

// 主画面：这一帧有几个物体 + 最有把握的那个（英文类别名）+ 累计最多的类别 + 网址。
void showMainScreen() {
  const uint8_t n = detCount;
  int best = -1;
  for (uint8_t i = 0; i < n; i++) {
    if (best < 0 || dets[i].score > dets[best].score) best = i;
  }
  int top = -1;
  for (int c = 0; c < 80; c++) {
    if (classSeen[c] > 0 && (top < 0 || classSeen[c] > classSeen[top])) top = c;
  }

  display.clearBuffer();
  drawTitleBar("物体识别", (millis() - lastSnapMs < 2000));

  display.setFont(u8g2_font_wqy12_t_gb2312);
  char buf[32];
  snprintf(buf, sizeof(buf), "画面 %u 个  %u 帧/秒", static_cast<unsigned>(n), static_cast<unsigned>(detRate));
  display.drawUTF8(4, 27, buf);

  display.setFont(u8g2_font_7x13B_tr);
  if (best >= 0) {
    snprintf(buf, sizeof(buf), "%.12s %u%%", itemList[dets[best].type].objectName, dets[best].score);
    display.drawStr(4, 42, buf);
  } else {
    display.drawStr(4, 42, "...");
  }

  display.setFont(u8g2_font_6x10_tr);
  snprintf(buf, sizeof(buf), "%.8s>%d", itemList[targetClass].objectName, servoAngle);
  display.drawStr(4, 54, buf);
  if (top >= 0) {
    snprintf(buf, sizeof(buf), "top:%.7s", itemList[top].objectName);
    display.drawStr(70, 54, buf);
  }

  IPAddress ip = WiFi.localIP();
  char ipText[28];
  snprintf(ipText, sizeof(ipText), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  display.drawStr(128 - display.getStrWidth(ipText) - 2, 62, ipText);
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
  Serial.println("- YOLO 物体识别网页 -");
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

  ObjDet.configVideo(configNN);
  ObjDet.setResultCallback(ODPostProcess);
  ObjDet.modelSelect(OBJECT_DETECTION, DEFAULT_YOLOV4TINY, NA_MODEL, NA_MODEL);
  ObjDet.begin();

  Camera.channelBegin(CHANNEL);

  videoStreamerNN.registerInput(Camera.getStream(CHANNELNN));
  videoStreamerNN.setStackSize();
  videoStreamerNN.setTaskPriority();
  videoStreamerNN.registerOutput(ObjDet);
  if (videoStreamerNN.begin() != 0) {
    Serial.println("NN StreamIO link start failed");
  }
  Camera.channelBegin(CHANNELNN);

  delay(500);
  printInfo();
}

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
  Serial.println("18 YOLO 物体识别 启动");

  pinMode(FUNC_PIN, INPUT_PULLUP);

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  showStatus("启动中", NULL);
  connectWifi();
  startCamera();
  servo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_US_AT_180);
  servoWrite(servoAngle);  // 开机先到 0 度
  server.begin();
}

void loop() {
  if (!wifiConnected()) {
    Serial.println("[WiFi] 断线，重新连线");
    connectWifi();
    server.begin();
    printInfo();
  }

  WiFiClient client = server.available();
  if (client) {
    bool adopted = false;
    handleClient(client, adopted);
    if (!adopted) {
      if (hadRequest) delay(20);
      client.stop();
    }
  }

  printVideoStats();
  pumpEvents();
  updateServo();

  if (funcPressed()) {
    Serial.println("[FUNC] 清除累计统计");
    totalSeen = 0;
    memset(classSeen, 0, sizeof(classSeen));
    if (eventActive && eventClient.connected()) eventClient.print("event: reset\ndata: 1\n\n");
  }

  static uint32_t lastDrawMs = 0;
  if (millis() - lastDrawMs >= 250) {
    lastDrawMs = millis();
    showMainScreen();
  }
  delay(1);
}

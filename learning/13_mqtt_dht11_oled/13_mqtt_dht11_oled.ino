/*
  HUB-8735 Ultra - DHT11 温湿度 -> MQTT 发布 + OLED 连线进度/仪表板

  每 30 秒把 DHT11 的温度、湿度用 MQTT 发布到公开的 broker：

    服务器：mqttgo.io:1883
    topic ：kai0124/class105/data
    payload：{"temp": 25, "humi": 60, "light": 75}
            （温度、湿度、亮度都是整数；DHT11 没有小数；light 是亮度百分比，
            0 = 最暗、100 = 最亮，换算方式同 04_light_sensor_oled）

  OLED：
    开机后「连线进度」直接显示在画面上：
      第 1 列 WiFi（SSID）  -> 连线中 / 成功 / 失败
      第 2 列 MQTT（服务器）-> 连线中 / 成功 / 失败（附 rc 错误码）
      底下一行显示细节（第几次尝试、IP、错误码）
    两个都成功后进入仪表板：温度/湿度卡片 + 下次发布倒数 + 已发布次数。
    中途 WiFi 或 MQTT 断线，会自动回到连线进度画面重连，连上后继续发布。
    标题列：左边 WiFi 讯号格；右边 MQTT 状态（实心圆点=已连线，空心=断线，
    刚发布完 0.8 秒内显示向上箭头）。

  光敏电阻：AO -> IO0（A2 / PF_2），同 04
  DHT11：Data -> IO20（单线通讯，自己刻 bit-bang，同 05~12）
  OLED ：SDA -> IO3, SCL -> IO4（U8g2 软体 I2C，原因见 03/06）

  WiFi：
    SSID / 密码放在同一个资料夹的 secrets.h（已 gitignore，不会被 commit），
    第一次使用请复制 secrets.h.example 成 secrets.h 再填入。

  注意：
    mqttgo.io 是公开 broker，任何人都能订阅这个 topic 看到资料，也能
    往里面乱发；不要放敏感资料。clientId 用亂数（hub8735-XXXXXXXX），每次
    连线重新产生，避免跟别人撞名（同名连线会把对方踢掉）。
    MQTT 的 WiFiClient 照官方范例设成非阻塞模式（setNonBlockingMode），
    PubSubClient 自己处理收发。

  画面文字使用简体中文（OLED 字型只保证涵盖 GB2312 字集）。
*/

#include <U8g2lib.h>
#include <analogin_api.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"

const uint8_t DHT11_PIN = 20;
// 光敏电阻：类比输出 -> IO0（A2 / PF_2，ADC channel 2），同 04_light_sensor_oled。
// 注意 A0 并不是 IO0，这块板子的 IO0 要用 PF_2。
const uint16_t LIGHT_ADC_DARK = 4000;     // 原始值 4000 以上 = 0%（最暗）
const uint32_t PAGE_INTERVAL_MS = 5000;   // 仪表板两页（温湿度 / 光线）每 5 秒轮播
const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;

const char MQTT_HOST[] = "mqttgo.io";
const uint16_t MQTT_PORT = 1883;
const char MQTT_TOPIC[] = "kai0124/class105/data";
const uint32_t PUBLISH_INTERVAL_MS = 30UL * 1000UL;
const uint32_t SAMPLE_INTERVAL_MS = 3000;  // DHT11 至少要间隔 1 秒以上
const uint32_t WIFI_ATTEMPT_MS = 8000;

U8G2 display;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

char clientId[24];

analogin_t lightAdc;
uint16_t lightRaw = 0;
int lightPercent = 0;  // 亮度百分比：原始值 0 -> 100%，4000 -> 0%

bool dhtValid = false;
uint8_t humidity = 0;
uint8_t temperature = 0;

uint32_t lastSampleMs = 0;
uint32_t lastPublishMs = 0;      // 上一次「尝试发布」的时间
uint32_t lastPublishDoneMs = 0;  // 上一次发布成功的时间（画箭头用）
bool lastPublishOk = true;
uint32_t publishCount = 0;

enum StepState : uint8_t { STEP_PENDING, STEP_WORKING, STEP_OK, STEP_FAIL };

uint8_t u8x8_gpio_and_delay_fast(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
      pinMode(OLED_SDA, OUTPUT);
      pinMode(OLED_SCL, OUTPUT);
      digitalWrite(OLED_SDA, HIGH);
      digitalWrite(OLED_SCL, HIGH);
      break;
    case U8X8_MSG_DELAY_MILLI:
      delay(arg_int);
      break;
    case U8X8_MSG_DELAY_I2C:
      delayMicroseconds(arg_int <= 2 ? 5 : 2);
      break;
    case U8X8_MSG_GPIO_I2C_CLOCK:
      digitalWrite(OLED_SCL, arg_int);
      break;
    case U8X8_MSG_GPIO_I2C_DATA:
      digitalWrite(OLED_SDA, arg_int);
      break;
    default:
      return 0;
  }
  return 1;
}

// ---------------------------------------------------------------- DHT11

// 等待腳位變成指定電位，超過 timeoutUs 就放棄，回傳等待掉的時間（微秒）。
// 逾時回傳 0xFFFFFFFF。
uint32_t waitForLevel(uint8_t pin, uint8_t level, uint32_t timeoutUs) {
  const uint32_t startUs = micros();
  while (digitalRead(pin) != level) {
    if (micros() - startUs > timeoutUs) {
      return 0xFFFFFFFF;
    }
  }
  return micros() - startUs;
}

// 读取 DHT11（做法同 06）。成功回传 true，并填入湿度(%)与温度(度C)整数值。
bool readDht11(uint8_t pin, uint8_t &humidityOut, uint8_t &temperatureOut) {
  uint8_t data[5] = { 0, 0, 0, 0, 0 };

  // 起始信号：主动拉低至少 18ms，再切成 INPUT_PULLUP 让感测器接手。
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delay(20);
  pinMode(pin, INPUT_PULLUP);

  if (waitForLevel(pin, LOW, 200) == 0xFFFFFFFF) return false;
  if (waitForLevel(pin, HIGH, 200) == 0xFFFFFFFF) return false;
  if (waitForLevel(pin, LOW, 200) == 0xFFFFFFFF) return false;

  // 40 个 bit：每个 bit 先 50us 低电位，再用高电位持续时间判断 0 或 1。
  for (uint8_t i = 0; i < 40; i++) {
    if (waitForLevel(pin, HIGH, 200) == 0xFFFFFFFF) return false;
    const uint32_t highUs = waitForLevel(pin, LOW, 200);
    if (highUs == 0xFFFFFFFF) return false;

    data[i / 8] <<= 1;
    if (highUs > 40) {
      data[i / 8] |= 1;
    }
  }

  const uint8_t checksum = data[0] + data[1] + data[2] + data[3];
  if (checksum != data[4]) {
    Serial.println("[DHT11] checksum 不符，略过这次读取");
    return false;
  }

  humidityOut = data[0];
  temperatureOut = data[2];
  return true;
}

// ---------------------------------------------------------------- 光敏电阻

// 跟 04 一样的处理：直接用 analogin_read_u16() 读原始值（绕过核心 analogRead()
// 内建「电压过低强制归零」的杂讯过滤），再线性换算成亮度百分比，
// 原始值 0 -> 100%，4000 以上 -> 0%。
int lightValueToPercent(uint16_t rawAdc) {
  const uint16_t clamped = rawAdc > LIGHT_ADC_DARK ? LIGHT_ADC_DARK : rawAdc;
  return map(clamped, 0, LIGHT_ADC_DARK, 100, 0);
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

// 左上角讯号格：4 根由矮到高的长条，有讯号的实心，没讯号的只留底下一个点；
// 完全没连线就在旁边加一个小叉叉。画在反白标题列上，所以用黑色画。
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

// 反白标题列：左边 WiFi 讯号格，中间标题，右边 MQTT 状态。
void drawTitleBar(const char *title) {
  display.drawBox(0, 0, 128, 14);
  drawWifiIcon(3, 11);

  display.setDrawColor(0);
  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8((128 - display.getUTF8Width(title)) / 2, 11, title);

  // MQTT 状态：刚发布完 0.8 秒内画向上箭头；已连线实心圆；断线空心圆。
  if (lastPublishDoneMs != 0 && millis() - lastPublishDoneMs < 800) {
    display.drawTriangle(118, 2, 113, 8, 123, 8);
    display.drawBox(116, 8, 5, 4);
  } else if (mqtt.connected()) {
    display.drawDisc(118, 7, 3);
  } else {
    display.drawCircle(118, 7, 3);
  }
  display.setDrawColor(1);
}

// 连线进度的一列：圆角外框 + 标签 + 说明文字 + 右边状态记号。
void drawStepRow(int16_t y, const char *label, const char *detail, StepState state) {
  display.drawRFrame(2, y, 124, 15, 3);

  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8(7, y + 12, label);

  display.setFont(u8g2_font_6x10_tr);
  display.drawStr(42, y + 11, detail);

  const int16_t cx = 114;
  const int16_t cy = y + 7;
  switch (state) {
    case STEP_PENDING:
      display.drawHLine(cx - 3, cy, 7);
      break;
    case STEP_WORKING: {
      const uint8_t phase = (millis() / 250) % 4;  // 0~3 个点依序亮起
      for (uint8_t i = 0; i < 3; i++) {
        if (i < phase) {
          display.drawDisc(cx - 6 + i * 6, cy, 1);
        } else {
          display.drawPixel(cx - 6 + i * 6, cy);
        }
      }
      break;
    }
    case STEP_OK:  // 勾勾
      display.drawLine(cx - 4, cy, cx - 1, cy + 3);
      display.drawLine(cx - 1, cy + 3, cx + 4, cy - 3);
      display.drawLine(cx - 4, cy + 1, cx - 1, cy + 4);
      display.drawLine(cx - 1, cy + 4, cx + 4, cy - 2);
      break;
    case STEP_FAIL:  // 叉叉
      display.drawLine(cx - 3, cy - 3, cx + 3, cy + 3);
      display.drawLine(cx - 3, cy + 3, cx + 3, cy - 3);
      display.drawLine(cx - 2, cy - 3, cx + 4, cy + 3);
      display.drawLine(cx - 2, cy + 3, cx + 4, cy - 3);
      break;
  }
}

// 连线进度画面。footer 是最下面的一行说明（尝试次数 / IP / 错误码）。
void showConnectScreen(StepState wifiState, StepState mqttState, const char *footer) {
  char wifiDetail[16];
  snprintf(wifiDetail, sizeof(wifiDetail), "%.9s", WIFI_SSID);

  display.clearBuffer();
  drawTitleBar("连线中");
  drawStepRow(17, "WiFi", wifiDetail, wifiState);
  drawStepRow(34, "MQTT", MQTT_HOST, mqttState);

  display.setFont(u8g2_font_6x10_tr);
  display.drawStr((128 - display.getStrWidth(footer)) / 2, 62, footer);
  display.sendBuffer();
}

// 水滴图示（湿度用），(x, y) 是外框左上角，尺寸约 10x14，依湿度百分比从底部
// 往上填满。圆心在三角形底边下方一点点，只画下半圆跟两条斜边接起来才像水滴。
void drawDropletIcon(int16_t x, int16_t y, uint8_t humidityPercent) {
  const int16_t tipX = x + 5;
  const int16_t tipY = y;
  const int16_t leftX = x + 1;
  const int16_t rightX = x + 9;
  const int16_t baseY = y + 7;
  const int16_t circleCx = x + 5;
  const int16_t circleCy = y + 9;
  const uint8_t circleR = 5;
  const int16_t iconBottom = circleCy + circleR;

  display.drawLine(tipX, tipY, leftX, baseY);
  display.drawLine(tipX, tipY, rightX, baseY);
  display.drawCircle(circleCx, circleCy, circleR, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);

  const uint8_t clampedPercent = humidityPercent > 100 ? 100 : humidityPercent;
  const uint8_t iconHeight = iconBottom - tipY;
  const uint8_t fillHeight = (static_cast<uint16_t>(clampedPercent) * iconHeight) / 100;
  if (fillHeight > 0) {
    display.setClipWindow(x, iconBottom - fillHeight, x + 10, iconBottom);
    display.drawTriangle(tipX, tipY, leftX, baseY, rightX, baseY);
    display.drawDisc(circleCx, circleCy, circleR);
    display.setMaxClipWindow();
  }
}

// 温度计图示，(x, y) 是外框左上角，尺寸约 8x17；10~40 度对应 0~100% 填满。
void drawThermometerIcon(int16_t x, int16_t y, uint8_t temp) {
  const uint8_t stemHeight = 10;
  const uint8_t bulbCy = y + stemHeight + 4;

  display.drawRFrame(x + 2, y, 4, stemHeight, 1);
  display.drawDisc(x + 4, bulbCy, 4);

  const uint8_t clampedTemp = temp < 10 ? 10 : (temp > 40 ? 40 : temp);
  const uint8_t percent = (static_cast<uint16_t>(clampedTemp - 10) * 100) / 30;
  const uint8_t innerHeight = stemHeight - 2;
  const uint8_t fillHeight = (static_cast<uint16_t>(percent) * innerHeight) / 100;
  if (fillHeight > 0) {
    display.drawBox(x + 3, y + 1 + (innerHeight - fillHeight), 2, fillHeight);
  }
}

// 一张数值卡片：外框 + 图示 + 标签 + 大字数值 + 单位。
void drawStatCard(int16_t cardX, uint8_t value, const char *label, const char *unit, bool isTemperature) {
  const uint8_t cardY = 17;
  const uint8_t cardW = 60;
  const uint8_t cardH = 34;

  display.drawRFrame(cardX, cardY, cardW, cardH, 3);

  if (isTemperature) {
    drawThermometerIcon(cardX + 5, cardY + 2, value);
  } else {
    drawDropletIcon(cardX + 4, cardY + 2, value);
  }

  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8(cardX + 18, cardY + 11, label);
  display.drawHLine(cardX + 4, cardY + 14, cardW - 8);

  char valueText[6];
  snprintf(valueText, sizeof(valueText), "%d", value);
  display.setFont(u8g2_font_10x20_tn);
  display.drawStr(cardX + 6, cardY + 31, valueText);

  const uint16_t valueWidth = display.getStrWidth(valueText);
  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8(cardX + 6 + valueWidth + 3, cardY + 31, unit);
}

// 太阳图示：中心圆 + 8 条光芒，亮度越高亮起的光芒越多、中心越实心。
void drawSunIcon(int16_t cx, int16_t cy, int percent) {
  static const int8_t dirX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
  static const int8_t dirY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
  // 斜向光芒用 7/10 的长度，视觉上跟上下左右的长度一样
  const uint8_t rays = (percent * 8 + 99) / 100;  // 0~8 条

  if (percent >= 10) {
    display.drawDisc(cx, cy, 5);
  } else {
    display.drawCircle(cx, cy, 5);
  }
  for (uint8_t i = 0; i < 8; i++) {
    if (i >= rays) continue;
    const bool diagonal = dirX[i] != 0 && dirY[i] != 0;
    const int16_t r0 = diagonal ? 7 : 8;
    const int16_t r1 = diagonal ? 9 : 11;
    display.drawLine(cx + dirX[i] * r0, cy + dirY[i] * r0, cx + dirX[i] * r1, cy + dirY[i] * r1);
  }
}

// 光线页：太阳图示 + 亮度百分比大字 + 横向量表。
void drawLightPage() {
  display.drawRFrame(2, 17, 124, 34, 3);
  drawSunIcon(22, 34, lightPercent);

  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8(44, 29, "亮度");

  char valueText[6];
  snprintf(valueText, sizeof(valueText), "%d", lightPercent);
  display.setFont(u8g2_font_10x20_tn);
  display.drawStr(44, 47, valueText);
  const uint16_t valueWidth = display.getStrWidth(valueText);
  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8(44 + valueWidth + 3, 47, "%");

  // 横向量表：右上角，宽 52、高 6
  display.drawFrame(70, 20, 52, 6);
  const uint8_t fillW = (50 * lightPercent) / 100;
  if (fillW > 0) display.drawBox(71, 21, fillW, 4);
}

// 仪表板：两页每 5 秒轮播（温湿度卡片 / 光线），底下一行（下次发布倒数 /
// 发布失败，右边已发布次数）固定显示。
void showDashboard() {
  display.clearBuffer();
  drawTitleBar("MQTT 数据发布");

  const bool showLightPage = (millis() / PAGE_INTERVAL_MS) % 2 == 1;
  if (showLightPage) {
    drawLightPage();
  } else if (dhtValid) {
    drawStatCard(2, humidity, "湿度", "%", false);
    drawStatCard(66, temperature, "温度", "度", true);
  } else {
    display.setFont(u8g2_font_wqy12_t_gb2312);
    display.drawUTF8(20, 32, "读取传感器中...");
    display.drawUTF8(12, 46, "请检查 DHT11 接线");
  }

  display.setFont(u8g2_font_wqy12_t_gb2312);
  char left[24];
  if (!lastPublishOk) {
    snprintf(left, sizeof(left), "发布失败");
  } else if (!dhtValid) {
    snprintf(left, sizeof(left), "等待数据");
  } else {
    const uint32_t elapsed = millis() - lastPublishMs;
    const uint32_t remainMs = elapsed >= PUBLISH_INTERVAL_MS ? 0 : PUBLISH_INTERVAL_MS - elapsed;
    snprintf(left, sizeof(left), "下次 %lus", static_cast<unsigned long>((remainMs + 999) / 1000));
  }
  display.drawUTF8(4, 62, left);

  char right[24];
  snprintf(right, sizeof(right), "已发布 %lu", static_cast<unsigned long>(publishCount));
  display.drawUTF8(124 - display.getUTF8Width(right), 62, right);

  display.sendBuffer();
}

// ---------------------------------------------------------------- 连线

// 连 WiFi：每次试 8 秒，失败就换下一次，一直试到连上为止；过程画在 OLED 上。
void connectWifi() {
  char footer[24];
  for (uint16_t attempt = 1; !wifiConnected(); attempt++) {
    Serial.print("[WiFi] 连线 ");
    Serial.print(WIFI_SSID);
    Serial.print("（第 ");
    Serial.print(attempt);
    Serial.println(" 次）");
    WiFi.begin(const_cast<char *>(WIFI_SSID), const_cast<char *>(WIFI_PASS));

    snprintf(footer, sizeof(footer), "try #%u", attempt);
    const uint32_t startMs = millis();
    while (!wifiConnected() && millis() - startMs < WIFI_ATTEMPT_MS) {
      showConnectScreen(STEP_WORKING, STEP_PENDING, footer);
      delay(100);
    }
    if (!wifiConnected()) {
      showConnectScreen(STEP_FAIL, STEP_PENDING, "WiFi failed, retry");
      delay(1000);
    }
  }

  IPAddress ip = WiFi.localIP();
  Serial.print("[WiFi] 已连线，IP = ");
  Serial.println(ip);
  snprintf(footer, sizeof(footer), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  showConnectScreen(STEP_OK, STEP_PENDING, footer);
  delay(600);
}

// 连 MQTT：失败就显示错误码，2 秒后重试；中途 WiFi 掉了就先回去重连 WiFi。
void connectMqtt() {
  char footer[24];
  for (uint16_t attempt = 1; !mqtt.connected(); attempt++) {
    if (!wifiConnected()) {
      connectWifi();
    }

    // clientId = "hub8735-" + 8 位亂数十六进位；每次连线都重新产生，公开 broker
    // 上才不会跟别人（或自己上一次还没被踢掉的旧连线）撞名。
    snprintf(clientId, sizeof(clientId), "hub8735-%04X%04X",
             static_cast<unsigned>(random(0x10000)), static_cast<unsigned>(random(0x10000)));

    Serial.print("[MQTT] 连线 ");
    Serial.print(MQTT_HOST);
    Serial.print("，clientId = ");
    Serial.print(clientId);
    Serial.print("（第 ");
    Serial.print(attempt);
    Serial.println(" 次）");

    snprintf(footer, sizeof(footer), "try #%u", attempt);
    showConnectScreen(STEP_OK, STEP_WORKING, footer);

    if (mqtt.connect(clientId)) {
      Serial.println("[MQTT] 已连线");
      showConnectScreen(STEP_OK, STEP_OK, "connected");
      delay(800);
      return;
    }

    const int rc = mqtt.state();
    Serial.print("[MQTT] 失败，rc = ");
    Serial.println(rc);
    snprintf(footer, sizeof(footer), "failed rc=%d", rc);
    showConnectScreen(STEP_OK, STEP_FAIL, footer);
    delay(2000);
  }
}

// ---------------------------------------------------------------- 主程式

void publishReading() {
  // payload 格式固定：{"temp": 25, "humi": 60, "light": 75}
  char payload[56];
  snprintf(payload, sizeof(payload), "{\"temp\": %d, \"humi\": %d, \"light\": %d}", temperature, humidity, lightPercent);

  const bool ok = mqtt.publish(MQTT_TOPIC, payload);
  lastPublishOk = ok;
  Serial.print("[MQTT] 发布 ");
  Serial.print(MQTT_TOPIC);
  Serial.print(" ");
  Serial.print(payload);
  Serial.println(ok ? " -> 成功" : " -> 失败");
  if (ok) {
    publishCount++;
    lastPublishDoneMs = millis();
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("13 DHT11 -> MQTT（mqttgo.io）启动");

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  wifiClient.setNonBlockingMode();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(30);

  analogin_init(&lightAdc, PF_2);  // A2 / IO0

  showConnectScreen(STEP_PENDING, STEP_PENDING, "starting...");
  connectWifi();
  // WiFi 连线花的时间每次都不一样，拿来当亂数种子；clientId 在每次 MQTT 连线
  // 时重新产生（见 connectMqtt）。
  randomSeed(micros() ^ (static_cast<uint32_t>(WiFi.RSSI()) * 7919UL));
  connectMqtt();
  lastPublishMs = millis() - PUBLISH_INTERVAL_MS;  // 读到第一笔有效数据就马上发布
}

void loop() {
  if (!wifiConnected() || !mqtt.connected()) {
    Serial.println("[MQTT] 断线，重新连线");
    connectMqtt();
  }
  mqtt.loop();

  if (millis() - lastSampleMs >= SAMPLE_INTERVAL_MS || lastSampleMs == 0) {
    lastSampleMs = millis();
    if (lastSampleMs == 0) lastSampleMs = 1;
    uint8_t h = 0;
    uint8_t t = 0;
    if (readDht11(DHT11_PIN, h, t)) {
      humidity = h;
      temperature = t;
      dhtValid = true;
    }
  }

  // 光线：每圈都读（很快），发布时用最新的值。
  lightRaw = analogin_read_u16(&lightAdc);
  lightPercent = lightValueToPercent(lightRaw);

  if (dhtValid && millis() - lastPublishMs >= PUBLISH_INTERVAL_MS) {
    lastPublishMs = millis();
    publishReading();
  }

  showDashboard();
  delay(200);
}

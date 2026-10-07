/*
  HUB-8735 Ultra - 空气品质看板（环保署开放 API + OLED）

  用板子内建 WiFi，透过环境部（原环保署）开放资料平台的 AQX_P_432
  （空气品质指标 AQI，即时资料）抓「桃园市 中坜」测站的数据，显示在
  OLED 上。画面每 10 秒自动切换五页（或按 FUNC 键手动换页），标题列左边固定显示 WiFi 讯号格数：

    第 1 页：AQI 大数字 + 表情图示 + 等级文字 + 6 阶色阶量表（游标）
    第 2 页：PM2.5 / PM10 / O3 / NO2 四张浓度卡片（附迷你量表）
    第 3 页：风速 + 风向罗盘 + 资料发布时间
    第 4 页：室内温湿度卡片（DHT11，温度计/水滴图示依数值填满）
    第 5 页：室内温湿度历史趋势折线图（最近 64 笔，约 3 分钟）

  DHT11：
    Data -> IO20（读取方式同 05~07，不依赖外部函式库）。

  LED / 蜂鸣器（同 09）：
    绿 IO24、黄 IO23、红 IO22；蜂鸣器讯号脚 IO19（需要 PWM）。
    室内温度 > 25 度亮红灯、室内湿度 > 80% 亮黄灯（可同时亮），都没超过
    才亮绿灯；红或黄亮起就是警报：蜂鸣器播警笛声，标题列也会出现闪烁的
    警告三角形（不管在哪一页都看得到）。警笛不使用 delay()，不会卡住
    画面与按钮；抓资料的几秒会先静音。

  OLED：
    SDA -> IO3, SCL -> IO4, VCC -> 3.3V, GND -> GND
    IO3/IO4 是硬体 I2C0，会跟 JTAG pinmux 冲突，所以用 U8g2 软体 I2C +
    自订 GPIO callback（开机只 pinMode 一次），原因见 03/06 的说明。
    这个 sketch 不接其他外部装置，所以 IO16/IO17 等保留脚位都不会碰到。

  WiFi / API 金钥：
    放在同一个资料夹的 secrets.h（已加入 .gitignore，不会被 commit）。
    第一次使用请复制 secrets.h.example 成 secrets.h，再填入自己的
    WiFi 名称、密码与 API key。

  HTTPS：
    data.moenv.gov.tw 只接受 HTTPS。这里没有设定根凭证（核心的 SSL
    client 在没有 rootCA 时会略过凭证验证），只拿来读公开的空品数据，
    不传送任何敏感资料，所以可以接受；若之后要严格验证，用
    client.setRootCA() 载入 TWCA Global Root CA 即可。

  换页：
    没动作时每 10 秒自动换页；按 FUNC 键（IO12）立刻换到下一页，并重新计时。

  资料更新：
    测站资料大约每小时才更新一次，这里每 1 分钟抓一次；抓取中标题列会显示
    updating...。抓取失败
    则 30 秒后重试，画面会沿用上一次成功的数据，标题列右边的圆点会
    变成「!」提醒资料可能过期。

  画面文字使用简体中文（OLED 字型只保证涵盖 GB2312 字集）。
  AQI 等级文字由数值自行判断，不使用 API 回传的繁体字串。
*/

#include <U8g2lib.h>
#include <WiFi.h>
#include "secrets.h"

const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;
const uint8_t DHT11_PIN = 20;  // DHT11 Data（自己刻 bit-bang，接法同 05~07）
const uint8_t LED_GREEN_PIN = 24;
const uint8_t LED_YELLOW_PIN = 23;
const uint8_t LED_RED_PIN = 22;
const uint8_t BUZZER_PIN = 19;  // 需要 PWM 脚位（tone() 会检查）
const uint8_t FUNC_PIN = 12;  // FUNC 按钮：开机高电位，按下接地变低电位

const char API_HOST[] = "data.moenv.gov.tw";
// 查询用的测站名是繁体「中壢」的 URL 编码（API 的资料是繁体）；画面显示才用简体。
// fields 只拿画面会用到的栏位，让回应保持很小。
const char API_QUERY[] =
  "&limit=1&format=json&filters=sitename,EQ,%E4%B8%AD%E5%A3%A2"
  "&fields=aqi,pm2.5,pm10,o3,no2,wind_speed,wind_direc,publishtime";

const uint32_t PAGE_INTERVAL_MS = 10000;  // 没按按钮时，每 10 秒自动换页
const uint32_t REFRESH_INTERVAL_MS = 60UL * 1000UL;
const uint32_t RETRY_INTERVAL_MS = 30UL * 1000UL;
const uint32_t HTTP_TIMEOUT_MS = 10000;
const uint8_t HUMIDITY_YELLOW_THRESHOLD = 80;  // 室内湿度 > 80% 亮黄灯
const uint8_t TEMPERATURE_RED_THRESHOLD = 25;  // 室内温度 > 25 度亮红灯
const uint16_t SIREN_LOW_HZ = 500;
const uint16_t SIREN_HIGH_HZ = 1200;
const uint16_t SIREN_PERIOD_MS = 1000;  // 警笛一次来回（低->高->低）的时间
const uint8_t PAGE_COUNT = 5;
const uint8_t HISTORY_SIZE = 64;
const uint32_t SAMPLE_INTERVAL_MS = 3000;  // DHT11 取样间隔（至少要 1 秒以上）

U8G2 display;
WiFiSSLClient client;

// 室内温湿度（DHT11）与历史纪录（环状缓冲区，趋势页用）。
bool indoorValid = false;
uint8_t indoorHumidity = 0;
uint8_t indoorTemperature = 0;
uint8_t humidityHistory[HISTORY_SIZE] = { 0 };
uint8_t temperatureHistory[HISTORY_SIZE] = { 0 };
uint8_t historyHead = 0;   // 下一笔要写入的位置（环状）
uint8_t historyCount = 0;  // 目前已经存了几笔
uint32_t lastSampleMs = 0;
bool alarmActive = false;  // 红灯或黄灯亮 = 警报中
bool sirenOn = false;
uint8_t currentPage = 0;
uint32_t lastPageChangeMs = 0;

struct AirData {
  bool valid;          // 至少成功抓过一次
  int aqi;             // -1 = 测站没有这项数据
  float pm25;
  float pm10;
  float o3;
  float no2;
  float windSpeed;
  int windDir;         // 度，风「吹来」的方向；-1 = 无资料
  char publishTime[12];  // "MM/DD HH:MM"
};

AirData air = { false, -1, -1, -1, -1, -1, -1, -1, "--/-- --:--" };
bool dataStale = false;
bool updating = false;  // 抓取中：标题列改显示 updating...
uint32_t lastFetchAttemptMs = 0;
uint32_t nextFetchDelayMs = 0;  // 开机后立刻抓第一次

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

// 讀取 DHT11。成功回傳 true，並填入濕度(%)與溫度(度C)整數值。
// DHT11 本身沒有小數精度，回傳的都是整數。
bool readDht11(uint8_t pin, uint8_t &humidity, uint8_t &temperature) {
  uint8_t data[5] = { 0, 0, 0, 0, 0 };

  // 起始信號：主動拉低至少 18ms，再放開讓感測器接手。
  // 直接切成 INPUT_PULLUP 讓上拉電阻把線拉高，不要自己再主動拉高一次，
  // 避免跟感測器準備回應的時間點互相干擾。
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delay(20);
  pinMode(pin, INPUT_PULLUP);

  // 感測器回應：先拉低約 80us，再拉高約 80us。
  if (waitForLevel(pin, LOW, 200) == 0xFFFFFFFF) {
    Serial.println("[DHT11] 沒等到感測器的回應（低電位），檢查供電/接線/腳位");
    return false;
  }
  if (waitForLevel(pin, HIGH, 200) == 0xFFFFFFFF) {
    Serial.println("[DHT11] 感測器回應的低電位之後沒有轉高電位");
    return false;
  }
  if (waitForLevel(pin, LOW, 200) == 0xFFFFFFFF) {
    Serial.println("[DHT11] 感測器回應完成後，沒有進入資料傳輸");
    return false;
  }

  // 接下來 40 個 bit：每個 bit 先 50us 低電位，接著用高電位持續時間
  // 判斷是 0 還是 1（約 26-28us=0，約 70us=1）。
  for (uint8_t i = 0; i < 40; i++) {
    if (waitForLevel(pin, HIGH, 200) == 0xFFFFFFFF) {
      Serial.print("[DHT11] 第 ");
      Serial.print(i);
      Serial.println(" 個 bit 等不到高電位");
      return false;
    }
    const uint32_t highUs = waitForLevel(pin, LOW, 200);
    if (highUs == 0xFFFFFFFF) {
      Serial.print("[DHT11] 第 ");
      Serial.print(i);
      Serial.println(" 個 bit 的高電位量不到結束時間");
      return false;
    }

    data[i / 8] <<= 1;
    if (highUs > 40) {
      data[i / 8] |= 1;
    }
  }

  const uint8_t checksum = data[0] + data[1] + data[2] + data[3];
  if (checksum != data[4]) {
    Serial.print("[DHT11] checksum 不符: 算出來是 0x");
    Serial.print(checksum, HEX);
    Serial.print("，收到的是 0x");
    Serial.println(data[4], HEX);
    return false;
  }

  humidity = data[0];
  temperature = data[2];
  return true;
}

void pushHistory(uint8_t humidity, uint8_t temperature) {
  humidityHistory[historyHead] = humidity;
  temperatureHistory[historyHead] = temperature;
  historyHead = (historyHead + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) {
    historyCount++;
  }
}

// ---------------------------------------------------------------- LED / 蜂鸣器

// 三颗灯各自独立判断（同 09）：湿度、温度分别跟自己的门槛比较，两个都没
// 超过才亮绿灯，超过的话对应的灯就亮（可能黄红同时亮）。红灯或黄灯亮
// 就算警报，给蜂鸣器判断用。
void updateLeds(uint8_t humidity, uint8_t temperature) {
  const bool humidityHigh = humidity > HUMIDITY_YELLOW_THRESHOLD;
  const bool temperatureHigh = temperature > TEMPERATURE_RED_THRESHOLD;

  digitalWrite(LED_YELLOW_PIN, humidityHigh ? HIGH : LOW);
  digitalWrite(LED_RED_PIN, temperatureHigh ? HIGH : LOW);
  digitalWrite(LED_GREEN_PIN, (!humidityHigh && !temperatureHigh) ? HIGH : LOW);

  alarmActive = humidityHigh || temperatureHigh;
}

// 警笛：频率在 500~1200Hz 之间呈三角波来回扫（同 08/09 的声音），但改成
// 用 millis() 算当下频率、一次只设一个音，不用 delay() 卡住主迴圈，
// 这样画面、按钮、DHT11 取样在警报响的时候都还能照常运作。
// 抓资料（updating）的那几秒主迴圈会被卡住，音调会固定不动，很刺耳，
// 所以抓取期间先静音，抓完再继续。
void updateSiren() {
  const bool shouldSound = alarmActive && !updating;
  if (!shouldSound) {
    if (sirenOn) {
      noTone(BUZZER_PIN);
      sirenOn = false;
    }
    return;
  }

  const uint32_t phase = millis() % SIREN_PERIOD_MS;  // 0 ~ PERIOD
  const uint32_t half = SIREN_PERIOD_MS / 2;
  const uint32_t up = phase < half ? phase : SIREN_PERIOD_MS - phase;  // 0 -> half -> 0
  const uint16_t freq = SIREN_LOW_HZ + ((SIREN_HIGH_HZ - SIREN_LOW_HZ) * up) / half;
  tone(BUZZER_PIN, freq);
  sirenOn = true;
}

// ---------------------------------------------------------------- 资料解析

// 在 JSON 本文里找 "key":"value"，把 value（去掉跳脱用的反斜线）复制到 out。
// 找不到或值是空字串回传 false。API 的数值全部都是字串型态。
bool jsonString(const char *body, const char *key, char *out, size_t outSize) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(body, pattern);
  if (!p) return false;
  p += strlen(pattern);
  while (*p == ' ' || *p == ':') p++;
  if (*p != '"') return false;
  p++;

  size_t n = 0;
  while (*p && *p != '"' && n < outSize - 1) {
    if (*p != '\\') out[n++] = *p;
    p++;
  }
  out[n] = '\0';
  return n > 0;
}

float jsonNumber(const char *body, const char *key) {
  char text[16];
  if (!jsonString(body, key, text, sizeof(text))) return -1;
  return atof(text);
}

bool parseAirData(const char *body) {
  char text[24];
  if (!strchr(body, '{')) return false;  // 没有任何一笔资料（例如测站名称查不到）

  AirData next = air;
  next.valid = true;
  next.aqi = jsonString(body, "aqi", text, sizeof(text)) ? atoi(text) : -1;
  next.pm25 = jsonNumber(body, "pm2.5");
  next.pm10 = jsonNumber(body, "pm10");
  next.o3 = jsonNumber(body, "o3");
  next.no2 = jsonNumber(body, "no2");
  next.windSpeed = jsonNumber(body, "wind_speed");
  next.windDir = jsonString(body, "wind_direc", text, sizeof(text)) ? atoi(text) : -1;

  // publishtime 格式 "2026/10/07 14:00:00" -> "10/07 14:00"
  if (jsonString(body, "publishtime", text, sizeof(text)) && strlen(text) >= 16) {
    snprintf(next.publishTime, sizeof(next.publishTime), "%.5s %.5s", text + 5, text + 11);
  }

  air = next;
  return true;
}

// 回传 true 代表抓到并解析成功。
bool fetchAirQuality() {
  Serial.println("[API] 连线 data.moenv.gov.tw ...");
  client.stop();
  if (!client.connect(API_HOST, 443)) {
    Serial.println("[API] HTTPS 连线失败");
    return false;
  }

  // 用 HTTP/1.0：伺服器就不会用 chunked 编码回应，本文可以直接解析。
  client.print("GET /api/v2/aqx_p_432?api_key=");
  client.print(API_KEY);
  client.print(API_QUERY);
  client.print(" HTTP/1.0\r\nHost: ");
  client.print(API_HOST);
  client.print("\r\nConnection: close\r\n\r\n");

  static char response[1536];
  size_t len = 0;
  const uint32_t startMs = millis();
  while (millis() - startMs < HTTP_TIMEOUT_MS) {
    while (client.available() && len < sizeof(response) - 1) {
      response[len++] = client.read();
    }
    if (!client.connected() && !client.available()) break;
    delay(5);
  }
  response[len] = '\0';
  client.stop();

  Serial.print("[API] 收到 ");
  Serial.print(len);
  Serial.println(" bytes");

  if (strncmp(response, "HTTP/1.", 7) != 0 || !strstr(response, " 200")) {
    Serial.println("[API] 回应不是 200，前 80 字元：");
    response[len < 80 ? len : 80] = '\0';
    Serial.println(response);
    return false;
  }

  const char *body = strstr(response, "\r\n\r\n");
  if (!body || !parseAirData(body + 4)) {
    Serial.println("[API] 找不到中坜测站的资料");
    return false;
  }

  Serial.print("[API] AQI=");
  Serial.print(air.aqi);
  Serial.print(" PM2.5=");
  Serial.print(air.pm25);
  Serial.print(" PM10=");
  Serial.print(air.pm10);
  Serial.print(" 发布时间=");
  Serial.println(air.publishTime);
  return true;
}

// ---------------------------------------------------------------- WiFi

bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

// 最多试 3 次、每次等 8 秒。
bool connectWifi() {
  for (uint8_t attempt = 1; attempt <= 3 && !wifiConnected(); attempt++) {
    Serial.print("[WiFi] 连线 ");
    Serial.print(WIFI_SSID);
    Serial.print("（第 ");
    Serial.print(attempt);
    Serial.println(" 次）");
    WiFi.begin(const_cast<char *>(WIFI_SSID), const_cast<char *>(WIFI_PASS));

    const uint32_t startMs = millis();
    while (!wifiConnected() && millis() - startMs < 8000) {
      showMessage("连接 WiFi", WIFI_SSID, (millis() / 400) % 4);
      delay(100);
    }
  }
  if (wifiConnected()) {
    Serial.print("[WiFi] 已连线，IP = ");
    Serial.println(WiFi.localIP());
  }
  return wifiConnected();
}

// ---------------------------------------------------------------- 绘图

// AQI 等级：0 良好 / 1 普通 / 2 对敏感族群不健康 / 3 对所有族群不健康 /
// 4 非常不健康 / 5 危害（环境部分级：50/100/150/200/300）。
uint8_t aqiLevel(int aqi) {
  if (aqi <= 50) return 0;
  if (aqi <= 100) return 1;
  if (aqi <= 150) return 2;
  if (aqi <= 200) return 3;
  if (aqi <= 300) return 4;
  return 5;
}

const char *const LEVEL_TEXT[6] = {
  "良好", "普通", "对敏感族群不健康", "对所有族群不健康", "非常不健康", "危害"
};

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

// 左上角的讯号格：4 根由矮到高的长条，有讯号的实心，没讯号的只留底下一个点；
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

// 反白标题列：左边 WiFi 讯号格，中间标题（抓取中改显示 updating...），
// 右边页码圆点（目前页实心），资料过期时圆点改画 "!"。
void drawTitleBar(const char *title, uint8_t page) {
  display.drawBox(0, 0, 128, 14);
  drawWifiIcon(3, 11);

  display.setDrawColor(0);
  if (alarmActive) {
    // 警报中：左边讯号格旁边闪烁的警告三角形，不管在哪一页都看得到
    if ((millis() / 400) % 2 == 0) {
      display.drawTriangle(17, 11, 25, 11, 21, 3);
    } else {
      display.drawLine(17, 11, 25, 11);
      display.drawLine(17, 11, 21, 3);
      display.drawLine(25, 11, 21, 3);
    }
  }
  display.setFont(u8g2_font_wqy12_t_gb2312);
  if (updating) title = "updating...";
  display.drawUTF8((128 - display.getUTF8Width(title)) / 2, 11, title);

  if (dataStale) {
    display.drawUTF8(119, 11, "!");
  } else {
    for (uint8_t i = 0; i < PAGE_COUNT; i++) {
      const int16_t cx = 104 + i * 5;
      if (i == page) {
        display.drawDisc(cx, 7, 2);
      } else {
        display.drawPixel(cx, 7);
      }
    }
  }
  display.setDrawColor(1);
}

// 把数值转成显示字串：-1 -> "--"，大于等于 100 或整数显示整数，其余留一位小数。
void formatValue(char *out, size_t size, float v) {
  if (v < 0) {
    snprintf(out, size, "--");
  } else if (v >= 100 || v == static_cast<int>(v)) {
    snprintf(out, size, "%d", static_cast<int>(v + 0.5f));
  } else {
    snprintf(out, size, "%d.%d", static_cast<int>(v), static_cast<int>(v * 10 + 0.5f) % 10);
  }
}

// 表情脸，(cx, cy) 是圆心，半径 11。等级越高表情越差。
void drawFace(int16_t cx, int16_t cy, uint8_t level) {
  display.drawCircle(cx, cy, 11);

  if (level >= 4) {
    // X 眼睛
    display.drawLine(cx - 6, cy - 5, cx - 3, cy - 2);
    display.drawLine(cx - 6, cy - 2, cx - 3, cy - 5);
    display.drawLine(cx + 3, cy - 5, cx + 6, cy - 2);
    display.drawLine(cx + 3, cy - 2, cx + 6, cy - 5);
  } else {
    display.drawDisc(cx - 4, cy - 3, 1);
    display.drawDisc(cx + 4, cy - 3, 1);
  }

  switch (level) {
    case 0:  // 笑
      display.drawCircle(cx, cy + 1, 6, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    case 1:  // 平
      display.drawHLine(cx - 4, cy + 5, 9);
      break;
    case 2:  // 微皱
      display.drawCircle(cx, cy + 10, 6, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    default:  // 皱眉
      display.drawCircle(cx, cy + 11, 7, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
  }
}

// 横向量表：外框 + 依数值填满，并在 50/100/150/200 画刻度（反色，在填色上也看得到）。
// 量表范围 0~300，超过就满格。
void drawAqiGauge(int16_t x, int16_t y, uint8_t w, uint8_t h, int aqi) {
  display.drawFrame(x, y, w, h);
  const int clamped = aqi < 0 ? 0 : (aqi > 300 ? 300 : aqi);
  const uint8_t fillW = (static_cast<uint32_t>(w - 2) * clamped) / 300;
  if (fillW > 0) display.drawBox(x + 1, y + 1, fillW, h - 2);

  display.setDrawColor(2);  // XOR
  for (uint8_t i = 1; i <= 4; i++) {
    const int16_t tx = x + 1 + (static_cast<uint32_t>(w - 2) * i * 50) / 300;
    display.drawVLine(tx, y + 1, h - 2);
  }
  display.setDrawColor(1);
}

// ---- 室内温湿度图示与折线图（沿用 06 的做法）----

// 水滴圖示（濕度用），(x, y) 是圖示外框左上角，尺寸約 10x14。
// 空心輪廓 + 依濕度百分比從底部往上填滿，跟溫度計一樣是視覺化量表。
// 用 setClipWindow 限制只在下面那一段範圍內重畫實心圖案，做出液面效果。
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
  // 圓心在三角形底邊下方一點點，圓的上半弧會露出來、破壞水滴的外型，
  // 只畫下半圓（左下+右下象限）跟三角形的兩條斜邊接起來就好。
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

// 溫度計圖示（溫度用），(x, y) 是圖示外框左上角，尺寸約 8x17。
// 管身裡的填充高度會依溫度值變化，是視覺化的量表效果。
// 10~40 度對應 0~100% 填滿高度（跟室內常見溫度範圍對得上，解析度較好）。
void drawThermometerIcon(int16_t x, int16_t y, uint8_t temperature) {
  const uint8_t stemHeight = 10;
  const uint8_t bulbCy = y + stemHeight + 4;

  display.drawRFrame(x + 2, y, 4, stemHeight, 1);
  display.drawDisc(x + 4, bulbCy, 4);

  const uint8_t clampedTemp = temperature < 10 ? 10 : (temperature > 40 ? 40 : temperature);
  const uint8_t percent = (static_cast<uint16_t>(clampedTemp - 10) * 100) / 30;
  const uint8_t innerHeight = stemHeight - 2;
  const uint8_t fillHeight = (static_cast<uint16_t>(percent) * innerHeight) / 100;
  if (fillHeight > 0) {
    display.drawBox(x + 3, y + 1 + (innerHeight - fillHeight), 2, fillHeight);
  }
}

// 下面兩個是給折線圖頁面用的放大版圖示：左右並排騰出空間後，圖示可以
// 畫大一點，填充比例邏輯跟小圖示完全一樣。

void drawDropletIconBig(int16_t x, int16_t y, uint8_t humidityPercent) {
  const int16_t tipX = x + 8;
  const int16_t tipY = y;
  const int16_t leftX = x + 1;
  const int16_t rightX = x + 15;
  const int16_t baseY = y + 11;
  const int16_t circleCx = x + 8;
  const int16_t circleCy = y + 14;
  const uint8_t circleR = 8;
  const int16_t iconBottom = circleCy + circleR;

  display.drawLine(tipX, tipY, leftX, baseY);
  display.drawLine(tipX, tipY, rightX, baseY);
  // 圓心在三角形底邊下方一點點，圓的上半弧會露出來、破壞水滴的外型，
  // 只畫下半圓（左下+右下象限）跟三角形的兩條斜邊接起來就好。
  display.drawCircle(circleCx, circleCy, circleR, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);

  const uint8_t clampedPercent = humidityPercent > 100 ? 100 : humidityPercent;
  const uint8_t iconHeight = iconBottom - tipY;
  const uint8_t fillHeight = (static_cast<uint16_t>(clampedPercent) * iconHeight) / 100;
  if (fillHeight > 0) {
    display.setClipWindow(x, iconBottom - fillHeight, x + 16, iconBottom);
    display.drawTriangle(tipX, tipY, leftX, baseY, rightX, baseY);
    display.drawDisc(circleCx, circleCy, circleR);
    display.setMaxClipWindow();
  }
}

void drawThermometerIconBig(int16_t x, int16_t y, uint8_t temperature) {
  const uint8_t stemHeight = 16;
  const uint8_t stemWidth = 6;
  const uint8_t bulbRadius = 7;
  const int16_t bulbCy = y + stemHeight + bulbRadius;

  display.drawRFrame(x, y, stemWidth, stemHeight, 2);
  display.drawCircle(x + stemWidth / 2, bulbCy, bulbRadius);

  const uint8_t clampedTemp = temperature < 10 ? 10 : (temperature > 40 ? 40 : temperature);
  const uint8_t percent = (static_cast<uint16_t>(clampedTemp - 10) * 100) / 30;
  const uint8_t innerHeight = stemHeight - 2;
  const uint8_t fillHeight = (static_cast<uint16_t>(percent) * innerHeight) / 100;

  display.drawDisc(x + stemWidth / 2, bulbCy, bulbRadius - 2);
  if (fillHeight > 0) {
    display.drawBox(x + 1, y + 1 + (innerHeight - fillHeight), stemWidth - 2, fillHeight);
  }
}

// 折線圖：把環狀緩衝區裡最近 count 筆資料畫成折線，依資料本身的
// 最大/最小值自動縮放，讓小小的變化量在螢幕上也看得出來。
void drawLineChart(int16_t x, int16_t y, uint8_t w, uint8_t h, const uint8_t *history) {
  display.drawFrame(x, y, w, h);

  if (historyCount < 2) {
    return;
  }

  uint8_t rawMin = 255;
  uint8_t rawMax = 0;
  for (uint8_t i = 0; i < historyCount; i++) {
    const uint8_t idx = (historyHead + HISTORY_SIZE - historyCount + i) % HISTORY_SIZE;
    const uint8_t v = history[idx];
    if (v < rawMin) rawMin = v;
    if (v > rawMax) rawMax = v;
  }

  // 上下都留一點空間，不要讓線一開始就貼著最底部（等於從 0 起跳），
  // 這樣後續數值往上或往下變化時才有地方可以看出來。
  const uint8_t rawRange = rawMax - rawMin;
  uint8_t padding = rawRange / 3;
  if (padding < 2) padding = 2;

  const int16_t minValue = (static_cast<int16_t>(rawMin) - padding < 0) ? 0 : rawMin - padding;
  const int16_t maxValue = static_cast<int16_t>(rawMax) + padding;

  const int16_t innerX = x + 1;
  const int16_t innerY = y + 1;
  const uint8_t innerW = w - 2;
  const uint8_t innerH = h - 2;

  int16_t prevX = 0;
  int16_t prevY = 0;
  for (uint8_t i = 0; i < historyCount; i++) {
    const uint8_t idx = (historyHead + HISTORY_SIZE - historyCount + i) % HISTORY_SIZE;
    const uint8_t v = history[idx];

    const int16_t px = innerX + (static_cast<uint32_t>(i) * (innerW - 1)) / (historyCount > 1 ? historyCount - 1 : 1);
    const int16_t py = innerY + innerH - 1 - (static_cast<uint32_t>(v - minValue) * (innerH - 1)) / (maxValue - minValue);

    if (i > 0) {
      display.drawLine(prevX, prevY, px, py);
    }
    prevX = px;
    prevY = py;
  }
}

void drawAqiPage() {
  drawTitleBar("中坜空气质量", 0);

  if (air.aqi < 0) {
    display.setFont(u8g2_font_wqy12_t_gb2312);
    display.drawUTF8(14, 40, "测站暂无 AQI 数据");
    return;
  }

  const uint8_t level = aqiLevel(air.aqi);

  char aqiText[6];
  snprintf(aqiText, sizeof(aqiText), "%d", air.aqi);
  display.setFont(u8g2_font_logisoso24_tn);
  display.drawStr(6, 41, aqiText);

  drawFace(98, 28, level);

  // 等级文字置中；最长的「对敏感族群不健康」96px 放得下。
  display.setFont(u8g2_font_wqy12_t_gb2312);
  const char *levelText = LEVEL_TEXT[level];
  display.drawUTF8((128 - display.getUTF8Width(levelText)) / 2, 52, levelText);

  drawAqiGauge(2, 56, 124, 7, air.aqi);
}

// 一张污染物卡片：标签（含单位）+ 大字数值 + 右侧迷你量表（数值/参考上限）。
void drawPollutantCard(int16_t x, int16_t y, const char *label, float value, float limit) {
  const uint8_t w = 61;
  const uint8_t h = 22;
  display.drawRFrame(x, y, w, h, 3);

  display.setFont(u8g2_font_5x7_tr);
  display.drawStr(x + 4, y + 8, label);

  char valueText[8];
  formatValue(valueText, sizeof(valueText), value);
  display.setFont(u8g2_font_7x14B_tn);
  display.drawStr(x + 4, y + 20, valueText);

  // 迷你量表：垂直、高 10px，由下往上填满
  const int16_t barX = x + w - 9;
  const int16_t barY = y + 9;
  display.drawFrame(barX, barY, 5, 11);
  if (value >= 0) {
    float ratio = value / limit;
    if (ratio > 1) ratio = 1;
    const uint8_t fillH = static_cast<uint8_t>(ratio * 9 + 0.5f);
    if (fillH > 0) display.drawBox(barX + 1, barY + 10 - fillH, 3, fillH);
  }
}

void drawPollutantPage() {
  drawTitleBar("污染物浓度", 1);
  drawPollutantCard(2, 16, "PM2.5 ug/m3", air.pm25, 55);
  drawPollutantCard(65, 16, "PM10 ug/m3", air.pm10, 125);
  drawPollutantCard(2, 40, "O3 ppb", air.o3, 125);
  drawPollutantCard(65, 40, "NO2 ppb", air.no2, 100);
}

const char *const WIND_DIR_TEXT[8] = { "北", "东北", "东", "东南", "南", "西南", "西", "西北" };

// 罗盘：圆圈 + 四个刻度 + 指针。风向角度是风「吹来」的方向，指针画成
// 风吹去的方向（角度 + 180），符合气象图上箭头的习惯。
void drawCompass(int16_t cx, int16_t cy, uint8_t r, int windDir) {
  display.drawCircle(cx, cy, r);
  display.drawVLine(cx, cy - r, 3);
  display.drawVLine(cx, cy + r - 2, 3);
  display.drawHLine(cx - r, cy, 3);
  display.drawHLine(cx + r - 2, cy, 3);

  display.setFont(u8g2_font_5x7_tr);
  display.drawStr(cx - 2, cy - r - 2, "N");

  if (windDir < 0) {
    display.drawDisc(cx, cy, 2);
    return;
  }

  const float rad = ((windDir + 180) % 360) * 3.14159265f / 180.0f;
  const float dx = sinf(rad);
  const float dy = -cosf(rad);
  const int16_t tipX = cx + static_cast<int16_t>(dx * (r - 3));
  const int16_t tipY = cy + static_cast<int16_t>(dy * (r - 3));
  const int16_t tailX = cx - static_cast<int16_t>(dx * (r - 8));
  const int16_t tailY = cy - static_cast<int16_t>(dy * (r - 8));
  display.drawLine(tailX, tailY, tipX, tipY);

  // 箭头：从尖端往回退 5px，左右各偏 3px
  const float px = -dy;  // 垂直方向
  const float py = dx;
  const int16_t baseX = tipX - static_cast<int16_t>(dx * 5);
  const int16_t baseY = tipY - static_cast<int16_t>(dy * 5);
  display.drawTriangle(tipX, tipY,
                       baseX + static_cast<int16_t>(px * 3), baseY + static_cast<int16_t>(py * 3),
                       baseX - static_cast<int16_t>(px * 3), baseY - static_cast<int16_t>(py * 3));
  display.drawDisc(cx, cy, 1);
}

void drawWindPage() {
  drawTitleBar("风速风向", 2);

  drawCompass(30, 42, 17, air.windDir);

  display.drawVLine(62, 18, 44);

  char speedText[8];
  formatValue(speedText, sizeof(speedText), air.windSpeed);
  display.setFont(u8g2_font_10x20_tn);
  display.drawStr(70, 37, speedText);
  const uint16_t speedWidth = display.getStrWidth(speedText);
  display.setFont(u8g2_font_6x10_tr);
  display.drawStr(70 + speedWidth + 3, 37, "m/s");

  display.setFont(u8g2_font_wqy12_t_gb2312);
  if (air.windDir >= 0) {
    const char *name = WIND_DIR_TEXT[((air.windDir + 22) / 45) % 8];
    char dirText[24];
    snprintf(dirText, sizeof(dirText), "%s风 %d", name, air.windDir);
    display.drawUTF8(70, 52, dirText);
  } else {
    display.drawUTF8(70, 52, "风向 --");
  }

  display.setFont(u8g2_font_5x7_tr);
  display.drawStr(70, 62, air.publishTime);
}

// 一张数值卡片：外框 + 图示 + 标签 + 大字数值 + 单位（沿用 06 的卡片）。
void drawStatCard(int16_t cardX, uint8_t value, const char *label, const char *unit, bool isTemperature) {
  const uint8_t cardY = 17;
  const uint8_t cardW = 60;
  const uint8_t cardH = 45;

  display.drawRFrame(cardX, cardY, cardW, cardH, 3);

  if (isTemperature) {
    drawThermometerIcon(cardX + 5, cardY + 3, value);
  } else {
    drawDropletIcon(cardX + 4, cardY + 3, value);
  }

  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8(cardX + 18, cardY + 12, label);
  display.drawHLine(cardX + 4, cardY + 16, cardW - 8);

  char valueText[6];
  snprintf(valueText, sizeof(valueText), "%d", value);
  display.setFont(u8g2_font_10x20_tn);
  display.drawStr(cardX + 6, cardY + 40, valueText);

  const uint16_t valueWidth = display.getStrWidth(valueText);
  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8(cardX + 6 + valueWidth + 3, cardY + 40, unit);
}

void drawIndoorPage() {
  drawTitleBar("室内温湿度", 3);
  if (indoorValid) {
    drawStatCard(2, indoorHumidity, "湿度", "%", false);
    drawStatCard(66, indoorTemperature, "温度", "度", true);
  } else {
    display.setFont(u8g2_font_wqy12_t_gb2312);
    display.drawUTF8(20, 36, "读取失败");
    display.drawUTF8(12, 54, "请检查接线与供电");
  }
}

void drawTrendPage() {
  drawTitleBar("温湿度趋势", 4);

  // 左右并排：左边温度、右边湿度，图示依数值比例填满，下方是折线图。
  display.drawVLine(64, 15, 48);

  drawThermometerIconBig(29, 15, indoorTemperature);
  drawLineChart(4, 47, 56, 15, temperatureHistory);

  drawDropletIconBig(88, 15, indoorHumidity);
  drawLineChart(68, 47, 56, 15, humidityHistory);
}

void showMessage(const char *line1, const char *line2, uint8_t dots) {
  display.clearBuffer();
  drawTitleBar("空气质量看板", 0);
  display.setFont(u8g2_font_wqy12_t_gb2312);

  char text[32];
  snprintf(text, sizeof(text), "%s%.*s", line1, dots, "...");
  display.drawUTF8((128 - display.getUTF8Width(line1)) / 2, 36, text);

  if (line2) {
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr((128 - display.getStrWidth(line2)) / 2, 54, line2);
  }
  display.sendBuffer();
}

void showPage() {
  display.clearBuffer();
  const uint8_t page = currentPage;
  if (page == 0) {
    drawAqiPage();
  } else if (page == 1) {
    drawPollutantPage();
  } else if (page == 2) {
    drawWindPage();
  } else if (page == 3) {
    drawIndoorPage();
  } else if (historyCount >= 2) {
    drawTrendPage();
  } else {
    drawIndoorPage();  // 历史资料还不够画折线时先停在卡片页
  }
  display.sendBuffer();
}

// ---------------------------------------------------------------- 主程式

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("10 空气品质看板 启动（AQX_P_432 / 桃园市中坜）");

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  pinMode(FUNC_PIN, INPUT_PULLUP);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  digitalWrite(LED_GREEN_PIN, LOW);
  digitalWrite(LED_YELLOW_PIN, LOW);
  digitalWrite(LED_RED_PIN, LOW);
  showMessage("启动中", NULL, 0);
}

// 画一帧目前该显示的内容（有资料就轮播页面，没有就显示等待画面）。
void render() {
  if (air.valid) {
    showPage();
  } else {
    showMessage(updating ? "获取数据" : "等待数据", dataStale ? "retry in 30s" : NULL, (millis() / 400) % 4);
  }
}

// FUNC 按钮被按下（高 -> 低的瞬间）回传 true，带简单防弹跳。
bool funcPressed() {
  static bool wasHigh = true;
  const bool high = (digitalRead(FUNC_PIN) == HIGH);
  const bool pressed = wasHigh && !high;
  wasHigh = high;
  if (pressed) delay(20);
  return pressed;
}

void loop() {
  const bool firstFetch = (lastFetchAttemptMs == 0);
  if (firstFetch || millis() - lastFetchAttemptMs >= nextFetchDelayMs) {
    bool ok = false;

    updating = true;
    updateSiren();  // 抓取期间静音
    render();  // 先把 updating... 画出来，再进入会卡住几秒的抓取
    if (wifiConnected() || connectWifi()) {
      ok = fetchAirQuality();
    } else {
      Serial.println("[WiFi] 连线失败");
      if (!air.valid) showMessage("WiFi 连接失败", "check secrets.h", 0);
    }
    updating = false;

    lastFetchAttemptMs = millis();
    if (lastFetchAttemptMs == 0) lastFetchAttemptMs = 1;
    dataStale = !ok;
    nextFetchDelayMs = ok ? REFRESH_INTERVAL_MS : RETRY_INTERVAL_MS;
  }

  // 室内温湿度：每 3 秒读一次 DHT11，成功才更新数值并存进历史纪录。
  if (millis() - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = millis();
    uint8_t humidity = 0;
    uint8_t temperature = 0;
    if (readDht11(DHT11_PIN, humidity, temperature)) {
      indoorValid = true;
      indoorHumidity = humidity;
      indoorTemperature = temperature;
      pushHistory(humidity, temperature);
      updateLeds(humidity, temperature);
    }
  }

  // 没按按钮就每 10 秒自动换到下一页。
  if (millis() - lastPageChangeMs >= PAGE_INTERVAL_MS) {
    currentPage = (currentPage + 1) % PAGE_COUNT;
    lastPageChangeMs = millis();
  }

  render();

  // 等 250ms 再重画，期间每 10ms 看一次按钮，按下就马上换到下一页，
  // 并重新计 10 秒（不会刚按完又马上自动跳页）。
  for (uint8_t i = 0; i < 25; i++) {
    if (funcPressed()) {
      currentPage = (currentPage + 1) % PAGE_COUNT;
      lastPageChangeMs = millis();
      Serial.print("[FUNC] 换到第 ");
      Serial.print(currentPage + 1);
      Serial.println(" 页");
      break;
    }
    updateSiren();
    delay(10);
  }
}

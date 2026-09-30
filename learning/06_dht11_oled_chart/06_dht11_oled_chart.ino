/*
  HUB-8735 Ultra - DHT11 溫濕度感測器 + OLED 儀表板 + 歷史趨勢折線圖

  這是 05_dht11_oled 的「華麗版」分支：保留卡片式儀表板，額外加入
  溫度/濕度歷史紀錄折線圖，畫面每幾秒自動切換一次，方便觀察變化量。
  05 本身則退回原本單純顯示數值的版本。

  DHT11：
    Data -> IO20（單線通訊，自己刻 bit-bang 讀取，不依賴外部函式庫）
    腳位選擇原因見 05_dht11_oled 的說明（IO16/17 是系統保留的除錯/燒
    錄序列埠腳位，不能接外部裝置）。

  OLED：
    SDA -> IO3, SCL -> IO4, VCC -> 3.3V, GND -> GND
    IO3/IO4 是硬體 I2C0，會跟這顆 SoC 的 JTAG pinmux 衝突，所以用
    U8g2 軟體 I2C，並用自訂的 GPIO callback（開機只 pinMode 一次，
    之後全用 digitalWrite），避開重複呼叫 pinMode() 的速度陷阱。

  畫面文字全部使用中文（簡體，因為 OLED 字型只保證涵蓋 GB2312 字集；
  試過繁體字用 unifont 顯示，後來決定還是維持簡體）。

  歷史趨勢圖：
    每次讀取成功就把數值存進一個 64 筆的環狀緩衝區，畫面每 4 秒
    自動在「目前數值卡片」和「歷史趨勢折線圖」兩個頁面之間切換。
    折線圖會依緩衝區裡目前的最大/最小值自動縮放高度，並且上下各留
    一點空間、不會讓線一開始就貼著最底部，方便觀察後續變化。

    折線圖頁面改成左右並排（中間一條分隔線）：左邊溫度、右邊濕度，
    上方各放一個放大版的溫度計/水滴圖示，下方各放一個縮窄的折線圖。
    兩個圖示都是空心輪廓 + 依比例從底部往上填滿（用 setClipWindow
    限制填色範圍做出液面效果），溫度計 10~40 度對應 0~100% 填滿，
    水滴 0~100% 濕度直接對應填滿——第一頁儀表板卡片上的小圖示也用
    同一套邏輯，水滴不再是永遠全滿的固定圖案。
*/

#include <U8g2lib.h>

const uint8_t DHT11_PIN = 20;
const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;

const uint8_t HISTORY_SIZE = 64;
const uint32_t PAGE_INTERVAL_MS = 4000;   // 每個頁面顯示 4 秒
const uint32_t SAMPLE_INTERVAL_MS = 2000; // DHT11 至少要間隔 1 秒以上

U8G2 display;

uint8_t humidityHistory[HISTORY_SIZE] = { 0 };
uint8_t temperatureHistory[HISTORY_SIZE] = { 0 };
uint8_t historyHead = 0;   // 下一筆要寫入的位置（環狀）
uint8_t historyCount = 0;  // 目前已經存了幾筆

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

// 反白標題列，正中央置中文字。
void drawTitleBar(const char *title) {
  display.drawBox(0, 0, 128, 14);
  display.setDrawColor(0);
  display.setFont(u8g2_font_wqy12_t_gb2312);
  const uint16_t titleWidth = display.getUTF8Width(title);
  display.drawUTF8((128 - titleWidth) / 2, 11, title);
  display.setDrawColor(1);
}

// 一張數值卡片：外框 + 圖示 + 中文標籤 + 大字數值 + 單位。
void drawStatCard(int16_t cardX, uint8_t value, const char *label, const char *unit, bool isTemperature) {
  const uint8_t cardY = 17;
  const uint8_t cardW = 60;
  const uint8_t cardH = 45;

  display.setDrawColor(1);
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

void drawDashboardPage(bool ok, uint8_t humidity, uint8_t temperature) {
  drawTitleBar("温湿度环境监测");

  if (ok) {
    drawStatCard(2, humidity, "湿度", "%", false);
    drawStatCard(66, temperature, "温度", "度", true);
  } else {
    display.setFont(u8g2_font_wqy12_t_gb2312);
    display.drawUTF8(20, 36, "读取失败");
    display.drawUTF8(12, 54, "请检查接线与供电");
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

void drawTrendPage(uint8_t humidity, uint8_t temperature) {
  drawTitleBar("历史趋势图");

  // 改成左右並排：左邊溫度、右邊濕度，中間一條分隔線。圖示放大版畫
  // 在上方，折線圖縮窄放在下方，兩個圖示都會依數值比例把圖案填滿。
  display.drawVLine(64, 15, 48);

  drawThermometerIconBig(29, 15, temperature);
  drawLineChart(4, 47, 56, 15, temperatureHistory);

  drawDropletIconBig(88, 15, humidity);
  drawLineChart(68, 47, 56, 15, humidityHistory);
}

void showReading(bool ok, uint8_t humidity, uint8_t temperature) {
  display.clearBuffer();

  const bool showTrendPage = (millis() / PAGE_INTERVAL_MS) % 2 == 1;
  if (showTrendPage && historyCount >= 2) {
    drawTrendPage(humidity, temperature);
  } else {
    drawDashboardPage(ok, humidity, temperature);
  }

  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("06 DHT11 + OLED + 歷史趨勢圖 啟動，Data 腳位 IO20");

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();
}

void loop() {
  uint8_t humidity = 0;
  uint8_t temperature = 0;
  const bool ok = readDht11(DHT11_PIN, humidity, temperature);

  if (ok) {
    pushHistory(humidity, temperature);
  }

  showReading(ok, humidity, temperature);

  delay(SAMPLE_INTERVAL_MS);
}

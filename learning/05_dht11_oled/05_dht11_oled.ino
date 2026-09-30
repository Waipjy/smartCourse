/*
  HUB-8735 Ultra - DHT11 溫濕度感測器 + OLED 顯示

  DHT11：
    Data -> IO20（單線通訊，自己刻 bit-bang 讀取，不依賴外部函式庫）

    原本接在 IO16，結果整片板子燒錄不進去（ping retry fail /
    NOR flashloader loading fail）。查 variant.cpp 才發現全部腳位裡
    只有 D14/D16/D17 的 MODE_NOT_INITIAL 被拿掉，代表這幾根開機就
    被系統保留使用中，D16/D17 又都是 PIO_UART、成對出現，幾乎可以
    確定是板子內建的除錯/燒錄用序列埠腳位。DHT11 接在上面會干擾
    這條線，導致不管燒哪份程式都握手失敗，跟程式碼本身無關。
    IO20（PF_7）的 MODE_NOT_INITIAL 沒有被拿掉，不是系統保留腳位，
    雖然表上也標了 SPI/PWM，但那只是「這根腳位也支援」的能力，不
    代表被占用，可以放心當一般 GPIO 用。

  OLED：
    SDA -> IO3, SCL -> IO4, VCC -> 3.3V, GND -> GND
    IO3/IO4 是硬體 I2C0，會跟這顆 SoC 的 JTAG pinmux 衝突，所以用
    U8g2 軟體 I2C，並用自訂的 GPIO callback（開機只 pinMode 一次，
    之後全用 digitalWrite），避開重複呼叫 pinMode() 的速度陷阱
   （跟 03_oled_hello_u8g2 / 04_light_sensor_oled 原因相同）。

  畫面文字全部使用中文（簡體，因為 OLED 字型只保證涵蓋 GB2312 字集，
  繁體字有些字型不一定有對應字模，用簡體才能確保能正常顯示）。
*/

#include <U8g2lib.h>

const uint8_t DHT11_PIN = 20;
const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;

U8G2 display;

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

  Serial.print("[DHT11] 收到原始資料: ");
  for (uint8_t i = 0; i < 5; i++) {
    Serial.print(data[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

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

void showReading(bool ok, uint8_t humidity, uint8_t temperature) {
  display.clearBuffer();
  display.setFont(u8g2_font_wqy14_t_gb2312);

  display.drawUTF8(0, 16, "温湿度感测器");
  display.drawHLine(0, 20, 128);

  if (ok) {
    char line[32];

    snprintf(line, sizeof(line), "温度：%d 度", temperature);
    display.drawUTF8(0, 42, line);

    snprintf(line, sizeof(line), "湿度：%d %%", humidity);
    display.drawUTF8(0, 62, line);
  } else {
    display.drawUTF8(0, 42, "读取失败");
    display.drawUTF8(0, 62, "请检查接线");
  }

  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("05 DHT11 + OLED 啟動，Data 腳位 IO20");

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();
}

void loop() {
  uint8_t humidity = 0;
  uint8_t temperature = 0;
  const bool ok = readDht11(DHT11_PIN, humidity, temperature);

  showReading(ok, humidity, temperature);

  delay(2000);  // DHT11 取樣間隔至少要 1 秒以上，這裡用 2 秒穩一點。
}

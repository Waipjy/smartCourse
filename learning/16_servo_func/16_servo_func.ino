/*
  HUB-8735 Ultra - FUNC 键控制伺服马达：0 度 -> 90 度 -> 180 度 -> 0 度 ... 轮回

  接线：
    伺服马达讯号线（橘 / 黄） -> IO21（PF_8，支援 PWM）
    伺服马达电源线（红）      -> 外部 5V（建议）；GND（棕 / 黑）要跟板子的 GND 共地
    伺服马达启动时电流很大，直接用板子的 5V 脚供电可能让板子重开机，
    建议接独立的 5V 电源（或至少 USB 电源够力的 5V 脚），但 GND 一定要接在一起。
    讯号线 3.3V 的准位对一般伺服马达（SG90、MG90S 等）就够了。

  操作：
    按一下 FUNC 键（IO12）：目标角度依序 0 -> 90 -> 180 -> 0 -> 90 -> ... 循环。
    开机时先转到 0 度。

  转动方式：
    不是瞬间跳到目标，而是每 15 ms 转 3 度（约 200 度/秒），转动比较平顺，
    也减少启动电流冲击与齿轮撞击。OLED 画面上的指针会跟着实际的转动角度移动。
    转动途中再按 FUNC，会从「目前的位置」继续往新的目标转，不会卡住。

  程式库：
    核心内建的 AmebaServo（Analog 函式库），官方范例是 Analog/examples/PWM_ServoControl。

  OLED：SDA -> IO3, SCL -> IO4（U8g2 软体 I2C，原因见 03/06）。
  FUNC 键：IO12（开机高电位，按下接地变低电位）。

  画面文字使用简体中文（OLED 字型只保证涵盖 GB2312 字集）。
*/

#include <U8g2lib.h>
#include <AmebaServo.h>

const uint8_t SERVO_PIN = 21;
const uint8_t FUNC_PIN = 12;
const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;

const int ANGLES[3] = { 0, 90, 180 };
const uint8_t STEP_DEG = 3;       // 每次转几度
const uint16_t STEP_MS = 15;      // 每隔多久转一次

U8G2 display;
AmebaServo servo;

uint8_t targetIndex = 0;          // 目前的目标是 ANGLES 的第几个
int currentAngle = 0;             // 伺服马达目前实际被指令到的角度
uint32_t lastStepMs = 0;

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

// 半圆仪表：圆心 (cx, cy)、半径 r，0 度在左边、90 度在正上方、180 度在右边。
void drawGauge(int16_t cx, int16_t cy, uint8_t r, int angle, int target) {
  display.drawCircle(cx, cy, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  display.drawHLine(cx - r, cy, 2 * r + 1);

  // 0 / 90 / 180 三个刻度，以及目标角度的小三角
  for (uint8_t i = 0; i < 3; i++) {
    const float a = ANGLES[i] * 3.14159265f / 180.0f;
    const int16_t x0 = cx - static_cast<int16_t>(cosf(a) * (r - 1));
    const int16_t y0 = cy - static_cast<int16_t>(sinf(a) * (r - 1));
    const int16_t x1 = cx - static_cast<int16_t>(cosf(a) * (r - 6));
    const int16_t y1 = cy - static_cast<int16_t>(sinf(a) * (r - 6));
    display.drawLine(x0, y0, x1, y1);
  }
  const float ta = target * 3.14159265f / 180.0f;
  const int16_t tx = cx - static_cast<int16_t>(cosf(ta) * (r + 4));
  const int16_t ty = cy - static_cast<int16_t>(sinf(ta) * (r + 4));
  display.drawDisc(tx, ty, 2);

  // 指针
  const float pa = angle * 3.14159265f / 180.0f;
  const int16_t px = cx - static_cast<int16_t>(cosf(pa) * (r - 4));
  const int16_t py = cy - static_cast<int16_t>(sinf(pa) * (r - 4));
  display.drawLine(cx, cy, px, py);
  display.drawLine(cx + 1, cy, px + 1, py);
  display.drawDisc(cx, cy, 3);
}

void showScreen() {
  const int target = ANGLES[targetIndex];

  display.clearBuffer();

  // 标题列
  display.drawBox(0, 0, 128, 14);
  display.setDrawColor(0);
  display.setFont(u8g2_font_wqy12_t_gb2312);
  const char *title = "伺服马达";
  display.drawUTF8((128 - display.getUTF8Width(title)) / 2, 11, title);
  display.setDrawColor(1);

  // 左边：目前角度大字 + 目标
  char angleText[8];
  snprintf(angleText, sizeof(angleText), "%d", currentAngle);
  display.setFont(u8g2_font_logisoso24_tn);
  display.drawStr(4, 46, angleText);
  const uint16_t w = display.getStrWidth(angleText);
  display.setFont(u8g2_font_wqy12_t_gb2312);
  display.drawUTF8(4 + w + 2, 46, "度");

  char targetText[24];
  snprintf(targetText, sizeof(targetText), "目标 %d度", target);
  display.drawUTF8(4, 61, targetText);

  // 右边：半圆仪表
  drawGauge(94, 58, 30, currentAngle, target);

  display.sendBuffer();
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
  Serial.println("16 FUNC 键控制伺服马达（IO21）：0 -> 90 -> 180 -> 0 ...");

  pinMode(FUNC_PIN, INPUT_PULLUP);

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  servo.attach(SERVO_PIN);
  servo.write(currentAngle);  // 开机先到 0 度
  showScreen();
}

void loop() {
  if (funcPressed()) {
    targetIndex = (targetIndex + 1) % 3;
    Serial.print("[FUNC] 目标 -> ");
    Serial.print(ANGLES[targetIndex]);
    Serial.println(" 度");
  }

  // 平顺转动：每 STEP_MS 往目标靠近 STEP_DEG 度
  const int target = ANGLES[targetIndex];
  if (currentAngle != target && millis() - lastStepMs >= STEP_MS) {
    lastStepMs = millis();
    if (currentAngle < target) {
      currentAngle = min(currentAngle + STEP_DEG, target);
    } else {
      currentAngle = max(currentAngle - STEP_DEG, target);
    }
    servo.write(currentAngle);
    if (currentAngle == target) {
      Serial.print("[SERVO] 到达 ");
      Serial.print(currentAngle);
      Serial.println(" 度");
    }
  }

  static uint32_t lastDrawMs = 0;
  if (millis() - lastDrawMs >= 50) {
    lastDrawMs = millis();
    showScreen();
  }
}

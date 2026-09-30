/*
  HUB-8735 Ultra - Light sensor value on OLED

  Light sensor:
    Analog output -> IO0 (A2 / PF_2, ADC channel 2)

  External LEDs:
    LED 1 -> IO22
    LED 2 -> IO23
    LED 3 -> IO24

  OLED:
    SDA -> IO3, SCL -> IO4, VCC -> 3.3V, GND -> GND
    U8g2, SSD1306, 128x64, I2C address 0x3C

  IO3/IO4 are the hardware I2C0 (Wire) pins, but they share their pinmux
  with this SoC's JTAG function, so Wire.begin() on them fails. We use
  U8g2's software I2C to avoid that hardware peripheral entirely.

  U8g2's stock software-I2C GPIO callback calls pinMode() on every single
  bit it sends. On this core, pinMode() prints a warning line each time
  it is called on a pin that is already initialized, which is slow
  enough (one full frame is ~8000 bits) to make the sketch look frozen.
  This custom callback calls pinMode(OUTPUT) once and only uses
  digitalWrite() afterwards, so it doesn't pay that per-bit cost.
*/

#include <U8g2lib.h>
#include <analogin_api.h>

// analogRead() on this core floors any reading below raw=0xfa (~30-40mV)
// to exactly 0 as a noise filter (see wiring_analog.c). That hides
// whatever the sensor is actually outputting below that floor, so we
// read the ADC directly instead, bypassing analogRead()'s conversion
// and floor entirely.
analogin_t lightSensorAdc;

const uint8_t LED_1_PIN = 22;
const uint8_t LED_2_PIN = 23;
const uint8_t LED_3_PIN = 24;
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

int lightValueToPercent(uint16_t rawAdc) {
  // rawAdc = 0    -> 100%
  // rawAdc = 4000 -> 0%
  const uint16_t clamped = rawAdc > 4000 ? 4000 : rawAdc;
  return map(clamped, 0, 4000, 100, 0);
}

void updateLedsByBrightness(int brightnessPercent) {
  // 75~100%：一顆亮，25~75%：兩顆亮，0~25%：三顆亮
  if (brightnessPercent >= 75) {
    digitalWrite(LED_1_PIN, HIGH);
    digitalWrite(LED_2_PIN, LOW);
    digitalWrite(LED_3_PIN, LOW);
  } else if (brightnessPercent >= 25) {
    digitalWrite(LED_1_PIN, HIGH);
    digitalWrite(LED_2_PIN, HIGH);
    digitalWrite(LED_3_PIN, LOW);
  } else {
    digitalWrite(LED_1_PIN, HIGH);
    digitalWrite(LED_2_PIN, HIGH);
    digitalWrite(LED_3_PIN, HIGH);
  }
}

void setup() {
  analogin_init(&lightSensorAdc, PF_2);  // A2 / IO0
  pinMode(LED_1_PIN, OUTPUT);
  pinMode(LED_2_PIN, OUTPUT);
  pinMode(LED_3_PIN, OUTPUT);

  digitalWrite(LED_1_PIN, LOW);
  digitalWrite(LED_2_PIN, LOW);
  digitalWrite(LED_3_PIN, LOW);

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);

  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();
}

void loop() {
  const uint16_t rawAdc = analogin_read_u16(&lightSensorAdc);
  const int brightnessPercent = lightValueToPercent(rawAdc);
  updateLedsByBrightness(brightnessPercent);

  display.clearBuffer();

  display.setFont(u8g2_font_6x12_tf);
  display.drawStr(0, 12, "Light Sensor");
  display.drawHLine(0, 15, 128);

  display.setFont(u8g2_font_ncenB14_tr);
  display.setCursor(0, 38);
  display.print(brightnessPercent);
  display.print("%");

  display.setFont(u8g2_font_6x12_tf);
  display.setCursor(0, 57);
  display.print("Raw: ");
  display.print(rawAdc);

  display.sendBuffer();
  delay(250);
}

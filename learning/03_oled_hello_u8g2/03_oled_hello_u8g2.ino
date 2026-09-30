/*
  HUB-8735 Ultra - U8g2 SSD1306 OLED Hello test
  SDA -> IO3, SCL -> IO4, VCC -> 3.3V, GND -> GND
  SSD1306 128x64, I2C address 0x3C

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

void setup() {
  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);

  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);

  const uint8_t lineHeight = 10;
  const uint8_t lineCount = display.getDisplayHeight() / lineHeight;

  char lineText[16];
  for (uint8_t i = 0; i < lineCount; i++) {
    snprintf(lineText, sizeof(lineText), "Line %u test", i);
    display.drawStr(0, (i + 1) * lineHeight - 2, lineText);
  }

  display.sendBuffer();
}

void loop() {
}

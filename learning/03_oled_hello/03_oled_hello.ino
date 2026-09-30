/*
  HUB-8735 Ultra - 0.96 inch I2C OLED Hello test

  OLED wiring:
    SDA  -> IO0
    SCL/SCK -> IO1
    VCC  -> 3.3V
    GND  -> GND

  Assumptions:
    - 0.96 inch OLED
    - SSD1306 controller
    - 128 x 64 pixels
    - I2C address 0x3C

  This sketch only displays "Hello".
*/

#include <Wire.h>

const uint8_t OLED_SDA = 0;
const uint8_t OLED_SCL = 1;  // User labels this pin SCK; for I2C it is SCL.
const uint8_t OLED_WIDTH = 128;
const uint8_t OLED_HEIGHT = 64;
const uint8_t OLED_ADDRESS = 0x3C;

// On HUB-8735 Ultra, IO0/IO1 are I2C1, exposed as Wire1.
TwoWire &oledWire = Wire1;

uint8_t framebuffer[OLED_WIDTH * OLED_HEIGHT / 8];

void scanI2CBus() {
  Serial.println("Scanning I2C1 (IO0=SDA, IO1=SCL) bus...");
  uint8_t foundCount = 0;

  for (uint8_t address = 1; address < 127; address++) {
    oledWire.beginTransmission(address);
    uint8_t error = oledWire.endTransmission();

    if (error == 0) {
      Serial.print("  Device found at 0x");
      Serial.println(address, HEX);
      foundCount++;
    }
  }

  if (foundCount == 0) {
    Serial.println("  No I2C device found. Check wiring/power (SDA=IO0, SCL=IO1, VCC=3.3V, GND).");
  } else {
    Serial.print(foundCount);
    Serial.println(" device(s) found.");
  }
}

void sendCommand(uint8_t command) {
  oledWire.beginTransmission(OLED_ADDRESS);
  oledWire.write(0x00);
  oledWire.write(command);
  uint8_t error = oledWire.endTransmission();

  if (error != 0) {
    Serial.print("sendCommand 0x");
    Serial.print(command, HEX);
    Serial.print(" failed, I2C error=");
    Serial.println(error);
  }
}

void sendData(const uint8_t *data, size_t length) {
  while (length > 0) {
    const size_t chunk = (length > 16) ? 16 : length;

    oledWire.beginTransmission(OLED_ADDRESS);
    oledWire.write(0x40);
    oledWire.write(data, chunk);
    oledWire.endTransmission();

    data += chunk;
    length -= chunk;
  }
}

void clearDisplay() {
  memset(framebuffer, 0, sizeof(framebuffer));
}

void setPixel(uint8_t x, uint8_t y) {
  if (x >= OLED_WIDTH || y >= OLED_HEIGHT) {
    return;
  }

  framebuffer[x + (y / 8) * OLED_WIDTH] |= (1 << (y & 7));
}

void drawCharacter(uint8_t x, uint8_t y, char character) {
  // 5x7 font for the characters used by "Hello".
  static const uint8_t font[][5] = {
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // H
    {0x38, 0x54, 0x54, 0x54, 0x18},  // e
    {0x00, 0x41, 0x7F, 0x40, 0x00},  // l
    {0x00, 0x41, 0x7F, 0x40, 0x00},  // l
    {0x38, 0x44, 0x44, 0x44, 0x38}   // o
  };

  uint8_t fontIndex = 0;
  if (character == 'e') {
    fontIndex = 1;
  } else if (character == 'l') {
    fontIndex = 2;
  } else if (character == 'o') {
    fontIndex = 4;
  }

  for (uint8_t column = 0; column < 5; column++) {
    const uint8_t columnData = font[fontIndex][column];
    for (uint8_t row = 0; row < 7; row++) {
      if (columnData & (1 << row)) {
        setPixel(x + column, y + row);
      }
    }
  }
}

void drawHello() {
  const char text[] = "Hello";
  const uint8_t scale = 4;
  const uint8_t characterWidth = 5;
  const uint8_t characterGap = 1;
  const uint8_t textWidth = sizeof(text) - 1;
  const uint8_t xStart = 18;
  const uint8_t yStart = 18;

  for (uint8_t characterIndex = 0; characterIndex < textWidth; characterIndex++) {
    for (uint8_t column = 0; column < characterWidth; column++) {
      for (uint8_t row = 0; row < 7; row++) {
        // Draw at 4x scale using the same 5x7 character bitmap.
        uint8_t sourceX = xStart + characterIndex * (characterWidth + characterGap) * scale;
        uint8_t sourceY = yStart;
        uint8_t bit = 0;

        if (text[characterIndex] == 'H') {
          static const uint8_t glyph[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F};
          bit = glyph[column];
        } else if (text[characterIndex] == 'e') {
          static const uint8_t glyph[5] = {0x38, 0x54, 0x54, 0x54, 0x18};
          bit = glyph[column];
        } else if (text[characterIndex] == 'l') {
          static const uint8_t glyph[5] = {0x00, 0x41, 0x7F, 0x40, 0x00};
          bit = glyph[column];
        } else {
          static const uint8_t glyph[5] = {0x38, 0x44, 0x44, 0x44, 0x38};
          bit = glyph[column];
        }

        if (bit & (1 << row)) {
          for (uint8_t dx = 0; dx < scale; dx++) {
            for (uint8_t dy = 0; dy < scale; dy++) {
              setPixel(sourceX + column * scale + dx, sourceY + row * scale + dy);
            }
          }
        }
      }
    }
  }
}

void initializeDisplay() {
  const uint8_t commands[] = {
    0xAE,       // Display OFF
    0xD5, 0x80, // Clock divide
    0xA8, 0x3F, // Multiplex ratio: 64
    0xD3, 0x00, // Display offset
    0x40,       // Start line 0
    0x8D, 0x14, // Charge pump
    0x20, 0x00, // Horizontal addressing mode
    0xA1,       // Segment remap
    0xC8,       // COM scan direction
    0xDA, 0x12, // COM pins
    0x81, 0xCF, // Contrast
    0xD9, 0xF1, // Pre-charge
    0xDB, 0x40, // VCOM detect
    0xA4,       // Resume RAM display
    0xA6,       // Normal display
    0xAF        // Display ON
  };

  for (size_t index = 0; index < sizeof(commands); index++) {
    sendCommand(commands[index]);
  }
}

void refreshDisplay() {
  sendCommand(0x21);  // Column address
  sendCommand(0x00);
  sendCommand(OLED_WIDTH - 1);
  sendCommand(0x22);  // Page address
  sendCommand(0x00);
  sendCommand((OLED_HEIGHT / 8) - 1);
  sendData(framebuffer, sizeof(framebuffer));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("HUB-8735 Ultra OLED hello test (diagnostic build)");

  // Explicitly select the requested I2C pins.
  // IO0 = I2C1_SDA and IO1 = I2C1_SCL on this board.
  // Wire1 is already configured for these pins by the AmebaPro2 core.
  oledWire.begin();

  scanI2CBus();

  initializeDisplay();
  clearDisplay();
  drawHello();
  refreshDisplay();

  Serial.println("Init sequence sent. If no I2C device was found above, this will not show anything.");
}

void loop() {
  // The message remains on the OLED.
}

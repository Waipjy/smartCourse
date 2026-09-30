/*
  HUB-8735 Ultra - Camera preview on 0.96 inch OLED

  OLED wiring:
    SDA -> IO3, SCL -> IO4, VCC -> 3.3V, GND -> GND

  Camera:
    Uses the HUB-8735 Ultra built-in camera through VideoStream, captured
    as JPEG and decoded with the JPEGDEC library that already ships with
    this core (same one used by the official Camera_2_Lcd_JPEGDEC
    example). This is the SDK's own supported pattern for "camera onto a
    display" -- not VIDEO_RGB, see below.

  Important limitation:
    A 0.96 inch OLED is normally a 128x64 monochrome display.
    This sketch converts the camera frame to a 128x64 black-and-white
    preview. It is not a full-color video output.

  Bugs fixed here, found by checking the serial log / core source /
  official examples rather than guessing:

  1. OLED on IO0/IO1 conflicts with the camera (no display at all):
     IO0/IO1 is Wire1 / I2C1, which we used successfully for a plain
     OLED sketch with no camera involved. But this board's built-in
     camera sensor is itself controlled over I2C1, and the camera
     subsystem claims those pins at boot (see "Load ISP_IQ" in the
     serial log, before setup() even runs). Once a Camera/VideoStream
     object exists in the sketch, Wire1.begin() gets rejected with
     "[MISC Err] Pin 0[1] is conflicted" and the OLED never initializes.
     Fix: move the OLED to IO3/IO4 like the other OLED sketches. IO3/IO4
     is hardware I2C0, which conflicts with this SoC's JTAG pinmux, so
     we use U8g2 with a custom software-I2C GPIO callback (same fix as
     03_oled_hello_u8g2 / 04_light_sensor_oled) instead of the
     Adafruit_SSD1306 library, which only supports hardware I2C.

  2. VIDEO_RGB never delivered a frame:
     The original version of this sketch requested a raw VIDEO_RGB frame
     and polled it with Camera.getImage(). The serial log showed
     "RGB output only on ch4"; after moving to channel 4 the error went
     away but getImage() still never returned a non-zero address/length.
     The official examples (Camera_2_Lcd.ino, Camera_2_Lcd_JPEGDEC.ino)
     never use VIDEO_RGB for a "camera to display" preview at all -- they
     capture VIDEO_JPEG on channel 0 with snapshot mode and decode it
     with a JPEG decoder. VIDEO_RGB channels in this SDK appear to be
     meant for streaming into a StreamIO consumer (e.g. a neural-network
     model), not for polling full frames with getImage(). Fix: switch to
     VIDEO_JPEG + JPEGDEC, matching the proven official pattern.
*/

#include "VideoStream.h"
#include <U8g2lib.h>
// JPEGDEC lives inside the SPI library's src folder; SPI.h must be
// included first so Arduino adds that folder to the include path.
#include "SPI.h"
#include <JPEGDEC_Libraries/JPEGDEC.h>

#define CAMERA_CHANNEL 0

// VIDEO_VGA (640x480) decoded at 1/8 scale -> 80x60, close enough to the
// 128x64 OLED that we just nearest-neighbor map it in the draw callback.
const uint16_t DECODED_WIDTH = 80;
const uint16_t DECODED_HEIGHT = 60;
const uint8_t OLED_WIDTH = 128;
const uint8_t OLED_HEIGHT = 64;
const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;

U8G2 display;
JPEGDEC jpeg;

// snapshot=1 (full): VIDEO_JPEG is one of the encoders that supports it,
// unlike VIDEO_RGB.
VideoSetting cameraVideoSetting(VIDEO_VGA, CAM_FPS, VIDEO_JPEG, 1);

uint32_t imageAddress = 0;
uint32_t imageLength = 0;

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

uint8_t luminance(uint8_t red, uint8_t green, uint8_t blue) {
  // Integer approximation of perceived brightness:
  // Y = 0.299R + 0.587G + 0.114B
  return (77 * red + 150 * green + 29 * blue) >> 8;
}

// Called by JPEGDEC once per decoded pixel block. Block coordinates are
// already in the decoded (post-scale) 80x60 space.
int jpegDrawCallback(JPEGDRAW *pDraw) {
  for (int blockY = 0; blockY < pDraw->iHeight; blockY++) {
    const int srcY = pDraw->y + blockY;
    if (srcY >= DECODED_HEIGHT) {
      continue;
    }
    const uint16_t destY = (static_cast<uint32_t>(srcY) * OLED_HEIGHT) / DECODED_HEIGHT;

    for (int blockX = 0; blockX < pDraw->iWidth; blockX++) {
      const int srcX = pDraw->x + blockX;
      if (srcX >= DECODED_WIDTH) {
        continue;
      }
      const uint16_t destX = (static_cast<uint32_t>(srcX) * OLED_WIDTH) / DECODED_WIDTH;

      const uint16_t pixel565 = pDraw->pPixels[blockY * pDraw->iWidth + blockX];
      const uint8_t red = ((pixel565 >> 11) & 0x1F) << 3;
      const uint8_t green = ((pixel565 >> 5) & 0x3F) << 2;
      const uint8_t blue = (pixel565 & 0x1F) << 3;

      if (luminance(red, green, blue) >= 128) {
        display.drawPixel(destX, destY);
      }
    }
  }
  return 1;  // continue decode
}

void showCameraFrame() {
  Camera.getImage(CAMERA_CHANNEL, &imageAddress, &imageLength);

  if (imageAddress == 0 || imageLength == 0) {
    return;
  }

  display.clearBuffer();
  display.setDrawColor(1);

  jpeg.openFLASH(reinterpret_cast<uint8_t *>(imageAddress), imageLength, jpegDrawCallback);
  jpeg.decode(0, 0, JPEG_SCALE_EIGHTH);
  jpeg.close();

  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("03-2 camera to OLED starting");
  Serial.println("OLED I2C: software I2C, SDA=IO3, SCL=IO4");

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 10, "Camera starting...");
  display.sendBuffer();

  Camera.configVideoChannel(CAMERA_CHANNEL, cameraVideoSetting);
  Camera.videoInit();
  Camera.channelBegin(CAMERA_CHANNEL);

  Serial.println("Camera started");
}

void loop() {
  showCameraFrame();
  delay(200);
}

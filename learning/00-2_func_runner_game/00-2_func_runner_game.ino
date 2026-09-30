/*
  HUB-8735 Ultra - FUNC 按鈕跳躍過障礙遊戲（類似暴龍快跑）

  換掉 DHT11 吹氣當輸入的原因：
    DHT11 反應太慢（規格上取樣間隔至少要 1 秒，感測器內部對濕度變化
    的實際反應又更慢），拿來做需要精準時機的遊戲輸入，常常吹了沒反
    應、或是判定延遲一大截，體驗很差。
    板載的 FUNC 按鈕是純數位訊號，按下/放開是瞬間反應，沒有這個問題，
    所以整個遊戲邏輯改成用按鈕控制，順便把遊戲步調也從原本配合 DHT11
    取樣間隔的「每 1.5 秒一步」，改成真正流暢的每 40ms 一個畫面。

  玩法：
    角色固定在畫面左側地面上奔跑，畫面會持續有仙人掌從右邊往左邊移
    動過來，按一下 FUNC 按鈕角色就會跳起來（真正的拋物線跳躍，不是
    瞬間移動），跳過障礙物。撞到就遊戲結束，分數是撐過的時間，速度
    會隨著分數越來越快、越來越難。遊戲結束畫面按一下 FUNC 重新開始。

    跳躍高度是馬力歐式的「按住時間決定高度」：起跳一律用最大力道
    （JUMP_VELOCITY）出發，如果按著不放、直到自然上升結束都沒放開，
    就會跳到完整的最大高度；如果提早放開按鈕，而且這時候角色還在
    往上升，就會把上升的力道「剪短」（降到 JUMP_CUT_VELOCITY），
    提早進入下墜，變成一個小跳。所以最大高度＝原本 -9 的上限，短按
    會跳得比較低，長按會跳得比較高，直到頂到上限為止。

  FUNC 按鈕：
    IO12（PF_15），開機沒接任何東西時是高電位，按下時接地變成低電位
    （跟官方範例 Camera_2_Lcd.ino 用 INPUT_IRQ_FALL 偵測下降緣的用法
    一致），這裡用簡單輪詢加邊緣偵測，不需要中斷。

  OLED：
    SDA -> IO3, SCL -> IO4, VCC -> 3.3V, GND -> GND
    IO3/IO4 是硬體 I2C0，會跟這顆 SoC 的 JTAG pinmux 衝突，所以用
    U8g2 軟體 I2C，並用自訂的 GPIO callback（開機只 pinMode 一次，
    之後全用 digitalWrite），避開重複呼叫 pinMode() 的速度陷阱。

  畫面文字使用簡體中文（OLED 字型只保證涵蓋 GB2312 簡體字集）。
*/

#include <U8g2lib.h>

const uint8_t FUNC_PIN = 12;
const uint8_t OLED_SDA = 3;
const uint8_t OLED_SCL = 4;
const uint8_t OLED_ADDRESS = 0x3C;

const uint32_t TICK_MS = 40;  // 遊戲每格畫面的間隔，約 25fps

// 場地與角色。
const uint8_t GROUND_Y = 58;
const uint8_t PLAYER_TOP_BOUND = 15;  // 跳躍最高不超過這個 y 座標，避免蓋到分數列
const uint8_t PLAYER_X = 18;
const uint8_t PLAYER_SIZE = 8;
const int8_t JUMP_VELOCITY = -9;      // 按鈕按下的瞬間就是用這個力道起跳（上限）
const int8_t JUMP_CUT_VELOCITY = -3;  // 提早放開時，跳躍會被「剪短」成這個力道
const int8_t GRAVITY = 1;

// 障礙物。
const uint8_t OBSTACLE_COUNT = 3;
const uint8_t OBSTACLE_WIDTH = 6;
const uint16_t OBSTACLE_MIN_GAP = 40;
const uint16_t OBSTACLE_MAX_GAP = 80;
const uint8_t OBSTACLE_MIN_HEIGHT = 8;
const uint8_t OBSTACLE_MAX_HEIGHT = 16;

// 難度：分數每累積這麼多，速度加快一次，直到封頂。
const uint16_t SPEED_UP_EVERY = 300;
const uint16_t BASE_SPEED_X100 = 450;  // 用 x100 存小數速度，避免用 float
const uint16_t MAX_SPEED_X100 = 950;
const uint8_t SPEED_STEP_X100 = 55;

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

// ---- 按鈕：輪詢 + 邊緣偵測 ----
// 馬力歐式的可變跳躍高度，需要同時知道「剛按下」跟「剛放開」兩種邊緣：
// 剛按下＝起跳，剛放開＝如果還在上升就把跳躍「剪短」。
//
// 這裡用兩個輸出參數（而不是回傳一個自訂 struct），是因為 Arduino IDE
// 會自動在檔案最前面插入函式原型宣告，但插入的位置比自訂 struct 的定義
// 還早，會導致「'ButtonEdges' does not name a type」這種編譯錯誤；
// 只用 bool 這種基本型別當參數/回傳值就不會有這個問題。

void readButtonEdges(bool &pressedEdge, bool &releasedEdge) {
  static bool lastHeld = false;

  const bool held = (digitalRead(FUNC_PIN) == LOW);
  pressedEdge = held && !lastHeld;
  releasedEdge = !held && lastHeld;
  lastHeld = held;
}

// ---- 遊戲狀態 ----

int16_t playerY = GROUND_Y - PLAYER_SIZE;
int8_t playerVelocity = 0;
bool jumping = false;
bool holdingJump = false;  // 這次跳躍的按鈕是不是還按著

int16_t obstacleX[OBSTACLE_COUNT];
uint8_t obstacleHeight[OBSTACLE_COUNT];
bool obstacleScored[OBSTACLE_COUNT];

uint16_t score = 0;
uint16_t speedX100 = BASE_SPEED_X100;
int16_t moveAccumulatorX100 = 0;  // 累積小數位移，滿 100 才真正移動 1px
bool gameOver = false;

uint16_t randomGapAfter() {
  return random(OBSTACLE_MIN_GAP, OBSTACLE_MAX_GAP);
}

uint8_t randomObstacleHeight() {
  return random(OBSTACLE_MIN_HEIGHT, OBSTACLE_MAX_HEIGHT + 1);
}

void resetGame() {
  playerY = GROUND_Y - PLAYER_SIZE;
  playerVelocity = 0;
  jumping = false;
  holdingJump = false;

  score = 0;
  speedX100 = BASE_SPEED_X100;
  moveAccumulatorX100 = 0;
  gameOver = false;

  int16_t nextX = 128;
  for (uint8_t i = 0; i < OBSTACLE_COUNT; i++) {
    obstacleX[i] = nextX;
    obstacleHeight[i] = randomObstacleHeight();
    obstacleScored[i] = false;
    nextX += randomGapAfter();
  }
}

void updateGame(bool jumpPressedEdge, bool jumpReleasedEdge) {
  if (gameOver) {
    if (jumpPressedEdge) {
      resetGame();
    }
    return;
  }

  // 跳躍：只有在地面上時按鈕才會觸發新的跳躍，避免空中連續加速上飛。
  // 起跳一律用 JUMP_VELOCITY（上限）出發，馬力歐式的「按越久跳越高」
  // 其實是反過來做：提早放開的話，就把還在上升中的速度「剪短」，
  // 讓跳躍提早進入下墜，按住不放就會維持完整的上限高度。
  if (jumpPressedEdge && !jumping) {
    jumping = true;
    holdingJump = true;
    playerVelocity = JUMP_VELOCITY;
  }

  if (jumping) {
    playerVelocity += GRAVITY;

    if (holdingJump && jumpReleasedEdge) {
      holdingJump = false;
      if (playerVelocity < JUMP_CUT_VELOCITY) {
        playerVelocity = JUMP_CUT_VELOCITY;
      }
    }

    playerY += playerVelocity;

    if (playerY <= PLAYER_TOP_BOUND) {
      playerY = PLAYER_TOP_BOUND;
      if (playerVelocity < 0) {
        playerVelocity = 0;  // 撞頂了，直接開始往下掉，不繼續往上加速
      }
    }

    if (playerY >= GROUND_Y - PLAYER_SIZE) {
      playerY = GROUND_Y - PLAYER_SIZE;
      playerVelocity = 0;
      jumping = false;
      holdingJump = false;
    }
  }

  // 難度：分數每滿 SPEED_UP_EVERY 就加速一次，封頂在 MAX_SPEED_X100。
  if (speedX100 < MAX_SPEED_X100 && (score / SPEED_UP_EVERY) * SPEED_STEP_X100 + BASE_SPEED_X100 > speedX100) {
    speedX100 += SPEED_STEP_X100;
  }

  moveAccumulatorX100 += speedX100;
  const int16_t moveThisTick = moveAccumulatorX100 / 100;
  moveAccumulatorX100 %= 100;

  const int16_t playerTop = playerY;
  const int16_t playerBottom = playerY + PLAYER_SIZE;

  for (uint8_t i = 0; i < OBSTACLE_COUNT; i++) {
    obstacleX[i] -= moveThisTick;

    if (!obstacleScored[i] && obstacleX[i] + OBSTACLE_WIDTH < PLAYER_X) {
      obstacleScored[i] = true;
      score += 10;
    }

    const bool overlapsX =
      (obstacleX[i] <= PLAYER_X + (int16_t)PLAYER_SIZE) &&
      (obstacleX[i] + OBSTACLE_WIDTH >= PLAYER_X);
    if (overlapsX) {
      const int16_t obstacleTop = GROUND_Y - obstacleHeight[i];
      if (playerBottom > obstacleTop) {
        gameOver = true;
      }
    }

    if (obstacleX[i] + OBSTACLE_WIDTH < 0) {
      int16_t rightmostX = obstacleX[0];
      for (uint8_t j = 1; j < OBSTACLE_COUNT; j++) {
        if (obstacleX[j] > rightmostX) {
          rightmostX = obstacleX[j];
        }
      }
      obstacleX[i] = rightmostX + randomGapAfter();
      obstacleHeight[i] = randomObstacleHeight();
      obstacleScored[i] = false;
    }
  }

  score++;  // 每格畫面都加一點分數，代表撐過的時間
}

void drawGame() {
  display.clearBuffer();

  display.setFont(u8g2_font_wqy12_t_gb2312);
  char scoreText[16];
  snprintf(scoreText, sizeof(scoreText), "得分：%u", score);
  display.drawUTF8(2, 11, scoreText);
  display.drawHLine(0, 14, 128);

  display.drawHLine(0, GROUND_Y, 128);

  for (uint8_t i = 0; i < OBSTACLE_COUNT; i++) {
    if (obstacleX[i] + OBSTACLE_WIDTH < 0 || obstacleX[i] > 128) {
      continue;
    }
    display.drawBox(obstacleX[i], GROUND_Y - obstacleHeight[i], OBSTACLE_WIDTH, obstacleHeight[i]);
  }

  display.drawBox(PLAYER_X, playerY, PLAYER_SIZE, PLAYER_SIZE);

  if (gameOver) {
    display.setDrawColor(0);
    display.drawBox(14, 22, 100, 24);
    display.setDrawColor(1);
    display.drawFrame(14, 22, 100, 24);

    display.setFont(u8g2_font_wqy12_t_gb2312);
    display.drawUTF8(24, 34, "游戏结束");

    char resultText[20];
    snprintf(resultText, sizeof(resultText), "得分%u 按键重来", score);
    display.drawUTF8(16, 44, resultText);
  }

  display.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("00-2 FUNC 按钮跳跃游戏 启动，按钮腳位 IO12");

  pinMode(FUNC_PIN, INPUT_PULLUP);

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast);
  u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
  display.setI2CAddress(OLED_ADDRESS << 1);
  display.begin();

  randomSeed(micros());
  resetGame();
}

void loop() {
  bool jumpPressedEdge = false;
  bool jumpReleasedEdge = false;
  readButtonEdges(jumpPressedEdge, jumpReleasedEdge);

  updateGame(jumpPressedEdge, jumpReleasedEdge);
  drawGame();

  delay(TICK_MS);
}

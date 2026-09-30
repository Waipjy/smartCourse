/*
  HUB-8735 Ultra - 蜂鳴器警笛聲

  蜂鳴器：
    訊號腳位 -> IO19（PF_6），另一隻腳接 GND。
    IO19 選這根是因為 tone() 這個函式在這顆核心裡會強制檢查腳位必須
    支援 PWM（Tone.cpp 裡呼叫 amb_ard_pin_check_fun(pin, PIO_PWM)），
    不是隨便一根腳位都能用；IO19 支援 PWM，而且沒有被這個專案其他
    sketch 占用（IO20 是 DHT11、IO12 是 FUNC 按鈕、IO22-26 是 LED、
    IO0/1/3/4 是相機跟 OLED）。

  如果手上是無源蜂鳴器（沒有極性，兩隻腳隨便接），這樣接就會響；
  如果是有源蜂鳴器且接反了，可能不會出聲，把兩隻腳對調試試看。

  警笛效果：頻率在 SIREN_LOW_HZ 跟 SIREN_HIGH_HZ 之間來回掃過，
  每一步用 tone() 換一次頻率、短暫停留 STEP_MS，聽起來就是救護車
  那種一高一低的聲音。
*/

const uint8_t BUZZER_PIN = 19;

const uint16_t SIREN_LOW_HZ = 500;
const uint16_t SIREN_HIGH_HZ = 1200;
const uint16_t SIREN_STEP_HZ = 20;
const uint16_t STEP_MS = 15;

void sweep(uint16_t fromHz, uint16_t toHz, int16_t stepHz) {
  for (int32_t freq = fromHz; (stepHz > 0) ? (freq <= toHz) : (freq >= toHz); freq += stepHz) {
    tone(BUZZER_PIN, freq);
    delay(STEP_MS);
  }
}

void setup() {
}

void loop() {
  sweep(SIREN_LOW_HZ, SIREN_HIGH_HZ, SIREN_STEP_HZ);
  sweep(SIREN_HIGH_HZ, SIREN_LOW_HZ, -SIREN_STEP_HZ);
}

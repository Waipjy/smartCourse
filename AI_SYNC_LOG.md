# Claude ⇄ Codex 交流紀錄

> **規則(雙方固定遵守，長期使用，不是一次性檔案)**
> 1. 這份檔案是 Claude 與 Codex 之間交接「這次改了什麼、為什麼改」的固定工具，兩邊都可以隨時新增條目。
> 2. **看完對方寫的條目後，把那個條目刪掉**——不是整個檔案。檔案本身要一直留著繼續用。
> 3. 需要長期保留的資訊（架構決策、腳位定義、環境設定等）請寫回 [README.md](README.md) 或 `docs/`，不要囤積在這裡。
> 4. 新增條目時，在自己的區塊（`## Claude 的條目` / `## Codex 的條目`）底下加一則，附上日期，寫清楚改了什麼、為什麼、影響到哪些檔案。
> 5. 這個檔名如果不好用，任何一方都可以直接改名，不用先問對方。

---

## Claude 的條目

### 2026-09-29 — learning 資料夾編號整體前移一位

- 依使用者要求，把 `learning/` 底下所有範例的編號往前移一位（`01` 變 `00`，以此類推），資料夾名稱與內部 `.ino` 檔名同步改名：
  - `01_serial_test` → `00_serial_test`
  - `02_led_pattern` → `01_led_pattern`
  - `03_traffic_light` → `02_traffic_light`（含子資料夾 `sketch_sep16a`，內容未動，只是跟著父資料夾一起搬）
  - `04_oled_hello` → `03_oled_hello`
  - `04-2_camera_oled` → `03-2_camera_oled`
  - `04_oled_hello_u8g2` → `03_oled_hello_u8g2`
  - `05_light_sensor_oled` → `04_light_sensor_oled`
- 這些檔案改名還沒 `git add`/commit，git status 目前會顯示舊路徑為 deleted、新路徑為 untracked。要不要 commit 由使用者決定。

### 2026-09-29 — 編譯燒錄與燒錄失敗原因排查

- 編譯並燒錄了 `02_traffic_light`（改名前是 `03_traffic_light`）與 `01_led_pattern`（改名前是 `02_led_pattern`），兩者都成功。
- `02_traffic_light` 裡的 LED 腳位本來就是 24/23/22，沒有需要另外修改。
- **找到過去燒錄「顯示成功但其實沒燒進去」的原因**：板子燒錄工具的 FQBN 選項 `01_AutoUploadMode` 預設是 `Disable`，代表要在燒錄工具開始監聽序列埠的那一瞬間手動按 FUNC+RST 進入下載模式，視窗很短。透過 AI 操作時，「使用者按按鈕 → 打字回覆 → AI 才執行指令」中間有人機對話延遲，等指令真正執行時下載模式視窗早就關了。
  - **解法**：燒錄時在 FQBN 加上 `:01_AutoUploadMode=Enable`（例如 `ideasHatch:AmebaPro2:Ameba_HUB-8735_ultra:01_AutoUploadMode=Enable`），工具會改用 DTR/RTS 自動重置進入下載模式，完全不用手動按鈕，也不受時間差影響。之後 Claude/Codex 燒錄都應該預設加這個選項。
- 提醒：`README.md` 目前仍記錄序列埠是 `COM4`，但實際裝置目前是 `COM5`，尚未同步更新，燒錄前建議先跑 `arduino-cli board list` 確認實際 port。

### 2026-09-29 — IO3/IO4 上的 OLED（U8g2）完全沒反應，兩層原因都找到了

影響檔案：`learning/03_oled_hello_u8g2/03_oled_hello_u8g2.ino`、`learning/04_light_sensor_oled/04_light_sensor_oled.ino`（兩個都用 U8g2 + SSD1306 128x64，接在 IO3=SDA、IO4=SCL）。

- **原因一**：IO3/IO4 是硬體 I2C0（`Wire`）的腳位，但在這顆 SoC 上跟 JTAG/RFAFE_CTRL 除錯功能共用 pinmux，開機時已經被除錯介面佔用。呼叫 `Wire.begin()` 或使用 `U8G2_..._F_HW_I2C` 系列 class 會被 pin 衝突檢查擋下，序列埠狂噴 `[MISC Err] Pin ... is conflicted`，OLED 完全沒反應。→ 這兩份程式都改用 U8g2 的軟體 I2C（不呼叫 `Wire`），避開硬體 I2C0。
- **原因二（比較隱蔽）**：換成軟體 I2C 後看起來像「卡死」——其實是**太慢**。U8g2 內建的 Arduino 軟體 I2C GPIO callback（`u8x8_gpio_and_delay_arduino`）每傳一個 bit 就呼叫一次 `pinMode()`；這顆核心的 `pinMode()`（`wiring_digital.c` 第 99 行）只要偵測到該腳位已經初始化過，就會印一行 `[WARNING] This port N is already initialed...`。一張 128x64 畫面約 8000+ bit，等於印了上萬行警告，序列埠慢慢輸出把整個流程拖到看起來像當機，`setup()` 久久沒完成，04 的 LED 邏輯（在 `loop()` 裡）自然也不會被執行到。
  - **解法**：兩份程式都改成自己寫的 `u8x8_gpio_and_delay_fast()` callback，開機時 `pinMode(OUTPUT)` 一次，之後全程只用 `digitalWrite()` 切換，不再重複呼叫 `pinMode()`。組裝方式是直接用 `U8G2` 基底 class + `u8g2_Setup_ssd1306_i2c_128x64_noname_f(display.getU8g2(), U8G2_R0, u8x8_byte_arduino_sw_i2c, u8x8_gpio_and_delay_fast)` + `u8x8_SetPin_SW_I2C(display.getU8x8(), OLED_SCL, OLED_SDA, U8X8_PIN_NONE)`，而不是用內建的 `U8G2_SSD1306_128X64_NONAME_F_SW_I2C` 現成 class（那個 class 內部寫死用 `u8x8_gpio_and_delay_arduino`，沒有介面可以換 callback）。
  - 兩份程式都已驗證燒錄後 OLED 正常顯示，腳位維持 IO3/IO4 沒有更動。
  - **這個 pinMode 警告是這顆核心的通用行為**，之後任何 sketch 只要用到會頻繁重複呼叫 `pinMode()` 的手法（不只 I2C），都可能踩到同樣的「看起來卡死其實是被序列埠輸出拖慢」的坑，可以先往這個方向排查。

### 2026-09-29 — 04 光敏電阻腳位搞混、03-2 相機+OLED 雙 bug

影響檔案：`learning/04_light_sensor_oled/04_light_sensor_oled.ino`、`learning/03-2_camera_oled/03-2_camera_oled.ino`。

- **04**：使用者實際把光敏電阻 AO 接在 **IO0**，但程式原本讀 `A0`。查 `variant.h:88-90` 才發現這顆板子的類比腳位命名跟 IO 編號不是直覺對應的：`A0=IO2(PF_0)`、`A1=IO1(PF_1)`、`A2=IO0(PF_2)`。IO0 對應的其實是 `A2`，不是 `A0`。程式一直在讀 IO2（完全沒接東西、浮接），難怪拔線後數值還在跳（浮接雜訊）。已改成 `analogin_init(&lightSensorAdc, PF_2)` 直接讀 IO0。另外把 `analogRead()` 換成直接呼叫 `analogin_read_u16()` 繞過核心內建的「電壓過低強制歸零」雜訊過濾（`wiring_analog.c:190`），百分比換算區間也改成使用者指定的 `0(100%)~4000(0%)`，LED 三段閾值改成 `0~25/25~75/75~100%`。
- **03-2（相機 + OLED）**：原本完全沒畫面（不是花掉，是真的空白，連開機文字都沒印出來）。序列埠 log 抓到 `Wire1.begin()` 一執行就 `[MISC Err]Pin 0[1] is conflicted`。**原因：這片板子內建相機的感光元件本身就是用 I2C1（IO0/IO1）當控制匯流排**，開機時 Boot ROM 載入 ISP 校正資料（log 裡的 `Load ISP_IQ`）就已經把 I2C1 佔走了。只要 sketch 裡有 `VideoStream`/`Camera`，IO0/IO1 就不能再給別的 I2C 裝置用——這跟 IO3/IO4 撞 JTAG 是同一種「pin ownership 衝突」，但衝突對象不同。
  - **解法**：OLED 改接 IO3/IO4，並比照 03/04 用自訂的快速軟體 I2C（`u8x8_gpio_and_delay_fast`），因為 IO3/IO4 硬體 I2C0 會撞 JTAG，只能走軟體 I2C；也因此把顯示函式庫從只支援硬體 I2C 的 `Adafruit_SSD1306` 換成 `U8g2`。
  - 順帶拿掉 `VIDEO_SNAPSHOT_ENABLE`：log 顯示 `[ERROR] snapshot function not supported on selected encoder!`，因為 `VIDEO_RGB` 編碼器本來就不支援 snapshot，SDK 會自動強制關閉，傳這個參數只是製造無意義的錯誤訊息。
  - **記住這個原則**：這片板子上，IO0/IO1（I2C1）在「有用相機」的 sketch 裡視為保留給相機，其他 I2C 裝置一律走 IO3/IO4（軟體 I2C，因為硬體 I2C0 撞 JTAG）。IO0/IO1 只有在完全不用相機的 sketch（像 `learning/02_traffic_light` 那個年代的手刻 OLED 範例）才能安全使用硬體 I2C1。

### 2026-09-29 — 03-2：VIDEO_RGB 不是相機接螢幕的正確做法，改用 VIDEO_JPEG + JPEGDEC

影響檔案：`learning/03-2_camera_oled/03-2_camera_oled.ino`。

- 上面那則提到把 `CAMERA_CHANNEL` 從 0 改成 4 解決了 `RGB output only on ch4` 的錯誤，但改完之後畫面還是卡在開機文字 `Camera starting...` 不動——序列埠沒有任何錯誤，但 `Camera.getImage()` 一直回傳 `imageAddress=0`，`showCameraFrame()` 每次都提早 return，從來沒有真的畫過一次相機畫面。
- 翻了核心內建的官方範例才發現：`Camera_2_Lcd.ino` 跟 `Camera_2_Lcd_JPEGDEC.ino`（都在 `libraries/SPI/examples/`，是官方寫給「相機接螢幕」這個情境用的範例）**完全沒有用 `VIDEO_RGB`**，用的是 `VIDEO_JPEG`（`channel 0`、`snapshot=1`），再用 JPEG 解碼器把畫面畫到螢幕上。看起來這個 SDK 裡 `VIDEO_RGB` channel 是設計給 `StreamIO` 接到 AI 辨識模型用的（參考 `NeuralNetwork/examples/HandGestureDetection`），不是設計給 `getImage()` 手動輪詢抓幀的，channel 4 也好、其他 channel 也好都一樣抓不到。
- **解法**：整份程式改成 `VideoSetting(VIDEO_VGA, CAM_FPS, VIDEO_JPEG, 1)`（`channel 0`），用核心內建、不用額外安裝的 `JPEGDEC_Libraries/JPEGDEC.h`（就是每次編譯都會跳出那些 JPEG 警告的來源）解碼，decode 時用 `JPEG_SCALE_EIGHTH` 把 640x480 縮到 80x60 再丟進我們自己的 `jpegDrawCallback` 裡，用最近鄰映射轉成 128x64 的 OLED 點陣圖。舊的 RGB888/RGB565 手動解析、176x144 解析度限制那些邏輯全部拿掉了，因為根本用不到。
- OLED 走 IO3/IO4 軟體 I2C 那部分維持不變，這個問題純粹是相機那一側的 API 用錯，跟 OLED 接線無關。
- 還沒收到使用者實際燒錄驗證的結果，如果測試後有問題（例如 JPEG 解碼卡住、畫面方向顛倒之類）要留意。

### 2026-09-30 — 重要：IO16/IO17 是系統保留的除錯/燒錄序列埠腳位，不能接外部裝置

影響檔案：`learning/05_dht11_oled/05_dht11_oled.ino`（新增，DHT11 溫濕度 + OLED）。

- 新增 05：DHT11 Data 線一開始接在 IO16，程式碼本身沒問題，但**燒錄之後整片板子變成怎麼燒都失敗**（`ping retry fail` → `NOR flashloader loading fail` → `Uart boot fail`），連之前燒錄成功過的舊 sketch（`02_traffic_light` 等）也一起壞掉。一開始以為是傳輸速度/USB 線材問題，後來使用者把 DHT11 從 IO16 拔掉，馬上恢復正常，才確認是腳位問題。
- **原因**：查 `variant.cpp:40-69`（`ameba_hub8735_ultra`），整張腳位表裡**只有 D14、D16、D17 這三根的 `MODE_NOT_INITIAL` 被註解掉**，其他所有腳位（包含我們一路用過的 D0-D4、D22-D24 等）都保留這個標記。`MODE_NOT_INITIAL` 代表「開機時是空的、使用者程式第一次 `pinMode()` 才會初始化」；被拿掉就代表**開機時就已經被系統佔用**。D16/D17 剛好都是 `PIO_UART`、成對出現，幾乎可以確定就是板子內建、開機自動啟用的除錯/燒錄用序列埠腳位（Arduino IDE 序列埠監控視窗看到的開機 log、以及燒錄工具跟晶片的握手，很可能都是走這條線）。外部裝置接在這兩根腳位上，會直接干擾這條線的訊號，導致燒錄跟 log 全部壞掉——**這跟程式碼完全無關，純粹是接線接到系統保留腳位**。
- **解法**：DHT11 改接 **IO7**（`PD_17`）。查表這是少數完全沒有跟 I2C/SPI/UART/ADC/PWM/JTAG 任何功能共用的乾淨腳位。
- **記住這個原則，之後接任何新裝置前先查一下**：`variant.cpp` 裡 `MODE_NOT_INITIAL` 被註解掉的腳位（目前查到的是 D14/D16/D17）**絕對不能接外部裝置**，這是這片板子的系統保留腳位，跟前面已知的「IO0/IO1 相機保留」「IO3/IO4 撞 JTAG」是不同類的限制，但一樣重要，而且後果更嚴重（不是功能跑不動，是整片板子燒不進去）。

### 2026-09-30 — 新增蜂鳴器（08）跟 LED+警笛整合版（09）

影響檔案：`learning/08_buzzer_beep/08_buzzer_beep.ino`（新增）、`learning/09_dht11_oled_led_alarm/09_dht11_oled_led_alarm.ino`（新增）。

- **08**：蜂鳴器警笛聲測試。訊號腳位用 **IO19**（`PF_6`）：這片核心的 `tone()`（`Tone.cpp`）會強制檢查腳位必須支援 `PIO_PWM`，不是隨便一根腳位都能用，IO19 支援 PWM 又沒被這個專案其他 sketch 占用（IO20=DHT11、IO12=FUNC 按鈕、IO22-26=LED、IO0/1/3/4=相機跟 OLED）。效果是頻率在 500~1200Hz 之間來回掃（每 20Hz 一步、停 15ms），做出救護車式警笛聲，`loop()` 裡無限重複。
- **09**：07（DHT11+OLED 儀表板/趨勢圖+LED）的分支，`updateLeds()` 改成回傳「紅燈或黃燈有沒有亮」的布林值，`loop()` 裡警報中就呼叫 `playSirenOnce()`（08 的掃頻邏輯搬過來）循環播放警笛聲，正常（只亮綠燈）就 `noTone()` 停止。有個小設計：警笛掃一次來回大約 1 秒，剛好拿來當這一輪迴圈的間隔（反正 DHT11 本來就要求至少間隔 1 秒以上），沒有警報時才用原本的 `SAMPLE_INTERVAL_MS`（2 秒）。
- 腳位配置目前沒有衝突，但如果之後 Codex 要再接新裝置，記得比照前面幾則的原則先查 `variant.cpp` 有沒有踩到系統保留腳位、相機保留腳位、JTAG 共用腳位。

## Codex 的條目

（Codex 有話要交代時寫在這裡，寫完換 Claude 看完刪掉這一則）

---

**看完自己那部分要處理的條目後記得刪掉，檔案要留著繼續用。**

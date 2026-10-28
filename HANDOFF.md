# HUB-8735 Ultra 智慧節能 — 專案交接文件

這份文件給第一次接手這個專案的人看（fork、新同學、未來的自己）。目標是讓你不用重踩我們這幾週已經踩過的坑。`README.md` 偏簡短，這份文件補完整的上下文。

## 開發環境建置

1. 安裝 **Arduino IDE 2.3.10**（或更新版本應該也可以，但目前驗證過的是這個版本）。
2. 檔案 → 偏好設定 → **額外的開發板管理員網址**，加入：
   ```
   https://github.com/ideashatch/HUB-8735/raw/main/amebapro2_arduino/Arduino_package/ideasHatch.json
   ```
3. 工具 → 開發板 → 開發板管理員，搜尋 `AmebaPro2`，安裝 `ideasHatch:AmebaPro2`（目前驗證過的版本：**4.0.15-Release**）。
4. 工具 → 開發板，選 **`HUB-8735 ultra`**（FQBN：`ideasHatch:AmebaPro2:Ameba_HUB-8735_ultra`）。
5. 安裝 **U8g2** 函式庫（Library Manager 搜尋 `U8g2`，作者 oliver / olikraus）—OLED 相關的 sketch 都靠它，而且用的是自訂的軟體 I2C callback（原因見下面「OLED」小節），不是用它內建的 hardware I2C class。
6. CH340 USB 轉序列晶片驅動：`twgo.io/ch340`，驅動裝好後裝置管理員應該會看到 `USB-SERIAL CH340 (COMx)`。

## 燒錄：先看這裡，不然會卡很久

- **燒錄前一定要在 Arduino IDE 的開發板選項裡把「Auto Flash Mode」設成 `Enable`**（工具選單裡找 `01_AutoUploadMode` 那個子選單）。預設是 `Disable`，代表要在燒錄工具開始監聽序列埠的那一瞬間手動按住 FUNC、點一下 RST、立刻放開 RST、再放開 FUNC——時間窗很短，容易失敗。開了 Auto Flash Mode 之後，工具會自動用 DTR/RTS 訊號重置進入下載模式，完全不用手動按鍵。
- 如果燒錄出現 `ping retry fail` / `NOR flashloader loading fail` / `Uart boot fail`，而且**連以前燒錄成功過的舊 sketch 現在也失敗**，先檢查看看是不是接了什麼東西在 **IO16 或 IO17** 上（見下面「絕對不能用的腳位」），這跟程式碼無關，是接線干擾到板子內建的除錯/燒錄序列埠。拔掉就會恢復正常。
- 每次燒錄前建議先用 `工具 → 連接埠` 確認目前真正的 COM port（CH340 重新插拔常常會換號碼，`README.md` 裡記錄的號碼可能已經過期）。

## 這片板子的腳位限制（最重要的部分）

這些都是實測踩坑才確認的，不是查資料查到的，之後接任何新裝置前**先對照這份清單**：

| 腳位 | 限制 | 原因 |
|---|---|---|
| **IO16、IO17** | 絕對不能接任何外部裝置 | 查 `variant.cpp`（`ameba_hub8735_ultra`），整張腳位表裡只有 D14、D16、D17 這三根的 `MODE_NOT_INITIAL` 被拿掉，代表開機就被系統佔用。D16/D17 是 `PIO_UART` 成對出現，幾乎可以確定是板子內建的除錯/燒錄序列埠腳位。接了東西上去會讓**整片板子燒不進去**（不是功能跑不動，是完全燒不進去），跟接的裝置、程式碼都無關。 |
| **IO0、IO1（硬體 I2C1 / `Wire1`）** | 只要 sketch 裡用到 `Camera`/`VideoStream`，這兩根就不能再接別的 I2C 裝置 | 板載相機感光元件本身是用 I2C1 當控制匯流排，開機時 Boot ROM 載入 ISP 校正資料就已經把 I2C1 佔走了。沒有相機的 sketch（例如最早期的手刻 OLED 範例）才能安全用這兩根接 I2C 裝置。 |
| **IO3、IO4（硬體 I2C0 / `Wire`）** | 不能用硬體 I2C（`Wire.begin()` 會失敗） | 這兩根的 pinmux 跟這顆 SoC 的 JTAG/RFAFE_CTRL 除錯功能共用，開機就被除錯介面佔用。呼叫 `Wire.begin()` 會被 pin 衝突檢查擋下，序列埠狂噴 `[MISC Err] Pin ... is conflicted`。**所有 OLED sketch 都是接在 IO3(SDA)/IO4(SCL)，但用的是 U8g2 的「軟體 I2C」**，不走硬體 I2C0。 |
| **A0 ≠ IO2 的直覺對應** | 類比腳位命名跟 IO 編號不是你想的那樣 | `variant.h` 裡：`A0=IO2(PF_0)`、`A1=IO1(PF_1)`、`A2=IO0(PF_2)`。如果你把感測器接在 IO0，要讀 `A2`，不是 `A0`。接錯的話讀到的會是完全浮接、沒接東西的腳位，數值會是雜訊亂跳。 |
| **`pinMode()` 重複呼叫會變慢** | 不是功能問題，是效能陷阱 | 這顆核心的 `pinMode()`（`wiring_digital.c`）只要偵測到該腳位已經初始化過，就會印一行 `[WARNING] This port N is already initialed...`，這個 `printf` 很慢。U8g2 內建的 Arduino 軟體 I2C GPIO callback 每傳一個 bit 就呼叫一次 `pinMode()`，一張 OLED 畫面有幾千個 bit，會被這些警告拖到看起來像當機。**所有用到 U8g2 軟體 I2C 的 sketch，都是自己寫 `u8x8_gpio_and_delay_fast()` callback**，開機只 `pinMode(OUTPUT)` 一次，之後全程只用 `digitalWrite()`，不要照抄 U8g2 預設的 `U8G2_..._F_SW_I2C` 現成 class（那個 class 內部寫死用會變慢的官方 callback）。 |
| **`tone()` 要求 PWM 腳位** | 不是隨便一根腳位都能接蜂鳴器 | `Tone.cpp` 裡 `tone()` 會檢查腳位必須支援 `PIO_PWM`，沒有這個標記的腳位（例如 IO7）會失敗。目前蜂鳴器固定用 **IO19**（有 PWM、沒被其他 sketch 占用）。 |

查腳位能力的方法：看 `C:\Users\<你>\AppData\Local\Arduino15\packages\ideasHatch\hardware\AmebaPro2\4.0.15-Release\variants\ameba_hub8735_ultra\variant.cpp` 裡的 `g_APinDescription` 表格，每一行最後的 `PIO_xxx` 旗標就是那根腳位支援的功能，`MODE_NOT_INITIAL` 有沒有被註解掉就是有沒有被系統保留。

## 目前使用中的腳位總表

| 腳位 | 用途 | 用在哪些 sketch |
|---|---|---|
| IO0 / IO1 | 相機內建 I2C1（有相機時保留） | `03-2_camera_oled` |
| IO3 / IO4 | OLED 軟體 I2C（SDA/SCL） | `03_oled_hello_u8g2`、`04_light_sensor_oled`、`03-2_camera_oled`、`05~10`、`12` 全系列 |
| IO2 | 光敏電阻（`A0`） | `04_light_sensor_oled` |
| IO0 | 光敏電阻（`A2`，沒用相機時才可用） | `04_light_sensor_oled`、`13_mqtt_dht11_oled` |
| IO12 | FUNC 按鈕（開機高電位，按下接地變低電位） | `00-2_func_runner_game`、`10_air_quality_oled`、`12`（手動換頁） |
| IO19 | 蜂鳴器訊號腳（需要 PWM） | `08_buzzer_beep`、`09_dht11_oled_led_alarm`、`10_air_quality_oled`、`12` |
| IO20 | DHT11 溫濕度感測器 Data 腳（單線通訊，自己刻 bit-bang） | `05`、`06`、`07`、`09`、`10`、`12` |
| IO21 | 伺服馬達訊號線（PWM） | `16_servo_func`、`17_face_servo` |
| IO22 / IO23 / IO24 | 外接 LED（紅/黃/綠） | `02_traffic_light`、`04_light_sensor_oled`、`07`、`09`、`10`、`12` |
| IO25 / IO26 | 板載藍/綠 LED（`LED_G`/`LED_B`） | `01_led_pattern` |

## `learning/` 底下每個 sketch 在做什麼

資料夾名稱必須跟裡面的 `.ino` 檔名一樣，這是 Arduino 的硬性規定。編號大致照學習順序排，`-2` 結尾代表「同一個元件接法、換一種玩法」的分支版本。

- **00_serial_test**：最基本的序列埠輸出測試。
- **00-2_func_runner_game**：FUNC 按鈕（IO12）控制的暴龍快跑風格遊戲，馬力歐式可變跳躍高度（按住剪短放開）。
- **01_led_pattern**：板載藍/綠 LED 跑燈。
- **02_traffic_light**：外接三色 LED 模擬紅綠燈。
- **03_oled_hello**：手刻（非 U8g2）SSD1306 驅動，走 Wire1 硬體 I2C（IO0/IO1），證明這兩根腳位沒接相機時可以安全用硬體 I2C。
- **03_oled_hello_u8g2**：OLED 改用 U8g2，走 IO3/IO4 軟體 I2C（示範怎麼避開 JTAG 衝突 + pinMode 警告拖慢兩個坑）。
- **03-2_camera_oled**：板載相機畫面縮圖顯示到 OLED 上，用 `VIDEO_JPEG` + 內建 `JPEGDEC` 解碼（不是 `VIDEO_RGB`，那條路線在這顆晶片的 SDK 裡走不通）。
- **04_light_sensor_oled**：光敏電阻 + OLED 顯示 + 三色 LED 依亮度分級。
- **05_dht11_oled**：DHT11 溫濕度 + OLED 純文字顯示。
- **06_dht11_oled_chart**：DHT11 + OLED 卡片式儀表板 + 歷史趨勢折線圖，溫度計/水滴圖示依比例填滿。
- **07_dht11_oled_led**：06 的基礎上加三色 LED，依濕度/溫度門檻亮燈。
- **08_buzzer_beep**：蜂鳴器警笛聲（頻率掃描）測試。
- **09_dht11_oled_led_alarm**：07 + 08 合體，紅/黃燈亮起時蜂鳴器循環播警笛聲，正常時靜音。
- **10_air_quality_oled**：內建 WiFi + HTTPS 抓環境部 `AQX_P_432`（桃園市中壢站），OLED 五頁輪播（AQI 表情+色階量表 / PM2.5·PM10·O3·NO2 卡片 / 風速風向羅盤 / 室內溫濕度卡片 / 溫濕度趨勢圖），標題列有 WiFi 訊號格，每分鐘更新資料、10 秒自動換頁、FUNC 鍵手動換頁。WiFi 帳密與 API key 放 `secrets.h`（已 gitignore），第一次請複製 `secrets.h.example`。整合 09 的 LED（IO22~24）+ 蜂鳴器（IO19）室內溫濕度警報（警笛非阻塞、標題列閃爍警告三角形），另接 DHT11（IO20）與 FUNC 鍵（IO12），OLED 用 IO3/IO4。
- **12_air_quality_thingspeak**：10 的分支，額外每 15 秒把室內溫度、濕度、AQI、PM2.5 上傳到 ThingSpeak（field1~4）做長期追蹤。金鑰放 `secrets.h`（已 gitignore，範例見 `secrets.h.example`）。
- **13_mqtt_dht11_oled**：DHT11 溫濕度 + 光敏電阻（IO0/A2，處理方式同 04）每 30 秒用 MQTT 發佈到 `mqttgo.io:1883`、topic `kai0124/class105/data`，payload `{"temp": 25, "humi": 60, "light": 75}`。OLED 顯示 WiFi/MQTT 連線進度與儀表板（溫濕度卡片與光線頁每 5 秒輪播）。clientId 為隨機亂數。WiFi 帳密放 `secrets.h`（已 gitignore）。
- **14_camera_rtsp**：HD 1280x720 H.264 + AAC 聲音的 RTSP 串流，IP 顯示在 OLED。
- **15_face_detection_rtsp**：人臉偵測 + 識別（SCRFD + MobileFaceNet），陌生人自動登記成 P01、P02…，可在網頁登記／改名／刪除／備份；畫面用 JPEG 快照（`/snap`）輪流要，人臉框由網頁疊上去。最多登記 20 人。
- **15-1_hand_filter**：拇指尖＋食指尖拉出取景框、框內即時套 13 種濾鏡，捏合換濾鏡、停住自動拍照、彩繪模式。手部關鍵點用自己改寫的 `HandTracker`（核心的 NNGestureDetection 拿不到座標）。
- **16_servo_func**：FUNC 鍵讓 IO21 的伺服馬達 0→90→180→0 循環（`AmebaServo`）。
- **17_face_servo**：15 + 16，認識的人轉 70°、陌生人轉 -20°（脈寬下限放寬到 340µs）。已取消自動登記陌生人，只剩 FUNC 鍵／網頁手動登記。
- **15-2_finger_runner**：兩指跑酷（食指＋中指當兩條腿，輪流抬放跑步、同抬跳躍、V 字張開衝刺）。遊戲邏輯全在網頁，板子只送 `/snap` 與手部關鍵點 SSE。已編譯、用假資料伺服器測過網頁，**實機尚未驗證：目前回報偵測不到手、網頁沒有骨架**。已放寬手掌信心度到 0.35，並在序列埠每 5 秒印「手部NN 回呼／没找到手／校正失败」計數，下一步依數字判斷（回呼 0＝NN 管線沒跑；没找到手多＝模型看不到；校正失败多＝手太靠邊）。
- **18_yolo_detection**：內建範例 ObjectDetectionCallback 的 YOLOv4-tiny（80 類），網頁疊框＋中文類別名＋信心度過濾＋統計；伺服馬達沿用 17（看到觸發類別預設 person 轉 70°，消失 1.5 秒轉 -20°，網頁可換類別）。NN 用 576x432（4:3）對齊 640x480 JPEG。已編譯，實機未測。
- **19_yolo_fun**：18 另存＋左上角目前人數、進出記錄、最高人數、擁擠警示（閃紅框＋音效）、類別小圖示（全在網頁端）。已編譯，實機未測。

## 這片板子 / SDK 的額外踩坑（網頁與影像相關）

- **核心的 `snprintf` 遇到含中文的 `%s`，回傳長度會少算**：不要用回傳值累加位置，改用 `strlen()` 重新量（15 的狀態 JSON 因此壞掉，網頁一直顯示離線）。
- **`WiFiClient` 的解構函式會關掉 socket，複製出來的物件共用同一個 socket**：要把連線留下來時，先複製、再把原物件 `setNonBlockingMode()`；不要 `stop()` 已被瀏覽器關掉的舊連線（編號可能已被新連線重複使用）。
- **連續 MJPEG（multipart/x-mixed-replace）在瀏覽器只顯示第一張**：改成網頁一張一張向 `/snap` 要快照。`Camera.getImage()` 在 snapshot 模式每次都會拍新的，並等畫面送達才返回。
- **HTTP 回應一定要帶 `Content-Length`**，否則收到一半的 JSON 會被當成壞掉。
- **這個 SDK 只有 HTTPS 用戶端，沒有 HTTPS 伺服器**，區網內用 HTTP。
- **NN 標頭會取消 Arduino 的 `min()`/`max()` 巨集**，同一個檔案裡不要用，改寫判斷式。
- **IO0/IO1 是板載相機的 I2C1**，有用相機的 sketch 不要在這兩根接東西。

## 文件/溝通慣例

- **`AI_SYNC_LOG.md`**：Claude 跟 Codex 兩個 AI 交接用的固定工具，每次改動都會記錄原因（尤其是上面那些踩坑經驗），看完自己該處理的條目要刪掉那一則，但檔案本身要留著繼續用，別整個刪掉。
- **`docs/`**：接線圖、腳位查核來源、測試紀錄放這裡。
- **`project/`**：目前還是空的，留給後期正式的智慧節能專案用，建議開始前先建一個跟專案同名的 sketch 資料夾（例如 `project/SmartEnergy/SmartEnergy.ino`）。

## 這份文件可能還缺什麼

- 完整接線圖（目前只有文字描述腳位，沒有圖）。
- `project/` 底下的正式專案還沒開始寫。
- 沒有自動化測試或 CI，驗證方式目前都是人工燒錄 + 看 OLED/序列埠。

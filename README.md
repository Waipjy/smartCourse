# HUB-8735 Ultra 智慧節能

本資料夾用於 AmebaPro2／HUB-8735 Ultra 的學習與後續專案開發。

## 開發環境

- Arduino IDE 2.3.10
- AmebaPro2 core 4.0.15-Release
- 開發板：`HUB-8735_ultra`
- 目前偵測到的序列埠：`COM4`

## 資料夾

- `learning/`：循序學習與硬體驗證範例
- `project/`：後期正式智慧節能專案程式
- `docs/`：接線、需求、通訊協定與測試紀錄

Arduino sketch 的資料夾名稱必須與 `.ino` 檔案名稱相同。

## 已確認的板載 LED

- 藍色 LED：`LED_B`，板上腳位 `IO26`，AmebaPro2 Arduino 腳位 `D26`，底層 GPIO `PF_9`
- 綠色 LED：`LED_G`，板上腳位 `IO25`，AmebaPro2 Arduino 腳位 `D25`，底層 GPIO `PE_6`
- 紅色 LED：原廠電路圖顯示直接接 3.3V，沒有作為本範例的 GPIO 輸出

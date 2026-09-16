# 文件與測試紀錄

可在此保存以下資料：

- HUB-8735 Ultra 接線圖與 GPIO 對照
- 感測器及控制元件規格
- Modbus／MQTT／HTTP 通訊文件
- 測試步驟與測試結果
- 需求變更紀錄

## LED 腳位查核來源

原廠 HUB-8735 Ultra 電路圖顯示：綠色 LED 由 `GPIOE_6` 控制，藍色 LED 由 `LED_B` 控制；板上標示分別為 `IO25` 與 `IO26`，AmebaPro2 的 `HUB-8735_ultra` variant 將它們對應為 `D25` 與 `D26`。

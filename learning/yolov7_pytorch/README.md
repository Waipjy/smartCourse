# YOLOv7 + PyTorch（電腦端，跟 HUB-8735 板子無關）

用官方 [WongKinYiu/yolov7](https://github.com/WongKinYiu/yolov7) 做物體偵測：推論（圖片／影片／攝影機）與自訂資料集訓練。
官方程式碼不放進這個 repo，由 `setup_yolov7.py` 下載到 `yolov7/`；這裡只放包裝腳本與相容補丁。

## 快速開始

```bash
pip install -r requirements.txt        # torch / torchvision 請另外照 requirements.txt 開頭說明安裝 GPU 版
python setup_yolov7.py                 # clone 官方程式碼 + 下載 yolov7-tiny.pt（加 --full 再下載 yolov7.pt）
python detect.py                       # 預設跑 yolov7/inference/images，結果在 runs/detect/exp/
```

## 推論 `detect.py`

```bash
python detect.py --source photo.jpg
python detect.py --source video.mp4 --weights weights/yolov7.pt --conf 0.4
python detect.py --source 0 --view               # 攝影機 0，按 q 離開
python detect.py --source rtsp://... --classes 0 2   # 只留 人(0)、汽車(2)
python detect.py --source img.jpg --device cpu
```

| 參數 | 說明 |
|---|---|
| `--weights` | 權重檔，預設 `weights/yolov7-tiny.pt` |
| `--source` | 檔案、資料夾、攝影機編號或串流網址 |
| `--img` / `--conf` / `--iou` | 推論尺寸（預設 640）、信心度門檻（0.25）、NMS IoU（0.45） |
| `--classes` | 只保留指定的 COCO 類別編號 |
| `--device` | `0`（GPU）或 `cpu`，留空自動選 |
| `--view` / `--no-save` | 即時視窗／不存檔 |

## 訓練自己的資料集 `train.py`

1. 照 `data/custom.yaml` 裡的說明準備 YOLO 格式資料集（放 `datasets/mydata/`，不進 git），改好路徑與類別。
2. 開始訓練：

```bash
python train.py --data data/custom.yaml --epochs 100 --batch-size 16
```

沒給的參數用 yolov7-tiny 預設；其他參數（`--device`、`--resume`…）原樣傳給官方 `train.py`。
結果在 `runs/train/<name>/`（`weights/best.pt` 是最好的一次），拿去 `detect.py --weights` 用。
顯示卡記憶體不夠就調小 `--batch-size`；要更準可改用 `--weights weights/yolov7.pt --cfg yolov7/cfg/training/yolov7.yaml --hyp yolov7/data/hyp.scratch.p5.yaml`。

## 相容性補丁 `compat.py`（為什麼需要）

官方 YOLOv7 寫於 2022 年，在新環境（這裡是 Python 3.13、torch 2.6、numpy 2.x）會出問題，所以不改官方原始碼，改在載入前套補丁：

- **torch ≥ 2.6**：`torch.load` 預設 `weights_only=True`，讀不了 YOLOv7 權重 → 預設改回 `False`（只載入信任的權重檔）。
- **numpy ≥ 1.24**：`np.int`、`np.float` 等別名被移除，官方 `datasets.py` 還在用 → 補回別名。
- **OpenCV 讀不了中文路徑**：這個專案放在「智慧節能」資料夾底下，`cv2.imread/imwrite` 會失敗 → 改成用 numpy 讀寫位元組。

## 驗證過的

在這台電腦（RTX GPU、torch 2.6.0+cu124、Python 3.13）上：`setup_yolov7.py` 下載成功；`detect.py` 用預設圖片偵測正確（例如 bus.jpg：4 人 1 公車）並存檔；`train.py --help` 能正常載入官方腳本。**完整訓練還沒跑過**（需要自己的資料集）。

## 授權

YOLOv7 為 GPL-3.0。這裡下載的官方程式碼與權重各自適用其授權。

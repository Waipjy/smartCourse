"""訓練你自己的資料集（包裝官方 yolov7/train.py，並套用 compat 補丁）。

    python train.py --data data/custom.yaml --epochs 100
    python train.py --data data/custom.yaml --batch-size 8 --name my_exp
    python train.py --data data/custom.yaml --weights weights/yolov7.pt --cfg yolov7/cfg/training/yolov7.yaml \
                    --hyp yolov7/data/hyp.scratch.p5.yaml

沒給的參數會用 yolov7-tiny 的預設值；其他參數（--device、--workers、--resume ...）原樣傳給官方 train.py，
完整列表見 `python yolov7/train.py -h`。結果在 runs/train/<name>/，最好的權重是 weights/best.pt。
資料集格式見 README.md 與 data/custom.yaml。
"""
import runpy
import sys
from pathlib import Path

import compat

compat.check_repo()
ROOT = Path(__file__).resolve().parent

defaults = {
    "--weights": str(ROOT / "weights" / "yolov7-tiny.pt"),
    "--cfg": str(ROOT / "yolov7" / "cfg" / "training" / "yolov7-tiny.yaml"),
    "--data": str(ROOT / "data" / "custom.yaml"),
    "--hyp": str(ROOT / "yolov7" / "data" / "hyp.scratch.tiny.yaml"),
    "--epochs": "100",
    "--batch-size": "16",
    "--project": str(ROOT / "runs" / "train"),
    "--name": "exp",
}
args = sys.argv[1:]
for key, value in defaults.items():
    if key not in args:
        args += [key, value]
if "--img-size" not in args and "--img" not in args:
    args += ["--img-size", "640", "640"]

# 官方 train.py 用相對路徑找 data/、cfg/，所以要在 yolov7/ 底下執行
import os  # noqa: E402

os.chdir(ROOT / "yolov7")
sys.argv = [str(ROOT / "yolov7" / "train.py")] + args
runpy.run_path(sys.argv[0], run_name="__main__")

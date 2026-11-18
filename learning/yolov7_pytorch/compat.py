"""讓官方 YOLOv7 程式碼能在新版 PyTorch / NumPy 上跑的小補丁（不修改 yolov7/ 裡的原始碼）。

- PyTorch >= 2.6：torch.load 預設 weights_only=True，讀 YOLOv7 的 .pt 權重會失敗 -> 預設改回 False。
  只載入你信任的權重檔（官方 release 或自己訓練的）。
- NumPy >= 1.24：移除了 np.int / np.float / np.bool 這些別名，官方 datasets.py 等還在用 -> 補回去。

用法：在 import yolov7 的任何模組之前先 `import compat`。
"""
import sys
import warnings
from pathlib import Path

import numpy as np
import torch

ROOT = Path(__file__).resolve().parent
YOLOV7_DIR = ROOT / "yolov7"

# 讓 `from models.experimental import ...`、`from utils.general import ...` 找得到官方程式碼
if str(YOLOV7_DIR) not in sys.path:
    sys.path.insert(0, str(YOLOV7_DIR))

_orig_load = torch.load


def _load(*args, **kwargs):
    kwargs.setdefault("weights_only", False)
    return _orig_load(*args, **kwargs)


torch.load = _load

with warnings.catch_warnings():
    warnings.simplefilter("ignore")
    for _name, _type in (("int", int), ("float", float), ("bool", bool), ("object", object)):
        if _name not in vars(np):
            setattr(np, _name, _type)


# Windows 上 cv2.imread / imwrite 讀寫不了含中文的路徑（這個專案放在「智慧節能」底下就會遇到）-> 改用 numpy 讀寫位元組
import cv2  # noqa: E402

_orig_imread, _orig_imwrite = cv2.imread, cv2.imwrite


def _imread(path, flags=cv2.IMREAD_COLOR):
    try:
        return cv2.imdecode(np.fromfile(str(path), dtype=np.uint8), flags)
    except Exception:
        return _orig_imread(str(path), flags)


def _imwrite(path, img, params=None):
    ext = Path(str(path)).suffix or ".jpg"
    ok, buf = cv2.imencode(ext, img, params if params is not None else [])
    if ok:
        buf.tofile(str(path))
    return ok


cv2.imread, cv2.imwrite = _imread, _imwrite


def check_repo():
    if not (YOLOV7_DIR / "models").is_dir():
        raise SystemExit("找不到 yolov7/ 資料夾，請先執行：python setup_yolov7.py")

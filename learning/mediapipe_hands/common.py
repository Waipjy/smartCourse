"""01_yolo / 02_yolo 共用的小工具：中文字型、文字繪製、嗶聲。"""
import threading
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

try:
    import winsound
except ImportError:  # 非 Windows 就沒有嗶聲
    winsound = None

FONT_PATHS = ["C:/Windows/Fonts/msjh.ttc", "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/mingliu.ttc",
              "/System/Library/Fonts/PingFang.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"]
_fonts = {}


def font(size):
    if size not in _fonts:
        for p in FONT_PATHS:
            if Path(p).exists():
                _fonts[size] = ImageFont.truetype(p, size, index=0)
                break
        else:
            _fonts[size] = ImageFont.load_default()
    return _fonts[size]


def beep(freq, ms):
    if winsound:
        threading.Thread(target=winsound.Beep, args=(freq, ms), daemon=True).start()


class Texts:
    """收集要畫的文字，最後用 PIL 一次畫完（OpenCV 畫不出中文）。"""

    def __init__(self):
        self.items = []

    def add(self, xy, text, size=20, fg=(255, 255, 255), bg=None):
        self.items.append((xy, text, size, fg, bg))

    def draw(self, frame):
        if not self.items:
            return frame
        img = Image.fromarray(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB), "RGB")
        d = ImageDraw.Draw(img, "RGBA")
        for (x, y), text, size, fg, bg in self.items:
            f = font(size)
            if bg:
                l, t, r, b = d.textbbox((x, y), text, font=f)
                d.rectangle((l - 4, t - 2, r + 4, b + 3), fill=bg)
            d.text((x, y), text, font=f, fill=fg)
        return cv2.cvtColor(np.asarray(img), cv2.COLOR_RGB2BGR)

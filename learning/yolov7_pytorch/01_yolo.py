"""01_yolo：用電腦的 webcam 即時做 YOLOv7 物體辨識，直接執行就能玩。

    python 01_yolo.py

按鍵：
    ENTER  拍照（存到 snapshots/，連框一起存）      ESC  離開
    M      把「人」打馬賽克（隱私模式）              F    切換只看：全部 / 人 / 車輛
    G      開始 / 結束「尋寶遊戲」                    H    顯示 / 隱藏說明
    +/-    調整信心度門檻

有趣的功能：
    - 中文標籤、左上角即時人數與 FPS、右側各類物體數量
    - 人數達到 3 人會擁擠警示（紅框 + 嗶聲），有人進來 / 離開會跳提示
    - 尋寶遊戲：隨機指定一種物體（杯子、手機、書…），60 秒內在鏡頭前拿出來，越快分數越高
第一次執行會自動下載官方程式碼與 yolov7-tiny.pt（約 12 MB）。
"""
import random
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

# 第一次執行：自動取得官方程式碼和權重
if not (ROOT / "yolov7" / "models").is_dir() or not (ROOT / "weights" / "yolov7-tiny.pt").exists():
    import setup_yolov7

    setup_yolov7.clone()
    setup_yolov7.download("yolov7-tiny.pt")

import compat  # noqa: E402,F401

import cv2  # noqa: E402
import numpy as np  # noqa: E402
import torch  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

from models.experimental import attempt_load  # noqa: E402
from utils.datasets import letterbox  # noqa: E402
from utils.general import check_img_size, non_max_suppression, scale_coords  # noqa: E402
from utils.torch_utils import select_device  # noqa: E402

try:
    import winsound
except ImportError:  # 非 Windows 就沒有嗶聲
    winsound = None

WEIGHTS = ROOT / "weights" / "yolov7-tiny.pt"
SNAP_DIR = ROOT / "snapshots"
IMG_SIZE = 640
CAM_INDEX = 0
WINDOW = "01_yolo  [ENTER] photo  [ESC] quit  [H] help"
CROWD = 3  # 幾個人算擁擠

CN = ("人 腳踏車 汽車 機車 飛機 公車 火車 卡車 船 紅綠燈 消防栓 停止標誌 停車收費表 長椅 鳥 貓 狗 馬 羊 牛 大象 熊 斑馬 長頸鹿 背包 雨傘 手提包 領帶 行李箱 飛盤 "
      "滑雪板 單板滑雪 球 風箏 球棒 棒球手套 滑板 衝浪板 網球拍 瓶子 酒杯 杯子 叉子 刀子 湯匙 碗 香蕉 蘋果 三明治 柳橙 花椰菜 紅蘿蔔 熱狗 披薩 甜甜圈 蛋糕 椅子 沙發 盆栽 床 "
      "餐桌 馬桶 電視 筆電 滑鼠 遙控器 鍵盤 手機 微波爐 烤箱 烤麵包機 水槽 冰箱 書 時鐘 花瓶 剪刀 泰迪熊 吹風機 牙刷").split()
VEHICLES = {1, 2, 3, 5, 6, 7, 8}
# 尋寶遊戲可以指定的東西（在桌上容易找到的）
TREASURES = [39, 41, 67, 73, 63, 64, 65, 66, 76, 24, 46, 47, 74, 77, 45, 25]
FILTERS = ("全部", "只看人", "只看車輛")

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


def color_for(i):
    random.seed(i * 7 + 3)
    return tuple(random.randint(60, 255) for _ in range(3))  # BGR


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


def mosaic(frame, x1, y1, x2, y2, blocks=10):
    roi = frame[y1:y2, x1:x2]
    if roi.size == 0:
        return
    h, w = roi.shape[:2]
    small = cv2.resize(roi, (max(1, w // blocks), max(1, h // blocks)), interpolation=cv2.INTER_LINEAR)
    frame[y1:y2, x1:x2] = cv2.resize(small, (w, h), interpolation=cv2.INTER_NEAREST)


class Game:
    """尋寶遊戲：60 秒內，每個目標最多 20 秒，越快找到分數越高。"""

    TOTAL, PER = 60, 20

    def __init__(self):
        self.on = False
        self.score = 0
        self.msg = ""
        self.msg_until = 0

    def start(self):
        self.on, self.score, self.t_start = True, 0, time.time()
        self.best = 0
        self.next_target()

    def next_target(self):
        choices = [t for t in TREASURES if t != getattr(self, "target", None)]
        self.target = random.choice(choices)
        self.t_target = time.time()

    def toast(self, text):
        self.msg, self.msg_until = text, time.time() + 1.5

    def update(self, found_classes):
        if not self.on:
            return
        now = time.time()
        if now - self.t_start >= self.TOTAL:
            self.on = False
            self.toast(f"時間到！總分 {self.score}")
            self.msg_until = now + 4
            beep(600, 300)
            return
        if self.target in found_classes:
            pts = max(1, int(self.PER - (now - self.t_target)))
            self.score += pts
            self.toast(f"找到{CN[self.target]}！+{pts}")
            beep(1000, 120)
            self.next_target()
        elif now - self.t_target >= self.PER:
            self.toast(f"沒找到{CN[self.target]}，換下一個")
            self.next_target()


def main():
    device = select_device("")
    half = device.type != "cpu"
    model = attempt_load(str(WEIGHTS), map_location=device)
    stride = int(model.stride.max())
    imgsz = check_img_size(IMG_SIZE, s=stride)
    if half:
        model.half()
    model.eval()
    names = model.names
    print(f"模型載入完成，裝置：{device}")

    cap = cv2.VideoCapture(CAM_INDEX, cv2.CAP_DSHOW) if sys.platform == "win32" else cv2.VideoCapture(CAM_INDEX)
    if not cap.isOpened():
        raise SystemExit(f"打不開攝影機 {CAM_INDEX}（被其他程式占用，或把 CAM_INDEX 改成 1 試試）")
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)

    conf_thr, filt = 0.40, 0
    do_mosaic, show_help = False, True
    game = Game()
    fps, last_t = 0.0, time.time()
    prev_people, people_stable, people_hist = 0, 0, []
    toasts = []          # [(text, until)]
    flash_until = 0
    help_until = time.time() + 8

    print("ENTER 拍照 / ESC 離開 / H 說明")
    while True:
        ok, frame = cap.read()
        if not ok:
            print("讀不到攝影機畫面")
            break
        frame = cv2.flip(frame, 1)  # 鏡像，像照鏡子比較直覺

        # ---- 推論 ----
        img = letterbox(frame, imgsz, stride=stride)[0]
        img = np.ascontiguousarray(img[:, :, ::-1].transpose(2, 0, 1))
        t = torch.from_numpy(img).to(device)
        t = (t.half() if half else t.float()) / 255.0
        with torch.no_grad():
            pred = non_max_suppression(model(t[None])[0], conf_thr, 0.45)[0]
        dets = []
        if pred is not None and len(pred):
            pred[:, :4] = scale_coords(t.shape[1:], pred[:, :4], frame.shape).round()
            dets = [(int(x1), int(y1), int(x2), int(y2), float(c), int(k)) for x1, y1, x2, y2, c, k in pred.tolist()]
        if filt == 1:
            dets = [d for d in dets if d[5] == 0]
        elif filt == 2:
            dets = [d for d in dets if d[5] in VEHICLES]

        counts = {}
        for d in dets:
            counts[d[5]] = counts.get(d[5], 0) + 1
        people = counts.get(0, 0)

        # 人數連續 5 幀一致才算進出，避免閃爍
        people_hist = (people_hist + [people])[-5:]
        if len(people_hist) == 5 and len(set(people_hist)) == 1 and people_hist[0] != prev_people:
            if people_hist[0] > prev_people:
                toasts.append(("有人進來了", time.time() + 1.5))
                beep(880, 80)
            else:
                toasts.append(("有人離開了", time.time() + 1.5))
                beep(440, 80)
            prev_people = people_hist[0]
        crowded = people >= CROWD

        game.update({d[5] for d in dets})

        # ---- 畫面 ----
        out = frame.copy()
        texts = Texts()
        for x1, y1, x2, y2, c, k in dets:
            if k == 0 and do_mosaic:
                mosaic(out, max(0, x1), max(0, y1), x2, y2)
            col = color_for(k)
            hit = game.on and k == game.target
            cv2.rectangle(out, (x1, y1), (x2, y2), (0, 255, 255) if hit else col, 4 if hit else 2)
            label = f"{CN[k] if k < len(CN) else names[k]} {c:.0%}"
            texts.add((x1 + 2, max(0, y1 - 28)), label, 20, (0, 0, 0), tuple(reversed(col)) + (255,))

        h, w = out.shape[:2]
        if crowded:
            cv2.rectangle(out, (0, 0), (w - 1, h - 1), (0, 0, 255), 10 if int(time.time() * 3) % 2 else 4)

        # HUD：左上
        now = time.time()
        fps = fps * 0.9 + (1 / max(now - last_t, 1e-3)) * 0.1
        last_t = now
        texts.add((12, 10), f"人數 {people}" + ("　擁擠！" if crowded else ""), 34, (255, 255, 255), (200, 0, 0, 170) if crowded else (0, 0, 0, 150))
        texts.add((12, 60), f"FPS {fps:.0f}　信心度 {conf_thr:.0%}　{FILTERS[filt]}" + ("　馬賽克" if do_mosaic else ""), 18, (230, 230, 230), (0, 0, 0, 120))
        # 右側：各類數量
        y = 10
        for k, n in sorted(counts.items(), key=lambda kv: -kv[1])[:8]:
            texts.add((w - 170, y), f"{CN[k] if k < len(CN) else names[k]}　×{n}", 20, (255, 255, 255), (0, 0, 0, 130))
            y += 30
        # 遊戲
        if game.on:
            left = max(0, int(game.TOTAL - (now - game.t_start)))
            tleft = max(0, int(game.PER - (now - game.t_target)))
            texts.add((w // 2 - 190, 10), f"尋寶：找「{CN[game.target]}」！", 36, (255, 255, 0), (0, 0, 0, 170))
            texts.add((w // 2 - 190, 62), f"本題 {tleft} 秒　總時間 {left} 秒　分數 {game.score}", 22, (255, 255, 255), (0, 0, 0, 150))
        if game.msg and now < game.msg_until:
            texts.add((w // 2 - 150, h // 2 - 30), game.msg, 40, (255, 255, 120), (0, 0, 0, 180))
        # 提示
        toasts[:] = [(s, u) for s, u in toasts if u > now]
        for i, (s, _) in enumerate(toasts):
            texts.add((12, h - 100 - i * 36), s, 24, (255, 255, 255), (0, 120, 0, 200) if "進" in s else (160, 60, 0, 200))
        if show_help or now < help_until:
            lines = ["ENTER 拍照　ESC 離開　H 隱藏說明", "M 馬賽克　F 篩選　G 尋寶遊戲　+/- 信心度"]
            for i, s in enumerate(lines):
                texts.add((12, h - 64 + i * 28), s, 20, (255, 255, 255), (0, 0, 0, 140))

        out = texts.draw(out)
        if now < flash_until:  # 拍照閃光
            out = cv2.addWeighted(out, 0.4, np.full_like(out, 255), 0.6, 0)
        cv2.imshow(WINDOW, out)

        key = cv2.waitKey(1) & 0xFF
        if key == 27 or cv2.getWindowProperty(WINDOW, cv2.WND_PROP_VISIBLE) < 1:
            break
        if key in (13, 10):
            SNAP_DIR.mkdir(exist_ok=True)
            path = SNAP_DIR / f"{datetime.now():%Y%m%d_%H%M%S}.jpg"
            cv2.imwrite(str(path), out)
            flash_until = now + 0.15
            toasts.append((f"已拍照：{path.name}", now + 2))
            beep(1500, 60)
            print("已存檔", path)
        elif key in (ord("m"), ord("M")):
            do_mosaic = not do_mosaic
        elif key in (ord("f"), ord("F")):
            filt = (filt + 1) % 3
        elif key in (ord("g"), ord("G")):
            if game.on:
                game.on = False
            else:
                game.start()
        elif key in (ord("h"), ord("H")):
            show_help = not show_help
            help_until = 0
        elif key in (ord("+"), ord("=")):
            conf_thr = min(0.9, conf_thr + 0.05)
        elif key in (ord("-"), ord("_")):
            conf_thr = max(0.1, conf_thr - 0.05)

    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()

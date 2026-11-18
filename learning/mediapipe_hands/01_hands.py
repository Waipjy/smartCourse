"""01_hands：用 webcam 精準辨識「手指位置」和「手勢」（MediaPipe 手部 21 關鍵點 + 手勢辨識模型）。

    python 01_hands.py

為什麼不用 YOLO：yolov7_pytorch 的 YOLOv7（COCO）只能框出「人」，沒有手指關鍵點；手指、手勢要用手部專用模型。
這裡用 Google MediaPipe：每隻手 21 個關鍵點（指尖、指節、手腕），再加上手勢分類。第一次執行會自動下載模型（約 8 MB）。

按鍵：
    ENTER  拍照（存到 snapshots/）        ESC  離開
    D      空中繪畫：只伸食指就能在空中畫線；比 YA 換顏色；握拳 1 秒清除
    R      猜拳遊戲：電腦倒數 3 秒，你比石頭（握拳）/ 剪刀（YA）/ 布（張開手掌）
    S      顯示 / 隱藏骨架與指尖標示            H  顯示 / 隱藏說明      C  清除畫布

畫面上會顯示：每隻手的骨架、五根指尖名稱與座標、伸出幾根手指、手勢名稱與信心度、拇指和食指的捏合距離。
"""
import math
import random
import sys
import time
import urllib.request
from datetime import datetime
from pathlib import Path

import cv2
import mediapipe as mp
import numpy as np
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision

from common import Texts, beep

ROOT = Path(__file__).resolve().parent
MODEL = ROOT / "weights" / "gesture_recognizer.task"
MODEL_URL = "https://storage.googleapis.com/mediapipe-models/gesture_recognizer/gesture_recognizer/float16/1/gesture_recognizer.task"
SNAP_DIR = ROOT / "snapshots"
CAM_INDEX = 0
WINDOW = "01_hands  [ENTER] photo  [ESC] quit  [H] help"

# 21 關鍵點：0 手腕；1-4 拇指；5-8 食指；9-12 中指；13-16 無名指；17-20 小指
CONNECTIONS = [(0, 1), (1, 2), (2, 3), (3, 4), (0, 5), (5, 6), (6, 7), (7, 8), (5, 9), (9, 10), (10, 11), (11, 12),
               (9, 13), (13, 14), (14, 15), (15, 16), (13, 17), (17, 18), (18, 19), (19, 20), (0, 17)]
TIPS = [4, 8, 12, 16, 20]
FINGER_NAMES = ["拇指", "食指", "中指", "無名指", "小指"]
FINGER_COLORS = [(0, 165, 255), (0, 255, 255), (0, 255, 0), (255, 200, 0), (255, 0, 200)]  # BGR
GESTURE_CN = {"Closed_Fist": "握拳", "Open_Palm": "張開手掌", "Pointing_Up": "食指向上", "Thumb_Down": "倒讚",
              "Thumb_Up": "讚", "Victory": "勝利 YA", "ILoveYou": "我愛你"}
PAINT_COLORS = [(0, 255, 255), (0, 0, 255), (0, 255, 0), (255, 128, 0), (255, 0, 255), (255, 255, 255)]


def ensure_model():
    if MODEL.exists():
        return
    MODEL.parent.mkdir(exist_ok=True)
    print("下載手勢模型（約 8 MB）...")
    tmp = MODEL.with_suffix(".part")
    urllib.request.urlretrieve(MODEL_URL, tmp)
    tmp.rename(MODEL)


def dist(a, b):
    return math.hypot(a.x - b.x, a.y - b.y)


def finger_states(lm):
    """回傳 5 根手指是否伸直 [拇指, 食指, 中指, 無名指, 小指]。用「指尖比指節離手腕更遠」判斷，手轉到哪個角度都能用。"""
    wrist = lm[0]
    up = [dist(lm[4], lm[17]) > dist(lm[3], lm[17]) * 1.05 and dist(lm[4], lm[17]) > dist(lm[2], lm[17]) * 0.9]
    for tip, pip in ((8, 6), (12, 10), (16, 14), (20, 18)):
        up.append(dist(lm[tip], wrist) > dist(lm[pip], wrist) * 1.12)
    return up


def classify(lm, mp_name, mp_score):
    """先用自己的規則抓 MediaPipe 沒有的手勢（OK、搖滾、數字），其餘用 MediaPipe 的分類。"""
    up = finger_states(lm)
    palm = dist(lm[0], lm[9]) or 1e-3
    pinch = dist(lm[4], lm[8]) / palm
    n = sum(up)
    if pinch < 0.35 and up[2] and up[3] and up[4]:
        return "OK", "ok", n, pinch
    if up[1] and up[4] and not up[2] and not up[3]:
        return "搖滾", "rock", n, pinch
    if mp_name and mp_name != "None" and mp_score > 0.6:
        return GESTURE_CN.get(mp_name, mp_name), mp_name, n, pinch
    if pinch < 0.3:
        return "捏合", "pinch", n, pinch
    return f"數字 {n}" if n else "握拳", "count", n, pinch


class Painter:
    """空中繪畫：只伸食指 = 畫線；YA = 換顏色；握拳 1 秒 = 清除。"""

    def __init__(self):
        self.canvas = None
        self.last = None
        self.ci = 0
        self.fist_since = None
        self.victory_lock = 0

    def clear(self):
        if self.canvas is not None:
            self.canvas[:] = 0
        self.last = None

    def update(self, frame, hands):
        if self.canvas is None or self.canvas.shape[:2] != frame.shape[:2]:
            self.canvas = np.zeros_like(frame)
        h, w = frame.shape[:2]
        drawing = False
        now = time.time()
        for lm, g in hands:
            up = finger_states(lm)
            pt = (int(lm[8].x * w), int(lm[8].y * h))
            if up[1] and not up[2] and not up[3] and not up[4]:
                if self.last:
                    cv2.line(self.canvas, self.last, pt, PAINT_COLORS[self.ci], 8)
                self.last, drawing = pt, True
            if g[1] == "Victory" and now > self.victory_lock:
                self.ci = (self.ci + 1) % len(PAINT_COLORS)
                self.victory_lock = now + 1.0
                beep(900, 40)
            if g[1] == "Closed_Fist":
                self.fist_since = self.fist_since or now
                if now - self.fist_since > 1.0:
                    self.clear()
                    self.fist_since = None
            else:
                self.fist_since = None
        if not drawing:
            self.last = None
        mask = self.canvas.any(axis=2)
        frame[mask] = self.canvas[mask]
        return drawing


class RPS:
    """猜拳：倒數 3 秒，歸零瞬間讀你的手勢。"""

    NAMES = {"Closed_Fist": "石頭", "Victory": "剪刀", "Open_Palm": "布"}
    BEATS = {"石頭": "剪刀", "剪刀": "布", "布": "石頭"}

    def __init__(self):
        self.state = "idle"
        self.wins = self.losses = self.ties = 0
        self.until = 0
        self.text = ""

    def start(self):
        self.state, self.t0 = "count", time.time()

    def update(self, gestures):
        now = time.time()
        if self.state == "count":
            left = 3 - (now - self.t0)
            if left <= 0:
                mine = next((self.NAMES[g] for g in gestures if g in self.NAMES), None)
                comp = random.choice(list(self.NAMES.values()))
                if not mine:
                    self.text = f"沒看清楚你出什麼（電腦出{comp}）"
                elif mine == comp:
                    self.ties += 1
                    self.text = f"你出{mine}、電腦出{comp}：平手"
                elif self.BEATS[mine] == comp:
                    self.wins += 1
                    self.text = f"你出{mine}、電腦出{comp}：你贏了！"
                    beep(1000, 150)
                else:
                    self.losses += 1
                    self.text = f"你出{mine}、電腦出{comp}：你輸了"
                    beep(400, 200)
                self.state, self.until = "result", now + 2.5
            return f"{math.ceil(left)}" if left > 0 else None
        if self.state == "result" and now > self.until:
            self.state = "idle"
        return None


def main():
    ensure_model()
    opts = vision.GestureRecognizerOptions(
        # 用位元組載入：MediaPipe 的 C 函式庫打不開含中文的路徑（這個專案放在「智慧節能」底下）
        base_options=mp_python.BaseOptions(model_asset_buffer=MODEL.read_bytes()),
        running_mode=vision.RunningMode.VIDEO, num_hands=2,
        min_hand_detection_confidence=0.5, min_hand_presence_confidence=0.5, min_tracking_confidence=0.5)
    recognizer = vision.GestureRecognizer.create_from_options(opts)

    cap = cv2.VideoCapture(CAM_INDEX, cv2.CAP_DSHOW) if sys.platform == "win32" else cv2.VideoCapture(CAM_INDEX)
    if not cap.isOpened():
        raise SystemExit(f"打不開攝影機 {CAM_INDEX}（被其他程式占用，或把 CAM_INDEX 改成 1 試試）")
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)

    painter, rps = Painter(), RPS()
    paint_on, show_skel, show_help = False, True, True
    help_until = time.time() + 8
    flash_until, toast, toast_until = 0, "", 0
    fps, last_t, t_start = 0.0, time.time(), time.time()

    print("ENTER 拍照 / ESC 離開 / H 說明")
    while True:
        ok, frame = cap.read()
        if not ok:
            print("讀不到攝影機畫面")
            break
        frame = cv2.flip(frame, 1)
        h, w = frame.shape[:2]

        rgb = mp.Image(image_format=mp.ImageFormat.SRGB, data=cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
        res = recognizer.recognize_for_video(rgb, int((time.time() - t_start) * 1000))

        info = []  # (landmarks, 中文名稱, 代碼, 伸出幾根, 捏合, 左右手, 信心度)
        for i, lm in enumerate(res.hand_landmarks):
            top = res.gestures[i][0] if res.gestures and res.gestures[i] else None
            name, code, n, pinch = classify(lm, top.category_name if top else None, top.score if top else 0)
            side = res.handedness[i][0].category_name if res.handedness else "?"
            info.append((lm, name, code, n, pinch, side, top.score if top else 0))
        # 給繪畫/猜拳用：用 MediaPipe 原始手勢代碼
        raw = [(lm, (None, res.gestures[i][0].category_name if res.gestures[i] else "None")) for i, lm in enumerate(res.hand_landmarks)]

        drawing = painter.update(frame, raw) if paint_on else False
        count_text = rps.update([r[1][1] for r in raw]) if rps.state == "count" else (rps.update([]) if rps.state == "result" else None)

        texts = Texts()
        for lm, name, code, n, pinch, side, score in info:
            pts = [(int(p.x * w), int(p.y * h)) for p in lm]
            if show_skel:
                for a, b in CONNECTIONS:
                    cv2.line(frame, pts[a], pts[b], (255, 255, 255), 2)
                for p in pts:
                    cv2.circle(frame, p, 4, (60, 60, 255), -1)
                up = finger_states(lm)
                for f, tip in enumerate(TIPS):
                    cv2.circle(frame, pts[tip], 11 if up[f] else 7, FINGER_COLORS[f], -1 if up[f] else 2)
                    texts.add((pts[tip][0] + 10, pts[tip][1] - 26),
                              f"{FINGER_NAMES[f]} ({pts[tip][0]},{pts[tip][1]})", 15, (255, 255, 255), (0, 0, 0, 130))
                # 拇指–食指捏合線
                cv2.line(frame, pts[4], pts[8], (0, 255, 255) if pinch < 0.35 else (120, 120, 120), 3)
            xs, ys = [p[0] for p in pts], [p[1] for p in pts]
            x1, y1 = max(0, min(xs) - 20), max(0, min(ys) - 20)
            cv2.rectangle(frame, (x1, y1), (max(xs) + 20, max(ys) + 20), (80, 220, 80), 2)
            texts.add((x1, max(0, y1 - 62)), f"{'左手' if side == 'Left' else '右手'}　{name}", 26, (0, 0, 0), (80, 220, 80, 230))
            texts.add((x1, max(0, y1 - 30)), f"伸出 {n} 根　捏合 {pinch:.2f}　信心 {score:.0%}", 16, (255, 255, 255), (0, 0, 0, 150))

        now = time.time()
        fps = fps * 0.9 + (1 / max(now - last_t, 1e-3)) * 0.1
        last_t = now
        texts.add((12, 10), f"手 {len(info)} 隻", 34, (255, 255, 255), (0, 0, 0, 150))
        texts.add((12, 60), f"FPS {fps:.0f}" + ("　空中繪畫" if paint_on else "") + ("　骨架" if show_skel else ""), 18, (230, 230, 230), (0, 0, 0, 120))
        if paint_on:
            cv2.rectangle(frame, (w - 70, 10), (w - 10, 50), PAINT_COLORS[painter.ci], -1)
            texts.add((w - 190, 12), "畫筆顏色", 20, (255, 255, 255), (0, 0, 0, 130))
        if rps.state != "idle":
            if count_text:
                texts.add((w // 2 - 20, h // 2 - 60), count_text, 110, (255, 255, 0), None)
            else:
                texts.add((w // 2 - 260, h // 2 - 30), rps.text, 36, (255, 255, 120), (0, 0, 0, 190))
        if rps.wins + rps.losses + rps.ties:
            texts.add((12, 92), f"猜拳　贏 {rps.wins}　輸 {rps.losses}　平 {rps.ties}", 20, (255, 255, 255), (0, 0, 0, 130))
        if toast and now < toast_until:
            texts.add((12, h - 100), toast, 24, (255, 255, 255), (0, 120, 0, 200))
        if show_help or now < help_until:
            for i, s in enumerate(["ENTER 拍照　ESC 離開　H 隱藏說明　S 骨架", "D 空中繪畫（食指畫、YA 換色、握拳 1 秒清除）　R 猜拳　C 清除畫布"]):
                texts.add((12, h - 64 + i * 28), s, 20, (255, 255, 255), (0, 0, 0, 140))

        out = texts.draw(frame)
        if now < flash_until:
            out = cv2.addWeighted(out, 0.4, np.full_like(out, 255), 0.6, 0)
        cv2.imshow(WINDOW, out)

        key = cv2.waitKey(1) & 0xFF
        if key == 27 or cv2.getWindowProperty(WINDOW, cv2.WND_PROP_VISIBLE) < 1:
            break
        if key in (13, 10):
            SNAP_DIR.mkdir(exist_ok=True)
            path = SNAP_DIR / f"hand_{datetime.now():%Y%m%d_%H%M%S}.jpg"
            cv2.imwrite(str(path), out)
            flash_until, toast, toast_until = now + 0.15, f"已拍照：{path.name}", now + 2
            beep(1500, 60)
            print("已存檔", path)
        elif key in (ord("d"), ord("D")):
            paint_on = not paint_on
        elif key in (ord("r"), ord("R")) and rps.state == "idle":
            rps.start()
        elif key in (ord("s"), ord("S")):
            show_skel = not show_skel
        elif key in (ord("c"), ord("C")):
            painter.clear()
        elif key in (ord("h"), ord("H")):
            show_help, help_until = not show_help, 0

    cap.release()
    cv2.destroyAllWindows()
    recognizer.close()


if __name__ == "__main__":
    main()

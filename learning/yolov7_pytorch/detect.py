"""YOLOv7 推論：圖片 / 資料夾 / 影片 / 攝影機，框出物體並存檔。

    python detect.py --source yolov7/inference/images
    python detect.py --source video.mp4 --weights weights/yolov7.pt --conf 0.4
    python detect.py --source 0 --view          # 攝影機 0，按 q 離開
    python detect.py --source img.jpg --classes 0 2 --device cpu

結果存在 runs/detect/exp*/，並在終端機印出每張圖偵測到的物體數量。
"""
import argparse
import time
from pathlib import Path

import compat  # noqa: F401  必須在匯入官方模組之前

import cv2
import torch

compat.check_repo()
from models.experimental import attempt_load  # noqa: E402
from utils.datasets import LoadImages, LoadStreams  # noqa: E402
from utils.general import check_img_size, non_max_suppression, scale_coords  # noqa: E402
from utils.plots import plot_one_box  # noqa: E402
from utils.torch_utils import select_device  # noqa: E402

import random  # noqa: E402

ROOT = Path(__file__).resolve().parent


def next_dir(base: Path, name: str) -> Path:
    d = base / name
    i = 2
    while d.exists():
        d = base / f"{name}{i}"
        i += 1
    d.mkdir(parents=True)
    return d


def parse():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weights", default=str(ROOT / "weights" / "yolov7-tiny.pt"))
    ap.add_argument("--source", default=str(ROOT / "yolov7" / "inference" / "images"), help="檔案 / 資料夾 / 攝影機編號（0）/ 串流網址")
    ap.add_argument("--img", type=int, default=640, help="推論尺寸")
    ap.add_argument("--conf", type=float, default=0.25, help="信心度門檻")
    ap.add_argument("--iou", type=float, default=0.45, help="NMS IoU 門檻")
    ap.add_argument("--device", default="", help="cuda 編號（如 0）或 cpu；留空自動選")
    ap.add_argument("--classes", nargs="+", type=int, help="只保留這些類別編號（COCO：0=人 2=車 ...）")
    ap.add_argument("--view", action="store_true", help="即時顯示視窗")
    ap.add_argument("--no-save", action="store_true", help="不存檔")
    ap.add_argument("--name", default="exp", help="輸出資料夾名稱（runs/detect/<name>）")
    return ap.parse_args()


@torch.no_grad()
def main():
    opt = parse()
    device = select_device(opt.device)
    half = device.type != "cpu"  # GPU 用 FP16 比較快

    model = attempt_load(opt.weights, map_location=device)
    stride = int(model.stride.max())
    imgsz = check_img_size(opt.img, s=stride)
    if half:
        model.half()
    model.eval()
    names = model.module.names if hasattr(model, "module") else model.names
    colors = [[random.randint(0, 255) for _ in range(3)] for _ in names]

    webcam = opt.source.isnumeric() or opt.source.lower().startswith(("rtsp://", "rtmp://", "http://", "https://"))
    dataset = LoadStreams(opt.source, img_size=imgsz, stride=stride) if webcam else LoadImages(opt.source, img_size=imgsz, stride=stride)

    save_dir = None if opt.no_save else next_dir(ROOT / "runs" / "detect", opt.name)
    writers = {}

    # 先跑一次空圖暖機
    if device.type != "cpu":
        model(torch.zeros(1, 3, imgsz, imgsz).to(device).type_as(next(model.parameters())))

    t0 = time.time()
    for item in dataset:
        path, img, im0s, vid_cap = item[:4]
        img = torch.from_numpy(img).to(device)
        img = img.half() if half else img.float()
        img /= 255.0
        if img.ndimension() == 3:
            img = img.unsqueeze(0)

        t1 = time.time()
        pred = model(img)[0]
        pred = non_max_suppression(pred, opt.conf, opt.iou, classes=opt.classes)
        dt = (time.time() - t1) * 1000

        for i, det in enumerate(pred):
            if webcam:
                p, im0 = Path(path[i]), im0s[i].copy()
            else:
                p, im0 = Path(path), im0s
            summary = ""
            if len(det):
                det[:, :4] = scale_coords(img.shape[2:], det[:, :4], im0.shape).round()
                for c in det[:, -1].unique():
                    n = int((det[:, -1] == c).sum())
                    summary += f"{n} {names[int(c)]}{'s' * (n > 1)}, "
                for *xyxy, conf, cls in reversed(det):
                    plot_one_box(xyxy, im0, label=f"{names[int(cls)]} {conf:.2f}", color=colors[int(cls)], line_thickness=2)
            print(f"{p.name}: {summary or '(no detections), '}{dt:.1f}ms")

            if opt.view:
                cv2.imshow(str(p), im0)
                if cv2.waitKey(1) == ord("q"):
                    raise SystemExit
            if save_dir:
                out = save_dir / (p.name if not webcam else f"webcam{i}.mp4")
                if dataset.mode == "image":
                    cv2.imwrite(str(out), im0)
                else:  # 影片 / 攝影機
                    if out not in writers:
                        fps = vid_cap.get(cv2.CAP_PROP_FPS) if vid_cap else 30
                        h, w = im0.shape[:2]
                        writers[out] = cv2.VideoWriter(str(out.with_suffix(".mp4")), cv2.VideoWriter_fourcc(*"mp4v"), fps or 30, (w, h))
                    writers[out].write(im0)

    for w in writers.values():
        w.release()
    if save_dir:
        print(f"結果存在 {save_dir}")
    print(f"總共 {time.time() - t0:.1f} 秒")


if __name__ == "__main__":
    main()

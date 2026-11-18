"""下載官方 YOLOv7 程式碼與預訓練權重。

    python setup_yolov7.py            # 程式碼 + yolov7-tiny.pt（約 12 MB）
    python setup_yolov7.py --full     # 再加上 yolov7.pt（約 75 MB）

程式碼 clone 到 ./yolov7，權重放 ./weights；兩個資料夾都不進 git（見 .gitignore）。
"""
import argparse
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_URL = "https://github.com/WongKinYiu/yolov7.git"
RELEASE = "https://github.com/WongKinYiu/yolov7/releases/download/v0.1/"


def clone():
    dest = ROOT / "yolov7"
    if (dest / "models").is_dir():
        print(f"[skip] {dest} 已存在")
        return
    print("[clone] WongKinYiu/yolov7 ...")
    subprocess.run(["git", "clone", "--depth", "1", REPO_URL, str(dest)], check=True)


def download(name):
    dest = ROOT / "weights" / name
    if dest.exists():
        print(f"[skip] {dest.name} 已存在")
        return
    dest.parent.mkdir(exist_ok=True)
    print(f"[download] {name} ...")
    tmp = dest.with_suffix(".part")
    urllib.request.urlretrieve(RELEASE + name, tmp)
    tmp.rename(dest)
    print(f"  -> {dest} ({dest.stat().st_size / 1e6:.1f} MB)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true", help="也下載完整版 yolov7.pt")
    args = ap.parse_args()
    clone()
    download("yolov7-tiny.pt")
    if args.full:
        download("yolov7.pt")
    print("完成。試跑：python detect.py --source yolov7/inference/images")


if __name__ == "__main__":
    sys.exit(main())

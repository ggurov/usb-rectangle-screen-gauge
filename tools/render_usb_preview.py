"""Render the four gauges with the real renderer and build a preview sheet.

    python tools/render_usb_preview.py

Writes tools/preview/usb_rpm.png (480x800, what the panel gets) and
tools/preview/usb_all.png (all four side by side, half scale for the README).
The raws come from the app itself, via `gauge.exe --render`.
"""
from __future__ import annotations

import os
import subprocess
import sys

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "gauge.exe")
OUT = os.path.join(REPO, "tools", "preview")
TMP = os.path.join(REPO, "build")
W, H = 480, 800
VALUES = {"rpm": 5200.0, "temp": 92.0, "boost": 1.2, "volts": 13.8}


def to_bgr(raw: bytes) -> "np.ndarray":
    px = np.frombuffer(raw, dtype="<u2").reshape(H, W)
    b = ((px & 0x1F) << 3).astype(np.uint8)
    g = (((px >> 5) & 0x3F) << 2).astype(np.uint8)
    r = ((px >> 11) << 3).astype(np.uint8)
    return np.dstack([b, g, r])


def main() -> int:
    if not os.path.exists(EXE):
        sys.exit(f"build the host app first: {EXE} is missing")
    try:
        import cv2
    except ImportError:
        sys.exit("this needs opencv-python to write the PNGs")

    frames = []
    for gauge, value in VALUES.items():
        raw_path = os.path.join(TMP, f"preview_{gauge}.raw")
        subprocess.run([EXE, "--render", raw_path, "--gauge", gauge,
                        "--value", str(value)],
                       check=True, stdout=subprocess.DEVNULL)
        with open(raw_path, "rb") as fh:
            img = to_bgr(fh.read())
        frames.append(img)
        if gauge == "rpm":
            cv2.imwrite(os.path.join(OUT, "usb_rpm.png"), img)
            print("wrote tools/preview/usb_rpm.png")

    sheet = np.hstack(frames)
    sheet = cv2.resize(sheet, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA)
    cv2.imwrite(os.path.join(OUT, "usb_all.png"), sheet)
    print(f"wrote tools/preview/usb_all.png ({sheet.shape[1]}x{sheet.shape[0]})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

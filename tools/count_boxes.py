#!/usr/bin/env python3
"""Overlay sanity check: decode a mosaic .h264 and count frames whose box
colours (red = car, yellow = person) are present. usage: count_boxes.py FILE"""
import glob
import os
import subprocess
import sys
import tempfile

import cv2

tmp = tempfile.mkdtemp()
subprocess.run(["gst-launch-1.0", "-q", "filesrc", f"location={sys.argv[1]}", "!", "h264parse", "!",
                "nvv4l2decoder", "!", "nvvidconv", "!", "video/x-raw,format=I420", "!", "jpegenc", "!",
                "multifilesink", f"location={tmp}/f_%04d.jpg"], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
counts = []
for f in sorted(glob.glob(f"{tmp}/f_*.jpg")):
    im = cv2.imread(f).astype(int)
    b, g, r = im[:, :, 0], im[:, :, 1], im[:, :, 2]
    counts.append(int((((r > 230) & (g < 80) & (b < 80)) | ((r > 230) & (g > 230) & (b < 90))).sum()))
    os.remove(f)
os.rmdir(tmp)
low = sum(c < 300 for c in counts)
print(f"{sys.argv[1]}: {len(counts)} frames, median box pixels {sorted(counts)[len(counts)//2]}, "
      f"frames with almost no boxes: {low} ({100*low/len(counts):.1f}%)")

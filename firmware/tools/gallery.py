#!/usr/bin/env python3
"""Stitch dump_gallery PPMs into one contact-sheet PNG.

    gallery.py IN_DIR OUT.png [cols] [--raw]

By default each frame is shown as the panel shows it: the EPD driver thresholds
the 2bpp canvas to 1-bit (Gray >= G2 -> white, epd.cpp gray_threshold_ = 2).
--raw shows the 4-level canvas instead.
"""
import sys, pathlib
from PIL import Image, ImageDraw
args = [a for a in sys.argv[1:] if not a.startswith("--")]
raw = "--raw" in sys.argv
src = pathlib.Path(args[0]); out = args[1]; cols = int(args[2]) if len(args) > 2 else 5
files = sorted(src.glob("*.ppm"))
W, H, pad, lab = 480, 800, 24, 28
rows = (len(files) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (W + pad) + pad, rows * (H + pad + lab) + pad), (200, 200, 196))
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    x = pad + (i % cols) * (W + pad); y = pad + (i // cols) * (H + pad + lab)
    d.text((x, y), f.stem, fill=(20, 20, 20))
    im = Image.open(f).convert("L")
    if not raw:
        im = im.point(lambda v: 255 if v >= 170 else 0)  # G2 (170) and G3 (255) -> white
    sheet.paste(im.convert("RGB"), (x, y + lab))
sheet.save(out); print(out, sheet.size)

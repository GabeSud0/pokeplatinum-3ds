#!/usr/bin/env python3
"""Remake art/logo.png from the full-size logo: resized to exactly its width on
the banner (224 px, so 1 texel = 1 screen pixel), lightly sharpened, and
Floyd-Steinberg dithered to the 3DS's RGBA4 levels so it neither bands nor
shimmers. Not needed to build the banner (the PNG is committed); needs numpy
and Pillow.

    python3 tools/banner3d/dev/prep_logo.py LOGO.png tools/banner3d/art/logo.png 224
"""
import sys
import numpy as np
from PIL import Image, ImageFilter
src = Image.open(sys.argv[1]).convert("RGBA"); W = int(sys.argv[3])
src = src.crop(src.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox())
H = round(src.height * W / src.width)
im = src.resize((W, H), Image.LANCZOS).filter(ImageFilter.UnsharpMask(radius=0.8, percent=60, threshold=2))
a = np.asarray(im).astype(float)
rgb = a[..., :3] * (a[..., 3:4] / 255.0); out = np.zeros_like(a)
for c in range(3):
    ch = rgb[..., c].copy()
    for y in range(H):
        for x in range(W):
            old = ch[y, x]; new = np.round(old / 17) * 17; ch[y, x] = new; e = old - new
            if x + 1 < W: ch[y, x + 1] += e * 7 / 16
            if y + 1 < H:
                if x > 0: ch[y + 1, x - 1] += e * 3 / 16
                ch[y + 1, x] += e * 5 / 16
                if x + 1 < W: ch[y + 1, x + 1] += e / 16
    out[..., c] = ch
alq = np.round(a[..., 3] / 17) * 17; safe = np.where(alq > 0, alq, 1)
out[..., :3] = np.clip(out[..., :3] * 255 / safe[..., None], 0, 255); out[..., 3] = alq
Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGBA").save(sys.argv[2]); print(W, H)

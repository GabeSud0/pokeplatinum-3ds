#!/usr/bin/env python3
"""Remake art/portal.png, the banner's portal glow (original art): a swirling
violet vortex that fades to transparent at its edge, so it melts into the HOME
Menu theme behind it. Not needed to build the banner (the PNG is committed);
needs numpy and Pillow.

    python3 tools/banner3d/dev/make_portal.py tools/banner3d/art/portal.png
"""
import sys
import numpy as np
from PIL import Image

out = sys.argv[1]


def value_noise(h, w, cells, seed):
    r = np.random.default_rng(seed)
    g = r.random((cells + 1, cells + 1))
    y, x = np.mgrid[0:h, 0:w]
    fy, fx = y / h * cells, x / w * cells
    iy, ix = fy.astype(int), fx.astype(int)
    ty, tx = fy - iy, fx - ix
    ty, tx = ty * ty * (3 - 2 * ty), tx * tx * (3 - 2 * tx)
    a, b = g[iy % cells, ix % cells], g[iy % cells, (ix + 1) % cells]
    c, d = g[(iy + 1) % cells, ix % cells], g[(iy + 1) % cells, (ix + 1) % cells]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


# ---- faded portal: the same vortex, as a soft glow whose edges fade to transparent ----
W2, H2 = 256, 128
y2, x2 = np.mgrid[0:H2, 0:W2].astype(float)
ex, ey = (x2 - W2 / 2) / (W2 / 2), (y2 - H2 / 2) / (H2 / 2)
r2 = np.hypot(ex, ey) + 1e-6
a2 = np.arctan2(ey, ex)
sw2 = a2 + 3.0 * np.exp(-r2 * 1.8)
tb2 = sum(value_noise(H2, W2, c, s + 10) / (i + 1) for i, (c, s) in enumerate([(6, 1), (12, 2), (24, 3), (48, 4)]))
tb2 /= tb2.max()
arms2 = 0.5 + 0.5 * np.sin(5 * sw2 + 6 * tb2 + 7 * r2)
glow = np.exp(-(r2 / 0.42) ** 2)                         # bright lavender heart
rim2 = np.exp(-((r2 - 0.62) / 0.18) ** 2)
c2 = np.zeros((H2, W2, 3))
c2 += np.array([0.45, 0.20, 0.80]) * (0.35 + 0.65 * arms2 * (0.6 + 0.4 * tb2))[..., None]
c2 += np.array([0.85, 0.30, 0.75]) * (rim2 * arms2 * 0.55)[..., None]
c2 += np.array([0.95, 0.88, 1.00]) * (glow * 0.85)[..., None]
alpha2 = np.clip(1.0 - (r2 - 0.62) / 0.38, 0, 1) ** 1.3     # solid inside, fades out by the oval's edge
alpha2 *= np.clip(0.75 + 0.25 * tb2, 0, 1)                 # wispy edge
rgba2 = np.dstack([np.clip(c2, 0, 1), alpha2])
big = Image.fromarray((rgba2 * 255).astype(np.uint8), "RGBA")
big.resize((128, 64), Image.LANCZOS).save(out)            # the banner's 128x64 texture
print("wrote", out)

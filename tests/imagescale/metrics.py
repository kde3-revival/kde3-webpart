#!/usr/bin/env python3
"""metrics.py ref.png img.png [img2.png ...]: PSNR (dB, RGB) and SSIM (luma, 11x11 gaussian) vs ref."""
import sys
import numpy as np
from PIL import Image

def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float64)

def gauss(n=11, s=1.5):
    x = np.arange(n) - n // 2
    g = np.exp(-x * x / (2 * s * s)); return g / g.sum()

def blur(a, g):
    from numpy.lib.stride_tricks import sliding_window_view as sw
    k = len(g)
    a = (sw(a, k, axis=1) * g).sum(-1)
    a = (sw(a, k, axis=0) * g).sum(-1)
    return a

def ssim(a, b):
    ya = a @ [0.299, 0.587, 0.114]; yb = b @ [0.299, 0.587, 0.114]
    g = gauss(); C1 = (0.01 * 255) ** 2; C2 = (0.03 * 255) ** 2
    ma, mb = blur(ya, g), blur(yb, g)
    va = blur(ya * ya, g) - ma * ma; vb = blur(yb * yb, g) - mb * mb; cab = blur(ya * yb, g) - ma * mb
    return float((((2 * ma * mb + C1) * (2 * cab + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2))).mean())

def psnr(a, b):
    m = ((a - b) ** 2).mean()
    return float('inf') if m == 0 else 10 * np.log10(255 ** 2 / m)

ref = load(sys.argv[1])
for p in sys.argv[2:]:
    im = load(p)
    if im.shape != ref.shape:
        print(f"{p}: size {im.shape} != {ref.shape}"); continue
    print(f"{psnr(ref, im):7.2f} dB  SSIM {ssim(ref, im):.4f}  {p}")

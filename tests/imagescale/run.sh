#!/bin/sh
# run.sh SHOT.png SHOTB.png [BASE.png BASEB.png]: crop the cases of
# pages/imagescale.html out of screenshots of imagescale.html and
# imagescale.html#b (the content origin is found by the markers) and print
# PSNR/SSIM against Lanczos-3 (Pillow) references, the colour bleeding of the
# sprites (max difference from their own colour, 0 = pixel exact) and, with
# BASE*.png (screenshots of the same page by another build), whether the
# 1:1, scaled up and pixelated cases are identical to it.
# Needs zone.png, hero.png, sprites.png, strip.png and border.png next to the page (or in $PAGES).
#   python3 -m http.server -d tests/pages 8765 &
#   DISPLAY=:101 kwpeview http://127.0.0.1:8765/imagescale.html &
#   import -display :101 -window root shot.png  (and again for imagescale.html#b)
#   run.sh shot.png shotb.png
here=$(dirname "$(readlink -f "$0")")
exec python3 -I - "$here" "${PAGES:-$here/../pages}" "${OUT:-/tmp/claude-1000/imagescale}" "$@" <<'PY'
import os, sys
import numpy as np
from PIL import Image
here, pages, out, shot, shotb = sys.argv[1:6]
base = sys.argv[6:8] if len(sys.argv) > 7 else None
sys.dont_write_bytecode = True
sys.path.insert(0, here)
from metrics import psnr, ssim
os.makedirs(out, exist_ok=True)
L = Image.LANCZOS
def img(name): return Image.open(f'{pages}/{name}').convert('RGB')
def origin(im, colour, x0, y0):
    a = np.asarray(im)
    ys, xs = np.nonzero((a[..., 0] == colour[0]) & (a[..., 1] == colour[1]) & (a[..., 2] == colour[2]))
    i = np.argmin(ys * 100000 + xs)
    return xs[i] - x0, ys[i] - y0
class Shots:
    def __init__(self, a, b):
        self.a, self.b = Image.open(a).convert('RGB'), Image.open(b).convert('RGB')
        self.oa, self.ob = origin(self.a, (255, 0, 255), 286, 0), origin(self.b, (0, 255, 255), 410, 700)
    def crop(self, x, y, w, h):
        im, o = (self.a, self.oa) if y + h <= 700 else (self.b, self.ob)
        return im.crop((o[0] + x, o[1] + y, o[0] + x + w, o[1] + y + h))
hero, zone = img('hero.png'), img('zone.png')
h400 = hero.resize((400, 240), L)
z200 = zone.resize((200, 200), L)
def tiled(w, h, xs, ys):
    r = Image.new('RGB', (w, h), (255, 255, 255))
    for x in xs:
        for y in ys: r.paste(z200, (x, y))
    return r
def nine():
    r = Image.new('RGB', (400, 240))
    sx, sy, dx, dy = (0, 512, 1536, 2048), (0, 512, 1536, 2048), (0, 80, 320, 400), (0, 80, 160, 240)
    for i in range(3):
        for j in range(3):
            p = zone.crop((sx[i], sy[j], sx[i + 1], sy[j + 1])).resize((dx[i + 1] - dx[i], dy[j + 1] - dy[j]), L)
            r.paste(p, (dx[i], dy[j]))
    return r
# name: (x, y, w, h, reference)
cases = {
    'img-zone-280':      (0, 0, 280, 280, zone.resize((280, 280), L)),
    'img-hero-400':      (300, 0, 400, 240, h400),
    'img-hero-640':      (0, 300, 640, 384, hero.resize((640, 384), L)),
    'img-transform-400': (720, 0, 400, 240, h400),
    'bg-explicit-400':   (720, 300, 400, 240, h400),
    'bg-cover':          (0, 700, 400, 200, h400.crop((0, 20, 400, 220))),
    'bg-contain':        (420, 730, 400, 240, h400),
    'content-url':       (840, 700, 400, 240, h400),
    'bg-transform':      (0, 1020, 400, 240, h400),
    'bg-repeat':         (0, 1280, 600, 280, tiled(600, 280, (0, 200, 400), (0, 200))),
    'bg-space':          (620, 1280, 640, 200, tiled(640, 200, (0, 220, 440), (0,))),
    'border-stretch':    (420, 1020, 400, 240, nine()),
    'border-round':      (840, 1020, 400, 240, nine()),
    'sprite-zone':       (1340, 700, 64, 64, img('sprites.png').crop((0, 0, 256, 256)).resize((64, 64), L)),
}
solid = {
    'sprite-green':   (1260, 700, 64, 64, (0, 255, 0)),
    'sprite-green2d': (1420, 700, 48, 48, (0, 255, 0)),
    'sprite-blue':    (1480, 700, 50, 50, (0, 0, 255)),
}
same = {'same-1to1': (1140, 0, 300, 200), 'same-up': (1140, 480, 300, 200), 'same-pixelated': (1140, 220, 400, 240)}
def f64(i): return np.asarray(i, dtype=np.float64)
im = Shots(shot, shotb)
for n, (x, y, w, h, ref) in cases.items():
    c = im.crop(x, y, w, h); c.save(f'{out}/{n}.png'); ref.save(f'{out}/ref-{n}.png')
    a, b = f64(ref), f64(c)
    print(f'{n:18} {psnr(a, b):6.2f} dB  SSIM {ssim(a, b):.4f}')
for n, (x, y, w, h, col) in solid.items():
    c = im.crop(x, y, w, h); c.save(f'{out}/{n}.png')
    print(f'{n:18} bleed max {int(np.abs(f64(c) - col).max())}')
a = f64(im.crop(*cases['sprite-zone'][:4]))
print(f'{"sprite-zone":18} bleed max {int(max(np.abs(a[..., 0] - a[..., 1]).max(), np.abs(a[..., 2] - a[..., 1]).max()))} (grey: R=G=B)')
if base:
    bim = Shots(*base)
    for n, r in same.items():
        d = np.abs(f64(im.crop(*r)) - f64(bim.crop(*r))).max()
        print(f'{n:18} {"identical" if d == 0 else f"max diff {int(d)}"}')
PY

#!/usr/bin/env python3
"""sprites.py DIR: sprite sheets for pages/imagescale.html, made from DIR/zone.png,
and border.png, a copy of zone.png under another URL for the border-image cases.
sprites.png (512x512): 2x2 sprites of 256x256, zone plate (scaled down from
zone.png), red / blue, green.  strip.png (1024x256): red, green, blue, yellow.
Shown scaled down, a sprite must not take colour from its neighbours."""
import sys
from PIL import Image
d = sys.argv[1]
zone = Image.open(f'{d}/zone.png').convert('RGB').resize((256, 256), Image.LANCZOS)
s = Image.new('RGB', (512, 512))
s.paste(zone, (0, 0))
s.paste((255, 0, 0), (256, 0, 512, 256))
s.paste((0, 0, 255), (0, 256, 256, 512))
s.paste((0, 255, 0), (256, 256, 512, 512))
s.save(f'{d}/sprites.png')
t = Image.new('RGB', (1024, 256))
for i, c in enumerate(((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0))):
    t.paste(c, (256 * i, 0, 256 * i + 256, 256))
t.save(f'{d}/strip.png')
Image.open(f'{d}/zone.png').save(f'{d}/border.png')

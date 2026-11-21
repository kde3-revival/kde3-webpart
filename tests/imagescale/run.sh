#!/bin/sh
# run.sh SHOT.png Y: crop the images of pages/imagescale.html out of a
# screenshot whose page content starts at row Y and print PSNR/SSIM against
# Lanczos-3 (Pillow) references.  Needs zone.png and hero.png next to the page.
#   python3 -m http.server -d tests/pages 8765 &
#   DISPLAY=:101 kwpeview http://127.0.0.1:8765/imagescale.html &
#   import -display :101 -window root shot.png; run.sh shot.png 57
here=$(dirname "$(readlink -f "$0")")
pages=$here/../pages
shot=$1; y=${2:-0}
out=${OUT:-/tmp/claude-1000/imagescale}
mkdir -p "$out"
python3 -I - "$pages" "$out" <<'PY'
import sys
from PIL import Image
pages, out = sys.argv[1:3]
for name, size in (('zone', (280, 280)), ('hero', (400, 240)), ('hero', (640, 384))):
    Image.open(f'{pages}/{name}.png').convert('RGB').resize(size, Image.LANCZOS).save(f'{out}/ref-{name}-{size[0]}.png')
PY
crop() { magick "$shot" -crop "$1" +repage "$out/$2.png"; }
crop 280x280+0+$y zone-280
crop 400x240+300+$y hero-400
crop 640x384+0+$((300 + y)) hero-640
crop 400x240+720+$((300 + y)) background-400
crop 400x240+720+$y transform-400
m=$here/metrics.py
python3 -I "$m" "$out/ref-zone-280.png" "$out/zone-280.png"
python3 -I "$m" "$out/ref-hero-400.png" "$out/hero-400.png" "$out/background-400.png" "$out/transform-400.png"
python3 -I "$m" "$out/ref-hero-640.png" "$out/hero-640.png"

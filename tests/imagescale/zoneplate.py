#!/usr/bin/env python3
"""zoneplate.py out.png [size]: a zone plate whose frequency grows from 0 at
the centre to the Nyquist limit at the edges.  Scaled down well it is flat grey
outside the centre; aliasing shows up as rings (moire)."""
import sys
import numpy as np
from PIL import Image
n = int(sys.argv[2]) if len(sys.argv) > 2 else 2048
y, x = np.mgrid[0:n, 0:n].astype(np.float64)
z = 0.5 + 0.5 * np.cos(np.pi * ((x - n / 2) ** 2 + (y - n / 2) ** 2) / n)
Image.fromarray((z * 255).astype(np.uint8)).convert('RGB').save(sys.argv[1])

#!/usr/bin/env python3
"""Pair each port capture with the closest Dolphin frame dump.

usage: refmatch.py PORT_DIR DOLPHIN_FRAMES_DIR OUT_DIR
Writes OUT_DIR/<name>-pair.png (port left, Dolphin right) and prints the
mean absolute difference (0-255) of the best match."""
import os, re, sys
import numpy as np
from PIL import Image

port_dir, dol_dir, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)

def small(path):
    return np.asarray(Image.open(path).convert('RGB').resize((160, 112)), dtype=np.int16)

dol, smalls = [], []
for f in sorted(os.listdir(dol_dir)):
    if not f.endswith('.png'):
        continue
    try:
        smalls.append(small(os.path.join(dol_dir, f)))
    except OSError:             # the last dump of a killed run is truncated
        continue
    dol.append((int(re.search(r'(\d+)', f).group(1)), os.path.join(dol_dir, f)))
dol_small = np.stack(smalls)
for f in sorted(os.listdir(port_dir)):
    if not f.endswith('.png'):
        continue
    p = small(os.path.join(port_dir, f))
    if p.mean() < 2:            # black frame: nothing to match
        continue
    d = np.abs(dol_small - p).mean(axis=(1, 2, 3))
    i = int(d.argmin())
    a = Image.open(os.path.join(port_dir, f)).convert('RGB')
    b = Image.open(dol[i][1]).convert('RGB').resize(a.size)
    full = np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16)).mean()
    pair = Image.new('RGB', (a.width * 2 + 8, a.height), (255, 0, 255))
    pair.paste(a, (0, 0)); pair.paste(b, (a.width + 8, 0))
    name = f[:-4]
    pair.save(os.path.join(out, name + '-pair.png'))
    print('%-22s dolphin frame %5d  diff %.1f' % (name, dol[i][0], full))

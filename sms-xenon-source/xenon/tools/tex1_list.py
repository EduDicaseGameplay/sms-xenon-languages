#!/usr/bin/env python3
"""List the TEX1 textures of a J3D file (.bmd/.bdl/.bmt): format, size, mips, data offset.

usage: tex1_list.py FILE"""
import struct, sys

d = open(sys.argv[1], 'rb').read()
i = d.find(b'TEX1')
if i < 0:
    sys.exit('no TEX1')
size, count = struct.unpack('>IH', d[i + 4:i + 10])
hdr_off, names_off = struct.unpack('>II', d[i + 12:i + 20])
names = []
n0 = i + names_off
cnt = struct.unpack('>H', d[n0:n0 + 2])[0]
for k in range(cnt):
    so = struct.unpack('>H', d[n0 + 4 + k * 4 + 2:n0 + 4 + k * 4 + 4])[0]
    names.append(d[n0 + so:d.index(b'\0', n0 + so)].decode('latin1'))
print('TEX1 at 0x%x, %d textures' % (i, count))
for k in range(count):
    h = i + hdr_off + k * 32
    fmt = d[h]; w, hh = struct.unpack('>HH', d[h + 2:h + 6])
    mips = d[h + 0x18]; data = struct.unpack('>I', d[h + 0x1C:h + 0x20])[0]
    start = h + data
    nz = sum(1 for b in d[start:start + 64] if b)
    print('%2d %-24s fmt=%2d %4dx%-4d mips=%d header 0x%05x data 0x%05x (first 64 bytes nonzero: %d)' %
          (k, names[k] if k < len(names) else '?', fmt, w, hh, mips, h, start, nz))

#!/usr/bin/env python3
"""Decompress a Yaz0 .szs RARC and tell which file holds a given offset.

usage: rarc_at.py FILE.szs HEXOFFSET [OUT_DIR]"""
import struct, sys

def yaz0(d):
    assert d[:4] == b'Yaz0'
    size = struct.unpack('>I', d[4:8])[0]
    out = bytearray(); i = 16
    while len(out) < size:
        code = d[i]; i += 1
        for b in range(8):
            if len(out) >= size: break
            if code & (0x80 >> b):
                out.append(d[i]); i += 1
            else:
                b1, b2 = d[i], d[i + 1]; i += 2
                dist = ((b1 & 15) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0: n = d[i] + 0x12; i += 1
                else: n += 2
                for _ in range(n): out.append(out[-dist])
    return bytes(out)

def rarc_files(a):
    hdr = struct.unpack('>4sIIIIIII', a[:32])
    data_off = hdr[3] + 0x20
    info = 0x20
    nnodes, node_off, nent, ent_off, str_size, str_off = struct.unpack('>IIIIII', a[info:info + 24])
    node_off += 0x20; ent_off += 0x20; str_off += 0x20
    files = []
    for e in range(nent):
        idx, h, typ, noff, doff, size = struct.unpack('>HHHHII', a[ent_off + e * 20: ent_off + e * 20 + 16])
        name = a[str_off + noff: a.index(b'\0', str_off + noff)].decode('latin1')
        if typ & 0x0200 or name in ('.', '..'):
            continue
        files.append((data_off + doff, size, name))
    return files

d = open(sys.argv[1], 'rb').read()
a = yaz0(d) if d[:4] == b'Yaz0' else d
off = int(sys.argv[2], 16)
for start, size, name in rarc_files(a):
    if start <= off < start + size:
        print('%s: file offset 0x%x of %d bytes' % (name, off - start, size))
        print('bytes there:', a[off:off + 16].hex(' '))
        if len(sys.argv) > 3:
            open(sys.argv[3] + '/' + name, 'wb').write(a[start:start + size])

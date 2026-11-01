#!/usr/bin/env python3
"""Super Mario Sunshine for the Xbox 360: the game's files from your own disc image.

usage: python3 sms_extract.py DISC_IMAGE OUT_FOLDER

DISC_IMAGE is a dump of your Super Mario Sunshine disc (USA, GMSE01) as .iso/.gcm or .ciso.
(Dolphin converts .rvz, .gcz and .wia dumps to .iso: right-click the game, Convert File.)
OUT_FOLDER receives files/ and sys/, the disc's file system and system area, which the port
reads at run time. Copy both folders next to default.xex, so the console's game folder holds
default.xex, files\\ and sys\\.

Plain Python 3, standard library only. Nothing is downloaded and nothing is changed in the
image."""
import hashlib
import os
import struct
import sys

GAME_ID = b'GMSE01'
# The dump the port was made and tested with (revision 0): any other one is used too, after a
# warning, as long as it is GMSE01.
TESTED_SHA1 = '8d094f2c5c112aba9660f0478b16c7f2caf63cbf'


class Disc:
    """Reads a plain image (.iso/.gcm) or a CISO (.ciso) by disc offset."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.f.read(0x8000)
        self.ciso = head[:4] == b'CISO'
        if self.ciso:  # header: magic, block size, then one byte a block: present or not
            self.block = struct.unpack_from('<I', head, 4)[0]
            self.where = []
            stored = 0
            for present in head[8:0x8000]:
                self.where.append(stored if present else None)
                stored += present != 0

    def read(self, offset, size):
        if not self.ciso:
            self.f.seek(offset)
            data = self.f.read(size)
        else:
            out = bytearray()
            while size > 0:
                block, inside = divmod(offset, self.block)
                n = min(size, self.block - inside)
                at = self.where[block] if block < len(self.where) else None
                if at is None:
                    out += bytes(n)  # a block not stored is zeros
                else:
                    self.f.seek(0x8000 + at * self.block + inside)
                    out += self.f.read(n)
                offset += n
                size -= n
            data = bytes(out)
        if len(data) != size and not self.ciso:
            raise SystemExit('sms_extract: the image ends early (offset %#x): an incomplete dump?' % offset)
        return data


def be32(b, at):
    return struct.unpack_from('>I', b, at)[0]


def dol_size(dol):
    """A DOL's length: the end of its furthest section (7 text, 11 data)."""
    end = 0x100
    for i in range(18):
        off, size = be32(dol, i * 4), be32(dol, 0x90 + i * 4)
        if size:
            end = max(end, off + size)
    return end


def extract(image, out):
    disc = Disc(image)
    boot = disc.read(0, 0x440)
    if boot[:6] == b'GMSP01':
        raise SystemExit('sms_extract: %s is the European disc (GMSP01). The game runs on the USA disc; '
                         'sms_language.py makes language packs from this one (French, German, Spanish, '
                         'Italian, or a translation such as Portuguese).' % image)
    if boot[:6] != GAME_ID:
        raise SystemExit('sms_extract: %s is %r, not Super Mario Sunshine (USA, %s)'
                         % (image, boot[:6].decode('ascii', 'replace'), GAME_ID.decode()))
    if be32(boot, 0x1C) != 0xC2339F3D:
        raise SystemExit('sms_extract: %s has no GameCube disc header' % image)
    print('Super Mario Sunshine (%s), revision %d' % (GAME_ID.decode(), boot[7]))

    sysdir = os.path.join(out, 'sys')
    os.makedirs(sysdir, exist_ok=True)

    def put(name, data):
        with open(os.path.join(sysdir, name), 'wb') as f:
            f.write(data)

    put('boot.bin', boot)
    put('bi2.bin', disc.read(0x440, 0x2000))
    app = disc.read(0x2440, 0x20)
    put('apploader.img', disc.read(0x2440, 0x20 + be32(app, 0x14) + be32(app, 0x18)))
    dol_at, fst_at, fst_size = be32(boot, 0x420), be32(boot, 0x424), be32(boot, 0x428)
    head = disc.read(dol_at, 0x100)
    put('main.dol', disc.read(dol_at, dol_size(head)))
    fst = disc.read(fst_at, fst_size)
    put('fst.bin', fst)

    # The FST: entry 0 the root (its length the entry count), 12 bytes an entry, names after them.
    count = be32(fst, 8)
    names = count * 12
    files = os.path.join(out, 'files')
    os.makedirs(files, exist_ok=True)
    dirs = [(count, files)]  # (one past the directory's last entry, its folder)
    written = total = 0
    for i in range(1, count):
        while i >= dirs[-1][0]:
            dirs.pop()
        word, a, b = struct.unpack_from('>III', fst, i * 12)
        name_at = names + (word & 0xFFFFFF)
        name = fst[name_at:fst.index(b'\0', name_at)].decode('ascii')
        if name in ('', '.', '..') or '/' in name or '\\' in name:
            raise SystemExit('sms_extract: an odd name in the disc FST: %r' % name)
        path = os.path.join(dirs[-1][1], name)
        if word >> 24:  # a directory: b is one past its last entry
            os.makedirs(path, exist_ok=True)
            dirs.append((b, path))
        else:  # a file: a is its offset, b its length
            with open(path, 'wb') as f:
                left, at = b, a
                while left:
                    n = min(left, 4 << 20)
                    f.write(disc.read(at, n))
                    at += n
                    left -= n
            written += 1
            total += b
    print('%d files, %.0f MB, in %s' % (written, total / 1e6, files))

    if not disc.ciso:
        h = hashlib.sha1()
        with open(image, 'rb') as f:
            for chunk in iter(lambda: f.read(8 << 20), b''):
                h.update(chunk)
        if h.hexdigest() != TESTED_SHA1:
            print('note: this dump is not the one the port was tested with (SHA-1 %s); it should work '
                  'if it is a clean dump of the USA disc' % h.hexdigest())


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    extract(sys.argv[1], sys.argv[2])
    print('Done. Copy files and sys next to default.xex.')


if __name__ == '__main__':
    main()

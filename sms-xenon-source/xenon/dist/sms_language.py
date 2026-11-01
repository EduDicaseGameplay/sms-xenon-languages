#!/usr/bin/env python3
"""Super Mario Sunshine for the Xbox 360: language packs from your own European disc.

usage: python3 sms_language.py USA_IMAGE OTHER_IMAGE OUT_FOLDER [--lang SLOT=CODE ...]

USA_IMAGE is the dump of your North American disc (GMSE01), the one sms_extract.py takes; the
game runs on it. OTHER_IMAGE is a disc with the game's text in other languages (GMSP01): the
European release (English, French, German, Spanish, Italian), or a translation built on it, such
as the Brazilian Portuguese one, which has Portuguese where the European disc has Spanish.
OUT_FOLDER receives mods/languages/<code>/ for each language; copy mods\\ next to default.xex. The
game's menu then offers them when it starts (SMS_LANGUAGE=<code> in sms_env.txt skips the menu).

A disc's languages are its slots: en, fr, ge, it, sp (and us, jp on some translations). By
default the European disc gives fr, de, es and it, and a disc whose Spanish slot holds
Portuguese gives pt. --lang sp=pt (or ge=de, ...) chooses them instead.

Plain Python 3, standard library only. Nothing is downloaded and neither image is changed."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # sms_extract.py, beside this one
from sms_extract import Disc, be32

US_ID, PAL_ID = b'GMSE01', b'GMSP01'

# The North American disc's language-dependent archives, whole, and each language's copy on the
# European disc: data/<name>_<slot>.szs.
WHOLE = ('endsave', 'game_6', 'guide', 'select', 'subtitle')
# Words only Portuguese has, to tell a translation in the Spanish slot from the Spanish
# (bianco0's first message: Ahá! Já está de volta...).
PORTUGUESE_HINTS = (b'J\xe1 est\xe1', b'voc\xea', b'n\xe3o ', b'Voc\xea')


def fst_index(disc):
    """The disc's files: path ('/data/x.szs', lower case) -> (offset, size)."""
    boot = disc.read(0, 0x440)
    fst_at, fst_size = be32(boot, 0x424), be32(boot, 0x428)
    fst = disc.read(fst_at, fst_size)
    count = be32(fst, 8)
    names = count * 12
    out = {}
    dirs = [(count, '')]
    for i in range(1, count):
        while i >= dirs[-1][0]:
            dirs.pop()
        word, a, b = struct.unpack_from('>III', fst, i * 12)
        at = names + (word & 0xFFFFFF)
        path = dirs[-1][1] + '/' + fst[at:fst.index(b'\0', at)].decode('ascii').lower()
        if word >> 24:
            dirs.append((b, path))
        else:
            out[path] = (a, b)
    return boot, out


class Image:
    def __init__(self, path):
        self.path = path
        self.disc = Disc(path)
        self.boot, self.files = fst_index(self.disc)
        self.id = self.boot[:6]

    def has(self, path):
        return path in self.files

    def read(self, path):
        at, size = self.files[path]
        return self.disc.read(at, size)

    def archive(self, path):
        return Rarc(yaz0_decode(self.read(path)), path)


def yaz0_decode(src):
    if src[:4] != b'Yaz0':
        return src
    size = be32(src, 4)
    out = bytearray()
    i = 16
    while len(out) < size:
        code = src[i]
        i += 1
        for _ in range(8):
            if len(out) >= size:
                break
            if code & 0x80:
                out.append(src[i])
                i += 1
            else:
                b1, b2 = src[i], src[i + 1]
                i += 2
                dist = ((b1 & 15) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = src[i] + 0x12
                    i += 1
                else:
                    n += 2
                start = len(out) - dist
                if dist >= n:
                    out += out[start:start + n]
                else:
                    for k in range(n):
                        out.append(out[start + k])
            code <<= 1
    return bytes(out)


def yaz0_store(raw):
    """A valid Yaz0 stream holding raw as literals only, as the port's own rebuilt archives
    (platform/dvd/dvd.cpp): the loaders expect Yaz0 where the disc had it."""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(raw)) + bytes(8))
    for i in range(0, len(raw), 8):
        out.append(0xFF)
        out += raw[i:i + 8]
    return bytes(out)


def name_hash(name):
    h = 0
    for c in name.encode('ascii'):
        h = (h * 3 + c) & 0xFFFF
    return h


class Rarc:
    """A RARC archive as {path: (attributes, data)} and its folders, in the disc's order."""

    def __init__(self, a, what):
        if a[:4] != b'RARC':
            raise SystemExit('sms_language: %s is not an archive' % what)
        data = 0x20 + be32(a, 0x0C)
        nodes, node_at, entries, entry_at, _, str_at = struct.unpack_from('>6I', a, 0x20)
        node_at += 0x20
        entry_at += 0x20
        str_at += 0x20

        def name(off):
            return a[str_at + off:a.index(b'\0', str_at + off)].decode('ascii')

        self.root = name(be32(a, node_at + 4))
        self.files = {}  # '/timg/x.bti' -> (attributes, bytes), in the archive's order
        self.dirs = ['']
        self.lower = {}  # lower-case path -> path

        def walk(node, path):
            count, first = struct.unpack_from('>HI', a, node_at + node * 16 + 10)
            subs = []
            for e in range(first, first + count):
                _, _, attr, _, noff, at, size = struct.unpack_from('>HHBBHII', a, entry_at + e * 20)
                n = name(noff)
                if n in ('.', '..'):
                    continue
                if attr & 0x02:
                    subs.append((at, path + '/' + n))
                else:
                    p = path + '/' + n
                    self.files[p] = (attr, a[data + at:data + at + size])
                    self.lower[p.lower()] = p
            for node2, p in subs:
                self.dirs.append(p)
                walk(node2, p)

        walk(0, '')

    def get(self, path):
        p = self.lower.get(path.lower())
        return self.files[p][1] if p else None

    def by_name(self, name):
        """Paths of the files called name, in any folder."""
        name = name.lower()
        return [p for p in self.files if p.rsplit('/', 1)[1].lower() == name]

    def put(self, path, data, attr=0x11):
        """Replaces the file at path (any case), or adds it and its folders."""
        p = self.lower.get(path.lower())
        if p:
            self.files[p] = (self.files[p][0], data)
            return
        folder = path.rsplit('/', 1)[0]
        parts = folder.split('/')[1:]
        for i in range(1, len(parts) + 1):
            d = '/' + '/'.join(parts[:i])
            if d.lower() not in (x.lower() for x in self.dirs):
                self.dirs.append(d)
        self.files[path] = (attr, data)
        self.lower[path.lower()] = path

    def build(self):
        """The archive's bytes, laid out as Nintendo's tools do: per folder its files, its
        folders, then . and ..; the file IDs are the entry indexes."""
        dirs = sorted(set(self.dirs), key=lambda d: (self.dirs.index(d)))
        index = {d.lower(): i for i, d in enumerate(dirs)}
        children = {d.lower(): [] for d in dirs}
        for d in dirs[1:]:
            children[d.rsplit('/', 1)[0].lower()].append(d)
        files_in = {d.lower(): [] for d in dirs}
        for p in self.files:
            files_in[p.rsplit('/', 1)[0].lower()].append(p)

        # Names, keyed by path (a name in two folders is stored twice, as on the disc), in the
        # folders' order: each folder's, then its files'.
        strings = bytearray(b'.\0..\0')
        str_off = {}

        def string(path):
            key = path.lower()
            if key not in str_off:
                str_off[key] = len(strings)
                strings.extend((path.rsplit('/', 1)[1] if path else self.root).encode('ascii') + b'\0')
            return str_off[key]

        for d in dirs:
            string(d)
            for p in files_in[d.lower()]:
                string(p)

        node_bytes = bytearray()
        entry_bytes = bytearray()
        blob = bytearray()
        entry_count = 0
        for d in dirs:
            name = self.root if not d else d.rsplit('/', 1)[1]
            kind = b'ROOT' if not d else (name.upper().encode('ascii') + b'    ')[:4]
            items = files_in[d.lower()]
            subs = children[d.lower()]
            count = len(items) + len(subs) + 2
            node_bytes += struct.pack('>4sIHHI', kind, string(d), name_hash(name), count, entry_count)
            for p in items:
                attr, data = self.files[p]
                n = p.rsplit('/', 1)[1]
                entry_bytes += struct.pack('>HHBBHIII', entry_count, name_hash(n), attr, 0, string(p),
                                           len(blob), len(data), 0)
                blob += data
                blob += bytes(-len(blob) % 32)
                entry_count += 1
            for s in subs:
                n = s.rsplit('/', 1)[1]
                entry_bytes += struct.pack('>HHBBHIII', 0xFFFF, name_hash(n), 0x02, 0, string(s),
                                           index[s.lower()], 16, 0)
                entry_count += 1
            parent = 0xFFFFFFFF if not d else index[d.rsplit('/', 1)[0].lower()]
            entry_bytes += struct.pack('>HHBBHIII', 0xFFFF, name_hash('.'), 0x02, 0, 0, index[d.lower()], 16, 0)
            entry_bytes += struct.pack('>HHBBHIII', 0xFFFF, name_hash('..'), 0x02, 0, 2, parent, 16, 0)
            entry_count += 2

        def pad(b):
            return bytes(b) + bytes(-len(b) % 32)

        node_bytes, entry_bytes, strings = pad(node_bytes), pad(entry_bytes), pad(strings)
        node_at = 0x20
        entry_at = node_at + len(node_bytes)
        str_at = entry_at + len(entry_bytes)
        data_at = str_at + len(strings)  # relative to 0x20, as the header counts
        info = struct.pack('>6IHBB', len(dirs), node_at, entry_count, entry_at, len(strings), str_at,
                           entry_count, 1, 0) + bytes(4)
        total = 0x20 + data_at + len(blob)
        head = struct.pack('>4s7I', b'RARC', total, 0x20, data_at, len(blob), len(blob), 0, 0)
        return head + info + node_bytes + entry_bytes + strings + bytes(blob)


def us_messages(bmg):
    """A message file (BMG) in the North American layout. The European ones with a STR1 section
    keep each message's voice (JMSMesgEntry::mVoiceIndex, the sound a character makes when it
    talks) in byte 10 of its INF1 entry, after a 16-bit STR1 index; the game reads byte 8, where
    they always hold 0, so every character would speak with that one voice. The voice goes back
    to byte 8; STR1 stays, as the game skips it. Files without STR1 (subtitles, speech balloons)
    already have the North American layout."""
    if bmg[:8] != b'MESGbmg1':
        return bmg
    out = bytearray(bmg)
    at, inf, has_str1 = 0x20, None, False
    while at + 8 <= len(out):
        tag, size = bytes(out[at:at + 4]), be32(out, at + 4)
        if tag == b'INF1':
            inf = at
        elif tag == b'STR1':
            has_str1 = True
        if size == 0:
            break
        at += size
    if inf is None or not has_str1:
        return bmg
    count, size = struct.unpack_from('>HH', out, inf + 8)
    if size != 12:
        return bmg
    for i in range(count):
        e = inf + 16 + i * size
        out[e + 8:e + 12] = bytes([out[e + 10], 0, 0, 0])
    return bytes(out)


def textures_of(blo):
    """The texture names a screen layout (BLO) refers to."""
    out = []
    i = 0
    while True:
        i = blo.find(b'.bti', i)
        if i < 0:
            return out
        j = i
        while j > 0 and (chr(blo[j - 1]).isalnum() or blo[j - 1] in b'_/'):
            j -= 1
        out.append(blo[j:i + 4].decode('ascii'))
        i += 4


def make_pack(us, other, slot, code, out):
    root = os.path.join(out, 'mods', 'languages', code, 'files')
    counts = {'whole': 0, 'inside': 0}

    def write(rel, data, inside=False):
        path = os.path.join(root, *rel.strip('/').split('/'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
        counts['inside' if inside else 'whole'] += 1

    def merged(base_path, lang):
        """The North American archive with every file of the language's copy put in it: the
        language's files win, and the North American ones it lacks stay."""
        base = us.archive(base_path)
        for p, (attr, data) in lang.files.items():
            base.put(p, us_messages(data) if p.lower().endswith('.bmg') else data, attr)
        return yaz0_store(base.build())

    # Menus, the pause guide, the shine select, the save screen, movie subtitles: whole archives.
    for name in WHOLE:
        src = '/data/%s_%s.szs' % (name, slot)
        if not other.has(src):
            print('  (no %s on this disc)' % src)
            continue
        write('/data/%s.szs' % name, merged('/data/%s.szs' % name, other.archive(src)))

    # The system messages and GAME OVER: common.szs/2d/ on the North American disc.
    src = '/data/cmn2d_%s.szs' % slot
    if other.has(src):
        common = us.archive('/data/common.szs')
        for p, (_, data) in other.archive(src).files.items():
            target = '/2d/' + p.rsplit('/', 1)[1]
            if common.get(target) is None:
                print('  note: common.szs has no %s; skipped' % target)
                continue
            write('/data/common.szs' + target, us_messages(data), inside=True)

    # Every stage's dialogue: scene/<stage>.szs/map/message.bmg.
    stages = sorted(p for p in us.files if p.startswith('/data/scene/') and p.endswith('.szs'))
    for p in stages:
        if not other.has(p):
            continue
        msg = other.archive(p).get('/map/message_%s.bmg' % slot)
        if msg is not None:
            write(p + '/map/message.bmg', us_messages(msg), inside=True)

    # The font: the language's accented letters (a translation may add its own).
    font = other.archive('/data/nintendo.szs').get('/font/standard_fontex.bfn')
    if font is not None and font != us.archive('/data/nintendo.szs').get('/font/standard_fontex.bfn'):
        write('/data/nintendo.szs/font/standard_fontex.bfn', font, inside=True)

    # The title, file select and options: option.szs, rebuilt. The North American layouts stay
    # (the game's code looks for their panes), with the language's pictures in place of the
    # English ones; the title screen and the score screen take the language's own layouts, whose
    # PRESS START has as many letters as that language needs.
    us_opt, lang_opt = us.archive('/data/option.szs'), other.archive('/data/option.szs')
    opt = us.archive('/data/option.szs')
    put = []
    msg = lang_opt.get('/loadmessage_%s.bmg' % slot)
    if msg is not None:
        put.append(('/loadmessage.bmg', us_messages(msg)))
    layouts = []
    elsewhere = set()  # pictures the North American layouts take from another archive too
    for scrn in ('title_1', 'load_score'):
        blo = lang_opt.get('/scrn/%s_%s.blo' % (scrn, slot))
        if blo is not None:
            put.append(('/scrn/%s.blo' % scrn, blo))
            layouts.append(blo)
            elsewhere.update(t.lower() for t in textures_of(us_opt.get('/scrn/%s.blo' % scrn) or b'')
                             if not us_opt.by_name(t))

    def language_picture(name):
        """The language's copy of a picture: timg/<slot>/name (the European disc),
        timg/_<slot>_name or timg/<stem>_<slot>.bti (translations)."""
        stem = name[:-4]
        for p in ('/timg/%s/%s' % (slot, name), '/timg/_%s_%s' % (slot, name), '/timg/%s_%s.bti' % (stem, slot)):
            data = lang_opt.get(p)
            if data is not None:
                return data
        return None

    # Replaced pictures the North American layouts use.
    for p in us_opt.files:
        if p.startswith('/timg/'):
            data = language_picture(p.rsplit('/', 1)[1])
            if data is not None:
                put.append((p, data))
    # Pictures the language's own layouts use: its copy, else the one of that name on its disc.
    missing = set()
    for blo in layouts:
        for tex in textures_of(blo):
            name = tex.rsplit('/', 1)[-1]
            if name.lower() in elsewhere:
                continue
            data = language_picture(name)
            if data is None:
                found = [p for p in lang_opt.by_name(name) if p.count('/') == 2]
                if found:
                    name = found[0].rsplit('/', 1)[1]  # the disc's spelling
                    data = lang_opt.get(found[0])
            if data is None:
                missing.add(name)
                continue
            put.append(('/timg/' + name, data))
    for name in sorted(missing):
        print('  note: option.szs: no picture %s for the %s layouts' % (name, code))
    for p, data in put:
        opt.put(p, data)
    write('/data/option.szs', yaz0_store(opt.build()))

    print('  %d archives rebuilt, %d files inside archives' % (counts['whole'], counts['inside']))


def default_langs(other):
    """slot -> code: the European disc's four, or pt for a translation in the Spanish slot."""
    sample = b''
    if other.has('/data/scene/bianco0.szs'):
        sample = other.archive('/data/scene/bianco0.szs').get('/map/message_sp.bmg') or b''
    if any(h in sample for h in PORTUGUESE_HINTS):
        return {'sp': 'pt'}
    return {'fr': 'fr', 'ge': 'de', 'sp': 'es', 'it': 'it'}


def main():
    args = sys.argv[1:]
    langs = {}
    while '--lang' in args:
        i = args.index('--lang')
        try:
            slot, code = args[i + 1].split('=')
        except (IndexError, ValueError):
            sys.exit('sms_language: --lang takes SLOT=CODE, for example sp=pt')
        langs[slot.lower()] = code.lower()
        del args[i:i + 2]
    if len(args) != 3:
        sys.exit(__doc__)
    us, other = Image(args[0]), Image(args[1])
    if us.id != US_ID:
        sys.exit('sms_language: %s is %r, not the North American disc (%s)'
                 % (args[0], us.id.decode('ascii', 'replace'), US_ID.decode()))
    if other.id != PAL_ID:
        sys.exit('sms_language: %s is %r, not a European disc (%s) or a translation of it'
                 % (args[1], other.id.decode('ascii', 'replace'), PAL_ID.decode()))
    for slot, code in (langs or default_langs(other)).items():
        if code == 'en':
            sys.exit('sms_language: en is the North American disc\'s own text; give the pack another code')
        if not other.has('/data/game_6_%s.szs' % slot):
            sys.exit('sms_language: %s has no language slot %s' % (args[1], slot))
        print('%s (slot %s) -> mods/languages/%s' % (args[1], slot, code))
        make_pack(us, other, slot, code, args[2])
    print('Done. Copy mods next to default.xex; the game asks for the language when it starts.')


if __name__ == '__main__':
    main()

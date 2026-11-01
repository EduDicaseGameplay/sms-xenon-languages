"""A minimal J3D (.bmd) reader for the radar map generator (gen_maps.py):
world-space triangles with their texture coordinates and vertex colours,
each material's first texture and colour source, and the textures decoded
to RGBA. Block layouts follow decomp/libs/JSystem/include/JSystem/J3D/
J3DGraphLoader/*.hpp; textures are the GX formats (I4 ... CMPR, C4/C8/C14X2
with their palettes)."""
import struct
import numpy as np

def u8(d, o): return d[o]
def u16(d, o): return struct.unpack_from('>H', d, o)[0]
def s16(d, o): return struct.unpack_from('>h', d, o)[0]
def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def f32(d, o): return struct.unpack_from('>f', d, o)[0]

# ---------------------------------------------------------------- textures
def _tiles(raw, w, h, bw, bh, per):
    """raw block-ordered values (per = values per block) -> (h, w) array"""
    nbx, nby = (w + bw - 1) // bw, (h + bh - 1) // bh
    a = raw[:nbx * nby * per].reshape(nby, nbx, bh, bw)
    return a.transpose(0, 2, 1, 3).reshape(nby * bh, nbx * bw)[:h, :w]

def _rgb565(v):
    return np.stack([(v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31, np.full_like(v, 255)], -1)

def _rgb5a3(v):
    op = (v & 0x8000) != 0
    r = np.where(op, (v >> 10 & 31) * 255 // 31, (v >> 8 & 15) * 17)
    g = np.where(op, (v >> 5 & 31) * 255 // 31, (v >> 4 & 15) * 17)
    b = np.where(op, (v & 31) * 255 // 31, (v & 15) * 17)
    a = np.where(op, 255, (v >> 12 & 7) * 255 // 7)
    return np.stack([r, g, b, a], -1)

def _ia8(v):
    i = v & 255; a = v >> 8
    return np.stack([i, i, i, a], -1)

def decode_texture(d, o):
    """BTI header at o (data/palette offsets relative to o) -> (h, w, 4) uint8, wrapS, wrapT"""
    fmt, alpha, w, h, ws, wt = d[o], d[o+1], u16(d, o+2), u16(d, o+4), d[o+6], d[o+7]
    pfmt, pnum, poff = d[o+9], u16(d, o+10), u32(d, o+12)
    off = o + u32(d, o+0x1c)
    buf = np.frombuffer(d, np.uint8, offset=off)
    def be16(n): return np.frombuffer(d, '>u2', count=n, offset=off).astype(np.int32)
    nb = lambda bw, bh: ((w + bw - 1) // bw) * ((h + bh - 1) // bh)
    if fmt == 0:   # I4
        b = buf[:nb(8, 8) * 32]; v = np.stack([b >> 4, b & 15], -1).reshape(-1) * 17
        i = _tiles(v, w, h, 8, 8, 64); out = np.stack([i, i, i, i], -1)
    elif fmt == 1:  # I8
        i = _tiles(buf[:nb(8, 4) * 32], w, h, 8, 4, 32); out = np.stack([i, i, i, i], -1)
    elif fmt == 2:  # IA4
        b = _tiles(buf[:nb(8, 4) * 32], w, h, 8, 4, 32).astype(np.int32)
        i = (b & 15) * 17; a = (b >> 4) * 17; out = np.stack([i, i, i, a], -1)
    elif fmt == 3:  # IA8
        out = _ia8(_tiles(be16(nb(4, 4) * 16), w, h, 4, 4, 16))
    elif fmt == 4:
        out = _rgb565(_tiles(be16(nb(4, 4) * 16), w, h, 4, 4, 16))
    elif fmt == 5:
        out = _rgb5a3(_tiles(be16(nb(4, 4) * 16), w, h, 4, 4, 16))
    elif fmt == 6:  # RGBA8: per 4x4 block 32 bytes AR then 32 bytes GB
        n = nb(4, 4); b = buf[:n * 64].reshape(n, 2, 16, 2)
        a, r, g, bl = b[:, 0, :, 0], b[:, 0, :, 1], b[:, 1, :, 0], b[:, 1, :, 1]
        out = np.stack([_tiles(c.reshape(-1), w, h, 4, 4, 16) for c in (r, g, bl, a)], -1)
    elif fmt in (8, 9, 10):  # C4 / C8 / C14X2
        if fmt == 8:
            b = buf[:nb(8, 8) * 32]; idx = _tiles(np.stack([b >> 4, b & 15], -1).reshape(-1), w, h, 8, 8, 64)
        elif fmt == 9: idx = _tiles(buf[:nb(8, 4) * 32], w, h, 8, 4, 32)
        else: idx = _tiles(be16(nb(4, 4) * 16) & 0x3fff, w, h, 4, 4, 16)
        pal = np.frombuffer(d, '>u2', count=pnum, offset=o + poff).astype(np.int32)
        pal = {0: _ia8, 1: _rgb565, 2: _rgb5a3}[pfmt](pal)
        out = pal[np.minimum(idx, pnum - 1)]
    elif fmt == 14:  # CMPR
        n = nb(8, 8); b = buf[:n * 32].reshape(n, 4, 8)
        c0 = (b[..., 0].astype(np.int32) << 8) | b[..., 1]; c1 = (b[..., 2].astype(np.int32) << 8) | b[..., 3]
        p0, p1 = _rgb565(c0).astype(np.int32), _rgb565(c1).astype(np.int32)
        gt = (c0 > c1)[..., None]
        p2 = np.where(gt, (2 * p0 + p1) // 3, (p0 + p1) // 2)
        p3 = np.where(gt, (p0 + 2 * p1) // 3, 0); p2[..., 3] = 255; p3[..., 3] = np.where(gt[..., 0], 255, 0)
        pal = np.stack([p0, p1, p2, p3], 2)  # (n, 4sub, 4, 4ch)
        bits = (b[..., 4].astype(np.uint32) << 24 | b[..., 5].astype(np.uint32) << 16 | b[..., 6].astype(np.uint32) << 8 | b[..., 7])
        sh = 30 - 2 * np.arange(16, dtype=np.uint32)
        ix = (bits[..., None] >> sh) & 3  # (n, 4, 16)
        px = np.take_along_axis(pal, ix[..., None].astype(np.int64).repeat(4, -1), 2)  # (n,4,16,4)
        px = px.reshape(n, 2, 2, 4, 4, 4)  # block, suby, subx, y, x, ch
        px = px.transpose(0, 1, 3, 2, 4, 5).reshape(n, 8, 8, 4)
        nbx, nby = (w + 7) // 8, (h + 7) // 8
        out = px.reshape(nby, nbx, 8, 8, 4).transpose(0, 2, 1, 3, 4).reshape(nby * 8, nbx * 8, 4)[:h, :w]
    else:
        out = np.full((h, w, 4), 200)
    return np.ascontiguousarray(out.astype(np.uint8)), ws, wt

# ---------------------------------------------------------------- model
class BMD:
    def __init__(self, src):
        """src: the file's bytes, or its path"""
        d = self.d = src if isinstance(src, (bytes, bytearray)) else open(src, 'rb').read()
        assert d[:8] == b'J3D2bmd3', d[:8]
        o = 0x20; self.sec = {}
        for _ in range(u32(d, 0xc)):
            self.sec[d[o:o+4].decode()] = o; o += u32(d, o+4)
        self._vtx(); self._jnt(); self._drw(); self._mat(); self._tex(); self._shp(); self._inf()

    def _vtx(self):
        d, s = self.d, self.sec['VTX1']
        offs = [u32(d, s + 0xc + 4*i) for i in range(13)]  # pos nrm nbt clr0 clr1 tex0..7
        end = u32(d, s + 4)
        def length(i):
            nxt = [x for x in offs if x > offs[i]]
            return (min(nxt) if nxt else end) - offs[i]
        self.arr = {}
        f = s + u32(d, s + 8)
        while u32(d, f) != 0xff:
            attr, cnt, typ, frac = u32(d, f), u32(d, f+4), u32(d, f+8), d[f+12]
            slot = {9: 0, 10: 1, 11: 3, 12: 4}.get(attr, 5 + attr - 13 if 13 <= attr <= 20 else None)
            if slot is not None and offs[slot]:
                base, ln = s + offs[slot], length(slot)
                if attr in (11, 12):
                    if typ == 5: a = np.frombuffer(d, np.uint8, count=ln // 4 * 4, offset=base).reshape(-1, 4)
                    elif typ == 1: a = np.frombuffer(d, np.uint8, count=ln // 3 * 3, offset=base).reshape(-1, 3); a = np.concatenate([a, np.full((len(a), 1), 255, np.uint8)], 1)
                    elif typ == 2: a = np.frombuffer(d, np.uint8, count=ln // 4 * 4, offset=base).reshape(-1, 4).copy(); a[:, 3] = 255
                    elif typ == 3:
                        v = np.frombuffer(d, '>u2', count=ln // 2, offset=base).astype(np.int32)
                        a = np.stack([(v >> 12 & 15) * 17, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17], -1).astype(np.uint8)
                    else:
                        v = np.frombuffer(d, '>u2', count=ln // 2, offset=base).astype(np.int32); a = _rgb565(v).astype(np.uint8)
                    self.arr[attr] = a.astype(np.float32) / 255
                else:
                    nc = {9: 3 if cnt else 2, 10: 3}.get(attr, 2 if cnt else 1)
                    dt, sz = {0: ('u1', 1), 1: ('i1', 1), 2: ('>u2', 2), 3: ('>i2', 2), 4: ('>f4', 4)}[typ]
                    a = np.frombuffer(d, dt, count=ln // sz // nc * nc, offset=base).astype(np.float32).reshape(-1, nc)
                    if typ != 4: a /= (1 << frac)
                    self.arr[attr] = a
            f += 16

    def _jnt(self):
        d, s = self.d, self.sec['JNT1']; n = u16(d, s+8); e = s + u32(d, s+0xc); rm = s + u32(d, s+0x10)
        self.jlocal = []
        for i in range(n):
            j = e + u16(d, rm + 2*i) * 0x40
            sx, sy, sz = (f32(d, j+4+4*k) for k in range(3))
            rx, ry, rz = (s16(d, j+0x10+2*k) * np.pi / 32768 for k in range(3))
            tx, ty, tz = (f32(d, j+0x18+4*k) for k in range(3))
            cx, sx_ = np.cos(rx), np.sin(rx); cy, sy_ = np.cos(ry), np.sin(ry); cz, sz_ = np.cos(rz), np.sin(rz)
            Rx = np.array([[1, 0, 0], [0, cx, -sx_], [0, sx_, cx]]); Ry = np.array([[cy, 0, sy_], [0, 1, 0], [-sy_, 0, cy]]); Rz = np.array([[cz, -sz_, 0], [sz_, cz, 0], [0, 0, 1]])
            m = np.eye(4); m[:3, :3] = Rz @ Ry @ Rx @ np.diag([sx, sy, sz]); m[:3, 3] = [tx, ty, tz]
            self.jlocal.append(m)

    def _drw(self):
        d, s = self.d, self.sec['DRW1']; n = u16(d, s+8)
        self.drw_w = [d[s + u32(d, s+0xc) + i] for i in range(n)]
        self.drw_i = [u16(d, s + u32(d, s+0x10) + 2*i) for i in range(n)]

    def _mat(self):
        d, s = self.d, self.sec['MAT3']; n = u16(d, s+8); P = lambda k: s + u32(d, s + 0xc + 4*k)
        init, ids = P(0), P(1)
        self.mats = []
        for i in range(n):
            m = init + u16(d, ids + 2*i) * 0x14c
            col_i = u16(d, m + 8); chan_i = u16(d, m + 0xc)
            matcol = np.frombuffer(d, np.uint8, count=4, offset=P(5) + col_i*4).astype(np.float32) / 255 if col_i != 0xffff else np.ones(4, np.float32)
            src = d[P(7) + chan_i*8 + 1] if chan_i != 0xffff else 0
            tn = u16(d, m + 0x84); tex = u16(d, P(15) + 2*tn) if tn != 0xffff else None
            tc = u16(d, m + 0x28); tsrc = d[P(11) + tc*4 + 1] - 4 if tc != 0xffff else 0
            ac = u16(d, m + 0x146); bl = u16(d, m + 0x148)
            alpha_ref = d[P(24) + ac*8 + 1] if ac != 0xffff else 0
            blend = d[P(25) + bl*4] if bl != 0xffff else 0
            cull = u32(d, P(4) + 4 * d[m+1]) if d[m+1] != 0xff else 2
            self.mats.append(dict(color=matcol, vtxcolor=src == 1, tex=tex, texsrc=max(0, tsrc), alpha_ref=alpha_ref, blend=blend, mode=d[m], cull=cull))

    def _tex(self):
        d, s = self.d, self.sec['TEX1']; n = u16(d, s+8); h = s + u32(d, s+0xc)
        self.tex = [None] * n; self._texoff = [h + 0x20*i for i in range(n)]

    def texture(self, i):
        if self.tex[i] is None: self.tex[i] = decode_texture(self.d, self._texoff[i])
        return self.tex[i]

    def _shp(self):
        d, s = self.d, self.sec['SHP1']; self.nshape = u16(d, s+8)
        P = lambda k: s + u32(d, s + 0xc + 4*k)
        self.shp = (P(0), P(1), P(3), P(4), P(5), P(6), P(7))

    def _inf(self):
        """walk the scene graph: joint world matrices + (material, shape) pairs"""
        d, s = self.d, self.sec['INF1']; o = s + u32(d, s+0x14)
        stack = [np.eye(4)]; cur = np.eye(4); mat = 0
        self.jworld = [np.eye(4)] * len(self.jlocal); self.draws = []
        while True:
            t, i = u16(d, o), u16(d, o+2); o += 4
            if t == 0: break
            if t == 1: stack.append(cur)
            elif t == 2: cur = stack.pop()
            elif t == 0x10: cur = stack[-1] @ self.jlocal[i]; self.jworld[i] = cur
            elif t == 0x11: mat = i
            elif t == 0x12: self.draws.append((mat, i))

    def triangles(self, shape, texsrc=0):
        """-> pos (n,3,3) world, uv (n,3,2) or None, col (n,3,4) or None (cached)"""
        cache = self.__dict__.setdefault('_tri_cache', {})
        if (shape, texsrc) not in cache:
            cache[(shape, texsrc)] = self._triangles(shape, texsrc)
        return cache[(shape, texsrc)]

    def _triangles(self, shape, texsrc=0):
        d = self.d; init, remap, desc, mtxt, dl, mtxd, pkt = self.shp
        e = init + u16(d, remap + 2*shape) * 0x28
        ngroup, dsc, mfirst, pfirst = u16(d, e+2), u16(d, e+4), u16(d, e+6), u16(d, e+8)
        attrs = []; q = desc + dsc
        while u32(d, q) != 0xff: attrs.append((u32(d, q), u32(d, q+4))); q += 8
        tri_p, tri_uv, tri_c = [], [], []
        pos = self.arr.get(9); col = self.arr.get(11)
        uvattr = 13 + texsrc
        mats_prev = [np.eye(4)] * 10
        for g in range(ngroup):
            mi = mtxd + (mfirst + g) * 8; use, cnt, first = u16(d, mi), u16(d, mi+2), u32(d, mi+4)
            mats = list(mats_prev)
            for k in range(cnt):
                di = u16(d, mtxt + 2 * (first + k))
                if di != 0xffff and di < len(self.drw_i):
                    mats[k] = self.jworld[self.drw_i[di]] if not self.drw_w[di] else np.eye(4)
            mats_prev = mats
            pi = pkt + (pfirst + g) * 8; size, off = u32(d, pi), u32(d, pi+4)
            p, end = dl + off, dl + off + size
            while p < end:
                op = d[p]; p += 1
                if op == 0: continue
                if op & 0xf8 not in (0x80, 0x90, 0x98, 0xa0): break
                n = u16(d, p); p += 2
                verts = []
                for _ in range(n):
                    v = {}
                    for a, t in attrs:
                        if t == 1: v[a] = d[p]; p += 1
                        elif t == 2: v[a] = d[p]; p += 1
                        elif t == 3: v[a] = u16(d, p); p += 2
                    verts.append(v)
                prim = op & 0xf8
                if prim == 0x90: idx = [(i, i+1, i+2) for i in range(0, n - 2, 3)]
                elif prim == 0x98: idx = [(i, i+1, i+2) if i % 2 == 0 else (i+1, i, i+2) for i in range(n - 2)]
                elif prim == 0xa0: idx = [(0, i, i+1) for i in range(1, n - 1)]
                else: idx = [t for i in range(0, n - 3, 4) for t in ((i, i+1, i+2), (i, i+2, i+3))]
                for tri in idx:
                    pp = []
                    for vi in tri:
                        v = verts[vi]; m = mats[v.get(0, 0) // 3] if 0 in v else mats[0]
                        x = pos[v[9]]; pp.append(m[:3, :3] @ x + m[:3, 3])
                    tri_p.append(pp)
                    if uvattr in self.arr and uvattr in verts[0]: tri_uv.append([self.arr[uvattr][verts[vi][uvattr]] for vi in tri])
                    if col is not None and 11 in verts[0]: tri_c.append([col[verts[vi][11]] for vi in tri])
        P = np.array(tri_p, np.float32).reshape(-1, 3, 3)
        UV = np.array(tri_uv, np.float32).reshape(-1, 3, 2) if len(tri_uv) == len(tri_p) and tri_p else None
        C = np.array(tri_c, np.float32).reshape(-1, 3, 4) if len(tri_c) == len(tri_p) and tri_p else None
        return P, UV, C

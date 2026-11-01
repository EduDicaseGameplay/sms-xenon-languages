#!/usr/bin/env python3
"""Make top-down maps of every stage from a GMSE01 disc image (or a mod's, below).

usage: tools/radar/gen_maps.py --disc GAME.iso [--out assets/maps] [--only dolpic0,ricco0]
                               [--style full|radar] [--quality 95] [--except assets/maps/maps.txt]

For every scene archive in /data/scene, the stage's world model (map/map/
map.bmd, the sea or bath water, map/map/sea.bmd or water.bmd, over it, and
the scenery its scene.bin
places from mapobj/: trees, boats, a secret course's blocks; not the coins,
Shines and items, nor effects) is drawn straight down with
its own textures (filtered bilinearly) and baked vertex colours, framed to
the stage's walkable collision (map/map.col) with a margin, at up to 0.1
pixel per world unit (10 cm a pixel; 4096 pixels on the longest side), drawn
2x finer and scaled down. A soft light on the height field makes roofs,
steps and cliffs read. Looking down, each pixel shows the highest surface
below a ceiling a little above the highest floor Mario can stand on there,
so awnings, arches and leaves over a path drop away while roofs and bridges
he can walk on stay; baked shadow decals are drawn at half strength. Where
nothing is drawn the map is a dark slate (BACKDROP).

--style radar also greys the scenery away from the stage (distant hills, the
volcano) and draws buildings Mario cannot stand on as a floor plan's blocks.

Stages whose collision is the same share one map. Writes OUT/<scene>.jpg per
map (named for the first scene with it; JPEG, full-resolution colour) and
OUT/maps.txt, one line per map:
    <col key> <file> <x0> <z0> <pixels per unit> <width> <height> <scenes>
where (x0, z0) is the world x and z of the image's top left corner, x to the
right and z down, so world (x, z) is pixel ((x - x0) * ppu, (z - z0) * ppu).
The key (col_key) is a hash of the collision's values, which the game can
compute from the loaded map.col to find the map of the stage it is in. Needs
numpy, scipy and Pillow.

A mod's disc (Super Mario Eclipse's, GMSE04) makes a pack for the game's
mods/maps/<name> folder: --except assets/maps/maps.txt leaves out the stages
the retail maps already have, by key, so the pack holds only the mod's own
and those it changed. tools/mods/get.py installs Eclipse's.
"""
import argparse
import os
import struct
import sys
import time

import numpy as np
from PIL import Image
from scipy import ndimage

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import bmd  # noqa: E402
from bundle_disc import Disc, be32  # noqa: E402

DENSITY = 0.1       # pixels per world unit (1 unit = 1 cm): a pixel is 10 cm
OVERSAMPLE = 2      # drawn this much finer, then scaled down
MAX_SIDE = 4096     # the stored map's longest side at most
TILE = 4096         # drawn in tiles this big (oversampled pixels), to bound memory
OVERLAP = 48        # each tile's margin, so shading and filters meet without seams
CEILING = 350       # world units above the highest floor that still show
BIG_TRIANGLE = 12000  # a floor triangle wider than this does not frame the map
NEAR = 1500         # (--style radar) world units from a floor that are part of the stage
BACKDROP = (0.20, 0.24, 0.30)  # where nothing was drawn


# --- disc and archives -------------------------------------------------------

def be16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def scene_files(disc):
    """{name: (offset, size)} of the files in /data/scene"""
    boot = disc.read(0, 0x440)
    fst = disc.read(be32(boot, 0x424), be32(boot, 0x428))
    n = be32(fst, 8)
    strs = fst[n * 12:]

    def name(o):
        return strs[o:strs.index(b'\0', o)].decode('latin1')
    out, stack = {}, [(n, '')]
    for i in range(1, n):
        while stack and i >= stack[-1][0]:
            stack.pop()
        f = be32(fst, i * 12)
        nm = name(f & 0xffffff)
        prefix = stack[-1][1] if stack else ''
        if f >> 24:
            stack.append((be32(fst, i * 12 + 8), prefix + '/' + nm))
        elif prefix == '/data/scene':
            out[nm] = (be32(fst, i * 12 + 4), be32(fst, i * 12 + 8))
    return out


def yaz0(src):
    if src[:4] != b'Yaz0':
        return src
    size = be32(src, 4)
    out = bytearray(size)
    s, d = 16, 0
    while d < size:
        code = src[s]
        s += 1
        for bit in range(8):
            if d >= size:
                break
            if code & (0x80 >> bit):
                out[d] = src[s]
                d += 1
                s += 1
            else:
                b1, b2 = src[s], src[s + 1]
                s += 2
                dist = ((b1 & 0xf) << 8 | b2) + 1
                cnt = b1 >> 4
                if cnt == 0:
                    cnt = src[s] + 0x12
                    s += 1
                else:
                    cnt += 2
                if dist >= cnt:
                    out[d:d + cnt] = out[d - dist:d - dist + cnt]
                    d += cnt
                else:
                    for _ in range(cnt):
                        out[d] = out[d - dist]
                        d += 1
    return bytes(out)


def rarc_files(arc):
    """{lowercase path: bytes} of a RARC archive"""
    info = 0x20
    nodes = be32(arc, info + 4) + info
    ents = be32(arc, info + 0xc) + info
    st = be32(arc, info + 0x14) + info
    dat = be32(arc, 0xc) + 0x20

    def s(o):
        return arc[st + o:arc.index(b'\0', st + o)].decode('latin1')
    out = {}

    def walk(ni, pre):
        nb = nodes + ni * 16
        cnt, first = be16(arc, nb + 10), be32(arc, nb + 12)
        for e in range(first, first + cnt):
            eb = ents + e * 20
            t, no = arc[eb + 4], be16(arc, eb + 6)
            nm, a, ln = s(no), be32(arc, eb + 8), be32(arc, eb + 12)
            if nm in ('.', '..'):
                continue
            if t & 2:
                walk(a, pre + nm + '/')
            else:
                out[(pre + nm).lower()] = arc[dat + a:dat + a + ln]
    walk(0, '')
    return out


def col_key(col):
    """The map's key: FNV-1a (64-bit) over the collision's values, each in
    little-endian order: the vertex count and vertices (32-bit), the group
    count, then each group's surface type and triangle count and its
    triangles' vertex indices (16-bit). The port swaps a loaded .col to the
    host's order in place (platform/endian), so the game can hash the same
    values from memory."""
    nv, vo, ng, go = struct.unpack_from('>IIII', col, 0)
    parts = [struct.pack('<I', nv), np.frombuffer(col, '>u4', count=nv * 3, offset=vo).astype('<u4').tobytes(),
             struct.pack('<I', ng)]
    for g in range(ng):
        e = go + g * 0x18
        bg, tris = struct.unpack_from('>Hh', col, e)
        parts.append(struct.pack('<Hh', bg, tris))
        if tris > 0:
            parts.append(np.frombuffer(col, '>u2', count=tris * 3, offset=be32(col, e + 8)).astype('<u2').tobytes())
    h = 0xcbf29ce484222325
    for b in b''.join(parts):
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


# --- collision -----------------------------------------------------------------

SKIP_TYPES = {0x400, 0x401, 0x402, 0x600, 0x800, 0x801, 0x200, 0x201, 0x202, 0x203}


def floors(col, flip=False, M=None):
    """the triangles Mario can stand or swim on, (n, 3, 3); flip: either way up
    (a few small rooms, delfinoBoss's, have their floors wound the other way)"""
    nv, vo, ng, go = struct.unpack_from('>IIII', col, 0)
    V = np.array(struct.unpack_from('>%df' % (nv * 3), col, vo), np.float32).reshape(-1, 3)
    if M is not None:
        V = V @ M[:3, :3].T + M[:3, 3]
    out = []
    for g in range(ng):
        surf, cnt = struct.unpack_from('>HH', col, go + g * 0x18)
        io = be32(col, go + g * 0x18 + 8)
        if (surf & 0xfff) in SKIP_TYPES:
            continue
        idx = np.array(struct.unpack_from('>%dH' % (cnt * 3), col, io)).reshape(-1, 3)
        t = V[idx]
        n = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
        ln = np.linalg.norm(n, axis=1) + 1e-9
        out.append(t[np.abs(n[:, 1]) / ln > 0.5] if flip else t[n[:, 1] / ln > 0.5])
    return np.concatenate(out) if out else np.zeros((0, 3, 3), np.float32)


# --- placed objects ----------------------------------------------------------

# not drawn: what Mario collects or what moves about (the runtime draws those),
# and effects
SKIP_OBJECTS = ('coin', 'shine', 'mushroom', 'nozzle', 'normal_nozzle', 'back_nozzle', 'rocket_nozzle',
                'turbo_nozzle', 'hover_nozzle', 'shimmer', 'bottle', 'juiceblock', 'mariocap')
# objects whose model goes by another name
MODEL_ALIASES = {'normallift': 'normalblock', 'mapobjnail': 'kugi', 'mushroom1upx': 'mushroom1up',
                 'railblockb': 'allpurposeboardb', 'railblockr': 'allpurposeboardr', 'railblocky': 'allpurposeboardy'}


def scene_objects(d):
    """(type, key, position, rotation (degrees), scale) of the placed objects
    in a scene.bin: its TNameRef tree, each node [u32 size][u16 hash][u16 n]
    [type][u16 hash][u16 n][name] and its data; a placed object's data is
    position, rotation and scale (f32 x3 each), its manager's name, a word,
    and its key (the model's name for most)."""
    out = []

    def strings(p, end, count):
        got = []
        while p + 2 <= end and len(got) < count:
            n = be16(d, p)
            if n == 0 or n > 64 or p + 2 + n > end or b'\0' in d[p + 2:p + 2 + n]:
                break
            got.append(d[p + 2:p + 2 + n].decode('shift_jis', 'replace'))
            p += 2 + n
        return got, p

    def walk(o, end, acc):
        while o + 12 <= end:
            size = be32(d, o)
            if size < 12 or o + size > end:
                return False
            tl = be16(d, o + 6)
            t = d[o + 8:o + 8 + tl]
            if o + 12 + tl > end or not all(32 <= c < 127 for c in t):
                return False
            nl = be16(d, o + 10 + tl)
            p, e = o + 12 + tl + nl, o + size
            acc.append((t.decode('ascii'), p, e))
            for skip in (0, 4, 8):  # a group: [a word or two], u32 count, the children
                q = p + skip
                if q + 4 > e:
                    break
                n = be32(d, q)
                if 0 < n < 10000:
                    sub = []
                    if walk(q + 4, e, sub) and len(sub) >= n:
                        acc.extend(sub)
                        break
            o += size
        return o == end

    nodes = []
    walk(0, len(d), nodes)
    for t, p, e in nodes:
        if e - p < 44:
            continue
        mgr, q = strings(p + 36, e, 1)
        if not mgr or not mgr[0].endswith('キャラ'):
            continue
        key, _ = strings(q + 4, e, 1)
        if not key:
            continue
        v = struct.unpack_from('>9f', d, p)
        out.append((t, key[0], v[0:3], v[3:6], v[6:9]))
    return out


def trs(pos, rot, scale):
    """MsMtxSetTRS: translate, then Rz Ry Rx (degrees), then scale"""
    rx, ry, rz = np.radians(rot)
    cx, sx, cy, sy, cz, sz = np.cos(rx), np.sin(rx), np.cos(ry), np.sin(ry), np.cos(rz), np.sin(rz)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    M = np.eye(4, dtype=np.float32)
    M[:3, :3] = Rz @ Ry @ Rx @ np.diag(scale)
    M[:3, 3] = pos
    return M


def placed(files):
    """[(model, collision bytes or None, matrix)] of the stage's placed
    scenery: the objects whose model is in the archive's mapobj folder"""
    if 'map/scene.bin' not in files:
        return []
    models, out = {}, []
    for t, key, pos, rot, scale in scene_objects(files['map/scene.bin']):
        k = key.lower()
        k = MODEL_ALIASES.get(k, k)
        if k.startswith(SKIP_OBJECTS) or 'mapobj/%s.bmd' % k not in files:
            continue
        if not all(np.isfinite(pos + rot + scale)) or max(abs(x) for x in pos) > 1e6:
            continue
        if k not in models:
            try:
                models[k] = bmd.BMD(files['mapobj/%s.bmd' % k])
            except Exception as e:  # a model this reader does not take: left out
                print('  %s: %s' % (k, e), flush=True)
                models[k] = None
        if models[k] is None:
            continue
        out.append((models[k], files.get('mapobj/%s.col' % k), trs(pos, rot, scale)))
    return out


# --- drawing ---------------------------------------------------------------------

class Canvas:
    def __init__(self, lo, s, W, H):
        self.lo, self.s, self.W, self.H = lo, s, W, H

    def triangles(self, P):
        """pixel-space (x, y) of world triangles, and those that touch the canvas"""
        q = (P[:, :, [0, 2]] - self.lo) * self.s
        keep = ((q[:, :, 0].max(1) >= 0) & (q[:, :, 0].min(1) < self.W)
                & (q[:, :, 1].max(1) >= 0) & (q[:, :, 1].min(1) < self.H))
        return q, np.nonzero(keep)[0]

    def cover(self, qk):
        """(y0, x0, barycentric l1 l2 l3, inside mask) of one triangle, or None"""
        (ax, ay), (bx, by), (cx, cy) = qk
        den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(den) < 1e-9:
            return None
        x0, x1 = max(0, int(np.floor(min(ax, bx, cx)))), min(self.W - 1, int(np.ceil(max(ax, bx, cx))))
        y0, y1 = max(0, int(np.floor(min(ay, by, cy)))), min(self.H - 1, int(np.ceil(max(ay, by, cy))))
        if x1 < x0 or y1 < y0:
            return None
        gx = np.arange(x0, x1 + 1, dtype=np.float32)[None, :] + .5
        gy = np.arange(y0, y1 + 1, dtype=np.float32)[:, None] + .5
        l1 = ((by - cy) * (gx - cx) + (cx - bx) * (gy - cy)) / den
        l2 = ((cy - ay) * (gx - cx) + (ax - cx) * (gy - cy)) / den
        l3 = 1 - l1 - l2
        m = (l1 >= -1e-3) & (l2 >= -1e-3) & (l3 >= -1e-3)
        if not m.any():  # smaller than a pixel: its centre
            px, py = int((ax + bx + cx) / 3), int((ay + by + cy) / 3)
            if not (x0 <= px <= x1 and y0 <= py <= y1):
                return None
            m = np.zeros(l1.shape, bool)
            m[py - y0, px - x0] = True
            l1 = l2 = l3 = np.full(l1.shape, 1 / 3, np.float32)
        return y0, x0, l1, l2, l3, m


def floor_heights(cv, F):
    """the highest floor at each pixel (-inf where there is none)"""
    top = np.full((cv.H, cv.W), -np.inf, np.float32)
    q, keep = cv.triangles(F)
    for k in keep:
        c = cv.cover(q[k])
        if c is None:
            continue
        y0, x0, l1, l2, l3, m = c
        h = l1 * F[k, 0, 1] + l2 * F[k, 1, 1] + l3 * F[k, 2, 1]
        v = top[y0:y0 + m.shape[0], x0:x0 + m.shape[1]]
        np.maximum(v, np.where(m, h, -np.inf), out=v)
    return top


def draw_model(cv, model, rgb, zb, cap, capped, decal_alpha=0.5, alpha_mul=1.0, translucent=False, cull=True, M=None):
    later = []
    for mi, sh in model.draws:
        mat = model.mats[mi]
        P, UV, C = model.triangles(sh, mat['texsrc'])
        if not len(P):
            continue
        if M is not None:
            P = P @ M[:3, :3].T + M[:3, 3]
        if cull and mat['cull'] == 2:  # one-sided: what faces down (ceilings) is never seen from above
            keep = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])[:, 1] <= 0
            P = P[keep]
            UV = UV[keep] if UV is not None else None
            C = C[keep] if C is not None else None
            if not len(P):
                continue
        tex = model.texture(mat['tex']) if mat['tex'] is not None else None
        if not translucent and mat['blend'] == 1 and mat['mode'] != 1:
            later.append((P, UV, C, mat, tex))
            continue
        raster(cv, P, UV, C, mat, tex, rgb, zb, cap, capped, translucent, alpha_mul)
    for P, UV, C, mat, tex in later:
        raster(cv, P, UV, C, mat, tex, rgb, zb, cap, capped, True, decal_alpha)


def raster(cv, P, UV, C, mat, tex, rgb, zb, cap, capped, translucent, alpha_mul):
    q, keep = cv.triangles(P)
    for k in keep:
        c = cv.cover(q[k])
        if c is None:
            continue
        y0, x0, l1, l2, l3, m = c
        sl = (slice(y0, y0 + m.shape[0]), slice(x0, x0 + m.shape[1]))
        hgt = l1 * P[k, 0, 1] + l2 * P[k, 1, 1] + l3 * P[k, 2, 1]
        over = m & (hgt > cap[sl])
        capped[sl] |= over
        m = m & ~over & (hgt > zb[sl])
        if not m.any():
            continue
        if tex is not None and UV is not None:
            img, ws, wt = tex
            th, tw = img.shape[:2]
            # bilinear, as GX filters the stage's textures
            u = (l1 * UV[k, 0, 0] + l2 * UV[k, 1, 0] + l3 * UV[k, 2, 0])[m] * tw - 0.5
            v = (l1 * UV[k, 0, 1] + l2 * UV[k, 1, 1] + l3 * UV[k, 2, 1])[m] * th - 0.5
            u0, v0 = np.floor(u), np.floor(v)
            fu, fv = (u - u0)[:, None].astype(np.float32), (v - v0)[:, None].astype(np.float32)
            u0, v0 = u0.astype(np.int64), v0.astype(np.int64)
            ua, ub = wrap(u0, tw, ws), wrap(u0 + 1, tw, ws)
            va, vb = wrap(v0, th, wt), wrap(v0 + 1, th, wt)
            top_ = img[va, ua].astype(np.float32) * (1 - fu) + img[va, ub].astype(np.float32) * fu
            bot_ = img[vb, ua].astype(np.float32) * (1 - fu) + img[vb, ub].astype(np.float32) * fu
            col = (top_ * (1 - fv) + bot_ * fv) / 255
        else:
            col = np.ones((int(m.sum()), 4), np.float32)
        if mat['vtxcolor'] and C is not None:
            col = col * (l1[..., None] * C[k, 0] + l2[..., None] * C[k, 1] + l3[..., None] * C[k, 2])[m]
        else:
            col = col * mat['color']
        a = col[:, 3] * alpha_mul
        rv, zv = rgb[sl], zb[sl]
        if translucent:
            rv[m] = rv[m] * (1 - a[:, None]) + col[:, :3] * a[:, None]
        else:
            ok = a >= 0.5 if (mat['alpha_ref'] or mat['blend']) else np.ones(a.shape, bool)
            mm = m.copy()
            mm[m] = ok
            rv[mm] = col[ok, :3]
            zv[mm] = hgt[mm]


def wrap(i, n, mode):
    if mode == 0:  # clamp
        return np.clip(i, 0, n - 1)
    if mode == 2:  # mirror
        i = np.abs(i) % (2 * n)
        return np.where(i >= n, 2 * n - 1 - i, i)
    return i % n


def without_huge(F):
    """F less its single huge triangles (a sea bed, the plane far under a
    secret course or Bowser's bath)"""
    xz = F[:, :, [0, 2]]
    edge = np.max([np.linalg.norm(xz[:, (i + 1) % 3] - xz[:, i], axis=1) for i in range(3)], axis=0)
    return F[edge < BIG_TRIANGLE]


def frame(F):
    """the map's world rectangle and density: the floors' extent, padded,
    leaving out single huge triangles (a sea bed, the plane far under a
    secret course), which would stretch it over empty water or sky"""
    xz = F[:, :, [0, 2]]
    lo = np.percentile(xz.reshape(-1, 2), 0.1, axis=0)
    hi = np.percentile(xz.reshape(-1, 2), 99.9, axis=0)
    pad = 0.08 * (hi - lo).max() + 1500
    lo, hi = lo - pad, hi + pad
    dens = min(DENSITY, MAX_SIDE / (hi - lo).max())
    W_out, H_out = (np.ceil((hi - lo) * dens).astype(int) + 3) // 4 * 4
    return lo, dens, int(W_out), int(H_out)


def render_tile(files, F, flipped, objects, lo, s, W, H, style):
    """one tile, W x H oversampled pixels from world (lo); RGB float32"""
    cv = Canvas(lo.astype(np.float32), np.float32(s), W, H)
    top = floor_heights(cv, F)
    R = np.isfinite(top)
    # close the hairline gaps between floor triangles, then carry each floor's
    # height a few pixels out so a wall's top edge is still under its ceiling
    R = ndimage.binary_closing(R, iterations=2)
    filled = ndimage.grey_dilation(np.where(np.isfinite(top), top, -1e9), size=(5, 5)).astype(np.float32)
    del top
    cap = np.where(R, filled + CEILING, np.inf).astype(np.float32)

    rgb = np.zeros((H, W, 3), np.float32)
    zb = np.full((H, W), -np.inf, np.float32)
    capped = np.zeros((H, W), bool)
    draw_model(cv, bmd.BMD(files['map/map/map.bmd']), rgb, zb, cap, capped, cull=not flipped)
    for model, _, M in objects:
        draw_model(cv, model, rgb, zb, cap, capped, M=M)
    del cap, capped
    height = zb.copy()
    for water in ('map/map/sea.bmd', 'map/map/water.bmd'):  # the sea; Bowser's bath
        if water in files:
            draw_model(cv, bmd.BMD(files[water]), rgb, zb, np.full((H, W), np.inf, np.float32),
                       np.zeros((H, W), bool), alpha_mul=0.75, translucent=True)
    seen = np.isfinite(zb)
    del zb

    # relief: a soft light from the top left on the height field, so roofs,
    # steps and cliffs read from above
    hz = np.where(np.isfinite(height), height, np.nan)
    base = float(np.nanmin(hz)) if np.isfinite(hz).any() else 0.0
    hz = ndimage.gaussian_filter(np.nan_to_num(hz, nan=base), OVERSAMPLE * 0.75)
    gy, gx = np.gradient(hz * s)
    shade = np.clip(1 + (-gx * 0.5 - gy * 0.7) * 0.25, 0.6, 1.25)
    del gx, gy, hz
    img = np.clip(rgb * shade[..., None], 0, 1)
    del rgb, shade
    img[~seen] = BACKDROP
    if style != 'radar':
        return img

    # --style radar: the stage, structures by it, and the scenery around it
    dist, (iy, ix) = ndimage.distance_transform_edt(~R, return_indices=True)
    dist_u = (dist / s).astype(np.float32)
    near_floor = np.where(R, filled, filled[iy, ix])
    del dist, iy, ix
    above = ~R & (dist_u < NEAR) & np.isfinite(height) & (height > near_floor + CEILING)
    # a building's top is flat, a hillside slopes: blocks are the flat parts,
    # with the steep walls around them closed in
    sy, sx = np.gradient(np.where(np.isfinite(height), height, base).astype(np.float32) * s)
    flat = np.hypot(sx, sy) < 0.35
    block = ndimage.binary_opening(above & flat, iterations=2)
    block = ndimage.binary_closing(block, iterations=3) & above
    lum = (img @ np.array([0.3, 0.59, 0.11], np.float32))[..., None]
    vivid = lum + (img - lum) * 0.85
    back = lum * 0.55 + np.array(BACKDROP, np.float32) * 0.45
    k = np.clip((dist_u - 260) / 1300, 0, 1)[..., None]
    out = vivid * (1 - k) + back * k
    out[block] = (0.30, 0.34, 0.42)
    edge = block & ~ndimage.binary_erosion(block, iterations=max(1, int(round(OVERSAMPLE * 1.5))))
    out[edge] = (0.85, 0.88, 0.95)
    return out


def render(files, style='full'):
    col = files['map/map.col']
    F = floors(col)
    flipped = not len(F)
    if flipped:
        F = floors(col, flip=True)
    objects = placed(files)
    # the placed scenery's floors count too (a secret course is nearly all of it)
    extra = [floors(c, M=M) for _, c, M in objects if c]
    F = np.concatenate([F] + [x for x in extra if len(x)]) if extra else F
    if not len(F):
        return None
    # huge triangles neither set ceilings nor (unless there is nothing else)
    # frame the map
    small = without_huge(F)
    if len(small):
        lo, dens, W_out, H_out = frame(small)
    elif objects:  # no floor but huge ones (Bowser's bath): frame the placed models
        pts = []
        for model, _, M in objects:
            for mi, sh in model.draws:
                P = model.triangles(sh, model.mats[mi]['texsrc'])[0]
                if len(P):
                    pts.append(P.reshape(-1, 3) @ M[:3, :3].T + M[:3, 3])
        P = np.concatenate(pts)
        lo, dens, W_out, H_out = frame(np.stack([P, P, P], 1)[:len(P) // 3 * 3])
    else:
        lo, dens, W_out, H_out = frame(F)
    F = small
    s = dens * OVERSAMPLE
    out = Image.new('RGB', (W_out, H_out))
    step = TILE // OVERSAMPLE  # output pixels per tile
    for ty in range(0, H_out, step):
        for tx in range(0, W_out, step):
            tw, th = min(step, W_out - tx), min(step, H_out - ty)
            m = OVERLAP  # oversampled pixels of margin on every side
            W, H = tw * OVERSAMPLE + 2 * m, th * OVERSAMPLE + 2 * m
            tlo = lo + (np.array([tx, ty]) * OVERSAMPLE - m) / s
            img = render_tile(files, F, flipped, objects, tlo, s, W, H, style)[m:m + th * OVERSAMPLE, m:m + tw * OVERSAMPLE]
            tile = Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).resize((tw, th), Image.LANCZOS)
            out.paste(tile, (tx, ty))
    return out, lo, dens


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--disc', required=True)
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.dirname(HERE)), 'assets', 'maps'))
    ap.add_argument('--only', default='', help='comma-separated scene names (no .szs)')
    ap.add_argument('--quality', type=int, default=95, help='JPEG quality (full-resolution colour)')
    ap.add_argument('--style', choices=('full', 'radar'), default='full',
                    help='full: the stage as it is; radar: scenery greyed and buildings as blocks')
    ap.add_argument('--except', dest='skip', default='',
                    help="a maps.txt whose stages (by key) are left out: the retail maps, for a mod's pack")
    a = ap.parse_args()
    disc = Disc(a.disc)
    if disc.read(0, 6) not in (b'GMSE01', b'GMSE04'):  # retail; Super Mario Eclipse
        sys.exit('not a GMSE01 (or Eclipse GMSE04) disc image')
    os.makedirs(a.out, exist_ok=True)
    only = set(x for x in a.only.split(',') if x)
    scenes = scene_files(disc)
    skip = set()
    if a.skip:
        for line in open(a.skip):
            if line.strip() and not line.startswith('#'):
                skip.add(int(line.split()[0], 16))
    maps = {}  # col hash -> [file, x0, z0, density, w, h, [scenes]]
    manifest = os.path.join(a.out, 'maps.txt')
    if only and os.path.exists(manifest):  # add to the maps already made
        for line in open(manifest):
            if line.startswith('#') or not line.strip():
                continue
            h, fn, x0, z0, dens, w, hh, st = line.split()
            maps[int(h, 16)] = [fn, float(x0), float(z0), float(dens), int(w), int(hh), st.split(',')]
    for name in sorted(scenes):
        stem = name.rsplit('.', 1)[0]
        if only and stem not in only:
            continue
        off, size = scenes[name]
        files = rarc_files(yaz0(disc.read(off, size)))
        files = {k.split('/', 1)[1] if k.startswith('scene/') else k: v for k, v in files.items()}
        if 'map/map.col' not in files or 'map/map/map.bmd' not in files:
            print('%-14s no map' % stem, flush=True)
            continue
        h = col_key(files['map/map.col'])
        if h in skip:
            print('%-14s in %s' % (stem, a.skip), flush=True)
            continue
        if h in maps and stem in maps[h][6] and only:
            pass  # made again below
        elif h in maps:
            maps[h][6].append(stem)
            print('%-14s same as %s' % (stem, maps[h][6][0]), flush=True)
            continue
        t = time.time()
        r = render(files, a.style)
        if r is None:
            print('%-14s no floors' % stem, flush=True)
            continue
        im, lo, dens = r
        scenes_with = maps[h][6] if h in maps else [stem]
        fn = scenes_with[0] + '.jpg'
        im.save(os.path.join(a.out, fn), quality=a.quality, subsampling=0, optimize=True)
        maps[h] = [fn, float(lo[0]), float(lo[1]), float(dens), im.width, im.height, scenes_with]
        print('%-14s %4dx%-4d %.1f s' % (stem, im.width, im.height, time.time() - t), flush=True)
    with open(manifest, 'w') as f:
        f.write('# stage maps (tools/radar/gen_maps.py): col key, file, x0, z0, pixels per unit, '
                'width, height, scenes\n')
        for h, (fn, x0, z0, dens, w, hh, st) in sorted(maps.items(), key=lambda kv: kv[1][6][0]):
            f.write('%016x %s %.1f %.1f %.6f %d %d %s\n' % (h, fn, x0, z0, dens, w, hh, ','.join(st)))
    print('%d maps' % len(maps))


if __name__ == '__main__':
    main()

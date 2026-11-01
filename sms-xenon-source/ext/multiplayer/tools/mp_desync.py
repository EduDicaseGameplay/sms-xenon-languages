#!/usr/bin/env python3
"""Lays two games' actor dumps side by side (SMS_MULTIPLAYER_DUMP=1) to find
what each game runs its own way.

usage: mp_desync.py HOST_LOG CLIENT_LOG [--skip SECONDS] [--min UNITS]

Both games run on one computer, so their dumps share the wall clock: at each
moment both dumped, every actor (named by its manager's place and its own, as
the games name shared objects) is compared. Per class: how many actors, how
far apart on average, how often more than --min units apart, the worst, and
how often one game has it alive or shown while the other does not.
"""
import argparse
import collections
import math
import re

LINE = re.compile(r'\[mpdump\] (\d+) (\d+\.\d+) (\S+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) ([0-9a-f]+)(?: (-?\d+) (-?\d+))?(?: (.*))?')
DEAD, HIDDEN = 0x1, 0x2


def read(path):
    out = collections.defaultdict(dict)
    with open(path, encoding='shift_jis', errors='replace') as f:
        for line in f:
            m = LINE.search(line)
            if m:
                t, key, cls = int(m.group(1)), m.group(2), m.group(3)
                if m.group(11) and cls.endswith('TMapObjBase'):
                    cls += ':' + m.group(11).strip()  # the many plain map objects, by their name
                x, y, z, r = (float(m.group(i)) for i in range(4, 8))
                rx, rz = (float(m.group(9) or 0), float(m.group(10) or 0))
                out[t][key] = (cls, x, y, z, r, int(m.group(8), 16), rx, rz)
    return out


def demangle(name):
    name, _, label = name.partition(':')
    m = re.fullmatch(r'(\d+)(.*)', name)
    base = m.group(2) if m and len(m.group(2)) == int(m.group(1)) else name
    return base + (':' + label if label else '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host')
    ap.add_argument('client')
    ap.add_argument('--skip', type=float, default=10.0, help='seconds of shared dumps left out at the start')
    ap.add_argument('--min', type=float, default=50.0, help='units apart that count as apart')
    args = ap.parse_args()
    host, client = read(args.host), read(args.client)
    times = sorted(set(host) & set(client))
    times = [t for t in times if t >= (times[0] if times else 0) + args.skip * 1000]
    stats = collections.defaultdict(lambda: {'actors': set(), 'n': 0, 'sum': 0.0, 'far': 0, 'worst': 0.0,
                                             'flags': 0, 'rot': 0.0})
    mismatched = 0
    for t in times:
        for key, h in host[t].items():
            c = client[t].get(key)
            if not c:
                continue
            if c[0] != h[0]:
                mismatched += 1
                continue
            s = stats[demangle(h[0])]
            s['actors'].add(key)
            if (h[5] & DEAD) and (c[5] & DEAD):
                continue  # gone in both
            if (h[5] ^ c[5]) & (DEAD | HIDDEN):
                s['flags'] += 1
            d = math.dist(h[1:4], c[1:4])
            r = max(abs((h[i] - c[i] + 180) % 360 - 180) for i in (4, 6, 7))  # the most any axis is turned apart
            s['n'] += 1
            s['sum'] += d
            s['rot'] += r
            s['far'] += d > args.min
            s['worst'] = max(s['worst'], d)
    print(f'{len(times)} shared moments, {mismatched} actor slots naming different classes')
    print(f'{"class":40} {"actors":>6} {"avg off":>8} {"far %":>6} {"worst":>7} {"turn":>6} {"shown/alive differ %":>21}')
    rows = sorted(stats.items(), key=lambda kv: -(kv[1]['sum'] / max(kv[1]['n'], 1)) - 1000 * kv[1]['flags'] / max(kv[1]['n'], 1))
    for cls, s in rows:
        n = max(s['n'], 1)
        if s['sum'] / n < 1 and s['rot'] / n < 1 and not s['flags']:
            continue
        print(f'{cls:40} {len(s["actors"]):6} {s["sum"] / n:8.0f} {100 * s["far"] / n:6.0f} {s["worst"]:7.0f} '
              f'{s["rot"] / n:6.0f} {100 * s["flags"] / n:21.0f}')


if __name__ == '__main__':
    main()

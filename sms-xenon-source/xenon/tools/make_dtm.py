#!/usr/bin/env python3
"""Write a Dolphin .dtm movie from a press script, for Dolphin (-m) and the port (SMS_MOVIE).

usage: make_dtm.py OUT.dtm LAST_FIELD SCRIPT [BASE]
SCRIPT is SMS_AUTOPRESS syntax: control@field[+hold],... (fields in retail numbering,
hold in fields, default 8). Controls: start a b x y z l r up down left right
stick_left stick_right stick_up stick_down cstick_left cstick_right cstick_up cstick_down.
One record per controller poll; poll index of field F is 2F - 227, the mapping the
port's movie player uses without a poll map (platform/trace/README.md). BASE replaces
the 227: Dolphin with SkipIPL has no IPL fields, so its movies use BASE 0."""
import struct, sys

BITS = {'start': 0, 'a': 1, 'b': 2, 'x': 3, 'y': 4, 'z': 5, 'up': 6, 'down': 7,
        'left': 8, 'right': 9, 'l': 10, 'r': 11}
AXES = {'stick_left': (4, 0), 'stick_right': (4, 255), 'stick_down': (5, 0), 'stick_up': (5, 255),
        'cstick_left': (6, 0), 'cstick_right': (6, 255), 'cstick_down': (7, 0), 'cstick_up': (7, 255)}
CONNECTED = 1 << 14

BASE = 227

def poll(field):
    return 2 * field - BASE

def build(last_field, script):
    n = poll(last_field)
    recs = [[CONNECTED, 0, 0, 128, 128, 128, 128] for _ in range(n)]
    for tok in filter(None, script.split(',')):
        ctl, rest = tok.lower().split('@')
        at, _, hold = rest.partition('+')
        at, hold = int(at), int(hold or 8)
        for p in range(max(0, poll(at)), min(n, poll(at + hold))):
            r = recs[p]
            if ctl in BITS:
                r[0] |= 1 << BITS[ctl]
                if ctl in ('l', 'r'):
                    r[1 if ctl == 'l' else 2] = 255
            elif ctl in AXES:
                idx, val = AXES[ctl]
                r[idx - 1] = val
            else:
                sys.exit('unknown control %r' % ctl)
    return recs

def header(n_inputs, n_fields):
    h = bytearray(256)
    h[0:4] = b'DTM\x1a'
    h[4:10] = b'GMSE01'
    h[0x0B] = 1                                   # GC port 1
    struct.pack_into('<QQ', h, 0x0D, n_fields, n_inputs)
    h[0x31:0x31 + 9] = b'sms-xenon'
    h[0x51:0x51 + 16] = b'Software Renderer'[:16]
    # bSaveConfig: Dolphin applies these settings, so every run starts alike:
    # skip idle, single core, DSP HLE, JIT64, EFB access and copies to RAM (the goop map
    # needs them), memory card in slot A started from a clear save, FMA on.
    cfg = {0x89: 1, 0x8A: 1, 0x8D: 1, 0x8F: 1, 0x90: 1, 0x91: 1, 0x94: 1,
           0x97: 1, 0x98: 1, 0xA0: 1}
    for off, val in cfg.items():
        h[off] = val
    # tickCount: Dolphin ends playback once emulated ticks pass it, so never.
    struct.pack_into('<Q', h, 0xED, 0xFFFFFFFFFFFFFFFF)
    return bytes(h)

def main():
    global BASE
    out, last, script = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    if len(sys.argv) > 4:
        BASE = int(sys.argv[4])
    recs = build(last, script)
    with open(out, 'wb') as f:
        f.write(header(len(recs), last))
        for r in recs:
            f.write(struct.pack('<HBBBBBB', *r))

def selftest():
    r = build(300, 'start@200+2,stick_left@250')
    assert len(r) == 373
    assert r[poll(200)][0] == CONNECTED | 1 and r[poll(202)][0] == CONNECTED
    assert r[poll(250)][3] == 0 and r[poll(258)][3] == 128
    assert len(header(1, 1)) == 256

if __name__ == '__main__':
    if sys.argv[1:] == ['--test']:
        selftest(); print('ok')
    else:
        main()

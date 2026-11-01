#!/usr/bin/env python3
"""Drop the SDK stubs the Xbox 360 build defines for real.

usage: xstubs.py REPO STAGE

platform/sdk_stubs.cpp defines a weak stub for every Dolphin SDK function, and the
platform layer's real ones replace them at link time. COFF has no weak functions,
so the staged copy loses each stub whose function a unit of the 360 build defines:
the units' Linux objects (nm) give the names, and xenon/'s own sources add theirs
(any line "NAME(" defining a function at the start of a line, in xenon/*/)."""
import json, os, re, subprocess, sys

repo, stage = sys.argv[1], sys.argv[2]
cc = json.load(open(os.path.join(repo, 'build/linux-32/compile_commands.json')))
listed = set()
for n in ('units-port.txt', 'units-gx.txt'):
    listed |= {l.strip() for l in open(os.path.join(repo, 'xenon', n)) if l.strip() and not l.startswith('#')}
objs = []
for e in cc:
    f = os.path.relpath(e['file'], repo); c = e['command']
    if f.endswith('sdk_stubs.cpp'):
        continue
    game = '-DVERSION_GMSE01' in c and ('-std=gnu++03' in c or '-std=gnu99' in c) and not f.startswith('ext/multiplayer/')
    staged = 'game/' + f[len('decomp/'):] if f.startswith('decomp/') else f  # units-*.txt name staged paths
    if game or f in listed or staged in listed:
        # older CMake (Ubuntu 22.04's 3.22) writes no "output": the object is the command's -o
        out = e.get('output') or re.search(r'(?:^|\s)-o\s+(\S+)', c).group(1)
        objs.append(os.path.join(e.get('directory', os.path.join(repo, 'build/linux-32')), out))
missing = [o for o in objs if not os.path.exists(o)]
if missing:
    sys.exit('xstubs: %d of %d Linux objects are missing (first: %s); build the Linux port first: '
             'cmake --build build/linux-32 --target sms' % (len(missing), len(objs), missing[0]))
defined = set()
for i in range(0, len(objs), 200):
    out = subprocess.run(['nm', '--defined-only', '-C'] + objs[i:i + 200], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in 'TW':
            defined.add(re.split(r'[(<]', parts[2])[0].split('::')[-1])
for root, _, files in os.walk(os.path.join(repo, 'xenon')):
    if '/tools' in root:
        continue
    for f in files:
        if f.endswith('.cpp'):
            for m in re.finditer(r'^(?:extern "C" )?[\w\s\*&:]+?\b(\w+)\s*\([^;]*\)\s*\{', open(os.path.join(root, f)).read(), re.M):
                defined.add(m.group(1))
path = os.path.join(stage, 'platform/sdk_stubs.cpp')
lines = open(path).read().split('\n')
kept, dropped = [], 0
for l in lines:
    m = re.match(r'SDK_WEAK .*?\b(\w+)\s*\(', l)
    if m and (m.group(1) in defined or 'PS' + m.group(1) in defined):  # dolphin/mtx.h maps MTX* to PSMTX*
        dropped += 1
        continue
    kept.append(l)
open(path, 'w').write('\n'.join(kept))
print('xstubs: %d stubs dropped, %d kept' % (dropped, sum(1 for l in kept if l.startswith('SDK_WEAK'))))

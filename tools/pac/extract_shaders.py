#!/usr/bin/env python3
"""Extracts every Xbox 360 shader container from the game's archives.

Real Steel keeps its shaders inside BPE-compressed blocks of the Yuke's PAC
archives (2026-10-02: all 497 containers / 406 unique sit in the first block
of pac/_startup.pac; every pac is scanned anyway, title update included).
Containers are written as <vs|ps>_<FNV1a64 of the bytes>.xpu, the naming the
runtime dumper uses - pixel shaders are byte-identical to what the game
loads; vertex shaders are the unpatched on-disk form (the game rewrites the
vfetch block per vertex declaration).

Usage: python tools/pac/extract_shaders.py [out_dir=rs_shaders/archive]
       [--root=<game folder> --root=<title update folder>]  (default extracted/game, extracted/tu)
"""
import glob
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yukes_bpe

MAGIC = re.compile(re.escape(b'\x10\x2a\x11') + b'[\x00\x01]')


def fnv1a64(b):
    h = 0xCBF29CE484222325
    for c in b:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def containers(x):
    for m in MAGIC.finditer(x):
        o = m.start()
        if o + 36 > len(x):
            continue
        f = struct.unpack_from('>9I', x, o)
        if 36 <= f[1] <= 65536 and 0 < f[2] <= 262144 and f[6] < f[1] and f[7] == 0 and f[8] == 0:
            if o + f[1] + f[2] <= len(x):
                yield bool(f[0] & 1), x[o:o + f[1] + f[2]]


def blocks(data):
    yield 'raw', data
    for m in re.finditer(b'BPE ', data):
        try:
            out, _ = yukes_bpe.decode(data, m.start())
            yield 'bpe@%x' % m.start(), out
        except Exception:
            pass
    for m in re.finditer(b'\x78[\x9c\xda]', data):
        try:
            out = zlib.decompressobj().decompress(data[m.start():m.start() + (64 << 20)])
            if len(out) > 64:
                yield 'zlib@%x' % m.start(), out
        except Exception:
            pass


def extract(roots, out_dir, verbose=True):
    """Writes every unique container found under |roots| (folders) to
    |out_dir|; returns how many."""
    os.makedirs(out_dir, exist_ok=True)
    seen = {}
    files = []
    for root in roots:
        files += sorted(glob.glob(os.path.join(root, '**', '*'), recursive=True))
    for f in files:
        if not os.path.isfile(f):
            continue
        data = open(f, 'rb').read()
        for tag, x in blocks(data):
            n = 0
            for is_vs, c in containers(x):
                n += 1
                h = fnv1a64(c)
                if h not in seen:
                    seen[h] = (is_vs, c, f, tag)
            if n and verbose:
                print('%4d containers in %s [%s]' % (n, f, tag))
    for h, (is_vs, c, f, tag) in seen.items():
        open(os.path.join(out_dir, '%s_%016X.xpu' % ('vs' if is_vs else 'ps', h)), 'wb').write(c)
    nv = sum(1 for v in seen.values() if v[0])
    if verbose:
        print('unique: %d vertex, %d pixel -> %s' % (nv, len(seen) - nv, out_dir))
    return len(seen)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    roots = [a[7:] for a in sys.argv[1:] if a.startswith('--root=')]
    out_dir = args[0] if args else 'rs_shaders/archive'
    if not roots:
        roots = ['extracted/game', 'extracted/tu']
    extract(roots, out_dir)


if __name__ == '__main__':
    main()

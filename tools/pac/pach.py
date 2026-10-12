"""Yuke's PACH containers (inside EPAC entries): 'PACH', u32 count, then
count x (u32 id, u32 offset from the end of the table, u32 size); each item
is usually a 'BPE ' block.

  pach.py list <file>
  pach.py extract <file> <out_dir>     (items decoded, <id>.bin)
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yukes_bpe


def items(d):
    assert d[:4] == b'PACH', d[:4]
    count, = struct.unpack_from('<I', d, 4)
    base = 8 + count * 12
    for i in range(count):
        ident, off, size = struct.unpack_from('<3I', d, 8 + i * 12)
        raw = d[base + off:base + off + size]
        yield ident, raw


def decode(raw):
    if raw[:4] == b'BPE ':
        return yukes_bpe.decode(raw)[0]
    return raw


def main():
    d = open(sys.argv[2], 'rb').read()
    for ident, raw in items(d):
        out = decode(raw)
        print('  %04X  packed %7X  size %7X  %s  %s' % (ident, len(raw), len(out), out[:4], out[:24].hex()))
        if sys.argv[1] == 'extract':
            os.makedirs(sys.argv[3], exist_ok=True)
            open(os.path.join(sys.argv[3], '%04X.bin' % ident), 'wb').write(out)


if __name__ == '__main__':
    main()

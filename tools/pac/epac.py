"""Yuke's EPAC archives (Real Steel pac/*.pac).

Layout: 'EPAC', u32 directory bytes, u32 data bytes, u32 7, zero fill to
0x800; the directory at 0x800 is a list of groups (4-char name, u32 word
count = 3 per entry, u32 0) each followed by its entries (4-char name, u32
offset in 0x800 sectors from the data start, u32 size in 0x100 units); data
starts at 0x4000 (the header and directory area are fixed-size).

  epac.py list <file.pac>
  epac.py extract <file.pac> <out_dir>      (GROUP/NAME.bin, BPE left packed)
"""
import os, struct, sys


def read(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'EPAC', d[:4]
    dir_size, data_size, kind = struct.unpack_from('<3I', d, 4)
    data_start = 0x4000  # the directory area is fixed-size
    groups = []
    off, end = 0x800, 0x800 + dir_size
    while off < end:
        gname = d[off:off + 4]
        words, zero = struct.unpack_from('<2I', d, off + 4)
        off += 12
        entries = []
        for _ in range(words // 3):
            name = d[off:off + 4]
            sector, size = struct.unpack_from('<2I', d, off + 4)
            entries.append((name.decode('latin1'), data_start + sector * 0x800, size * 0x100))
            off += 12
        groups.append((gname.decode('latin1'), entries))
    return d, groups, (dir_size, data_size, kind, data_start)


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    d, groups, info = read(path)
    print('%s: dir %#x data %#x kind %d data_start %#x file %#x' % ((os.path.basename(path),) + info + (len(d),)))
    for gname, entries in groups:
        for name, off, size in entries:
            head = d[off:off + 16]
            print('  %s/%s  @%08X  %8X  %s  %s' % (gname, name, off, size, head[:4], head.hex()))
            if cmd == 'extract':
                out = os.path.join(sys.argv[3], gname.strip())
                os.makedirs(out, exist_ok=True)
                open(os.path.join(out, name.strip() + '.bin'), 'wb').write(d[off:off + size])


if __name__ == '__main__':
    main()

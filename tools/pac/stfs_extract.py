"""Extracts Xbox 360 STFS packages (LIVE / PIRS / CON), e.g. the Real Steel
add-on packages in dumps/dlc.

  python tools/pac/stfs_extract.py --list <packages>
  python tools/pac/stfs_extract.py --work <work dir> <game package> <title update package>
  python tools/pac/stfs_extract.py --install <user_data_root> <add-on packages>
  python tools/pac/stfs_extract.py --extract <out dir> <packages>

--work lays the game out for building (docs/building.md): the game package into
<work dir>/game, the title update into <work dir>/tu, and the update's
default.xexp beside the game's default.xex.

--install lays each package out the way the ReXGlue content manager expects
marketplace content (not tied to a profile):
  <root>/0000000000000000/<title id>/<content type>/<package file name>/...
  <root>/0000000000000000/<title id>/Headers/<content type>/<package file name>.header
"""
import glob
import os
import struct
import sys

BLOCK = 0x1000
PER_L0 = 0xAA      # data blocks per level-0 hash table
PER_L1 = 0x70E4    # data blocks per level-1 hash table


class Stfs:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        head = self.f.read(0xA000)
        self.head = head
        self.magic = head[:4]
        if self.magic not in (b'LIVE', b'PIRS', b'CON '):
            raise ValueError('%s: not an STFS package' % path)
        self.header_size = struct.unpack('>I', head[0x340:0x344])[0]
        self.content_type = struct.unpack('>I', head[0x344:0x348])[0]
        self.title_id = struct.unpack('>I', head[0x360:0x364])[0]
        self.display_name = head[0x411:0x491].decode('utf-16-be', 'replace').split('\0')[0]
        if struct.unpack('>I', head[0x3A9:0x3AD])[0] != 0:
            raise ValueError('%s: SVOD volume, not STFS' % path)
        self.read_only = (head[0x37B] & 1) != 0
        self.table_blocks = struct.unpack('<H', head[0x37C:0x37E])[0]
        self.table_start = int.from_bytes(head[0x37E:0x381], 'little')
        self.total_blocks = struct.unpack('>I', head[0x395:0x399])[0]
        self.data_start = (self.header_size + 0xFFF) & ~0xFFF
        self.shift = 0 if self.read_only else 1
        self.entries = self._read_table()

    def _data_offset(self, block):
        n = block
        for per in (PER_L0, PER_L0 * PER_L0, PER_L0 * PER_L0 * PER_L0):
            n += ((block + per) // per) << self.shift
            if block < per:
                break
        return self.data_start + n * BLOCK

    def _hash_entry(self, block):
        # Level-0 hash table covering |block|.
        step0 = 0xAB if self.read_only else 0xAC
        if block < PER_L0:
            table = 0
        else:
            table = (block // PER_L0) * step0
            table += ((block // PER_L1) + 1) << self.shift
            if block >= PER_L1:
                table += 1 << self.shift
        offset = self.data_start + table * BLOCK
        if not self.read_only:
            # Two copies of every table; the live one is flagged in the level
            # above. Packages from the marketplace are read-only, so this
            # path only tries the first copy.
            pass
        self.f.seek(offset + (block % PER_L0) * 0x18)
        entry = self.f.read(0x18)
        return entry[0x14], int.from_bytes(entry[0x15:0x18], 'big')

    def _chain(self, start, count):
        blocks = []
        block = start
        while len(blocks) < count and block != 0xFFFFFF:
            if block >= self.total_blocks:
                raise ValueError('%s: block %d out of range' % (self.path, block))
            blocks.append(block)
            block = self._hash_entry(block)[1]
        return blocks

    def _read_blocks(self, start, count):
        out = bytearray()
        for block in self._chain(start, count):
            self.f.seek(self._data_offset(block))
            out += self.f.read(BLOCK)
        return bytes(out)

    def _read_table(self):
        raw = self._read_blocks(self.table_start, self.table_blocks)
        entries = []
        for i in range(0, len(raw) - 0x3F, 0x40):
            e = raw[i:i + 0x40]
            if e[0] == 0:
                break
            flags = e[0x28]
            entries.append({
                'name': e[:flags & 0x3F].decode('ascii', 'replace'),
                'dir': bool(flags & 0x80),
                'blocks': int.from_bytes(e[0x29:0x2C], 'little'),
                'start': int.from_bytes(e[0x2F:0x32], 'little'),
                'parent': struct.unpack('>H', e[0x32:0x34])[0],
                'size': struct.unpack('>I', e[0x34:0x38])[0],
            })
        for e in entries:
            parts = [e['name']]
            p = e['parent']
            while p != 0xFFFF and p < len(entries):
                parts.append(entries[p]['name'])
                p = entries[p]['parent']
            e['path'] = '/'.join(reversed(parts))
        return entries

    def read_file(self, e):
        blocks = (e['size'] + BLOCK - 1) // BLOCK
        return self._read_blocks(e['start'], blocks)[:e['size']]

    def extract(self, out_dir):
        os.makedirs(out_dir, exist_ok=True)
        for e in self.entries:
            target = os.path.join(out_dir, *e['path'].split('/'))
            if e['dir']:
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            data = self.read_file(e)
            if len(data) != e['size']:
                raise ValueError('%s: %s is short (%d of %d bytes)' %
                                 (self.path, e['path'], len(data), e['size']))
            with open(target, 'wb') as f:
                f.write(data)


def main(argv):
    if len(argv) < 3 or argv[1] not in ('--list', '--install', '--extract', '--work'):
        print(__doc__)
        return 1
    mode = argv[1]
    root = None
    files = argv[2:]
    if mode in ('--install', '--extract', '--work'):
        root = argv[2]
        files = argv[3:]
    paths = []
    for pattern in files:
        paths += sorted(glob.glob(pattern)) or [pattern]
    for path in paths:
        pkg = Stfs(path)
        name = os.path.basename(path)
        total = sum(e['size'] for e in pkg.entries if not e['dir'])
        print('%s  %s  type %08X  title %08X  "%s"  %d files, %d bytes%s' % (
            name[:12], pkg.magic.decode(), pkg.content_type, pkg.title_id,
            pkg.display_name, sum(not e['dir'] for e in pkg.entries), total,
            '' if pkg.read_only else '  (writable format)'))
        if mode == '--list':
            for e in pkg.entries:
                print('    %-48s %s' % (e['path'], 'dir' if e['dir'] else e['size']))
            continue
        if mode == '--extract':
            pkg.extract(os.path.join(root, name))
            continue
        if mode == '--work':
            import shutil
            if pkg.title_id != 0x584111E0:
                print('    skipped: not Real Steel')
                continue
            if any(e['path'] == 'default.xex' for e in pkg.entries):
                pkg.extract(os.path.join(root, 'game'))
                print('    -> %s' % os.path.join(root, 'game'))
            elif any(e['path'] == 'default.xexp' for e in pkg.entries):
                pkg.extract(os.path.join(root, 'tu'))
                os.makedirs(os.path.join(root, 'game'), exist_ok=True)
                shutil.copy(os.path.join(root, 'tu', 'default.xexp'), os.path.join(root, 'game', 'default.xexp'))
                print('    -> %s (default.xexp copied into game/)' % os.path.join(root, 'tu'))
            else:
                print('    skipped: neither the game nor a title update')
            continue
        base = os.path.join(root, '0000000000000000', '%08X' % pkg.title_id)
        target = os.path.join(base, '%08X' % pkg.content_type, name)
        if not os.path.isdir(target):
            pkg.extract(target)
        header_dir = os.path.join(base, 'Headers', '%08X' % pkg.content_type)
        os.makedirs(header_dir, exist_ok=True)
        # The content manager's header file is an XCONTENT_AGGREGATE_DATA
        # (0x148 bytes, big-endian: device id, content type, display name
        # UTF-16, 42-char file name, xuid, title id) followed by the
        # package's licence mask - not the STFS header itself (a raw STFS
        # header made the game see device 'LIVE' and fail every open).
        data = struct.pack('>II', 1, pkg.content_type)
        data += pkg.display_name.encode('utf-16-be')[:254].ljust(256, b'\0')
        data += name.encode('ascii')[:42].ljust(44, b'\0')
        data = data.ljust(0x138, b'\0')
        data += struct.pack('>QII', 0, pkg.title_id, 0)
        assert len(data) == 0x148
        data += struct.pack('>I', 0xFFFFFFFF)
        with open(os.path.join(header_dir, name + '.header'), 'wb') as f:
            f.write(data)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

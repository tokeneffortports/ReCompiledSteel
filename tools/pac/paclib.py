"""Read / modify / write Real Steel archives: EPAC files, the PACH containers
nested inside them and BPE blocks.

An archive is loaded as a tree of Node objects. Untouched nodes are written
back byte-for-byte; a node whose data (or any descendant) changed is rebuilt,
re-packed as a stored BPE block when it was compressed before.

    arc = paclib.load('misc.pac')
    node = arc.find('/BATS/INIT/001D/0003/0001')
    node.set_data(new_bytes)
    arc.save('out/misc.pac')

  paclib.py verify <file.pac>...     rebuilds every node and compares
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import yukes_bpe

DATA_START = 0x4000


def bpe_store(data):
    """Packs data as a BPE block with identity tables (no compression)."""
    out = bytearray()
    for i in range(0, len(data), 0x1000):
        chunk = data[i:i + 0x1000]
        out += b'\xff\x80\xfe' + struct.pack('<H', len(chunk)) + chunk
    return b'BPE ' + struct.pack('<3I', 0x100, len(out), len(data)) + bytes(out)


def _parse_pach(blob):
    if blob[:4] != b'PACH' or len(blob) < 8:
        return None
    count, = struct.unpack_from('<I', blob, 4)
    base = 8 + count * 12
    if count > 0x4000 or base > len(blob):
        return None
    out = []
    align = 16
    for i in range(count):
        ident, off, size = struct.unpack_from('<3I', blob, 8 + i * 12)
        if base + off + size > len(blob):
            return None
        if (base + off) % 16:
            align = 4
        out.append((ident, blob[base + off:base + off + size]))
    return out, align


class Node:
    def __init__(self, name, raw):
        self.name = name
        self.raw = raw            # bytes as stored in the parent
        self.packed = raw[:4] == b'BPE '
        self.data = yukes_bpe.decode(raw)[0] if self.packed else raw
        self.dirty = False
        self.parent = None
        parsed = _parse_pach(self.data)
        self.children = None
        if parsed is not None:
            # items sit at file offsets aligned to 4 or (some tables) 16
            items, self.align = parsed
            self.children = [Node('%04X' % ident, r) for ident, r in items]
            for c in self.children:
                c.parent = self

    def touch(self):
        node = self
        while node is not None:
            node.dirty = True
            node = node.parent

    def set_data(self, data):
        self.data = bytes(data)
        self.children = None
        self.touch()

    def child(self, name):
        for c in self.children or []:
            if c.name == name:
                return c
        return None

    def add_child(self, name, raw):
        node = Node(name, raw)
        node.parent = self
        self.children.append(node)
        self.touch()
        return node

    def build_data(self):
        if self.children is None:
            return self.data
        table = bytearray()
        body = bytearray()
        base = 8 + len(self.children) * 12
        for c in self.children:
            raw = c.build()
            while (base + len(body)) % self.align:
                body.append(0)
            table += struct.pack('<3I', int(c.name, 16), len(body), len(raw))
            body += raw
        return b'PACH' + struct.pack('<I', len(self.children)) + bytes(table) + bytes(body)

    def build(self):
        if not self.dirty:
            return self.raw
        data = self.build_data()
        return bpe_store(data) if self.packed else data

    def walk(self, path=''):
        here = path + '/' + self.name
        yield here, self
        for c in self.children or []:
            yield from c.walk(here)


class Archive:
    def __init__(self, path):
        d = open(path, 'rb').read()
        assert d[:4] == b'EPAC', path
        dir_size, self.data_size, self.kind = struct.unpack_from('<3I', d, 4)
        self.trailer = d[DATA_START + self.data_size:]   # packer's signature block
        self.groups = []
        off, end = 0x800, 0x800 + dir_size
        while off < end:
            gname = d[off:off + 4]
            words, = struct.unpack_from('<I', d, off + 4)
            off += 12
            nodes = []
            for _ in range(words // 3):
                name = d[off:off + 4]
                sector, size = struct.unpack_from('<2I', d, off + 4)
                start = DATA_START + sector * 0x800
                node = Node(name.decode('latin1').strip(), d[start:start + size * 0x100])
                node.entry_name = name
                nodes.append(node)
                off += 12
            self.groups.append((gname, nodes))

    def find(self, path):
        parts = path.strip('/').split('/')
        for gname, nodes in self.groups:
            if gname.decode('latin1').strip() != parts[0]:
                continue
            for node in nodes:
                if node.name != parts[1]:
                    continue
                for p in parts[2:]:
                    node = node.child(p)
                    if node is None:
                        return None
                return node
        return None

    def group(self, name):
        for gname, nodes in self.groups:
            if gname.decode('latin1').strip() == name:
                return nodes
        return None

    def add_entry(self, group, name, raw):
        node = Node(name, raw)
        node.entry_name = name.encode('latin1').ljust(4)
        node.dirty = False
        self.group(group).append(node)
        return node

    def walk(self):
        for gname, nodes in self.groups:
            for node in nodes:
                yield from node.walk('/' + gname.decode('latin1').strip())

    def save(self, path):
        directory = bytearray()
        body = bytearray()
        for gname, nodes in self.groups:
            directory += gname + struct.pack('<2I', len(nodes) * 3, 0)
            for node in nodes:
                raw = node.build()
                raw += b'\0' * (-len(raw) % 0x100)
                node.sector, node.units = len(body) // 0x800, len(raw) // 0x100
                directory += node.entry_name + struct.pack('<2I', node.sector, node.units)
                body += raw + b'\0' * (-len(raw) % 0x800)
        assert len(directory) <= DATA_START - 0x800
        head = b'EPAC' + struct.pack('<3I', len(directory), len(body), self.kind)
        out = head.ljust(0x800, b'\0') + bytes(directory).ljust(DATA_START - 0x800, b'\0') + bytes(body) + self.trailer
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(out)
        return out


def load(path):
    return Archive(path)


# ---------------------------------------------------------------------------
# Archive index files (plist360.arc, info/catalog_hd.arc / catalog_sd.arc):
# the game finds archive entries through these, not through the archives' own
# directories. Big-endian: 4 bytes ff 00 00 00, then one section per archive
# in file-table order, sections separated by a marker (u16 ffff, u16 index of
# the next archive; ffff ffff ends the file). A section is the archive's directory: per group its
# name, u32 (words << 20 | 0x1000 when entry names are 8 characters), u32 0,
# then per entry its name, u32 sector, u32 size in 0x100 units.
# ---------------------------------------------------------------------------
def arc_sections(data):
    """Splits an index file into its per-archive directory sections."""
    assert data[:4] == b'\xff\x00\x00\x00', data[:4]
    sections = []
    pos = start = 4
    while pos < len(data):
        if data[pos:pos + 2] == b'\xff\xff':
            sections.append(data[start:pos])
            if data[pos + 2:pos + 4] == b'\xff\xff':   # end of file
                return sections
            pos += 4
            start = pos
            continue
        head, = struct.unpack_from('>I', data, pos + 4)
        name_len = 8 if head & 0x1000 else 4
        pos += 12 + (head >> 20) // (2 + name_len // 4) * (name_len + 8)
    sections.append(data[start:pos])
    return sections


def arc_join(sections):
    out = bytearray(b'\xff\x00\x00\x00')
    for i, section in enumerate(sections):
        if i:
            out += struct.pack('>HH', 0xFFFF, i)
        out += section
    return bytes(out) + b'\xff' * 4


def arc_section(archive):
    """The index section of a saved Archive (4-character entry names)."""
    out = bytearray()
    for gname, nodes in archive.groups:
        out += gname + struct.pack('>II', (len(nodes) * 3) << 20, 0)
        for node in nodes:
            out += node.entry_name + struct.pack('>II', node.sector, node.units)
    return bytes(out)


def main():
    for path in sys.argv[2:]:
        arc = load(path)
        original = open(path, 'rb').read()
        tmp = path + '.verify.tmp'
        same_file = arc.save(tmp) == original
        os.remove(tmp)
        nodes = exact = 0
        for where, node in arc.walk():
            if node.children is None:
                continue
            nodes += 1
            rebuilt = node.build_data()
            if rebuilt == node.data[:len(rebuilt)] and not any(node.data[len(rebuilt):]):
                exact += 1
            else:
                print('  differs: %s (%d vs %d bytes)' % (where, len(rebuilt), len(node.data)))
        print('%s: file round trip %s, %d/%d containers rebuild exactly' % (os.path.basename(path), 'exact' if same_file else 'DIFFERS', exact, nodes))


if __name__ == '__main__':
    main()

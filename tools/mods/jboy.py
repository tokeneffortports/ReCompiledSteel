"""Real Steel part models: Yuke's 'JBOY' (big-endian YOBJ).

All file offsets in the format are relative to byte 8. Layout as far as it
is understood (see the development notes):

  header   'JBOY', u32 size - 8, u32 0, u32 fixup table offset, u32 0, u32 ?,
           u32 mesh count (top bit set), u32 mesh table, u32 bone count,
           u32 texture count, u32 bones, u32 texture names, u32 groups,
           u32 group count
  mesh     0xB4 bytes: vertex count, ?, weights per vertex, 20 bone indices
           (ffffffff = unused), ?, ?, uv set count?, vertex block, colour
           block, uv block, ?, shader name[16], ?, ?, parameter count,
           parameter table, strip table, vertex count, ?, bounding sphere
           (x, y, z, r)
  vertices per weight a pointer, then per weight an array of 28-byte records
           (position, normal, weight/flags), positions relative to that bone
  strips   at the mesh's strip table: u32 strip count?, then per strip
           (u32 index count, u32 offset) -> u16 indices (triangle strip)
  bone     0x50 bytes: name[16], position (x, y, z, w), rotation (x, y, z,
           w), parent index, ...
  texture  name[16];  group: name[16], ?, ?, first mesh, mesh count

  jboy.py info <model.bin>
"""
import struct, sys


class Mesh:
    pass


class Model:
    def __init__(self, data):
        self.data = bytearray(data)
        d = self.data
        assert d[:4] == b'JBOY', d[:4]
        (self.size, _, self.fixups, _, self.unk14, mesh_count, self.mesh_table, self.bone_count,
         self.texture_count, self.bone_table, self.texture_table, self.group_table,
         self.group_count) = struct.unpack_from('>13I', d, 4)
        self.mesh_count = mesh_count & 0x7FFFFFFF
        self.meshes = []
        for i in range(self.mesh_count):
            o = 8 + self.mesh_table + i * 0xB4
            m = Mesh()
            m.offset = o
            m.vertex_count, m.unk4, m.weights = struct.unpack_from('>3I', d, o)
            m.bones = [b for b in struct.unpack_from('>20I', d, o + 0x0C) if b != 0xFFFFFFFF]
            m.unk5c, m.unk60, m.uv_sets, m.vertex_block, m.weight_block, m.uv_block, m.group = struct.unpack_from('>7I', d, o + 0x5C)
            m.shader = bytes(d[o + 0x78:o + 0x88]).split(b'\0')[0].decode('latin1')
            (m.unk88, m.unk8c, m.param_count, m.param_table, m.strip_table,
             m.vertex_count2, m.unka0) = struct.unpack_from('>7I', d, o + 0x88)
            m.sphere = struct.unpack_from('>4f', d, o + 0xA4)
            self.meshes.append(m)
        self.bones = []
        for i in range(self.bone_count):
            o = 8 + self.bone_table + i * 0x50
            name = bytes(d[o:o + 16]).split(b'\0')[0].decode('latin1')
            pos = struct.unpack_from('>4f', d, o + 16)
            rot = struct.unpack_from('>4f', d, o + 32)
            rest = struct.unpack_from('>8i', d, o + 48)
            self.bones.append((name, pos, rot, rest))
        self.textures = [bytes(d[8 + self.texture_table + i * 16:][:16]).split(b'\0')[0].decode('latin1')
                         for i in range(self.texture_count)]
        self.groups = []
        for i in range(self.group_count):
            o = 8 + self.group_table + i * 32
            self.groups.append((bytes(d[o:o + 16]).split(b'\0')[0].decode('latin1'),) + struct.unpack_from('>4I', d, o + 16))

    def strip(self, m):
        """The mesh's triangle strip (u16 vertex indices)."""
        kind, count, offset = struct.unpack_from('>3I', self.data, 8 + m.strip_table)
        return list(struct.unpack_from('>%dH' % count, self.data, 8 + offset))

    def vertices(self, m, weight=0):
        """(x, y, z, nx, ny, nz, colour) per vertex for one weight."""
        d = self.data
        ptr, = struct.unpack_from('>I', d, 8 + m.vertex_block + weight * 4)
        return [struct.unpack_from('>6fI', d, 8 + ptr + i * 28) for i in range(m.vertex_count)]

    def uvs(self, m):
        return [struct.unpack_from('>2f', self.data, 8 + m.uv_block + i * 8) for i in range(m.vertex_count)]


def main():
    model = Model(open(sys.argv[2], 'rb').read())
    print('size %#x fixups %#x unk14 %d meshes %d bones %d textures %s groups %s' % (
        model.size, model.fixups, model.unk14, model.mesh_count, model.bone_count, model.textures, model.groups))
    for i, (name, pos, rot, rest) in enumerate(model.bones):
        print('  bone %2d %-12s pos %8.3f %8.3f %8.3f %5.2f  rot %6.3f %6.3f %6.3f %6.3f  %s' % ((i, name) + pos + rot + (rest,)))
    for i, m in enumerate(model.meshes):
        print('  mesh %2d verts %5d weights %d bones %s shader %s params %d unk %s sphere %s' % (
            i, m.vertex_count, m.weights, m.bones, m.shader, m.param_count,
            (m.unk4, m.unk5c, m.unk60, m.uv_sets, m.group, m.unk88, m.unk8c, m.vertex_count2, m.unka0),
            ' '.join('%.2f' % v for v in m.sphere)))
        d = model.data
        o = 8 + m.strip_table
        print('          blocks v %#x c %#x uv %#x params %#x strips %#x: %s' % (
            m.vertex_block, m.weight_block, m.uv_block, m.param_table, m.strip_table, bytes(d[o:o + 28]).hex(' ', 4)))


if __name__ == '__main__':
    main()


# ---------------------------------------------------------------------------
# Editing in place: new geometry is written into an existing model's meshes
# without moving any block, so every offset and the fixup table stay valid.
# A mesh can take at most its original vertex count and strip length; what
# is left over is collapsed into degenerate triangles.
# ---------------------------------------------------------------------------
def quad_capacity(model, m):
    """How many quads (4 vertices, 6 strip indices each) the mesh can hold."""
    return min(m.vertex_count // 4, len(model.strip(m)) // 6)


def set_quads(model, m, quads):
    """quads: list of 4 corners (counter-clockwise seen from outside), each
    corner (position xyz, outward normal xyz, uv). Only for single-weight
    meshes. The game's own models store the normals pointing INTO the
    surface and wind their strips to match, so both are turned round here."""
    assert m.weights == 1 and len(quads) <= quad_capacity(model, m)
    d = model.data
    verts, uvs, strip = [], [], []
    for quad in quads:
        base = len(verts)
        for pos, normal, uv in quad:
            verts.append(tuple(pos) + tuple(-n for n in normal))
            uvs.append(tuple(uv))
        a, b, c, e = base, base + 1, base + 2, base + 3
        strip += [a, a, b, e, c, c]
    if not verts:
        return collapse(model, m, (0.0, 0.0, 0.0))
    while len(verts) < m.vertex_count:
        verts.append(verts[0])
        uvs.append(uvs[0])
    kind, count, offset = struct.unpack_from('>3I', d, 8 + m.strip_table)
    strip += [strip[-1]] * (count - len(strip))
    ptr, = struct.unpack_from('>I', d, 8 + m.vertex_block)
    for i, v in enumerate(verts):
        struct.pack_into('>6fI', d, 8 + ptr + i * 28, *(v + (0xFFFFFFFF,)))
        struct.pack_into('>2f', d, 8 + m.uv_block + i * 8, *uvs[i])
    struct.pack_into('>%dH' % count, d, 8 + offset, *strip)
    xs = [v[:3] for v in verts]
    lo = [min(p[k] for p in xs) for k in range(3)]
    hi = [max(p[k] for p in xs) for k in range(3)]
    centre = [(lo[k] + hi[k]) / 2 for k in range(3)]
    radius = max(sum((p[k] - centre[k]) ** 2 for k in range(3)) ** 0.5 for p in xs)
    struct.pack_into('>4f', d, m.offset + 0xA4, centre[0], centre[1], centre[2], radius + 0.05)


def collapse(model, m, point):
    """Hides a mesh: every vertex moves to one point (multi-weight meshes
    also have a single position array)."""
    d = model.data
    ptr, = struct.unpack_from('>I', d, 8 + m.vertex_block)
    for i in range(m.vertex_count):
        struct.pack_into('>3f', d, 8 + ptr + i * 28, *point)
    struct.pack_into('>4f', d, m.offset + 0xA4, point[0], point[1], point[2], 0.05)

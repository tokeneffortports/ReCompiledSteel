"""Writes Real Steel part models ('JBOY') from scratch: any number of meshes,
vertices, faces and bone weights. Shader settings, bones, texture names and
the object group are taken from a template model (a stock part).

    template = jboy.Model(open('ed01_MBBF_07D1.bin', 'rb').read())
    spec = MeshSpec(template.meshes[0])        # shader / material / group
    spec.bones = [3]; spec.weights = 1
    spec.vertices = [(x, y, z, nx, ny, nz), ...]
    spec.vertex_weights = [[(0, 1.0)], ...]     # per vertex: (bone slot, weight) x weights
    spec.uvs = [(u, v), ...]
    spec.strip = [i0, i1, i2, ...]              # one triangle strip
    data = write(template, [spec, ...])

Mesh fields: +0 vertex count, +8 number of bones in the mesh's list, +0x5C
weight entries per vertex (each u32 bone slot, f32 weight; unused entries
slot 0xFF, weight 0).

Layout (all offsets relative to byte 8): header, mesh table, then per mesh
its shader parameters, vertex block (pointer + 28-byte records), weight
block, uv block and strip; then bones, texture names, groups and the 'POF0'
pointer table. Reproduces the stock files byte for byte (round-trip test in
jboy_write.py's main).
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jboy

HEADER_SIZE = 0x40
MESH_SIZE = 0xB4
VERTEX_SIZE = 28


class MeshSpec:
    def __init__(self, template_mesh):
        self.template = template_mesh          # jboy.Mesh of the template model
        self.bones = list(template_mesh.bones)
        self.weights = template_mesh.unk5c           # weight entries per vertex (1..4)
        self.vertices = []                     # (x, y, z, nx, ny, nz[, colour])
        self.vertex_weights = []               # per vertex: list of (slot, weight)
        self.uvs = []
        self.strip = []
        self.group = template_mesh.group

    @classmethod
    def from_model(cls, model, mesh):
        """The mesh as it is in the model (for round trips and edits)."""
        spec = cls(mesh)
        spec.vertices = [tuple(v) for v in model.vertices(mesh)]
        d = model.data
        spec.vertex_weights = []
        for i in range(mesh.vertex_count):
            entry = []
            for w in range(mesh.unk5c):
                slot, weight = struct.unpack_from('>If', d, 8 + mesh.weight_block + (i * mesh.unk5c + w) * 8)
                entry.append((slot, weight))
            spec.vertex_weights.append(entry)
        spec.uvs = model.uvs(mesh)
        spec.strip = model.strip(mesh)
        return spec


def encode_pof0(positions):
    """Pointer positions (relative to byte 8) -> 'POF0' table bytes."""
    body = bytearray()
    last = 0
    for pos in sorted(positions):
        delta = (pos - last) // 4
        assert pos % 4 == 0 and delta >= 0
        if delta < 0x40:
            body.append(0x40 | delta)
        elif delta < 0x4000:
            body += bytes((0x80 | (delta >> 8), delta & 0xFF))
        else:
            body += bytes((0xC0 | (delta >> 24), (delta >> 16) & 0xFF, (delta >> 8) & 0xFF, delta & 0xFF))
        last = pos
    while len(body) % 4:
        body.append(0)
    return b'POF0' + struct.pack('>I', len(body)) + bytes(body)


def sphere_of(vertices):
    if not vertices:
        return (0.0, 0.0, 0.0, 0.05)
    lo = [min(v[k] for v in vertices) for k in range(3)]
    hi = [max(v[k] for v in vertices) for k in range(3)]
    centre = [(lo[k] + hi[k]) / 2 for k in range(3)]
    radius = max(sum((v[k] - centre[k]) ** 2 for k in range(3)) ** 0.5 for v in vertices)
    return (centre[0], centre[1], centre[2], radius)


def write(template, specs, bone_table=None, spheres=None):
    """template: jboy.Model. bone_table: replacement bytes for the bone
    table (e.g. scaled), default the template's. spheres: per mesh (x, y, z,
    r) or None to compute from the vertices."""
    td = template.data
    out = bytearray(HEADER_SIZE + MESH_SIZE * len(specs))
    pointers = [0x14, 0x20, 0x24, 0x28]
    out[0:HEADER_SIZE] = td[8:8 + HEADER_SIZE]
    struct.pack_into('>I', out, 0x10, 0x80000000 | len(specs))
    struct.pack_into('>I', out, 0x14, HEADER_SIZE)

    for i, spec in enumerate(specs):
        t = spec.template
        mo = HEADER_SIZE + i * MESH_SIZE
        out[mo:mo + MESH_SIZE] = td[t.offset:t.offset + MESH_SIZE]
        n = len(spec.vertices)
        assert len(spec.uvs) == n and len(spec.vertex_weights) == n, 'mesh %d: vertex, uv and weight counts differ' % i
        assert 1 <= spec.weights <= 4 and all(len(w) == spec.weights for w in spec.vertex_weights)
        assert 1 <= len(spec.bones) <= 20 and n < 65536
        struct.pack_into('>3I', out, mo, n, 1, len(spec.bones))
        struct.pack_into('>20I', out, mo + 0x0C, *(spec.bones + [0xFFFFFFFF] * (20 - len(spec.bones))))
        struct.pack_into('>I', out, mo + 0x5C, spec.weights)
        struct.pack_into('>I', out, mo + 0x74, spec.group)

        # shader parameters: the template's block, pointers moved
        param_table = len(out)
        region = td[8 + t.param_table:8 + t.vertex_block]
        delta = param_table - t.param_table
        out += region
        for k in range(t.param_count):
            p = struct.unpack_from('>I', td, 8 + t.param_table + k * 4)[0] + delta
            struct.pack_into('>I', out, param_table + k * 4, p)
            pointers.append(param_table + k * 4)
        struct.pack_into('>I', out, mo + 0x94, param_table)

        # vertices: pointer, then the records
        vertex_block = len(out)
        out += struct.pack('>I', vertex_block + 4)
        pointers.append(vertex_block)
        for v in spec.vertices:
            colour = v[6] if len(v) > 6 else 0xFFFFFFFF
            out += struct.pack('>6fI', *v[:6], colour)
        struct.pack_into('>I', out, mo + 0x68, vertex_block)

        weight_block = len(out)
        for entry in spec.vertex_weights:
            for slot, weight in entry:
                out += struct.pack('>If', slot, weight)
        struct.pack_into('>I', out, mo + 0x6C, weight_block)

        uv_block = len(out)
        for u, v in spec.uvs:
            out += struct.pack('>2f', u, v)
        struct.pack_into('>I', out, mo + 0x70, uv_block)

        strip_table = len(out)
        out += struct.pack('>3I', 6, len(spec.strip), strip_table + 12)
        pointers.append(strip_table + 8)
        out += struct.pack('>%dH' % len(spec.strip), *spec.strip)
        while len(out) % 4:
            out.append(0)
        struct.pack_into('>I', out, mo + 0x98, strip_table)
        struct.pack_into('>I', out, mo + 0x9C, n)
        sphere = spheres[i] if spheres and spheres[i] else sphere_of(spec.vertices)
        struct.pack_into('>4f', out, mo + 0xA4, *sphere)
        pointers += [mo + 0x68, mo + 0x6C, mo + 0x70, mo + 0x94, mo + 0x98]

    bones = bone_table if bone_table is not None else td[8 + template.bone_table:8 + template.bone_table + 0x50 * template.bone_count]
    while len(out) % 16:
        out.append(0)
    bone_table_off = len(out)
    out += bones
    struct.pack_into('>I', out, 0x20, bone_table_off)
    texture_table = len(out)
    out += td[8 + template.texture_table:8 + template.texture_table + 16 * template.texture_count]
    struct.pack_into('>I', out, 0x24, texture_table)
    group_table = len(out)
    groups = bytearray(td[8 + template.group_table:8 + template.group_table + 32 * template.group_count])
    if template.group_count == 1:
        struct.pack_into('>II', groups, 20, 0, len(specs))     # first mesh, mesh count
    out += groups
    struct.pack_into('>I', out, 0x28, group_table)
    fixups = len(out)
    out += encode_pof0(pointers)
    struct.pack_into('>I', out, 0x04, fixups)
    data = td[:8] + bytes(out)
    data = bytearray(data)
    struct.pack_into('>I', data, 4, fixups)
    struct.pack_into('>I', data, 0x0C, fixups)
    return bytes(data)


def main():
    """Round trip: rebuild stock models from their parsed data and compare."""
    for path in sys.argv[1:]:
        raw = open(path, 'rb').read()
        model = jboy.Model(raw)
        specs = [MeshSpec.from_model(model, m) for m in model.meshes]
        spheres = [m.sphere for m in model.meshes]
        out = write(model, specs, spheres=spheres)
        if out == raw:
            print('%s: identical' % os.path.basename(path))
        else:
            first = next((i for i in range(min(len(out), len(raw))) if out[i] != raw[i]), min(len(out), len(raw)))
            print('%s: DIFFERS at %#x (sizes %d vs %d)' % (os.path.basename(path), first, len(out), len(raw)))


if __name__ == '__main__':
    main()

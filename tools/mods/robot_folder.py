"""A robot as a folder of ordinary files: one OBJ model and PNG textures per
part. This is the editable form of a mod robot - export it, tidy it up in
any 3D tool, build it back into game files.

  robot_folder.py export <template dir> <folder>     COMET -> OBJ / PNG / FORMAT.md
  robot_folder.py check  <template dir> <folder>     validate a folder
  robot_folder.py build  <template dir> <folder> <out dir>
                                                     folder -> game models and
                                                     textures (then pass <out dir>
                                                     to the game's mod loader)

<template dir> holds the part set the models are written into (see
comet_robot.py). The rules for the folder are in the FORMAT.md that export
writes next to the files.
"""
import os, struct, sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comet_robot, jboy, jboy_write, pngio

# part file name -> (slot, {group name: skeleton bone})
PARTS = {
    'body': (1, {'chest': 3, 'abdomen': 2, 'hips': 1}),
    'legs': (2, {'thigh_l': 26, 'shin_l': 27, 'foot_l': 28, 'toe_l': 29,
                 'thigh_r': 30, 'shin_r': 31, 'foot_r': 32, 'toe_r': 33}),
    'head': (3, {'head': 5}),
    'arm_l': (4, {'upper': 11, 'forearm': 13}),
    'arm_r': (5, {'upper': 19, 'forearm': 21}),
    'fist_l': (6, {'fist': 14}),
    'fist_r': (7, {'fist': 22}),
}


# The folder's normal maps are the usual kind (green = up the picture); the
# game reads green the other way (checked in game with a map of domes), so
# build turns it over. ROBOT_NORMAL_GREEN=asis is for experiments.
NORMAL_GREEN_AS_IS = os.environ.get('ROBOT_NORMAL_GREEN') == 'asis'
# ROBOT_TEXTURES=full writes the normal / occlusion / glow maps as large as
# they were drawn instead of the stock half / quarter size. THE GAME CRASHES
# ON THEM (tried 2026-10-05: guest access violation while loading - it copies
# every part's maps into shared atlases of a fixed size), so the stock sizes
# are a hard limit; the switch only remains to repeat that experiment.
FULL_SIZE_MAPS = os.environ.get('ROBOT_TEXTURES') == 'full'


def tga(template, w, h, pixels, palette=None):
    """A TGA file like the template (the stock texture) but w x h."""
    head = bytearray(template[:18])
    struct.pack_into('<HH', head, 12, w, h)
    if palette is not None:
        return bytes(head) + palette + pixels
    old_w, old_h = struct.unpack_from('<HH', template, 12)
    return bytes(head) + pixels + template[18 + old_w * old_h * 4:]


def template_model(template_dir, slot):
    prefix = comet_robot.SLOTS[slot][0]
    return jboy.Model(open(comet_robot.find(template_dir, prefix, 'M', 2000 + slot), 'rb').read())


def budgets(template_dir, slot):
    """bone -> face limit. Models are written from scratch now, so the only
    limit is what the game renders comfortably (a guess at a sane ceiling)."""
    model = template_model(template_dir, slot)
    return {m.bones[0]: 20000 for m in model.meshes if len(m.bones) == 1}


def texture_size(template_dir, slot, base=1000):
    prefix = comet_robot.SLOTS[slot][0]
    data = open(comet_robot.find(template_dir, prefix, 'T', base + slot - 1), 'rb').read()
    return struct.unpack_from('<HH', data, 12)


# ---------------------------------------------------------------------------
# export
# ---------------------------------------------------------------------------
def export(template_dir, folder):
    os.makedirs(folder, exist_ok=True)
    rows = []
    for name, (slot, groups) in PARTS.items():
        prefix, make, rect = comet_robot.SLOTS[slot]
        part = make()
        w, h = texture_size(template_dir, slot)
        palette, image, glow = comet_robot.paint_textures(w, h, slot)
        rgb = np.frombuffer(palette, dtype=np.uint8).reshape(256, 4)[:, [2, 1, 0]][image]
        pngio.write_png(os.path.join(folder, name + '_diffuse.png'), rgb)
        pngio.write_png(os.path.join(folder, name + '_glow.png'), np.repeat(glow[:, :, None], 3, axis=2))
        with open(os.path.join(folder, name + '.mtl'), 'w') as f:
            f.write('newmtl %s\nKd 1 1 1\nmap_Kd %s_diffuse.png\n' % (name, name))
        bone_names = {bone: group for group, bone in groups.items()}
        budget = budgets(template_dir, slot)
        lines = ['# COMET %s - part space: +x = robot\'s left, +y = DOWN, front = -z' % name,
                 'mtllib %s.mtl' % name, 'usemtl %s' % name]
        count = 0
        for bone, quads in part.quads.items():
            lines.append('g %s' % bone_names[bone])
            for quad in quads:
                for pos, normal, (s, t, swatch) in quad:
                    cx, cy = comet_robot.cell_of(swatch)
                    lu = (cx + 0.004 + s * 0.992) / comet_robot.GRID
                    lv = (cy + 0.004 + t * 0.992) / comet_robot.GRID
                    lines.append('v %.5f %.5f %.5f' % tuple(pos))
                    lines.append('vt %.5f %.5f' % (lu, 1 - lv))
                    lines.append('vn %.5f %.5f %.5f' % tuple(normal))
                lines.append('f ' + ' '.join('%d/%d/%d' % (count + k, count + k, count + k) for k in (1, 2, 3, 4)))
                count += 4
            rows.append((name, bone_names[bone], len(quads), budget[bone]))
        with open(os.path.join(folder, name + '.obj'), 'w') as f:
            f.write('\n'.join(lines) + '\n')
        rows.append((name, 'texture', '%d x %d' % (w, h), ''))
    # the whole robot roughly assembled, to look at only (never read back)
    from robot_kit import ASSEMBLY as offsets                # the rest pose, from the template skeleton
    lines, base = ['# VIEW ONLY: all parts placed roughly together. Edit the per-part files instead.'], 0
    for name in PARTS:
        lines += ['mtllib %s.mtl' % name, 'usemtl %s' % name, 'g ' + name]
        count = 0
        for line in open(os.path.join(folder, name + '.obj')):
            t = line.split()
            if t and t[0] == 'v':
                lines.append('v %.5f %.5f %.5f' % tuple(float(t[k + 1]) + offsets[name][k] for k in range(3)))
                count += 1
            elif t and t[0] in ('vt', 'vn'):
                lines.append(line.strip())
            elif t and t[0] == 'f':
                lines.append('f ' + ' '.join('/'.join(str(int(i) + base) for i in c.split('/')) for c in t[1:]))
        base += count
    with open(os.path.join(folder, 'reference_assembled.obj'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return rows


# ---------------------------------------------------------------------------
# import
# ---------------------------------------------------------------------------
def read_obj(path, groups):
    """Returns {bone: [face]}; a face is 3 or 4 corners (pos, normal, (u, v))
    with v measured from the top of the texture."""
    verts, uvs, normals = [], [], []
    faces = {}
    group = None
    problems = []
    for number, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
        parts = line.split()
        if not parts or parts[0].startswith('#'):
            continue
        if parts[0] == 'v':
            verts.append(tuple(float(x) for x in parts[1:4]))
        elif parts[0] == 'vt':
            uvs.append((float(parts[1]), float(parts[2])))
        elif parts[0] == 'vn':
            normals.append(tuple(float(x) for x in parts[1:4]))
        elif parts[0] in ('g', 'o'):
            group = parts[1].lower() if len(parts) > 1 else None
            # tolerate exporter decorations such as "chest_Mesh.001"
            match = [g for g in groups if group == g or (group or '').startswith(g + '_') or (group or '').startswith(g + '.')]
            group = max(match, key=len) if match else group
        elif parts[0] == 'f':
            if group not in groups:
                problems.append('line %d: face in group %r - expected one of %s' % (number, group, ', '.join(groups)))
                continue
            corners = []
            for token in parts[1:]:
                idx = (token.split('/') + ['', ''])[:3]
                vi = int(idx[0])
                pos = verts[vi - 1 if vi > 0 else vi]
                if not idx[1]:
                    problems.append('line %d: face corner without a texture coordinate' % number)
                    break
                ti = int(idx[1])
                uv = uvs[ti - 1 if ti > 0 else ti]
                normal = None
                if idx[2]:
                    ni = int(idx[2])
                    normal = normals[ni - 1 if ni > 0 else ni]
                corners.append((pos, normal, uv))
            else:
                p = np.array([c[0] for c in corners])
                face_normal = np.cross(p[1] - p[0], p[2] - p[0])
                length = np.linalg.norm(face_normal)
                if length < 1e-12:
                    continue                                   # degenerate face: nothing to draw
                face_normal = tuple(face_normal / length)
                fixed = []
                for pos, normal, (u, v) in corners:
                    if not (-0.001 <= u <= 1.001 and -0.001 <= v <= 1.001):
                        problems.append('line %d: texture coordinate %.3f, %.3f outside 0..1' % (number, u, v))
                    fixed.append((pos, normal or face_normal, (min(1.0, max(0.0, u)), 1 - min(1.0, max(0.0, v)))))
                bone = groups[group]
                if len(fixed) <= 4:
                    faces.setdefault(bone, []).append(fixed)
                else:                                          # n-gon: fan of triangles
                    for k in range(1, len(fixed) - 1):
                        faces.setdefault(bone, []).append([fixed[0], fixed[k], fixed[k + 1]])
    return faces, problems[:20]


def load_folder(template_dir, folder):
    parts, problems = {}, []
    for name, (slot, groups) in PARTS.items():
        path = os.path.join(folder, name + '.obj')
        if not os.path.isfile(path):
            problems.append('%s.obj is missing' % name)
            continue
        faces, bad = read_obj(path, groups)
        problems += ['%s.obj %s' % (name, b) for b in bad]
        budget = budgets(template_dir, slot)
        bone_names = {bone: group for group, bone in groups.items()}
        for bone, items in faces.items():
            if len(items) > budget[bone]:
                problems.append('%s.obj group %s has %d faces, the limit is %d' % (name, bone_names[bone], len(items), budget[bone]))
        if not faces:
            problems.append('%s.obj has no faces' % name)
        if not os.path.isfile(os.path.join(folder, name + '_diffuse.png')):
            problems.append('%s_diffuse.png is missing' % name)
        parts[name] = faces
    return parts, problems


def skeleton_scale(folder):
    """skeleton_scale.txt: the bone tables of the part models (the lengths
    of the limbs, so the size of the whole robot) are multiplied by it."""
    path = os.path.join(folder, 'skeleton_scale.txt')
    return float(open(path).read().strip()) if os.path.isfile(path) else 1.0


def scale_bones(model, factor):
    for i in range(model.bone_count):
        o = 8 + model.bone_table + i * 0x50 + 16
        x, y, z = struct.unpack_from('>3f', model.data, o)
        struct.pack_into('>3f', model.data, o, x * factor, y * factor, z * factor)


def build(template_dir, folder, out_dir):
    parts, problems = load_folder(template_dir, folder)
    if problems:
        raise SystemExit('cannot build:\n  ' + '\n  '.join(problems))
    os.makedirs(out_dir, exist_ok=True)
    bones = skeleton_scale(folder)
    for name, (slot, groups) in PARTS.items():
        prefix, _, rect = comet_robot.SLOTS[slot]
        template = comet_robot.find(template_dir, prefix, 'M', 2000 + slot)
        model = jboy.Model(open(template, 'rb').read())
        # one mesh per bone group, written from scratch (jboy_write): the
        # shader settings come from the template's mesh on the same bone.
        # The game's own models store normals pointing INTO the surface and
        # wind their strips to match, so both are turned round here.
        specs = []
        for bone, faces in parts[name].items():
            source = next((m for m in model.meshes if m.bones == [bone] and m.unk5c == 1), None) or model.meshes[0]
            spec = None
            for face in faces:
                corners = [(pos, normal, (rect[0] + u * rect[2], rect[1] + v * rect[3])) for pos, normal, (u, v) in face]
                if len(corners) == 3:
                    corners.append(corners[2])                 # a triangle is a quad with a repeated corner
                if spec is None or len(spec.vertices) > 65000:
                    spec = jboy_write.MeshSpec(source)
                    spec.bones, spec.weights = [bone], 1
                    specs.append(spec)
                base = len(spec.vertices)
                for pos, normal, uv in corners:
                    spec.vertices.append(tuple(pos) + tuple(-n for n in normal))
                    spec.vertex_weights.append([(0, 1.0)])
                    spec.uvs.append(tuple(uv))
                a, b, c, e = base, base + 1, base + 2, base + 3
                spec.strip += [a, a, b, e, c, c]
        bone_table = bytearray(model.data[8 + model.bone_table:8 + model.bone_table + 0x50 * model.bone_count])
        if bones != 1.0:
            for i in range(model.bone_count):
                x, y, z = struct.unpack_from('>3f', bone_table, i * 0x50 + 16)
                struct.pack_into('>3f', bone_table, i * 0x50 + 16, x * bones, y * bones, z * bones)
        with open(os.path.join(out_dir, 'ed%02d_M_%04X.bin' % (slot, 2000 + slot)), 'wb') as f:
            f.write(jboy_write.write(model, specs, bone_table=bytes(bone_table)))
        # the part's joint records (the 0x961 series next to the model):
        # 20-byte entries (joint number, bone, x, y, z) saying where the
        # neighbouring parts plug in - the sockets move with the skeleton
        path = comet_robot.find(template_dir, prefix, 'M', 2400 + slot)
        data = bytearray(open(path, 'rb').read())
        for o in range(0, len(data) - 19, 20):
            x, y, z = struct.unpack_from('<3f', data, o + 8)
            struct.pack_into('<3f', data, o + 8, x * bones, y * bones, z * bones)
        with open(os.path.join(out_dir, 'ed%02d_M_%04X.bin' % (slot, 2400 + slot)), 'wb') as f:
            f.write(data)

        diffuse = pngio.read_png(os.path.join(folder, name + '_diffuse.png'))
        optional = {}
        for role in ('glow', 'normal', 'gloss', 'ao'):
            png = os.path.join(folder, '%s_%s.png' % (name, role))
            optional[role] = pngio.read_png(png) if os.path.isfile(png) else None
        glow = optional['glow']
        for base, role in ((1000, 'diffuse'), (1100, 'normal'), (1400, 'ao'), (1600, 'glow')):
            path = comet_robot.find(template_dir, prefix, 'T', base + slot - 1)
            template = open(path, 'rb').read()
            w, h = struct.unpack_from('<HH', template, 12)
            source = diffuse if role == 'diffuse' else optional[role]
            if FULL_SIZE_MAPS and source is not None:
                h, w = source.shape[:2]                        # as drawn, not cut down to the stock size
            if role == 'diffuse':
                palette, index = pngio.quantize(pngio.resize_nearest(diffuse, w, h))
                table = bytearray(1024)
                for i, (r, g, b) in enumerate(palette.tolist()):
                    table[i * 4:i * 4 + 4] = bytes((b, g, r, 255))
                data = tga(template, w, h, index[::-1].tobytes(), bytes(table))
            elif role == 'ao':
                shade = bytes([255]) * (w * h)
                if optional['ao'] is not None:
                    shade = pngio.resize(optional['ao'], w, h)[::-1, :, 0].tobytes()
                data = tga(template, w, h, shade, b''.join(bytes((i, i, i, 255)) for i in range(256)))
            elif role == 'normal':
                # blue green red = the normal's z y x, alpha = gloss
                px = np.empty((h, w, 4), dtype=np.uint8)
                px[:] = (0xFF, 0x80, 0x80, 0x50)
                if optional['normal'] is not None:
                    n = pngio.resize(optional['normal'], w, h)[::-1]
                    px[:, :, 0], px[:, :, 2] = n[:, :, 2], n[:, :, 0]
                    px[:, :, 1] = n[:, :, 1] if NORMAL_GREEN_AS_IS else 255 - n[:, :, 1]
                if optional['gloss'] is not None:
                    px[:, :, 3] = pngio.resize(optional['gloss'], w, h)[::-1, :, 0]
                data = tga(template, w, h, px.tobytes())
            else:
                px = np.zeros((h, w, 4), dtype=np.uint8)
                if glow is not None:
                    px[:, :, 2] = pngio.resize(glow, w, h)[::-1].max(axis=2)
                data = tga(template, w, h, px.tobytes())
            with open(os.path.join(out_dir, 'ed%02d_T_%04X.bin' % (slot, base + slot - 1)), 'wb') as f:
                f.write(data)
        print('%-7s %4d faces' % (name, sum(len(f) for f in parts[name].values())))


def main():
    cmd = sys.argv[1]
    if cmd == 'export':
        for row in export(sys.argv[2], sys.argv[3]):
            print('%-7s %-10s %8s %s' % row)
    elif cmd == 'check':
        _, problems = load_folder(sys.argv[2], sys.argv[3])
        print('\n'.join(problems) if problems else 'folder is valid')
        sys.exit(1 if problems else 0)
    elif cmd == 'build':
        build(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()

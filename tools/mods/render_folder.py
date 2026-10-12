"""Renders a robot folder (the OBJ + PNG files of robot_folder.py) without the
game: the parts in their rest positions, textured, lit with the normal /
gloss / occlusion maps - to judge models and textures before a game test.

  render_folder.py <folder> out.png [view ...] [--size 640x800] [--pose rest]

views: hero front back side bust legs arm top (default: hero front back).
Not the game's shader - a plain key / fill / rim light - but the same maps
with the same meaning.
"""
import math, os, sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngio
from robot_kit import ASSEMBLY, PARTS

# name -> (eye, target, field of view in degrees)
VIEWS = {
    'hero': ((10.0, 7.5, -36.0), (0.0, -1.2, 0.0), 37),
    'front': ((0.0, -1.0, -70.0), (0.0, -0.6, 0.0), 23),
    'back': ((0.0, -1.0, 70.0), (0.0, -0.6, 0.0), 23),
    'side': ((70.0, -1.0, 0.0), (0.0, -0.6, 0.0), 23),
    'bust': ((6.0, -9.5, -19.0), (0.3, -8.6, 0.0), 40),
    'legs': ((7.0, 5.0, -21.0), (0.0, 5.6, 0.0), 40),
    'arm': ((17.0, -4.0, -14.0), (6.3, -3.0, 0.0), 40),
    'top': ((0.0, -40.0, -22.0), (0.0, -3.0, 0.0), 30),
    'rear': ((-12.0, -6.0, 30.0), (0.0, -2.0, 0.0), 40),
}
SHOULDER = np.array((5.25, -7.58, 0.15))


def load(folder, pose='a'):
    """Triangles of all parts in the body space, plus the textures."""
    tris, uvs, nrms, ids, textures = [], [], [], [], []
    path = os.path.join(folder, 'skeleton_scale.txt')
    world = float(open(path).read()) if os.path.isfile(path) else 1.0
    for index, name in enumerate(PARTS):
        v, vt, vn = [], [], []
        faces = []
        for line in open(os.path.join(folder, name + '.obj')):
            t = line.split()
            if not t:
                continue
            if t[0] == 'v':
                v.append([float(x) for x in t[1:4]])
            elif t[0] == 'vt':
                vt.append([float(t[1]), 1 - float(t[2])])
            elif t[0] == 'vn':
                vn.append([float(x) for x in t[1:4]])
            elif t[0] == 'f':
                c = [[int(i) - 1 for i in tok.split('/')] for tok in t[1:]]
                for k in range(1, len(c) - 1):
                    faces.append((c[0], c[k], c[k + 1]))
        v, vt, vn = np.array(v) + np.array(ASSEMBLY[name]) * world, np.array(vt), np.array(vn)
        if pose == 'a' and name[:3] in ('arm', 'fis'):       # the game's idle stance: arms a little out
            side = 1 if name.endswith('_l') else -1
            ang = -side * math.radians(11)
            pivot = SHOULDER * (side, 1, 1) * world
            rot = np.array([[math.cos(ang), -math.sin(ang), 0], [math.sin(ang), math.cos(ang), 0], [0, 0, 1]])
            v = (v - pivot) @ rot.T + pivot
            vn = vn @ rot.T
        for f in faces:
            tris.append([v[c[0]] for c in f])
            uvs.append([vt[c[1]] for c in f])
            nrms.append([vn[c[2]] for c in f])
            ids.append(index)
        maps = {}
        for role in ('diffuse', 'normal', 'gloss', 'ao', 'glow'):
            path = os.path.join(folder, '%s_%s.png' % (name, role))
            maps[role] = pngio.read_png(path).astype(np.float32) / 255 if os.path.isfile(path) else None
        textures.append(maps)
    return np.array(tris), np.array(uvs), np.array(nrms), np.array(ids), textures


def sample(tex, uv):
    """Bilinear lookup; uv in 0..1, v down."""
    h, w = tex.shape[:2]
    x = np.clip(uv[:, 0] * w - 0.5, 0, w - 1.001)
    y = np.clip(uv[:, 1] * h - 0.5, 0, h - 1.001)
    x0, y0 = x.astype(int), y.astype(int)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    return (tex[y0, x0] * (1 - fx) + tex[y0, x0 + 1] * fx) * (1 - fy) + (tex[y0 + 1, x0] * (1 - fx) + tex[y0 + 1, x0 + 1] * fx) * fy


def render(scene, view, width, height, flat=False):
    tris, uvs, nrms, ids, textures = scene
    eye, target, fov = (np.array(view[0], dtype=float), np.array(view[1], dtype=float), view[2])
    fwd = (target - eye) / np.linalg.norm(target - eye)
    right = np.cross(fwd, (0.0, -1.0, 0.0))
    right /= np.linalg.norm(right)
    up = np.cross(right, fwd)
    focal = height / 2 / math.tan(math.radians(fov) / 2)
    rel = tris - eye
    depth = rel @ fwd
    sx = (rel @ right) / depth * focal + width / 2
    sy = -(rel @ up) / depth * focal + height / 2
    face_n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    visible = ((face_n * rel[:, 0]).sum(axis=1) < 0) & (depth.min(axis=1) > 0.5)

    best = np.zeros((height, width), dtype=np.float32)
    which = np.full((height, width), -1, dtype=np.int32)
    bary = np.zeros((height, width, 2), dtype=np.float32)
    for t in np.nonzero(visible)[0]:
        xs, ys, zs = sx[t], sy[t], depth[t]
        x0, x1 = int(max(0, math.floor(xs.min()))), int(min(width - 1, math.ceil(xs.max())))
        y0, y1 = int(max(0, math.floor(ys.min()))), int(min(height - 1, math.ceil(ys.max())))
        if x1 < x0 or y1 < y0:
            continue
        den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
        if abs(den) < 1e-9:
            continue
        px, py = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((ys[1] - ys[2]) * (px - xs[2]) + (xs[2] - xs[1]) * (py - ys[2])) / den
        w1 = ((ys[2] - ys[0]) * (px - xs[2]) + (xs[0] - xs[2]) * (py - ys[2])) / den
        w2 = 1 - w0 - w1
        a, b, c = w0 / zs[0], w1 / zs[1], w2 / zs[2]
        inv = a + b + c
        region = best[y0:y1 + 1, x0:x1 + 1]
        win = (w0 >= 0) & (w1 >= 0) & (w2 >= 0) & (inv > region)
        if not win.any():
            continue
        region[win] = inv[win]
        which[y0:y1 + 1, x0:x1 + 1][win] = t
        bary[y0:y1 + 1, x0:x1 + 1][win] = np.stack([a[win] / inv[win], b[win] / inv[win]], axis=1)

    # background: a dim studio gradient
    ramp = np.linspace(0, 1, height)[:, None]
    img = np.empty((height, width, 3), dtype=np.float32)
    img[:] = (np.array((0.035, 0.04, 0.05)) * (1 - ramp) + np.array((0.11, 0.105, 0.1)) * ramp)[:, None, :]
    hit = which >= 0
    t = which[hit]
    b0, b1 = bary[hit][:, 0:1], bary[hit][:, 1:2]
    b2 = 1 - b0 - b1
    pos = tris[t, 0] * b0 + tris[t, 1] * b1 + tris[t, 2] * b2
    uv = uvs[t, 0] * b0 + uvs[t, 1] * b1 + uvs[t, 2] * b2
    n = nrms[t, 0] * b0 + nrms[t, 1] * b1 + nrms[t, 2] * b2
    n /= np.linalg.norm(n, axis=1, keepdims=True)
    # tangent frame of each triangle from its texture coordinates (u right, v down)
    dp1, dp2 = tris[t, 1] - tris[t, 0], tris[t, 2] - tris[t, 0]
    du1, dv1 = (uvs[t, 1] - uvs[t, 0]).T
    du2, dv2 = (uvs[t, 2] - uvs[t, 0]).T
    det = du1 * dv2 - du2 * dv1
    det = np.where(np.abs(det) < 1e-12, 1e-12, det)
    tan = (dp1 * dv2[:, None] - dp2 * dv1[:, None]) / det[:, None]
    bit = -(dp2 * du1[:, None] - dp1 * du2[:, None]) / det[:, None]          # up the texture
    tan -= n * (tan * n).sum(axis=1, keepdims=True)
    tan /= np.maximum(np.linalg.norm(tan, axis=1, keepdims=True), 1e-9)
    bit -= n * (bit * n).sum(axis=1, keepdims=True)
    bit /= np.maximum(np.linalg.norm(bit, axis=1, keepdims=True), 1e-9)

    albedo = np.full((len(t), 3), 0.6, dtype=np.float32)
    gloss = np.full((len(t), 1), 0.3, dtype=np.float32)
    ao = np.ones((len(t), 1), dtype=np.float32)
    glow = np.zeros((len(t), 1), dtype=np.float32)
    shade_n = n.copy()
    part = ids[t]
    for index, maps in enumerate(textures):
        m = part == index
        if not m.any():
            continue
        if maps['diffuse'] is not None and not flat:
            albedo[m] = sample(maps['diffuse'], uv[m])
        if maps['normal'] is not None:
            nm = sample(maps['normal'], uv[m]) * 2 - 1
            shade_n[m] = tan[m] * nm[:, 0:1] + bit[m] * nm[:, 1:2] + n[m] * nm[:, 2:3]
        if maps['gloss'] is not None:
            gloss[m] = sample(maps['gloss'], uv[m])[:, 0:1]
        if maps['ao'] is not None:
            ao[m] = sample(maps['ao'], uv[m])[:, 0:1]
        if maps['glow'] is not None:
            glow[m] = sample(maps['glow'], uv[m])[:, 0:1]
    shade_n /= np.linalg.norm(shade_n, axis=1, keepdims=True)
    albedo = albedo ** 2.2
    to_eye = eye - pos
    to_eye /= np.linalg.norm(to_eye, axis=1, keepdims=True)
    light = albedo * (0.16 + 0.2 * np.clip(-shade_n[:, 1:2], 0, 1)) * ao * np.array((0.85, 0.92, 1.0))
    spec = 0.0
    for direction, colour, power in (((-0.55, -0.75, -0.6), (1.0, 0.95, 0.86), 2.3), ((0.8, -0.15, -0.5), (0.55, 0.65, 0.8), 0.5),
                                     ((0.35, -0.5, 0.8), (0.9, 0.9, 1.0), 0.7)):
        d = np.array(direction) / np.linalg.norm(direction)
        lam = np.clip((shade_n * d).sum(axis=1, keepdims=True), 0, 1) * power * (0.45 + 0.55 * ao)
        light = light + albedo * lam * np.array(colour)
        half = d + to_eye
        half /= np.linalg.norm(half, axis=1, keepdims=True)
        spec = spec + np.clip((shade_n * half).sum(axis=1, keepdims=True), 0, 1) ** (5 + 110 * gloss) * gloss * power * 0.5 * ao * np.array(colour)
    colour = light + spec + glow * np.array((0.35, 1.0, 0.6)) * 1.6
    img[hit] = np.clip(colour, 0, 1) ** (1 / 2.2)
    return img


def main():
    args = sys.argv[1:]
    size, pose, flat = (640, 800), 'a', False
    for flag in ('--size', '--pose'):
        if flag in args:
            i = args.index(flag)
            value = args[i + 1]
            del args[i:i + 2]
            if flag == '--size':
                size = tuple(int(x) for x in value.split('x'))
            else:
                pose = value
    if '--flat' in args:
        args.remove('--flat')
        flat = True
    folder, out = args[0], args[1]
    views = args[2:] or ['hero', 'front', 'back']
    scene = load(folder, pose)
    shots = []
    for name in views:
        big = render(scene, VIEWS[name], size[0] * 2, size[1] * 2, flat)
        shots.append(big.reshape(size[1], 2, size[0], 2, 3).mean(axis=(1, 3)))          # 2 x 2 anti-aliasing
    pngio.write_png(out, np.clip(np.rint(np.concatenate(shots, axis=1) * 255), 0, 255).astype(np.uint8))


if __name__ == '__main__':
    main()

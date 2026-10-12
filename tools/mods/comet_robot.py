"""COMET - a custom robot for Real Steel: a blocky white / teal / red / yellow
mobile-suit style machine with a V-fin head, chest vents and a backpack.

Generates the seven part models (body, legs, head, arms, fists) and their
textures from code. The models are written into copies of an existing part
set's models (same skeleton, same mesh slots - see jboy.py), the textures
replace that set's textures.

  comet_robot.py <template dir> <out dir>      template: files as written by
                                               rs_renders/jboy/dump1.py
  comet_robot.py --preview out.png <template dir>

Part spaces (from the template models): x = the robot's left (+) / right,
y = down, front = -z. Body and legs share one space with the hips at the
origin; head, each arm and each fist have their own.
"""
import os, random, struct, sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jboy

# ---------------------------------------------------------------------------
# Colour swatches: every face of the model maps onto one cell of a 4 x 4 grid
# in the part's texture. A cell is a flat colour with an inked outline and
# some wear, which gives each panel its own drawn edge.
# ---------------------------------------------------------------------------
WHITE, TEAL, RED, YELLOW, DARK, GREY, EYE, VENT, BLACK, SENSOR, THRUST, WHITE2 = range(12)
SWATCHES = [
    ('white', (226, 228, 216), False),
    ('teal', (62, 118, 132), False),
    ('red', (196, 66, 78), False),
    ('yellow', (238, 172, 48), False),
    ('dark', (62, 68, 76), False),
    ('grey', (136, 144, 148), False),
    ('eye', (120, 255, 170), True),
    ('vent', (238, 172, 48), False),
    ('black', (26, 28, 34), False),
    ('sensor', (255, 90, 80), True),
    ('thruster', (255, 190, 90), True),
    ('white2', (198, 204, 198), False),
]
GRID = 4
SHADES = 16


def cell_of(swatch):
    return swatch % GRID, swatch // GRID


def paint_textures(width, height, seed):
    """Returns (palette bytes BGRA x 256, index image h x w, glow h x w)."""
    rng = random.Random(seed)
    palette = bytearray(1024)
    for s, (_, rgb, _) in enumerate(SWATCHES):
        for shade in range(SHADES):
            f = 0.22 + 0.78 * shade / (SHADES - 1)
            i = (s * SHADES + shade) * 4
            palette[i:i + 4] = bytes((int(rgb[2] * f), int(rgb[1] * f), int(rgb[0] * f), 255))
    image = np.zeros((height, width), dtype=np.uint8)
    glow = np.zeros((height, width), dtype=np.uint8)
    cw, ch = width // GRID, height // GRID
    ys, xs = np.mgrid[0:ch, 0:cw]
    u, v = (xs + 0.5) / cw, (ys + 0.5) / ch
    edge = np.minimum(np.minimum(u, 1 - u), np.minimum(v, 1 - v))
    for s, (name, _, glows) in enumerate(SWATCHES):
        shade = np.full((ch, cw), 14.0)
        shade -= 1.2 * v                                   # a little darker towards the bottom
        shade[edge < 0.10] -= 2.5                          # soft shadow next to the outline
        shade[edge < 0.05] = 2.0                           # inked outline
        if name == 'vent':
            slat = ((v * 5) % 1.0) < 0.45
            shade[slat & (edge >= 0.05)] = 3.0
        if not glows:
            for _ in range(int(cw * ch / 900)):            # scratches and chips
                x, y = rng.randrange(cw), rng.randrange(ch)
                length = rng.randrange(3, max(4, cw // 10))
                dx, dy = rng.choice(((1, 0), (0, 1), (1, 1), (1, -1)))
                for k in range(length):
                    px, py = x + dx * k, y + dy * k
                    if 0 <= px < cw and 0 <= py < ch:
                        shade[py, px] -= rng.choice((2, 3, 4))
            shade += np.array([[rng.uniform(-0.6, 0.6) for _ in range(cw)] for _ in range(ch)])
        cx, cy = cell_of(s)
        block = (s * SHADES + np.clip(np.rint(shade), 0, SHADES - 1)).astype(np.uint8)
        image[cy * ch:(cy + 1) * ch, cx * cw:(cx + 1) * cw] = block
        if glows:
            glow[cy * ch:(cy + 1) * ch, cx * cw:(cx + 1) * cw] = np.where(edge >= 0.05, 255, 0)
    return bytes(palette), image, glow


# ---------------------------------------------------------------------------
# Modelling kit: everything is built from six-sided solids.
# ---------------------------------------------------------------------------
class Part:
    def __init__(self):
        self.quads = {}        # bone -> list of quads (4 corners: position, normal, cell uv)

    def hexa(self, bone, corners, swatch, mirror=False):
        """corners: 8 points, a ring of 4 then the opposite ring of 4."""
        pts = np.array(corners, dtype=float)
        if mirror:
            pts[:, 0] = -pts[:, 0]
        centre = pts.mean(axis=0)
        for face in ((0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
            p = pts[list(face)]
            normal = np.cross(p[1] - p[0], p[2] - p[0])
            if np.linalg.norm(normal) < 1e-9:
                normal = np.cross(p[2] - p[0], p[3] - p[0])
            if np.dot(normal, p.mean(axis=0) - centre) < 0:
                p = p[::-1]
                normal = -normal
            normal = normal / np.linalg.norm(normal)
            # texture axes: v follows the model's down direction where it can
            major = int(np.argmax(np.abs(normal)))
            ua, va = ((0, 2) if major == 1 else ((2, 1) if major == 0 else (0, 1)))
            lo, hi = p.min(axis=0), p.max(axis=0)
            span = np.maximum(hi - lo, 1e-6)
            quad = []
            for q in p:
                quad.append((tuple(q), tuple(normal), ((q[ua] - lo[ua]) / span[ua], (q[va] - lo[va]) / span[va], swatch)))
            self.quads.setdefault(bone, []).append(quad)

    def box(self, bone, x, y, z, swatch, mirror=False, top=None):
        """Axis-aligned box x=(x0, x1) y=(y0, y1) z=(z0, z1). top=(x0, x1,
        z0, z1) gives the upper (y0) face different extents (a taper)."""
        tx0, tx1, tz0, tz1 = top if top else (x[0], x[1], z[0], z[1])
        self.hexa(bone, [(tx0, y[0], tz0), (tx1, y[0], tz0), (tx1, y[0], tz1), (tx0, y[0], tz1),
                         (x[0], y[1], z[0]), (x[1], y[1], z[0]), (x[1], y[1], z[1]), (x[0], y[1], z[1])],
                  swatch, mirror)

    def both(self, bones, x, y, z, swatch, top=None):
        """The same box on the left (+x) with bones[0] and mirrored with bones[1]."""
        self.box(bones[0], x, y, z, swatch, False, top)
        self.box(bones[1], x, y, z, swatch, True, top)

    def beam(self, bone, a, b, width, depth, swatch, mirror=False):
        """A bar from point a to point b (in the x/y plane), width across,
        depth along z."""
        a, b = np.array(a, dtype=float), np.array(b, dtype=float)
        along = (b - a)[:2]
        side = np.array([-along[1], along[0]]) / np.linalg.norm(along) * width / 2
        ring = lambda p: [(p[0] - side[0], p[1] - side[1], p[2] - depth / 2), (p[0] + side[0], p[1] + side[1], p[2] - depth / 2),
                          (p[0] + side[0], p[1] + side[1], p[2] + depth / 2), (p[0] - side[0], p[1] - side[1], p[2] + depth / 2)]
        self.hexa(bone, ring(a) + ring(b), swatch, mirror)


def body():
    p = Part()
    CHEST, BELLY, HIP = 3, 2, 1
    # chest block, wider at the shoulders
    p.box(CHEST, (-3.0, 3.0), (-10.0, -6.3), (-3.1, 2.0), TEAL, top=(-3.5, 3.5, -2.5, 2.0))
    p.box(CHEST, (-1.5, 1.5), (-10.7, -10.0), (-1.7, 1.5), YELLOW)                 # collar
    for side in (False, True):
        p.box(CHEST, (1.05, 3.05), (-9.3, -7.7), (-3.45, -2.6), VENT, side)       # chest vents
        p.box(CHEST, (0.95, 3.15), (-9.45, -9.3), (-3.5, -2.6), YELLOW, side)
        p.box(CHEST, (3.3, 4.5), (-9.7, -7.4), (-1.6, 1.6), DARK, side)           # shoulder mounts
        p.box(CHEST, (1.5, 2.3), (-12.4, -9.9), (2.5, 3.3), WHITE, side)          # sabre hilts
        p.box(CHEST, (1.55, 2.25), (-12.9, -12.4), (2.55, 3.25), GREY, side)
        p.box(CHEST, (0.7, 2.2), (-6.6, -5.4), (3.3, 4.3), THRUST, side)          # backpack thrusters
    p.box(CHEST, (-0.95, 0.95), (-9.4, -6.3), (-3.6, -3.0), RED)                   # cockpit hatch
    p.box(CHEST, (-0.6, 0.6), (-8.9, -7.9), (-3.72, -3.55), TEAL)
    p.box(CHEST, (-2.6, 2.6), (-10.1, -5.6), (2.0, 3.5), WHITE2)                   # backpack
    p.box(CHEST, (-1.2, 1.2), (-9.4, -7.2), (3.5, 3.9), GREY)
    # abdomen
    p.box(BELLY, (-2.0, 2.0), (-6.3, -3.4), (-2.5, 1.6), RED, top=(-2.5, 2.5, -2.9, 1.8))
    p.box(BELLY, (-1.1, 1.1), (-5.6, -3.9), (-2.75, -2.5), DARK)
    # waist and skirt armour
    p.box(HIP, (-2.7, 2.7), (-3.4, -1.7), (-2.3, 1.9), WHITE)
    p.box(HIP, (-0.55, 0.55), (-2.4, 1.3), (-2.95, 1.7), RED)                      # centre block
    p.box(HIP, (-0.3, 0.3), (-2.1, -1.5), (-3.1, -2.95), YELLOW)
    for side in (False, True):
        p.box(HIP, (0.6, 3.0), (-1.9, 1.15), (-2.95, -2.2), WHITE, side, top=(0.6, 2.8, -2.6, -2.2))   # front skirt
        p.box(HIP, (1.1, 2.4), (-1.5, -0.45), (-3.12, -2.9), YELLOW, side)
        p.box(HIP, (2.75, 3.35), (-1.9, 0.9), (-1.7, 1.6), WHITE2, side)                                   # side skirt
        p.box(HIP, (0.6, 2.9), (-1.9, 0.9), (1.7, 2.35), WHITE, side)                                      # rear skirt
    return p


def legs():
    p = Part()
    THIGH, SHIN, FOOT, TOE = (26, 30), (27, 31), (28, 32), (29, 33)
    c = 2.25
    p.both(THIGH, (c - 1.0, c + 1.0), (-0.7, 0.7), (-1.0, 1.0), DARK)                       # hip joint
    p.both(THIGH, (c - 1.1, c + 1.1), (0.3, 3.9), (-1.35, 1.3), WHITE, top=(c - 1.3, c + 1.3, -1.5, 1.4))
    p.both(THIGH, (c - 0.7, c + 0.7), (1.0, 3.0), (-1.55, -1.35), WHITE2)
    p.both(SHIN, (c - 1.0, c + 1.0), (3.7, 5.3), (-1.5, 1.2), DARK)                         # knee joint
    p.both(SHIN, (c - 1.15, c + 1.15), (3.6, 5.4), (-2.3, -1.5), WHITE, top=(c - 0.9, c + 0.9, -2.0, -1.5))   # knee cap
    p.both(SHIN, (c - 0.6, c + 0.6), (4.1, 4.9), (-2.45, -2.3), YELLOW)
    p.both(SHIN, (c - 1.65, c + 1.65), (5.0, 9.7), (-2.1, 2.0), WHITE, top=(c - 1.15, c + 1.15, -1.55, 1.3))  # shin, flaring
    p.both(SHIN, (c - 1.0, c + 1.0), (6.0, 8.6), (1.7, 2.5), GREY)                          # calf thruster
    p.both(SHIN, (c - 0.8, c + 0.8), (8.6, 9.1), (1.8, 2.4), THRUST)
    p.both(SHIN, (c - 1.8, c + 1.8), (9.2, 10.0), (-2.3, 2.15), WHITE2)                     # ankle guard
    p.both(FOOT, (c - 0.9, c + 0.9), (9.6, 10.4), (-1.0, 1.0), DARK)                        # ankle
    p.both(FOOT, (c - 1.5, c + 1.5), (10.0, 11.27), (-3.3, 2.1), RED, top=(c - 1.2, c + 1.2, -2.6, 1.7))
    p.both(FOOT, (c - 1.3, c + 1.3), (9.5, 10.3), (-3.0, -1.2), WHITE)                      # instep armour
    p.both(FOOT, (c - 1.55, c + 1.55), (11.0, 11.29), (-3.3, 2.15), DARK)                   # sole
    p.both(TOE, (c - 1.4, c + 1.4), (10.45, 11.27), (-4.65, -3.3), RED, top=(c - 1.1, c + 1.1, -4.2, -3.3))
    return p


def head():
    p = Part()
    B = 5
    p.box(B, (-0.65, 0.65), (-4.5, -3.3), (-0.6, 0.7), DARK)                               # neck
    p.box(B, (-1.25, 1.25), (-7.0, -4.5), (-1.3, 1.4), WHITE, top=(-1.05, 1.05, -1.1, 1.2))  # helmet
    p.box(B, (-0.32, 0.32), (-7.55, -6.6), (-1.5, 1.2), WHITE)                             # crest
    p.box(B, (-0.22, 0.22), (-7.4, -6.9), (-1.62, -1.5), RED)                              # main camera
    p.box(B, (-0.95, 0.95), (-5.95, -4.75), (-1.5, -1.3), GREY)                            # face
    p.box(B, (-0.4, 0.4), (-5.2, -4.35), (-1.72, -1.4), RED)                               # chin
    p.box(B, (-0.3, 0.3), (-6.75, -6.2), (-1.6, -1.3), SENSOR)                             # forehead jewel
    p.box(B, (-1.0, 1.0), (-6.2, -5.95), (-1.55, -1.3), BLACK)                             # brow
    for side in (False, True):
        p.hexa(B, [(0.15, -5.98, -1.6), (0.92, -6.12, -1.6), (0.92, -6.12, -1.3), (0.15, -5.98, -1.3),
                   (0.15, -5.72, -1.6), (0.92, -5.78, -1.6), (0.92, -5.78, -1.3), (0.15, -5.72, -1.3)], EYE, side)
        p.beam(B, (0.12, -6.45, -1.5), (2.15, -7.95, -1.5), 0.42, 0.16, YELLOW, side)       # V-fin
        p.box(B, (1.2, 1.5), (-6.3, -5.2), (-0.6, 0.5), GREY, side)                          # ear vents
        p.box(B, (0.75, 1.0), (-6.9, -6.6), (-1.4, -1.2), DARK, side)                        # vulcans
        p.box(B, (0.25, 0.6), (-5.6, -5.0), (-1.56, -1.48), BLACK, side)                     # mouth slits
    p.box(B, (-0.5, 0.5), (-6.4, -5.0), (1.3, 1.6), GREY)                                  # rear sensor
    return p


def arm(mirror):
    p = Part()
    UPPER, FORE = (19, 21) if mirror else (11, 13)
    c = 2.25
    p.box(UPPER, (0.3, 4.4), (-2.6, 1.3), (-2.1, 2.1), WHITE, mirror, top=(0.7, 3.9, -1.7, 1.7))   # shoulder armour
    p.box(UPPER, (0.9, 3.8), (-2.85, -2.6), (-1.3, 1.3), WHITE2, mirror)
    p.box(UPPER, (4.4, 4.6), (-1.6, 0.4), (-1.2, 1.2), YELLOW, mirror)
    p.box(UPPER, (c - 0.9, c + 0.9), (0.9, 4.0), (-0.9, 0.9), GREY, mirror)                          # upper arm
    p.box(FORE, (c - 1.05, c + 1.05), (3.5, 4.9), (-1.05, 1.05), DARK, mirror)                       # elbow
    p.box(FORE, (c - 1.35, c + 1.35), (4.5, 8.7), (-1.4, 1.4), WHITE, mirror, top=(c - 1.05, c + 1.05, -1.1, 1.1))
    p.box(FORE, (c + 1.35, c + 1.6), (5.2, 7.9), (-0.9, 0.9), WHITE2, mirror)                        # forearm plate
    p.box(FORE, (c - 0.8, c + 0.8), (8.7, 9.05), (-0.8, 0.8), DARK, mirror)                          # wrist
    return p


def fist(mirror):
    p = Part()
    B = 22 if mirror else 14
    p.box(B, (-0.7, 0.7), (4.95, 5.7), (-0.7, 0.7), DARK, mirror)
    p.box(B, (-1.1, 1.1), (5.5, 7.5), (-1.2, 1.15), GREY, mirror)                           # palm
    p.box(B, (-1.2, 1.2), (5.6, 6.9), (1.15, 1.45), WHITE, mirror)                          # back-of-hand plate
    p.box(B, (-1.15, 1.15), (7.2, 8.45), (-1.3, 0.75), DARK, mirror)                        # fingers
    for k in range(4):
        z0 = -1.25 + k * 0.5
        p.box(B, (-1.22, 1.22), (7.3, 8.5), (z0 + 0.05, z0 + 0.42), GREY, mirror)
    p.box(B, (1.1, 1.55), (5.9, 7.4), (-1.0, -0.2), DARK, mirror)                           # thumb
    return p


# slot -> (file prefix, generator, texture rectangle in the shared atlas)
SLOTS = {
    1: ('ed01', body, (0.0, 0.5, 1.0, 0.5)),
    2: ('ed02', legs, (0.0, 0.5, 1.0, 0.5)),
    3: ('ed03', head, (0.0, 0.0, 0.5, 0.5)),
    4: ('ed04', lambda: arm(False), (0.5, 0.0, 0.5, 0.5)),
    5: ('ed05', lambda: arm(True), (0.0, 0.0, 0.5, 0.5)),
    6: ('ed06', lambda: fist(False), (0.5, 0.0, 0.5, 0.25)),
    7: ('ed07', lambda: fist(True), (0.5, 0.25, 0.5, 0.25)),
}


def find(folder, prefix, kind, ident):
    for name in os.listdir(folder):
        if name.startswith(prefix + '_' + kind) and name.endswith('_%04X.bin' % ident):
            return os.path.join(folder, name)
    raise SystemExit('template file %s %s %04X missing in %s' % (prefix, kind, ident, folder))


def build_model(template, part, rect):
    """Writes the part's quads into the template model's meshes."""
    model = jboy.Model(open(template, 'rb').read())
    inset = 0.004
    pending = {}
    for bone, quads in part.quads.items():
        out = []
        for quad in quads:
            corners = []
            for pos, normal, (s, t, swatch) in quad:
                cx, cy = cell_of(swatch)
                lu = (cx + inset + s * (1 - 2 * inset)) / GRID
                lv = (cy + inset + t * (1 - 2 * inset)) / GRID
                corners.append((pos, normal, (rect[0] + lu * rect[2], rect[1] + lv * rect[3])))
            out.append(corners)
        pending[bone] = out
    anchor = {bone: quads[0][0][0] for bone, quads in pending.items()}
    fallback = next(iter(anchor.values()))
    for m in model.meshes:
        bone = m.bones[0] if (m.weights == 1 and len(m.bones) == 1) else None
        quads = pending.get(bone, [])
        take = min(len(quads), jboy.quad_capacity(model, m)) if bone is not None else 0
        if take:
            jboy.set_quads(model, m, quads[:take])
            pending[bone] = quads[take:]
        else:
            jboy.collapse(model, m, anchor.get(m.bones[0], fallback))
    left = {bone: len(q) for bone, q in pending.items() if q}
    if left:
        raise SystemExit('%s: no room for %s quads (bone: count)' % (template, left))
    return model


def tga_dims(data):
    return struct.unpack_from('<HH', data, 12)


def build_textures(folder, prefix, slot, seed):
    """Diffuse (8-bit), normal (flat), ambient occlusion (white), glow mask -
    each in the template texture's size and header."""
    out = {}
    names = {}
    for ident_base, role in ((1000, 'diffuse'), (1100, 'normal'), (1400, 'ao'), (1600, 'glow')):
        path = find(folder, prefix, 'T', ident_base + slot - 1)
        names[role] = (path, bytearray(open(path, 'rb').read()))
    for role, (path, data) in names.items():
        w, h = tga_dims(data)
        palette, image, glow = paint_textures(w, h, seed)
        if role == 'diffuse':
            data[18:18 + 1024] = palette
            data[18 + 1024:18 + 1024 + w * h] = image[::-1].tobytes()      # TGA rows run bottom-up
        elif role == 'ao':
            for i in range(256):
                data[18 + i * 4:22 + i * 4] = bytes((i, i, i, 255))
            data[18 + 1024:18 + 1024 + w * h] = bytes([255]) * (w * h)
        elif role == 'normal':
            data[18:18 + w * h * 4] = bytes((0xFF, 0x80, 0x80, 0x50)) * (w * h)
        else:
            px = np.zeros((h, w, 4), dtype=np.uint8)
            px[:, :, 2] = glow[::-1]
            data[18:18 + w * h * 4] = px.tobytes()
        out[os.path.basename(path)] = bytes(data)
    return out


def generate(template_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for slot, (prefix, make, rect) in SLOTS.items():
        template = find(template_dir, prefix, 'M', 2000 + slot)
        part = make()
        model = build_model(template, part, rect)
        with open(os.path.join(out_dir, os.path.basename(template)), 'wb') as f:
            f.write(model.data)
        for name, data in build_textures(template_dir, prefix, slot, slot).items():
            with open(os.path.join(out_dir, name), 'wb') as f:
                f.write(data)
        print('slot %d: %d quads' % (slot, sum(len(q) for q in part.quads.values())))


def preview(out_png, template_dir):
    import preview as pv
    offsets = {1: (0, 0, 0), 2: (0, 0, 0), 3: (0, -7.2, 0.3), 4: (3.6, -8.4, 0), 5: (-3.6, -8.4, 0),
               6: (5.85, -4.6, 0), 7: (-5.85, -4.6, 0)}       # rough assembly, for the picture only
    tris, cols = [], []
    for slot, (prefix, make, rect) in SLOTS.items():
        part = make()
        for quads in part.quads.values():
            for quad in quads:
                pts = [np.array(c[0]) + np.array(offsets[slot]) for c in quad]
                rgb = SWATCHES[quad[0][2][2]][1]
                tris += [(pts[0], pts[1], pts[2]), (pts[0], pts[2], pts[3])]
                cols += [rgb, rgb]
    pv.write_png(out_png, pv.render(np.array(tris), np.array(cols, dtype=float), size=560))


if __name__ == '__main__':
    if sys.argv[1] == '--preview':
        preview(sys.argv[2], sys.argv[3])
    else:
        generate(sys.argv[1], sys.argv[2])

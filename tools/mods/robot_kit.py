"""Modelling kit for generated mod robots: convex solids (bevelled blocks,
cylinders, lofts) become faces, every face lies in a texture chart of its
own, the charts are packed into the part's texture and the result is written
as the OBJ files of a robot folder (see robot_folder.py / FORMAT.md).

Spaces as in the game: +x = the robot's left, +y = DOWN, front = -z; each
part has its own space (ASSEMBLY places them in the body / legs space).

A chart knows where every one of its texels is in space (world()), so the
painter (robot_paint.py) can weather the texture by position: wear on real
edges, dirt in real crevices, streaks that run down.
"""
import math

import numpy as np

DOWN = np.array([0.0, 1.0, 0.0])

# Where each part's own space sits in the body / legs space at rest, from the
# template skeleton: the head hangs off the chest bone, the arms off the
# collar bones, the fists off the elbows.
ASSEMBLY = {'body': (0.0, 0.0, 0.0), 'legs': (0.0, 0.0, 0.0), 'head': (0.0, -4.99, 0.0),
            'arm_l': (4.26, -7.52, 0.15), 'arm_r': (-4.26, -7.52, 0.15),
            'fist_l': (6.40, -3.38, 0.0), 'fist_r': (-6.40, -3.38, 0.0)}

# part -> ({group name: skeleton bone}, texture size)
PARTS = {
    'body': ({'chest': 3, 'abdomen': 2, 'hips': 1}, (1024, 512)),
    'legs': ({'thigh_l': 26, 'shin_l': 27, 'foot_l': 28, 'toe_l': 29,
              'thigh_r': 30, 'shin_r': 31, 'foot_r': 32, 'toe_r': 33}, (1024, 512)),
    'head': ({'head': 5}, (512, 512)),
    'arm_l': ({'upper': 11, 'forearm': 13}, (512, 512)),
    'arm_r': ({'upper': 19, 'forearm': 21}, (512, 512)),
    'fist_l': ({'fist': 14}, (512, 256)),
    'fist_r': ({'fist': 22}, (512, 256)),
}

PAD = 2          # texels of painted margin around every chart
# Everything built (and ASSEMBLY) is multiplied by this: a whole-robot size.
# The builder scales the skeleton in the part files by the same amount when
# the folder holds a skeleton_scale.txt (write_folder writes it).
WORLD_SCALE = 1.0


def unit(v):
    v = np.asarray(v, dtype=float)
    n = np.linalg.norm(v)
    return v / n if n > 1e-12 else v


def newell(p):
    """Area-weighted normal of a polygon (counter-clockwise = towards you)."""
    q = np.roll(p, -1, axis=0)
    return np.array([((p[:, 1] - q[:, 1]) * (p[:, 2] + q[:, 2])).sum(),
                     ((p[:, 2] - q[:, 2]) * (p[:, 0] + q[:, 0])).sum(),
                     ((p[:, 0] - q[:, 0]) * (p[:, 1] + q[:, 1])).sum()]) / 2


class Face:
    def __init__(self, bone, pts, normals, chart):
        self.bone = bone
        self.pts = np.asarray(pts, dtype=float)
        self.normals = np.asarray(normals, dtype=float)
        self.chart = chart
        self.st = chart.coords(self.pts)


class Chart:
    """A piece of surface with a rectangle of its own in the part's texture.
    Chart coordinates (s, t) are in model units, s to the right and t down
    in the texture; seen from outside the texture is never mirrored."""
    kind = 'plane'

    def __init__(self, mat, tags, weight, features, flip):
        self.mat = mat
        self.tags = set(tags)
        self.weight = weight
        self.features = list(features or [])
        self.flip = flip          # a mirrored copy: features swap left / right
        self.rect = None          # x, y, w, h in texels, margin included
        self.sx = self.sy = None  # texels per unit

    def texel(self, st):
        """Chart coordinates -> texture position in texels."""
        return np.stack([self.rect[0] + PAD + st[..., 0] * self.sx, self.rect[1] + PAD + st[..., 1] * self.sy], axis=-1)

    def grid(self):
        """(S, T): the chart coordinates of every texel of the rectangle."""
        x, y, w, h = self.rect
        return np.meshgrid((np.arange(w) + 0.5 - PAD) / self.sx, (np.arange(h) + 0.5 - PAD) / self.sy)


class PlaneChart(Chart):
    def __init__(self, pts, normal, mat, tags=(), weight=1.0, features=None, flip=False):
        Chart.__init__(self, mat, tags, weight, features, flip)
        pts = np.asarray(pts, dtype=float)
        n = unit(normal)
        v = DOWN - n * n.dot(DOWN)
        if np.linalg.norm(v) < 0.35:                 # a floor or a ceiling: 'down' is towards the viewer
            ref = np.array([0.0, 0.0, -1.0 if n[1] < 0 else 1.0])
            v = ref - n * n.dot(ref)
        v = unit(v)
        u = np.cross(n, v)
        s, t = pts @ u, pts @ v
        self.U, self.V, self.N = u, v, n
        self.o = u * s.min() + v * t.min() + n * (pts @ n).mean()
        self.size = (max(s.max() - s.min(), 1e-4), max(t.max() - t.min(), 1e-4))
        self.outline = np.stack([s - s.min(), t - t.min()], axis=1)
        self.wears = [True] * len(pts)          # per outline edge: does it wear like an exposed edge

    def coords(self, pts):
        d = np.asarray(pts, dtype=float) - self.o
        return np.stack([d @ self.U, d @ self.V], axis=-1)

    def world(self, s, t):
        return self.o + s[..., None] * self.U + t[..., None] * self.V

    def normals(self, s, t):
        return np.broadcast_to(self.N, s.shape + (3,))

    def edge_distance(self, s, t):
        """Distance to the nearest edge of the outline that wears."""
        best = np.full(s.shape, 1e9)
        o = self.outline
        for k in range(len(o)):
            a, b = o[k], o[(k + 1) % len(o)]
            e = b - a
            length = np.linalg.norm(e)
            if length < 1e-9 or not self.wears[k]:
                continue
            along = np.clip(((s - a[0]) * e[0] + (t - a[1]) * e[1]) / (length * length), 0, 1)
            best = np.minimum(best, np.hypot(s - a[0] - along * e[0], t - a[1] - along * e[1]))
        return best


class CylChart(Chart):
    """The unrolled wall of a cylinder or cone: s runs round, t along."""
    kind = 'cyl'

    def __init__(self, top, v, e1, e2, r_top, r_bot, length, theta0, mat, tags=(), weight=1.0, features=None, flip=False):
        Chart.__init__(self, mat, tags, weight, features, flip)
        self.top, self.V, self.e1, self.e2 = top, v, e1, e2
        self.r_top, self.r_bot, self.length, self.theta0 = r_top, r_bot, length, theta0
        self.rm = (r_top + r_bot) / 2
        self.size = (2 * math.pi * self.rm, length)
        self.outline = np.array([(0, 0), (self.size[0], 0), self.size, (0, self.size[1])], dtype=float)

    def coords(self, pts):
        d = np.asarray(pts, dtype=float) - self.top
        th = np.arctan2(d @ self.e2, d @ self.e1) - self.theta0
        th = np.mod(th + 1e-7, 2 * math.pi)
        return np.stack([th * self.rm, d @ self.V], axis=-1)

    def _angle(self, s):
        return self.theta0 + s / self.rm

    def normals(self, s, t):
        th = self._angle(s)
        return np.cos(th)[..., None] * self.e1 + np.sin(th)[..., None] * self.e2

    def world(self, s, t):
        r = self.r_top + (self.r_bot - self.r_top) * np.clip(t / self.length, 0, 1)
        return self.top + t[..., None] * self.V + r[..., None] * self.normals(s, t)

    def edge_distance(self, s, t):
        return np.clip(np.minimum(t, self.length - t), 0, None)


# hexahedron: corners 0..3 = one ring, 4..7 = the ring opposite.  For box():
# u = up (y0), d = down (y1), f = front (z0), k = back (z1), i = the x0 side,
# o = the x1 side (inner / outer on a left-hand piece, also when mirrored).
HEXA_FACES = {'u': (0, 1, 2, 3), 'd': (4, 5, 6, 7), 'f': (0, 1, 5, 4), 'o': (1, 2, 6, 5), 'k': (2, 3, 7, 6), 'i': (3, 0, 4, 7)}


class Part:
    def __init__(self, name, scale=1.0, pivot=(0.0, 0.0, 0.0), shift=(0.0, 0.0, 0.0)):
        """scale / pivot: everything built is enlarged about that point,
        then moved by shift."""
        self.name = name
        self.offset = np.array(ASSEMBLY[name], dtype=float) * WORLD_SCALE
        self.groups, self.size = PARTS[name]
        self.scale, self.pivot, self.shift = scale, np.array(pivot, dtype=float), np.array(shift, dtype=float)
        self.world = WORLD_SCALE
        self.faces = []
        self.charts = []
        self.solids = []          # convex solids for the occlusion bake: (planes n.p <= d, lo, hi)

    def _place(self, pts, mirror):
        pts = np.array(pts, dtype=float)
        if mirror:
            pts[..., 0] = -pts[..., 0]
        return ((pts - self.pivot) * self.scale + self.pivot + self.shift) * self.world

    # -- low level ---------------------------------------------------------
    def _plane(self, pts, normal, mat, tags, weight, features, flip):
        chart = PlaneChart(pts, normal, mat, tags, weight, features, flip)
        self.charts.append(chart)
        return chart

    def _emit(self, bone, pts, normals, chart):
        pts = np.asarray(pts, dtype=float)
        if np.linalg.norm(newell(pts)) < 1e-7:
            return
        self.faces.append(Face(bone, pts, normals, chart))

    def _solid(self, planes, pts):
        pts = np.asarray(pts, dtype=float)
        self.solids.append((np.asarray(planes, dtype=float), pts.min(axis=0), pts.max(axis=0)))

    def _fan(self, bone, pts, normal, chart):
        n = len(pts)
        i = 1
        while i < n - 1:
            idx = [0, i, i + 1] + ([i + 2] if i + 2 < n else [])
            self._emit(bone, pts[idx], [normal] * len(idx), chart)
            i += 2

    # -- solids ------------------------------------------------------------
    def hexa(self, bone, corners, mat, bevel=0.0, skip='', hard='', mats=None, mirror=False,
             weight=1.0, weights=None, tags=(), detail=None, solid=True, flip=None):
        """A six-sided solid from 8 corners (a ring of 4, then the ring
        opposite). bevel cuts the edges (shaded round); edges of faces named
        in hard stay sharp, faces named in skip are left out. mats / weights
        / detail: per face letter. Returns {face letter: chart}."""
        if flip is None:                               # a new solid (slabs come in already placed)
            c = self._place(corners, mirror)
            bevel *= self.scale * self.world
        else:
            c = np.array(corners, dtype=float)
        mirror = mirror if flip is None else flip      # from here on: do the charts' features swap sides
        centre = c.mean(axis=0)
        mats, weights, detail = mats or {}, weights or {}, detail or {}
        faces = {}
        for key, idx in HEXA_FACES.items():
            idx = list(idx)
            n = newell(c[idx])
            if np.linalg.norm(n) < 1e-9:
                continue
            n = unit(n)
            if n.dot(c[idx].mean(axis=0) - centre) < 0:
                idx.reverse()
                n = -n
            faces[key] = (idx, n)
        if solid:
            self._solid([np.append(n, n.dot(c[idx[0]])) for idx, n in faces.values()], c)
        edges = {}
        for key, (idx, n) in faces.items():
            for a in range(4):
                i, j = idx[a], idx[(a + 1) % 4]
                if np.linalg.norm(c[i] - c[j]) > 1e-9:
                    edges.setdefault(frozenset((i, j)), []).append(key)

        def soft(e):
            keys = edges.get(e, ())
            return bevel > 0 and len(keys) == 2 and all(k not in skip and k not in hard for k in keys)

        def corner(key, i):
            idx = faces[key][0]
            a = idx.index(i)
            p, q = idx[a - 1], idx[(a + 1) % 4]
            pos = c[i].copy()
            for near, far in ((p, q), (q, p)):          # edge (near, i) is cut: slide towards far
                if soft(frozenset((near, i))):
                    d = c[far] - c[i]
                    length = np.linalg.norm(d)
                    if length > 1e-9:
                        pos += d / length * min(bevel, 0.45 * length)
            return pos

        out = {}
        for key, (idx, n) in faces.items():
            if key in skip:
                continue
            pts = np.array([corner(key, i) for i in idx])
            chart = self._plane(pts, n, mats.get(key, mat), tags, weight * weights.get(key, 1.0), detail.get(key), mirror)
            self._emit(bone, pts, [n] * 4, chart)
            out[key] = chart
        edge_tags = tuple(tags) + ('edge',)
        for e, keys in edges.items():
            if not soft(e):
                continue
            i, j = tuple(e)
            f, g = keys
            pts = np.array([corner(f, i), corner(f, j), corner(g, j), corner(g, i)])
            nrm = [faces[f][1], faces[f][1], faces[g][1], faces[g][1]]
            if newell(pts).dot(pts.mean(axis=0) - centre) < 0:
                pts, nrm = pts[::-1], nrm[::-1]
            chart = self._plane(pts, unit(faces[f][1] + faces[g][1]), mats.get(f, mat), edge_tags, weight, None, mirror)
            self._emit(bone, pts, nrm, chart)
        for i in range(8):
            at = [e for e in edges if i in e]
            keys = [k for k, (idx, n) in faces.items() if i in idx]
            if len(at) == 3 and len(keys) == 3 and all(soft(e) for e in at):
                pts = np.array([corner(k, i) for k in keys])
                nrm = [faces[k][1] for k in keys]
                if newell(pts).dot(pts.mean(axis=0) - centre) < 0:
                    pts, nrm = pts[::-1], nrm[::-1]
                chart = self._plane(pts, unit(sum(nrm)), mats.get(keys[0], mat), edge_tags, weight, None, mirror)
                self._emit(bone, pts, nrm, chart)
        return out

    def box(self, bone, x, y, z, mat, top=None, **kw):
        """Axis-aligned box x=(x0, x1) y=(y0, y1) z=(z0, z1). top=(x0, x1,
        z0, z1) gives the upper (y0) face different extents (a taper)."""
        tx0, tx1, tz0, tz1 = top if top else (x[0], x[1], z[0], z[1])
        return self.hexa(bone, [(tx0, y[0], tz0), (tx1, y[0], tz0), (tx1, y[0], tz1), (tx0, y[0], tz1),
                                (x[0], y[1], z[0]), (x[1], y[1], z[0]), (x[1], y[1], z[1]), (x[0], y[1], z[1])], mat, **kw)

    def on(self, chart, s0, t0, s1, t1, lift=0.0):
        """A rectangle on a flat chart, in fractions of its extent (s to the
        right, t down as seen from outside): 4 points, lifted off the
        surface by lift."""
        if chart.flip:
            s0, s1 = 1 - s1, 1 - s0
        w, h = chart.size
        return [chart.o + chart.U * (s * w) + chart.V * (t * h) + chart.N * lift
                for s, t in ((s0, t0), (s1, t0), (s1, t1), (s0, t1))]

    def slab(self, bone, quad, thick, mat, skip='d', grow=0.0, **kw):
        """A plate standing thick proud of the rectangle quad (from on()).
        Faces: u = the outside, f / o / k / i = the upper / right / lower /
        left rim. grow < 0 tapers the outside in."""
        q = np.array(quad, dtype=float)
        n = -unit(newell(q))                         # on() goes round clockwise as seen from outside
        centre = q.mean(axis=0)
        outer = q + n * thick * self.world + (q - centre) * grow
        return self.hexa(bone, list(outer) + list(q), mat, skip=skip, flip=kw.pop('mirror', False), **kw)

    def cyl(self, bone, a, b, r, mat, n=8, r2=None, caps='ab', cap_mat=None, mirror=False, weight=1.0,
            tags=(), detail=None, cap_detail=None, phase=0.5, seam=None, flat=False):
        """A cylinder (or cone, r2 = radius at b) from a to b with n sides,
        shaded round. caps: which ends are closed."""
        a, b = self._place(a, mirror), self._place(b, mirror)
        r2 = (r if r2 is None else r2) * self.scale * self.world
        r = r * self.scale * self.world
        axis = unit(b - a)
        length = np.linalg.norm(b - a)
        if axis.dot(DOWN) < -1e-6:                   # texture 'down' follows the model's down
            top, v, r_top, r_bot = b, -axis, r2, r
        else:
            top, v, r_top, r_bot = a, axis, r, r2
        ref = np.array(seam, dtype=float) if seam is not None else np.array([0.0, 0.0, 1.0])
        if mirror and seam is not None:
            ref[0] = -ref[0]
        e1 = ref - v * v.dot(ref)
        if np.linalg.norm(e1) < 0.3:
            e1 = DOWN - v * v.dot(DOWN)
        e1 = unit(e1)
        e2 = np.cross(e1, v)
        theta0 = 2 * math.pi * phase / n
        chart = CylChart(top, v, e1, e2, r_top, r_bot, length, theta0, mat, tags, weight, detail, mirror)
        self.charts.append(chart)
        ang = theta0 + 2 * math.pi * np.arange(n + 1) / n
        ring = np.cos(ang)[:, None] * e1 + np.sin(ang)[:, None] * e2
        tops, bots = top + ring * r_top, top + v * length + ring * r_bot
        s = (ang - theta0) * chart.rm
        planes = []
        for k in range(n):
            pts = np.array([tops[k], tops[k + 1], bots[k + 1], bots[k]])
            nrm = [ring[k], ring[k + 1], ring[k + 1], ring[k]]
            mid = unit(ring[k] + ring[k + 1])
            if flat:
                nrm = [mid] * 4
            st = np.array([(s[k], 0), (s[k + 1], 0), (s[k + 1], length), (s[k], length)])
            if newell(pts).dot(mid) < 0:
                pts, nrm, st = pts[::-1], nrm[::-1], st[::-1]
            if np.linalg.norm(newell(pts)) > 1e-7:
                face = Face(bone, pts, nrm, chart)
                face.st = st                         # exact, also across the seam
                self.faces.append(face)
            planes.append(np.append(mid, mid.dot(tops[k] if r_top >= r_bot else bots[k])))
        for which, centre, normal, radius, pts in (('a' if top is a else 'b', top, -v, r_top, tops[:n]),
                                                   ('b' if top is a else 'a', top + v * length, v, r_bot, bots[:n])):
            planes.append(np.append(normal, normal.dot(centre)))
            if which in caps and radius > 1e-6:
                if newell(pts).dot(normal) < 0:
                    pts = pts[::-1]
                cap = self._plane(pts, normal, cap_mat or mat, tags, weight, cap_detail, mirror)
                self._fan(bone, pts, normal, cap)
        self._solid(planes, np.concatenate([tops, bots]))
        return chart

    def loft(self, bone, rings, mat, caps='', mats=None, mirror=False, weight=1.0, tags=(), detail=None,
             round_odd=True, cap_mat=None):
        """Skins a stack of rings (each the same number of points, going
        round the same way). With round_odd the odd-numbered sides are
        treated as cut corners and shaded round (see oct_ring). mats /
        detail: per side number. Returns {(row, side): chart}."""
        rings = [self._place(r, mirror) for r in rings]
        n = len(rings[0])
        centre = np.concatenate(rings).mean(axis=0)
        mats, detail = mats or {}, detail or {}
        quads, planes, out = {}, [], {}
        for j in range(len(rings) - 1):
            for k in range(n):
                idx = [(j, k), (j, (k + 1) % n), (j + 1, (k + 1) % n), (j + 1, k)]
                pts = np.array([rings[a][b] for a, b in idx])
                nrm = newell(pts)
                if np.linalg.norm(nrm) < 1e-9:
                    continue
                nrm = unit(nrm)
                if nrm.dot(pts.mean(axis=0) - centre) < 0:
                    idx.reverse()
                    pts, nrm = pts[::-1], -nrm
                quads[(j, k)] = (idx, pts, nrm)
                planes.append(np.append(nrm, nrm.dot(pts[0])))
        for (j, k), (idx, pts, nrm) in quads.items():
            normals = [nrm] * 4
            edge = False
            if round_odd and k % 2 == 1:            # a cut corner: take the neighbours' normals
                normals = []
                for a, b in idx:
                    side = (k - 1) % n if b == k else (k + 1) % n
                    normals.append(quads[(j, side)][2] if (j, side) in quads else nrm)
                edge = np.linalg.norm(rings[j][k] - rings[j][(k + 1) % n]) < 0.3 * self.world
            chart = self._plane(pts, nrm, mats.get((j, k), mats.get(k, mat)), tuple(tags) + (('edge',) if edge else ()),
                                weight, detail.get((j, k), detail.get(k)), mirror)
            # the seams between rows are not edges that wear
            chart.wears = [not (idx[e][0] == idx[(e + 1) % 4][0] and 0 < idx[e][0] < len(rings) - 1) for e in range(4)]
            self._emit(bone, pts, normals, chart)
            out[(j, k)] = chart
        for which, ring in (('a', rings[0]), ('b', rings[-1])):
            nrm = unit(newell(ring))
            if nrm.dot(ring.mean(axis=0) - centre) < 0:
                ring, nrm = ring[::-1], -nrm
            planes.append(np.append(nrm, nrm.dot(ring[0])))
            if which in caps:
                cap = self._plane(ring, nrm, mats.get(which, cap_mat or mat), tags, weight, detail.get(which), mirror)
                self._fan(bone, ring, nrm, cap)
                out[which] = cap
        self._solid(planes, np.concatenate(rings))
        return out


def oct_ring(cx, y, cz, hx, hz, cut, cut_back=None):
    """Eight points of a rectangle with cut corners in the plane y: sides 0 /
    2 / 4 / 6 are the front (-z), +x, back and -x faces, the odd ones the
    corners."""
    cb = cut if cut_back is None else cut_back
    return [(cx - hx + cut, y, cz - hz), (cx + hx - cut, y, cz - hz), (cx + hx, y, cz - hz + cut), (cx + hx, y, cz + hz - cb),
            (cx + hx - cb, y, cz + hz), (cx - hx + cb, y, cz + hz), (cx - hx, y, cz + hz - cb), (cx - hx, y, cz - hz + cut)]


# ---------------------------------------------------------------------------
# packing the charts into the part's texture
# ---------------------------------------------------------------------------
def pack(charts, width, height):
    """Gives every chart its rectangle, at the largest texel density that
    fits (shelves, tallest first). Returns texels per unit."""
    order = sorted(range(len(charts)), key=lambda i: -charts[i].size[1] * charts[i].weight)

    def attempt(density):
        x = y = shelf = 0
        placed = []
        for i in order:
            ch = charts[i]
            w = max(2, int(math.ceil(ch.size[0] * density * ch.weight))) + 2 * PAD
            h = max(2, int(math.ceil(ch.size[1] * density * ch.weight))) + 2 * PAD
            if w > width:
                return None
            if x + w > width:
                x, y, shelf = 0, y + shelf, 0
            if y + h > height:
                return None
            placed.append((i, x, y, w, h))
            x += w
            shelf = max(shelf, h)
        return placed

    lo, hi = 1.0, 400.0
    for _ in range(26):
        mid = (lo + hi) / 2
        if attempt(mid):
            lo = mid
        else:
            hi = mid
    for i, x, y, w, h in attempt(lo):
        ch = charts[i]
        ch.rect = (x, y, w, h)
        ch.sx, ch.sy = (w - 2 * PAD) / ch.size[0], (h - 2 * PAD) / ch.size[1]
    return lo


# ---------------------------------------------------------------------------
# output
# ---------------------------------------------------------------------------
def write_obj(part, path, title):
    width, height = part.size
    names = {bone: group for group, bone in part.groups.items()}
    lines = ['# %s %s - part space: +x = robot\'s left, +y = DOWN, front = -z' % (title, part.name),
             'mtllib %s.mtl' % part.name, 'usemtl %s' % part.name]
    count = 0
    for group, bone in part.groups.items():
        faces = [f for f in part.faces if f.bone == bone]
        if not faces:
            continue
        lines.append('g %s' % names[bone])
        for face in faces:
            uv = face.chart.texel(face.st)
            for pos, nrm, (u, v) in zip(face.pts, face.normals, uv):
                lines.append('v %.5f %.5f %.5f' % tuple(pos))
                lines.append('vt %.6f %.6f' % (u / width, 1 - v / height))
                lines.append('vn %.4f %.4f %.4f' % tuple(nrm))
            k = len(face.pts)
            lines.append('f ' + ' '.join('%d/%d/%d' % (count + i, count + i, count + i) for i in range(1, k + 1)))
            count += k
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def face_counts(part):
    names = {bone: group for group, bone in part.groups.items()}
    out = {}
    for face in part.faces:
        out[names[face.bone]] = out.get(names[face.bone], 0) + 1
    return out

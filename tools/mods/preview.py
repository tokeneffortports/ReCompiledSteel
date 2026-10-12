"""Offline preview of part models: flat-shaded orthographic views (front =
looking along +z, side = along +x, top = along -y) written as a PNG, so
models and generated geometry can be checked without starting the game.

  preview.py out.png model.bin [model.bin ...]
"""
import os, struct, sys, zlib
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jboy

PALETTE = [(200, 200, 200), (230, 120, 60), (90, 160, 230), (120, 210, 120), (230, 210, 80),
           (200, 110, 200), (110, 220, 220), (240, 150, 150), (160, 160, 240), (150, 150, 90)]


def write_png(path, img):
    h, w, _ = img.shape
    raw = b''.join(b'\0' + img[y].tobytes() for y in range(h))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def strip_triangles(indices):
    tris = []
    for i in range(len(indices) - 2):
        a, b, c = indices[i], indices[i + 1], indices[i + 2]
        if a == b or b == c or a == c:
            continue
        tris.append((a, b, c) if i % 2 == 0 else (b, a, c))
    return tris


def render(tris, colours, size=420, views=((0, 1, 2, 1), (2, 1, 0, -1), (0, 2, 1, 1))):
    """tris: (n, 3, 3) array of positions; colours: (n, 3). Each view is
    (horizontal axis, vertical axis, depth axis, depth sign)."""
    lo = tris.reshape(-1, 3).min(axis=0)
    hi = tris.reshape(-1, 3).max(axis=0)
    centre = (lo + hi) / 2
    scale = (size - 20) / max(hi - lo)
    sheet = np.zeros((size, size * len(views), 3), dtype=np.uint8)
    sheet[:] = (24, 28, 36)
    normal = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    length = np.linalg.norm(normal, axis=1)
    length[length == 0] = 1
    normal = normal / length[:, None]
    for v, (ax, ay, az, sign) in enumerate(views):
        depth = np.full((size, size), -1e9)
        px = (tris[:, :, ax] - centre[ax]) * scale * (1 if az != 0 else 1) + size / 2
        if az == 0:
            px = size - px
        py = (tris[:, :, ay] - centre[ay]) * scale + size / 2      # +y is down in these models
        pz = tris[:, :, az] * -sign                                   # larger = nearer the viewer
        shade = 0.35 + 0.65 * np.abs(normal[:, az])
        for t in range(len(tris)):
            x0, x1 = int(max(0, np.floor(px[t].min()))), int(min(size - 1, np.ceil(px[t].max())))
            y0, y1 = int(max(0, np.floor(py[t].min()))), int(min(size - 1, np.ceil(py[t].max())))
            if x1 < x0 or y1 < y0:
                continue
            xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            (ax0, ay0), (bx0, by0), (cx0, cy0) = zip(px[t], py[t])
            den = (by0 - cy0) * (ax0 - cx0) + (cx0 - bx0) * (ay0 - cy0)
            if abs(den) < 1e-9:
                continue
            w0 = ((by0 - cy0) * (xs - cx0) + (cx0 - bx0) * (ys - cy0)) / den
            w1 = ((cy0 - ay0) * (xs - cx0) + (ax0 - cx0) * (ys - cy0)) / den
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            z = w0 * pz[t, 0] + w1 * pz[t, 1] + w2 * pz[t, 2]
            region = depth[y0:y1 + 1, x0:x1 + 1]
            win = inside & (z > region)
            region[win] = z[win]
            sheet[y0:y1 + 1, v * size + x0:v * size + x1 + 1][win] = (colours[t] * shade[t]).astype(np.uint8)
    return sheet


def model_triangles(model, mesh_filter=None):
    tris, cols = [], []
    for i, m in enumerate(model.meshes):
        if mesh_filter and i not in mesh_filter:
            continue
        verts = np.array([v[:3] for v in model.vertices(m)])
        for a, b, c in strip_triangles(model.strip(m)):
            tris.append((verts[a], verts[b], verts[c]))
            cols.append(PALETTE[m.group % len(PALETTE)])
    return np.array(tris), np.array(cols, dtype=float)


def main():
    tris, cols = [], []
    for path in sys.argv[2:]:
        t, c = model_triangles(jboy.Model(open(path, 'rb').read()))
        tris.append(t)
        cols.append(c)
    tris, cols = np.concatenate(tris), np.concatenate(cols)
    print('%d triangles, bounds %s .. %s' % (len(tris), tris.reshape(-1, 3).min(axis=0).round(2), tris.reshape(-1, 3).max(axis=0).round(2)))
    write_png(sys.argv[1], render(tris, cols))


if __name__ == '__main__':
    main()

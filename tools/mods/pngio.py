"""Minimal PNG reading / writing (8-bit RGB or RGBA, greyscale and palette
PNGs, not interlaced) without external libraries."""
import struct, zlib

import numpy as np


def read_png(path):
    """Returns an (h, w, 3) uint8 RGB array."""
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('%s is not a PNG file' % path)
    pos, idat, palette = 8, b'', None
    while pos < len(d):
        n, kind = struct.unpack_from('>I4s', d, pos)
        body = d[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or interlace != 0 or ctype not in (0, 2, 3, 4, 6):
                raise SystemExit('%s: only 8-bit, non-interlaced PNGs are supported' % path)
            ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
        elif kind == b'PLTE':
            palette = np.frombuffer(body, dtype=np.uint8).reshape(-1, 3)
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw = np.frombuffer(zlib.decompress(idat), dtype=np.uint8).reshape(h, 1 + w * ch)
    out = np.zeros((h, w * ch), dtype=np.uint8)
    prev = np.zeros(w * ch, dtype=np.int32)
    for y in range(h):
        f = raw[y, 0]
        line = raw[y, 1:].astype(np.int32)
        if f == 0:
            cur = line
        elif f == 2:
            cur = (line + prev) & 255
        elif f == 1:
            cur = line.reshape(w, ch).cumsum(axis=0).reshape(-1) & 255
        else:
            cur = [0] * (w * ch)
            lp, ll = prev.tolist(), line.tolist()
            for i in range(w * ch):
                a = cur[i - ch] if i >= ch else 0
                b = lp[i]
                if f == 3:
                    pred = (a + b) >> 1
                else:
                    c = lp[i - ch] if i >= ch else 0
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (ll[i] + pred) & 255
            cur = np.array(cur, dtype=np.int32)
        out[y] = cur
        prev = cur
    px = out.reshape(h, w, ch)
    if ctype == 3:
        return palette[px[:, :, 0]]
    if ch <= 2:
        return np.repeat(px[:, :, :1], 3, axis=2)
    return np.ascontiguousarray(px[:, :, :3])


def write_png(path, img):
    """img: (h, w, 3) uint8 RGB."""
    img = np.ascontiguousarray(img, dtype=np.uint8)
    h, w, _ = img.shape
    raw = b''.join(b'\0' + img[y].tobytes() for y in range(h))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def resize_nearest(img, w, h):
    ys = (np.arange(h) * img.shape[0] // h)
    xs = (np.arange(w) * img.shape[1] // w)
    return img[ys][:, xs]


def resize(img, w, h):
    """Averages down when the picture is a whole multiple of the target
    size (the smaller maps of a robot folder), else picks nearest texels."""
    ih, iw = img.shape[:2]
    if (iw, ih) == (w, h):
        return img
    if iw % w == 0 and ih % h == 0:
        fx, fy = iw // w, ih // h
        return np.rint(img.reshape(h, fy, w, fx, -1).astype(np.float32).mean(axis=(1, 3))).astype(np.uint8)
    return resize_nearest(img, w, h)


def quantize(img, colours=256):
    """Returns (palette (n, 3) uint8, index image (h, w) uint8)."""
    h, w, _ = img.shape
    flat = img.reshape(-1, 3).astype(np.int32)
    keys = (flat[:, 0] << 16) | (flat[:, 1] << 8) | flat[:, 2]
    uniq, inverse = np.unique(keys, return_inverse=True)
    if len(uniq) <= colours:
        palette = np.stack([(uniq >> 16) & 255, (uniq >> 8) & 255, uniq & 255], axis=1).astype(np.uint8)
        return palette, inverse.reshape(h, w).astype(np.uint8)
    coarse = ((flat[:, 0] >> 3) << 10) | ((flat[:, 1] >> 3) << 5) | (flat[:, 2] >> 3)
    bins, binv, counts = np.unique(coarse, return_inverse=True, return_counts=True)
    top = np.argsort(-counts, kind="stable")[:colours]      # ties: lower bin first (as the game's builder)
    sums = np.zeros((len(bins), 3))
    np.add.at(sums, binv, flat)
    palette = (sums[top] / counts[top][:, None]).round().astype(np.int32)
    centres = (sums / counts[:, None])
    nearest = np.zeros(len(bins), dtype=np.int32)
    for start in range(0, len(bins), 4096):
        block = centres[start:start + 4096]
        dist = ((block[:, None, :] - palette[None, :, :]) ** 2).sum(axis=2)
        nearest[start:start + 4096] = dist.argmin(axis=1)
    return palette.astype(np.uint8), nearest[binv].reshape(h, w).astype(np.uint8)

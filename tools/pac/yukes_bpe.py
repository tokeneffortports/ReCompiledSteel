# Yuke's "BPE " block: header 'BPE ', u32 0x100, u32 compressed size, u32
# uncompressed size (little-endian), then Philip Gage's byte-pair encoding.
import struct


def decode(data, off=0):
    assert data[off:off + 4] == b'BPE ', data[off:off + 4]
    _, csize, usize = struct.unpack_from('<3I', data, off + 4)
    src = memoryview(data)[off + 16:off + 16 + csize]
    out = bytearray()
    p = 0
    n = len(src)
    while p < n and len(out) < usize:
        left = list(range(256))
        right = [0] * 256
        count = src[p]; p += 1
        c = 0
        while True:
            if count > 127:
                c += count - 127
                count = 0
            if c == 256:
                break
            for _ in range(count + 1):
                left[c] = src[p]; p += 1
                if c != left[c]:
                    right[c] = src[p]; p += 1
                c += 1
            if c == 256:
                break
            count = src[p]; p += 1
        size = src[p] | (src[p + 1] << 8); p += 2  # little-endian (Gage: big)
        end = p + size
        stack = []
        while True:
            if stack:
                c = stack.pop()
            elif p < end:
                c = src[p]; p += 1
            else:
                break
            l = left[c]
            if c == l:
                out.append(c)
            else:
                stack.append(right[c])
                stack.append(l)
    return bytes(out[:usize]), usize

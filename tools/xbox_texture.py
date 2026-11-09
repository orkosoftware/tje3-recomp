"""Decode Xbox (NV2A) texture data to RGBA and write PNGs, without dependencies.

Compressed formats (DXT1/3/5) are stored linearly. Uncompressed formats
without a LIN_ prefix are swizzled: texel (x, y) lives at the Morton index
formed by interleaving the bits of x and y, x first, while each dimension
still has bits left.
"""
import struct
import zlib


def _morton_tables(w, h):
    xs, ys = [0] * w, [0] * h
    out_bit, b = 0, 1
    while b < w or b < h:
        if b < w:
            for x in range(w):
                if x & b:
                    xs[x] |= 1 << out_bit
            out_bit += 1
        if b < h:
            for y in range(h):
                if y & b:
                    ys[y] |= 1 << out_bit
            out_bit += 1
        b <<= 1
    return xs, ys


def _rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _decode_dxt(data, w, h, kind):
    out = bytearray(w * h * 4)
    block = 8 if kind == 1 else 16
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * block
            alpha = None
            if kind == 3:
                bits = int.from_bytes(data[o:o + 8], "little")
                alpha = [((bits >> (4 * i)) & 15) * 17 for i in range(16)]
                o += 8
            elif kind == 5:
                a0, a1 = data[o], data[o + 1]
                bits = int.from_bytes(data[o + 2:o + 8], "little")
                if a0 > a1:
                    pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else:
                    pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                alpha = [pal[(bits >> (3 * i)) & 7] for i in range(16)]
                o += 8
            c0, c1, idx = struct.unpack_from("<HHI", data, o)
            p0, p1 = _rgb565(c0), _rgb565(c1)
            if c0 > c1 or kind != 1:
                p2 = tuple((2 * a + b) // 3 for a, b in zip(p0, p1))
                p3 = tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))
                cols = [p0 + (255,), p1 + (255,), p2 + (255,), p3 + (255,)]
            else:
                p2 = tuple((a + b) // 2 for a, b in zip(p0, p1))
                cols = [p0 + (255,), p1 + (255,), p2 + (255,), (0, 0, 0, 0)]
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x >= w or y >= h:
                    continue
                r, g, b, a = cols[(idx >> (2 * i)) & 3]
                if alpha is not None:
                    a = alpha[i]
                out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((r, g, b, a))
    return bytes(out)


def _texel_decoder(fmt):
    """Return (bytes_per_texel, fn(raw_bytes) -> (r, g, b, a)) for uncompressed formats."""
    def c16(f):
        return 2, lambda t: f(t[0] | t[1] << 8)
    if fmt in (0x06, 0x07, 0x12, 0x1E):          # (LIN_)A8R8G8B8 / X8R8G8B8
        opaque = fmt in (0x07, 0x1E)
        return 4, lambda t: (t[2], t[1], t[0], 255 if opaque else t[3])
    if fmt in (0x05, 0x11):                      # R5G6B5
        return c16(lambda c: _rgb565(c) + (255,))
    if fmt in (0x02, 0x03, 0x10):                # A1R5G5B5 / X1R5G5B5
        def f(c):
            r, g, b = (c >> 10) & 31, (c >> 5) & 31, c & 31
            a = 255 if fmt == 0x03 or c & 0x8000 else 0
            return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, a)
        return c16(f)
    if fmt in (0x04, 0x1D):                      # A4R4G4B4
        return c16(lambda c: (((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17, (c >> 12) * 17))
    if fmt in (0x00, 0x13):                      # L8
        return 1, lambda t: (t[0], t[0], t[0], 255)
    if fmt == 0x01:                              # AL8: one byte used as both
        return 1, lambda t: (t[0], t[0], t[0], t[0])
    if fmt == 0x19:                              # A8
        return 1, lambda t: (255, 255, 255, t[0])
    if fmt == 0x1A:                              # A8L8
        return 2, lambda t: (t[0], t[0], t[0], t[1])
    if fmt == 0x28:                              # V8U8 (signed), shown as a normal map
        return 2, lambda t: ((t[0] + 128) & 255, (t[1] + 128) & 255, 255, 255)
    raise ValueError(f"unsupported format 0x{fmt:02x}")


LINEAR = {0x10, 0x11, 0x12, 0x13, 0x1D, 0x1E}


def decode(fmt, w, h, data):
    """Decode the top mip level to RGBA8 bytes."""
    if fmt == 0x0C:
        return _decode_dxt(data, w, h, 1)
    if fmt == 0x0E:
        return _decode_dxt(data, w, h, 3)
    if fmt == 0x0F:
        return _decode_dxt(data, w, h, 5)
    bpp, texel = _texel_decoder(fmt)
    out = bytearray(w * h * 4)
    if fmt in LINEAR:
        index = lambda x, y: y * w + x
    else:
        xs, ys = _morton_tables(w, h)
        index = lambda x, y: xs[x] | ys[y]
    for y in range(h):
        for x in range(w):
            o = index(x, y) * bpp
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(texel(data[o:o + bpp]))
    return bytes(out)


def title_image(xbe):
    """(size, RGBA) of the square dashboard image inside an XbeImage."""
    _name, _va, _vsize, raw, rsize = xbe.section("$$XTIMAGE")
    xpr = xbe.data[raw:raw + rsize]
    header = struct.unpack_from("<I", xpr, 8)[0]
    fmt = struct.unpack_from("<I", xpr, 24)[0]
    size = 1 << ((fmt >> 20) & 15)
    return size, bytes(decode((fmt >> 8) & 0xff, size, size, xpr[header:]))


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))

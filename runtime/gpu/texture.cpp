// NV2A texture formats -> RGBA8 for upload.
//
// Uncompressed formats without a LIN_ prefix are swizzled: texel (x, y) lives at
// the Morton index formed by interleaving the bits of x and y, x first, while
// each dimension still has bits left. Each mip level (and cube face) is
// swizzled on its own. DXT formats are stored as plain 4x4 blocks. Same rules
// as tools/xbox_texture.py.
#include <cstring>

#include "gpu.h"
#include "texture.h"

namespace tje::gpu {

FormatInfo format_info(uint32_t f) {
    switch (f) {
    case 0x00: case 0x01: case 0x0b: case 0x19: return {8, false, false, true};
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1a: case 0x28: return {16, false, false, true};
    case 0x24: case 0x25: case 0x27: case 0x29: case 0x2c: case 0x2d: case 0x32: case 0x38: case 0x39:
        return {16, false, false, false};
    case 0x06: case 0x07: case 0x3a: case 0x3b: case 0x3c: return {32, false, false, true};
    case 0x2a: case 0x2b: case 0x33: return {32, false, false, false};
    case 0x0c: return {4, false, true, true};
    case 0x0e: case 0x0f: return {8, false, true, true};
    case 0x13: case 0x1b: case 0x1f: return {8, true, false, true};
    case 0x10: case 0x11: case 0x1c: case 0x1d: case 0x20: return {16, true, false, true};
    case 0x16: case 0x17: case 0x30: case 0x31: case 0x35: case 0x37: return {16, true, false, false};
    case 0x12: case 0x1e: case 0x3f: case 0x40: case 0x41: return {32, true, false, true};
    case 0x2e: case 0x2f: case 0x36: return {32, true, false, false};
    default: return {32, false, false, false};
    }
}

uint32_t level_size(uint32_t format, uint32_t w, uint32_t h, uint32_t pitch) {
    FormatInfo f = format_info(format);
    if (f.compressed) return ((w + 3) / 4) * ((h + 3) / 4) * (f.bpp == 4 ? 8 : 16);
    if (f.linear) return (pitch ? pitch : w * f.bpp / 8) * h;
    return w * h * f.bpp / 8;
}

uint32_t texture_size(const TextureRef& t) {
    uint32_t total = 0, w = t.width, h = t.height;
    for (uint32_t l = 0; l < t.levels; l++) {
        total += level_size(t.format, w, h, t.pitch) * t.depth;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if (t.cube) total = ((total + 127) & ~127u) * 6;
    return total;
}

namespace {

uint8_t ex5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
uint8_t ex6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

void rgb565(uint16_t c, uint8_t* o) {
    o[0] = ex5((c >> 11) & 31);
    o[1] = ex6((c >> 5) & 63);
    o[2] = ex5(c & 31);
}

void decode_dxt(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* out) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, block = kind == 1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = src + (by * bw + bx) * block;
            uint8_t alpha[16];
            bool has_alpha = false;
            if (kind == 3) {
                for (int i = 0; i < 16; i++) alpha[i] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                has_alpha = true;
                b += 8;
            } else if (kind == 5) {
                uint32_t a0 = b[0], a1 = b[1], pal[8];
                pal[0] = a0;
                pal[1] = a1;
                if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                else {
                    for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                    pal[6] = 0;
                    pal[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = (uint8_t)pal[(bits >> (3 * i)) & 7];
                has_alpha = true;
                b += 8;
            }
            uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
            uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
            uint8_t col[4][4];
            rgb565(c0, col[0]);
            rgb565(c1, col[1]);
            col[0][3] = col[1][3] = 255;
            if (c0 > c1 || kind != 1) {
                for (int k = 0; k < 3; k++) {
                    col[2][k] = (uint8_t)((2 * col[0][k] + col[1][k]) / 3);
                    col[3][k] = (uint8_t)((col[0][k] + 2 * col[1][k]) / 3);
                }
                col[2][3] = col[3][3] = 255;
            } else {
                for (int k = 0; k < 3; k++) col[2][k] = (uint8_t)((col[0][k] + col[1][k]) / 2);
                col[2][3] = 255;
                std::memset(col[3], 0, 4);
            }
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                uint8_t* o = out + (y * w + x) * 4;
                std::memcpy(o, col[(idx >> (2 * i)) & 3], 4);
                if (has_alpha) o[3] = alpha[i];
            }
        }
}

// Morton tables: bit positions of x and y in the swizzled index.
void morton(uint32_t w, uint32_t h, std::vector<uint32_t>& xs, std::vector<uint32_t>& ys) {
    xs.assign(w, 0);
    ys.assign(h, 0);
    uint32_t out_bit = 0;
    for (uint32_t b = 1; b < w || b < h; b <<= 1) {
        if (b < w) {
            for (uint32_t x = 0; x < w; x++) if (x & b) xs[x] |= 1u << out_bit;
            out_bit++;
        }
        if (b < h) {
            for (uint32_t y = 0; y < h; y++) if (y & b) ys[y] |= 1u << out_bit;
            out_bit++;
        }
    }
}

// One texel to RGBA8. Returns false for formats not handled.
inline bool texel(uint32_t f, const uint8_t* t, const uint8_t* palette, uint8_t* o) {
    uint32_t c;
    switch (f) {
    case 0x06: case 0x12:                             // A8R8G8B8
        o[0] = t[2]; o[1] = t[1]; o[2] = t[0]; o[3] = t[3]; return true;
    case 0x07: case 0x1e:                             // X8R8G8B8
        o[0] = t[2]; o[1] = t[1]; o[2] = t[0]; o[3] = 255; return true;
    case 0x3a: case 0x3f:                             // A8B8G8R8
        o[0] = t[0]; o[1] = t[1]; o[2] = t[2]; o[3] = t[3]; return true;
    case 0x3b: case 0x40:                             // B8G8R8A8
        o[0] = t[1]; o[1] = t[2]; o[2] = t[3]; o[3] = t[0]; return true;
    case 0x3c: case 0x41:                             // R8G8B8A8
        o[0] = t[3]; o[1] = t[2]; o[2] = t[1]; o[3] = t[0]; return true;
    case 0x05: case 0x11:                             // R5G6B5
        c = t[0] | t[1] << 8;
        rgb565((uint16_t)c, o);
        o[3] = 255;
        return true;
    case 0x02: case 0x03: case 0x10: case 0x1c:       // A1R5G5B5 / X1R5G5B5
        c = t[0] | t[1] << 8;
        o[0] = ex5((c >> 10) & 31); o[1] = ex5((c >> 5) & 31); o[2] = ex5(c & 31);
        o[3] = (f == 0x03 || f == 0x1c || (c & 0x8000)) ? 255 : 0;
        return true;
    case 0x04: case 0x1d:                             // A4R4G4B4
        c = t[0] | t[1] << 8;
        o[0] = (uint8_t)(((c >> 8) & 15) * 17); o[1] = (uint8_t)(((c >> 4) & 15) * 17);
        o[2] = (uint8_t)((c & 15) * 17); o[3] = (uint8_t)((c >> 12) * 17);
        return true;
    case 0x00: case 0x13:                             // L8
        o[0] = o[1] = o[2] = t[0]; o[3] = 255; return true;
    case 0x01: case 0x1b:                             // AL8: one byte for both
        o[0] = o[1] = o[2] = o[3] = t[0]; return true;
    case 0x19: case 0x1f:                             // A8
        o[0] = o[1] = o[2] = 255; o[3] = t[0]; return true;
    case 0x1a: case 0x20:                             // A8L8
        o[0] = o[1] = o[2] = t[0]; o[3] = t[1]; return true;
    case 0x28:                                        // V8U8 (signed), biased into 0..255
        o[0] = (uint8_t)(t[0] + 128); o[1] = (uint8_t)(t[1] + 128); o[2] = 255; o[3] = 255; return true;
    case 0x0b:                                        // P8: D3DCOLOR palette
        if (!palette) { o[0] = o[1] = o[2] = o[3] = 255; return true; }
        o[0] = palette[t[0] * 4 + 2]; o[1] = palette[t[0] * 4 + 1]; o[2] = palette[t[0] * 4]; o[3] = palette[t[0] * 4 + 3];
        return true;
    default: return false;
    }
}

}  // namespace

bool decode_texture(const TextureRef& t, const uint8_t* src, const uint8_t* palette, std::vector<uint8_t>& out,
                    std::vector<DecodedLevel>& levels) {
    FormatInfo fi = format_info(t.format);
    uint32_t faces = t.cube ? 6 : 1;
    uint32_t face_bytes = 0;
    {
        uint32_t w = t.width, h = t.height;
        for (uint32_t l = 0; l < t.levels; l++) {
            face_bytes += level_size(t.format, w, h, t.pitch);
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        if (t.cube) face_bytes = (face_bytes + 127) & ~127u;
    }
    size_t total = 0;
    levels.clear();
    for (uint32_t face = 0; face < faces; face++) {
        uint32_t w = t.width, h = t.height;
        for (uint32_t l = 0; l < t.levels; l++) {
            levels.push_back({face, l, w, h, total});
            total += (size_t)w * h * 4;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
    }
    out.resize(total);
    bool ok = true;
    std::vector<uint32_t> xs, ys;
    for (const DecodedLevel& lv : levels) {
        uint32_t off = lv.face * face_bytes;
        {
            uint32_t w = t.width, h = t.height;
            for (uint32_t l = 0; l < lv.level; l++) {
                off += level_size(t.format, w, h, t.pitch);
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
            }
        }
        const uint8_t* s = src + off;
        uint8_t* o = out.data() + lv.offset;
        uint32_t w = lv.width, h = lv.height;
        if (fi.compressed) {
            decode_dxt(s, w, h, t.format == 0x0c ? 1 : t.format == 0x0e ? 3 : 5, o);
            continue;
        }
        uint32_t bpp = fi.bpp / 8;
        if (fi.linear) {
            uint32_t pitch = t.pitch ? t.pitch : w * bpp;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    if (!texel(t.format, s + y * pitch + x * bpp, palette, o + (y * w + x) * 4)) ok = false;
        } else {
            morton(w, h, xs, ys);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    if (!texel(t.format, s + (size_t)(xs[x] | ys[y]) * bpp, palette, o + (y * w + x) * 4)) ok = false;
        }
        if (!ok) {                                    // unknown format: magenta
            for (size_t i = 0; i < (size_t)w * h; i++) { o[i * 4] = 255; o[i * 4 + 1] = 0; o[i * 4 + 2] = 255; o[i * 4 + 3] = 255; }
        }
    }
    return ok;
}

}  // namespace tje::gpu

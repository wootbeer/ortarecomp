/* Xbox texture decoding. See xtex.h. */

#include "gfx/xtex.h"

#include <string.h>

enum layout { SWIZZLED, LINEAR, COMPRESSED };

typedef enum pix {
    PIX_L8, PIX_AL8, PIX_A1R5G5B5, PIX_X1R5G5B5, PIX_A4R4G4B4, PIX_R5G6B5,
    PIX_A8R8G8B8, PIX_X8R8G8B8, PIX_A8, PIX_A8L8, PIX_A8B8G8R8, PIX_B8G8R8A8,
    PIX_R8G8B8A8, PIX_V8U8, PIX_DXT1, PIX_DXT3, PIX_DXT5,
} pix;

static const struct fmt {
    uint32_t    code;
    const char *name;
    enum layout layout;
    pix         pix;
    uint32_t    bpp;    /* bits per texel */
} formats[] = {
    { 0x00, "L8",            SWIZZLED,   PIX_L8,       8 },
    { 0x01, "AL8",           SWIZZLED,   PIX_AL8,      8 },
    { 0x02, "A1R5G5B5",      SWIZZLED,   PIX_A1R5G5B5, 16 },
    { 0x03, "X1R5G5B5",      SWIZZLED,   PIX_X1R5G5B5, 16 },
    { 0x04, "A4R4G4B4",      SWIZZLED,   PIX_A4R4G4B4, 16 },
    { 0x05, "R5G6B5",        SWIZZLED,   PIX_R5G6B5,   16 },
    { 0x06, "A8R8G8B8",      SWIZZLED,   PIX_A8R8G8B8, 32 },
    { 0x07, "X8R8G8B8",      SWIZZLED,   PIX_X8R8G8B8, 32 },
    { 0x0C, "DXT1",          COMPRESSED, PIX_DXT1,     4 },
    { 0x0E, "DXT3",          COMPRESSED, PIX_DXT3,     8 },
    { 0x0F, "DXT5",          COMPRESSED, PIX_DXT5,     8 },
    { 0x10, "LIN_A1R5G5B5",  LINEAR,     PIX_A1R5G5B5, 16 },
    { 0x11, "LIN_R5G6B5",    LINEAR,     PIX_R5G6B5,   16 },
    { 0x12, "LIN_A8R8G8B8",  LINEAR,     PIX_A8R8G8B8, 32 },
    { 0x13, "LIN_L8",        LINEAR,     PIX_L8,       8 },
    { 0x19, "A8",            SWIZZLED,   PIX_A8,       8 },
    { 0x1A, "A8L8",          SWIZZLED,   PIX_A8L8,     16 },
    { 0x1B, "LIN_AL8",       LINEAR,     PIX_AL8,      8 },
    { 0x1C, "LIN_X1R5G5B5",  LINEAR,     PIX_X1R5G5B5, 16 },
    { 0x1D, "LIN_A4R4G4B4",  LINEAR,     PIX_A4R4G4B4, 16 },
    { 0x1E, "LIN_X8R8G8B8",  LINEAR,     PIX_X8R8G8B8, 32 },
    { 0x1F, "LIN_A8",        LINEAR,     PIX_A8,       8 },
    { 0x20, "LIN_A8L8",      LINEAR,     PIX_A8L8,     16 },
    { 0x28, "V8U8",          SWIZZLED,   PIX_V8U8,     16 },
    { 0x3A, "A8B8G8R8",      SWIZZLED,   PIX_A8B8G8R8, 32 },
    { 0x3B, "B8G8R8A8",      SWIZZLED,   PIX_B8G8R8A8, 32 },
    { 0x3C, "R8G8B8A8",      SWIZZLED,   PIX_R8G8B8A8, 32 },
    { 0x3F, "LIN_A8B8G8R8",  LINEAR,     PIX_A8B8G8R8, 32 },
    { 0x40, "LIN_B8G8R8A8",  LINEAR,     PIX_B8G8R8A8, 32 },
    { 0x41, "LIN_R8G8B8A8",  LINEAR,     PIX_R8G8B8A8, 32 },
};

static const struct fmt *find(uint32_t code)
{
    for (size_t i = 0; i < sizeof formats / sizeof *formats; i++)
        if (formats[i].code == code)
            return &formats[i];
    return NULL;
}

const char *pdo_xtex_format_name(uint32_t format)
{
    const struct fmt *f = find(format);
    return f ? f->name : "unknown";
}

void pdo_xtex_describe(uint32_t format_word, uint32_t size_word, pdo_xtex_info *info)
{
    const struct fmt *f;

    memset(info, 0, sizeof *info);
    info->format = (format_word >> 8) & 0xFF;
    info->levels = (format_word >> 16) & 0xF;
    f = find(info->format);
    info->known = f != NULL;

    if (size_word) {
        info->width = (size_word & 0xFFF) + 1;
        info->height = ((size_word >> 12) & 0xFFF) + 1;
        info->pitch = (((size_word >> 24) & 0xFF) + 1) * 64;
    } else {
        info->width = 1u << ((format_word >> 20) & 0xF);
        info->height = 1u << ((format_word >> 24) & 0xF);
    }
    if (!f)
        return;
    if (f->layout == COMPRESSED)
        info->bytes = ((info->width + 3) / 4) * ((info->height + 3) / 4) * (f->bpp * 2);
    else if (f->layout == LINEAR)
        info->bytes = info->pitch * info->height;
    else
        info->bytes = info->width * info->height * f->bpp / 8;
}

static uint32_t level_bytes(const struct fmt *f, uint32_t w, uint32_t h)
{
    if (f->layout == COMPRESSED)
        return ((w + 3) / 4) * ((h + 3) / 4) * (f->bpp * 2);
    return w * h * f->bpp / 8;
}

uint32_t pdo_xtex_level_count(const pdo_xtex_info *info)
{
    const struct fmt *f = find(info->format);
    uint32_t n = info->levels ? info->levels : 1, max = 1, w = info->width, h = info->height;

    if (!f || f->layout == LINEAR)
        return 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        max++;
    }
    return n < max ? n : max;
}

void pdo_xtex_level(const pdo_xtex_info *info, uint32_t level, pdo_xtex_info *out, uint32_t *offset)
{
    const struct fmt *f = find(info->format);
    uint32_t w = info->width, h = info->height, off = 0;

    *out = *info;
    *offset = 0;
    if (!f || f->layout == LINEAR)
        return;
    for (uint32_t l = 0; l < level; l++) {
        off += level_bytes(f, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    out->width = w;
    out->height = h;
    out->bytes = level_bytes(f, w, h);
    out->levels = 1;
    *offset = off;
}

/* Morton offset of (x, y) in a w x h swizzled texture: interleave the bits,
 * u first, dropping a coordinate's bits once it runs out. */
static uint32_t swizzle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t off = 0, o = 0;
    for (uint32_t i = 0; (1u << i) < w || (1u << i) < h; i++) {
        if ((1u << i) < w)
            off |= ((x >> i) & 1u) << o++;
        if ((1u << i) < h)
            off |= ((y >> i) & 1u) << o++;
    }
    return off;
}

static void put(uint8_t *d, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    d[0] = (uint8_t)r; d[1] = (uint8_t)g; d[2] = (uint8_t)b; d[3] = (uint8_t)a;
}

static uint32_t x5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t x6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t x4(uint32_t v) { return v * 17; }

static void texel(pix p, const uint8_t *s, uint8_t *d)
{
    uint32_t v;

    switch (p) {
    case PIX_L8:       put(d, s[0], s[0], s[0], 255); break;
    case PIX_AL8:      put(d, s[0], s[0], s[0], s[0]); break;
    case PIX_A8:       put(d, 255, 255, 255, s[0]); break;
    case PIX_A8L8:     put(d, s[0], s[0], s[0], s[1]); break;
    case PIX_A1R5G5B5:
        v = (uint32_t)(s[0] | s[1] << 8);
        put(d, x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), (v >> 15) ? 255 : 0);
        break;
    case PIX_X1R5G5B5:
        v = (uint32_t)(s[0] | s[1] << 8);
        put(d, x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), 255);
        break;
    case PIX_A4R4G4B4:
        v = (uint32_t)(s[0] | s[1] << 8);
        put(d, x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15), x4(v >> 12));
        break;
    case PIX_R5G6B5:
        v = (uint32_t)(s[0] | s[1] << 8);
        put(d, x5(v >> 11), x6((v >> 5) & 63), x5(v & 31), 255);
        break;
    case PIX_A8R8G8B8:  put(d, s[2], s[1], s[0], s[3]); break;  /* memory: B G R A */
    case PIX_X8R8G8B8:  put(d, s[2], s[1], s[0], 255); break;
    case PIX_A8B8G8R8:  put(d, s[0], s[1], s[2], s[3]); break;  /* memory: R G B A */
    case PIX_B8G8R8A8:  put(d, s[1], s[2], s[3], s[0]); break;  /* memory: A R G B */
    case PIX_R8G8B8A8:  put(d, s[3], s[2], s[1], s[0]); break;  /* memory: A B G R */
    /* Signed U, V (memory: U V) for bump mapping, stored offset by 128 so
     * filtering stays monotonic; the shader maps them back to -1..1. */
    case PIX_V8U8:      put(d, s[0] ^ 0x80u, s[1] ^ 0x80u, 255, 255); break;
    default:            put(d, 255, 0, 255, 255); break;
    }
}

/* ── DXT ─────────────────────────────────────────────────────────────── */

static void dxt_colors(const uint8_t *b, uint8_t c[4][4], bool dxt1)
{
    uint32_t c0 = (uint32_t)(b[0] | b[1] << 8), c1 = (uint32_t)(b[2] | b[3] << 8);
    uint32_t r0 = x5(c0 >> 11), g0 = x6((c0 >> 5) & 63), b0 = x5(c0 & 31);
    uint32_t r1 = x5(c1 >> 11), g1 = x6((c1 >> 5) & 63), b1 = x5(c1 & 31);

    put(c[0], r0, g0, b0, 255);
    put(c[1], r1, g1, b1, 255);
    if (!dxt1 || c0 > c1) {
        put(c[2], (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        put(c[3], (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    } else {
        put(c[2], (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        put(c[3], 0, 0, 0, 0);
    }
}

static void decode_dxt(pix p, const pdo_xtex_info *info, const uint8_t *src, uint8_t *out)
{
    uint32_t bw = (info->width + 3) / 4, bh = (info->height + 3) / 4;
    uint32_t block_bytes = p == PIX_DXT1 ? 8 : 16;

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + (by * bw + bx) * block_bytes;
            const uint8_t *cb = p == PIX_DXT1 ? b : b + 8;
            uint8_t c[4][4], alpha[16];
            uint32_t idx = (uint32_t)(cb[4] | cb[5] << 8 | cb[6] << 16) | (uint32_t)cb[7] << 24;

            dxt_colors(cb, c, p == PIX_DXT1);
            if (p == PIX_DXT3) {
                for (int i = 0; i < 16; i++)
                    alpha[i] = (uint8_t)x4((b[i / 2] >> ((i & 1) * 4)) & 15);
            } else if (p == PIX_DXT5) {
                uint32_t a0 = b[0], a1 = b[1], a[8];
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++)
                    bits |= (uint64_t)b[2 + i] << (8 * i);
                a[0] = a0; a[1] = a1;
                if (a0 > a1) {
                    for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                } else {
                    for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                    a[6] = 0; a[7] = 255;
                }
                for (int i = 0; i < 16; i++)
                    alpha[i] = (uint8_t)a[(bits >> (3 * i)) & 7];
            }
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                uint8_t *d;
                if (x >= info->width || y >= info->height)
                    continue;
                d = out + (y * info->width + x) * 4;
                memcpy(d, c[(idx >> (2 * i)) & 3], 4);
                if (p != PIX_DXT1)
                    d[3] = alpha[i];
            }
        }
    }
}

int pdo_xtex_bc(const pdo_xtex_info *info)
{
    const struct fmt *f = find(info->format);
    if (!f || f->layout != COMPRESSED)
        return 0;
    return f->pix == PIX_DXT1 ? 1 : f->pix == PIX_DXT3 ? 2 : 3;
}

uint32_t pdo_xtex_bc_bytes(const pdo_xtex_info *info)
{
    return ((info->width + 3) / 4) * ((info->height + 3) / 4) * (pdo_xtex_bc(info) == 1 ? 8u : 16u);
}

void pdo_xtex_decode(const pdo_xtex_info *info, const uint8_t *src, uint8_t *rgba)
{
    const struct fmt *f = find(info->format);
    uint32_t w = info->width, h = info->height;

    if (!f) {
        for (uint32_t i = 0; i < w * h; i++)
            put(rgba + i * 4, 255, 0, 255, 255);
        return;
    }
    if (f->layout == COMPRESSED) {
        decode_dxt(f->pix, info, src, rgba);
        return;
    }
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t off = f->layout == LINEAR ? y * info->pitch + x * f->bpp / 8
                                               : swizzle(x, y, w, h) * f->bpp / 8;
            texel(f->pix, src + off, rgba + (y * w + x) * 4);
        }
    }
}

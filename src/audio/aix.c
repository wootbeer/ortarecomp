/* CRI AIX and ADX decoding. See aix.h.
 *
 * AIX: chunks of "AIXF" (header), "AIXP" (data) and "AIXE" (end), each
 * a 4-byte tag and a big-endian 32-bit size. An AIXP chunk carries the next
 * piece of one stream: its index at +8, then the data from +16. A stream's
 * pieces in order make a whole ADX file.
 *
 * ADX: 0x80 0x00, then the big-endian offset of the "(c)CRI" mark (data
 * starts 4 bytes after it); +4 encoding (3: fixed coefficients), +5 frame
 * size per channel (18), +6 bits per sample (4), +7 channels, +8 rate, +12
 * samples, +16 high-pass frequency, +19 flags (8 or 9: encrypted). A frame
 * per channel is a big-endian scale and 32 signed 4-bit deltas, high nibble
 * first; a sample is delta * scale plus a two-tap prediction from the
 * high-pass frequency. A scale with the top bit set ends the stream. */
#include "aix.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

typedef struct { uint8_t *data; size_t len, cap; } bytes;

static bool append(bytes *b, const uint8_t *p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1 << 20;
        while (cap < b->len + n)
            cap *= 2;
        uint8_t *d = realloc(b->data, cap);
        if (!d)
            return false;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return true;
}

/* One ADX stream into interleaved PCM; false if not a plain ADX stream. */
static bool decode_adx(const uint8_t *s, size_t len, pdo_aix *out, int k)
{
    if (len < 20 || s[0] != 0x80 || s[1] != 0x00)
        return false;
    size_t start = (size_t)be16(s + 2) + 4;
    int type = s[4], fsize = s[5], bits = s[6], ch = s[7];
    uint32_t rate = be32(s + 8), total = be32(s + 12), hp = be16(s + 16);
    if (type != 3 || fsize != 18 || bits != 4 || ch < 1 || ch > 2 || !rate || start >= len) {
        SDL_Log("aix: stream %d is not plain ADX (type %d, frame %d, bits %d, %d ch)", k, type, fsize, bits, ch);
        return false;
    }
    if (s[19] == 8 || s[19] == 9) {
        SDL_Log("aix: stream %d is encrypted", k);
        return false;
    }

    /* sqrt(2) and pi spelled out: MSVC's math.h has no M_SQRT2 or M_PI. */
    const double sqrt2 = 1.41421356237309504880, pi = 3.14159265358979323846;
    double a = sqrt2 - cos(2.0 * pi * hp / rate), b = sqrt2 - 1.0;
    double c = (a - sqrt((a + b) * (a - b))) / b;
    int c1 = (int)(c * 8192.0), c2 = (int)(c * c * -4096.0);

    size_t frames = (len - start) / ((size_t)fsize * ch);
    uint32_t n = (uint32_t)(frames * 32);
    if (total && total < n)
        n = total;
    int16_t *pcm = calloc((size_t)n * ch, sizeof *pcm);
    if (!pcm)
        return false;

    int hist1[2] = {0, 0}, hist2[2] = {0, 0};
    uint32_t written = 0;
    for (size_t f = 0; f < frames && written < n; f++) {
        bool end = false;
        for (int c_ = 0; c_ < ch; c_++) {
            const uint8_t *fr = s + start + (f * ch + (size_t)c_) * fsize;
            uint16_t sc = be16(fr);
            if (sc & 0x8000) {          /* end of stream */
                end = true;
                break;
            }
            int scale = (sc & 0x1FFF) + 1;
            for (int i = 0; i < 32 && written + (uint32_t)i < n; i++) {
                int nib = (i & 1) ? (fr[2 + i / 2] & 0x0F) : (fr[2 + i / 2] >> 4);
                if (nib & 8)
                    nib -= 16;
                int v = nib * scale + ((c1 * hist1[c_] + c2 * hist2[c_]) >> 12);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                hist2[c_] = hist1[c_];
                hist1[c_] = v;
                pcm[(size_t)(written + (uint32_t)i) * ch + c_] = (int16_t)v;
            }
        }
        if (end)
            break;
        written += 32;
    }
    if (written > n)
        written = n;
    out->channels[k] = ch;
    out->rate[k] = rate;
    out->frames[k] = written;
    out->pcm[k] = pcm;
    return true;
}

bool pdo_aix_load(const char *path, pdo_aix *out)
{
    size_t len = 0;
    uint8_t *d = SDL_LoadFile(path, &len);
    bytes st[PDO_AIX_MAX_STREAMS];
    int count = 0;
    bool ok = true;

    memset(out, 0, sizeof *out);
    memset(st, 0, sizeof st);
    if (!d)
        return false;
    if (len < 8 || memcmp(d, "AIXF", 4) != 0) {
        SDL_free(d);
        return false;
    }
    for (size_t i = 0; i + 8 <= len;) {
        uint32_t sz = be32(d + i + 4);
        if (i + 8 + (size_t)sz > len)
            break;
        if (memcmp(d + i, "AIXP", 4) == 0 && sz >= 8) {
            int sid = d[i + 8];
            if (sid < PDO_AIX_MAX_STREAMS) {
                if (sid + 1 > count)
                    count = sid + 1;
                if (!append(&st[sid], d + i + 16, sz - 8)) {
                    ok = false;
                    break;
                }
            }
        } else if (memcmp(d + i, "AIXE", 4) == 0)
            break;
        i += 8 + (size_t)sz;
    }
    SDL_free(d);

    for (int k = 0; ok && k < count; k++)
        ok = decode_adx(st[k].data, st[k].len, out, k);
    for (int k = 0; k < PDO_AIX_MAX_STREAMS; k++)
        free(st[k].data);
    out->streams = count;
    if (!ok || !count) {
        pdo_aix_free(out);
        return false;
    }
    return true;
}

void pdo_aix_free(pdo_aix *a)
{
    for (int k = 0; k < PDO_AIX_MAX_STREAMS; k++)
        free(a->pcm[k]);
    memset(a, 0, sizeof *a);
}

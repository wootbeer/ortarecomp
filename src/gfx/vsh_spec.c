/* Vertex programs as their own GPU shaders (vsh_spec.h). The generated
 * run_spec does what vsh.vert's run() does with the same code, instruction
 * by instruction; run() is the reference, and a change there is a change
 * here. */
#include "gfx/vsh_spec.h"
#include "gfx/shaders_spv.h"
#include "gfx/renderer.h"
#include "config.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PDO_HAVE_GLSLANG
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#endif

/* ── GLSL ─────────────────────────────────────────────────────────────── */

typedef struct { char *p; size_t n, cap; } sbuf;

static void put(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    for (;;) {
        size_t room = b->cap - b->n;
        va_start(ap, fmt);
        n = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? room : 0, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if (b->p && (size_t)n < room) {
            b->n += (size_t)n;
            return;
        }
        b->cap = b->cap * 2 + (size_t)n + 4096;
        b->p = realloc(b->p, b->cap);
        if (!b->p) {
            b->cap = b->n = 0;
            return;
        }
    }
}

static const char comp[4] = { 'x', 'y', 'z', 'w' };

/* An operand word (vsh.c, gpu_src): row, constant bit 8, negate bit 9,
 * swizzle two bits a component from bit 12. Constants read K, the
 * instruction's constant. */
static void operand(sbuf *b, uint32_t w)
{
    char sw[6] = ".";
    bool ident = true;

    for (int i = 0; i < 4; i++) {
        uint32_t c = (w >> (12 + 2 * i)) & 3u;
        sw[1 + i] = comp[c];
        ident = ident && c == (uint32_t)i;
    }
    sw[5] = 0;
    put(b, "%s", (w >> 9) & 1u ? "-" : "");
    if ((w >> 8) & 1u)
        put(b, "K");
    else
        put(b, "R%u", w & 0xFFu);
    if (!ident)
        put(b, "%s", sw);
}

/* dst.mask = val.mask, run()'s mix(dst, val, mask_of(m)). */
static void write_masked(sbuf *b, const char *dst, uint32_t idx, const char *val, uint32_t m)
{
    char sw[5];
    int n = 0;

    m &= 15u;
    if (!m)
        return;
    if (m == 15u) {
        put(b, "        %s%u = %s;\n", dst, idx, val);
        return;
    }
    for (int i = 0; i < 4; i++)
        if (m & (8u >> i))
            sw[n++] = comp[i];
    sw[n] = 0;
    put(b, "        %s%u.%s = %s.%s;\n", dst, idx, sw, val, sw);
}

static char *gen(const uint32_t *code, uint32_t count, bool compute)
{
    sbuf b = {0};
    bool v_used[16] = {0}, rel_any = false;
    const char *src = vsh_vert_src;
    const char *nl = strchr(src, '\n');

    /* #version stays first. */
    put(&b, "%.*s", (int)(nl - src + 1), src);
    put(&b, "#define PDO_VSH_SPEC 1\n");
    if (compute)
        put(&b, "#define PDO_VSH_COMPUTE 1\n");
    put(&b, "%s", nl + 1);

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t *w = code + 8 * i;
        uint32_t use = (w[0] >> 8) & 7u;
        for (int k = 0; k < 3; k++) {
            uint32_t s = w[k == 0 ? 3 : 3 + k];
            if (!(use & (1u << k)) || ((s >> 8) & 1u))
                continue;
            if ((s & 0xFFu) >= 13 && (s & 0xFFu) <= 28)
                v_used[(s & 0xFFu) - 13] = true;
        }
        rel_any = rel_any || ((w[0] >> 12) & 1u) || (w[0] & 15u) == 13u;
    }

    put(&b, "\nvoid run_spec()\n{\n");
    put(&b, "    vec4 Z = opaque_zero();\n");
    if (rel_any)
        put(&b, "    int a0 = 0;\n");
    for (int r = 0; r < 30; r++) {
        if (r >= 13 && r <= 28) {
            if (v_used[r - 13])
                put(&b, "    vec4 R%d = fetch(%d);\n", r, r - 13);
            else
                put(&b, "    vec4 R%d = Z;\n", r);   /* never read */
        } else {
            put(&b, "    vec4 R%d = %s;\n", r, r == 12 ? "vec4(Z.xyz, 1.0)" : "Z");
        }
    }
    for (int o = 0; o < 16; o++)
        put(&b, "    vec4 O%d = %s;\n", o,
            o == 3 || o == 5 || o == 7 ? "vec4(1.0)" : o == 6 ? "vec4(1.0, 0.0, 0.0, 0.0)" : "vec4(0.0)");

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t *w = code + 8 * i;
        uint32_t mac = w[0] & 15u, ilu = (w[0] >> 4) & 7u, use = (w[0] >> 8) & 7u;
        uint32_t rel = (w[0] >> 12) & 1u, cidx = (w[0] >> 16) & 0xFFu, mux = (w[0] >> 24) & 1u;
        uint32_t mac_row = w[1] & 0xFFu, ilu_row = (w[1] >> 8) & 0xFFu;
        uint32_t o_kind = (w[1] >> 16) & 0xFFu, o_index = w[1] >> 24;
        uint32_t ops[3] = { w[3], w[4], w[5] };
        bool k_used = false;

        for (int k = 0; k < 3; k++)
            k_used = k_used || ((use & (1u << k)) && ((ops[k] >> 8) & 1u));
        put(&b, "    {   // %u\n", i);
        if (k_used) {
            if (rel)
                put(&b, "        vec4 K = cread(clamp(%u + a0, 0, 191));\n", cidx);
            else
                put(&b, "        vec4 K = cread(%u);\n", cidx);
        }
        for (int k = 0; k < 3; k++) {
            put(&b, "        vec4 %c = ", "ABC"[k]);
            if (use & (1u << k))
                operand(&b, ops[k]);
            else
                put(&b, "vec4(1.0)");
            put(&b, ";\n");
        }
        put(&b, "        vec4 M = vec4(0.0), I = vec4(0.0);\n");
        switch (mac) {
        case 1: put(&b, "        M = A;\n"); break;
        case 2: put(&b, "        M = mulz(A, B);\n"); break;
        case 3: put(&b, "        M = A + C;\n"); break;
        case 4: put(&b, "        M = mulz(A, B) + C;\n"); break;
        case 5: case 6: case 7:
            put(&b, "        float d = mulz1(A.x, B.x) + mulz1(A.y, B.y);\n");
            put(&b, "        d = d + mulz1(A.z, B.z);\n");
            if (mac == 7)
                put(&b, "        d = d + mulz1(A.w, B.w);\n");
            else if (mac == 6)
                put(&b, "        d = d + B.w;\n");
            put(&b, "        M = vec4(d);\n");
            break;
        case 8: put(&b, "        M = vec4(1.0, mulz1(A.y, B.y), A.z, B.w);\n"); break;
        case 9: put(&b, "        M = mix(B, A, lessThan(A, B));\n"); break;
        case 10: put(&b, "        M = mix(B, A, greaterThan(A, B));\n"); break;
        case 11: put(&b, "        M = vec4(lessThan(A, B));\n"); break;
        case 12: put(&b, "        M = vec4(greaterThanEqual(A, B));\n"); break;
        case 13: put(&b, "        a0 = int(floor(A.x));\n"); break;
        default: break;
        }
        switch (ilu) {
        case 1: put(&b, "        I = C;\n"); break;
        case 2: put(&b, "        I = vec4(C.x == 1.0 ? 1.0 : 1.0 / C.x);\n"); break;
        case 3: put(&b, "        I = vec4(rcc(C.x));\n"); break;
        case 4: put(&b, "        I = vec4(1.0 / sqrt(abs(C.x)));\n"); break;
        case 5:
            put(&b, "        float f = floor(C.x);\n");
            put(&b, "        I = vec4(exp2(f), C.x - f, exp2(C.x), 1.0);\n");
            break;
        case 6:
            put(&b, "        float x = abs(C.x);\n");
            put(&b, "        if (x == 0.0) {\n");
            put(&b, "            I = vec4(uintBitsToFloat(0xFF800000u));\n");
            put(&b, "            I.w = 1.0;\n");
            put(&b, "        } else {\n");
            put(&b, "            int e;\n");
            put(&b, "            float m = frexp(x, e);\n");
            put(&b, "            I = vec4(float(e - 1), m * 2.0, log2(x), 1.0);\n");
            put(&b, "        }\n");
            break;
        case 7:
            put(&b, "        float dl = max(C.x, 0.0), sp = max(C.y, 0.0), e = clamp(C.w, -127.9961, 127.9961);\n");
            put(&b, "        I = vec4(1.0, dl, C.x > 0.0 ? pow(sp, e) : 0.0, 1.0);\n");
            break;
        default: break;
        }
        /* Writes, after both units, in run()'s order. */
        if (mac_row != 255u)
            write_masked(&b, "R", mac_row, "M", w[2]);
        if (ilu_row != 255u)
            write_masked(&b, "R", ilu_row, "I", w[2] >> 4);
        if (o_kind == 1u)
            write_masked(&b, "R", o_index, mux ? "I" : "M", w[2] >> 8);
        else if (o_kind == 2u)
            write_masked(&b, "O", o_index, mux ? "I" : "M", w[2] >> 8);
        put(&b, "    }\n");
    }
    put(&b, "    R[12] = R12;\n");
    for (int o = 0; o < 16; o++)
        put(&b, "    O[%d] = O%d;\n", o, o);
    put(&b, "}\n");
    return b.p;
}

static bool code_valid(const uint32_t *code, uint32_t count)
{
    /* Rows the generator names must exist: 0-29, or 255 for none. */
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t *w = code + 8 * i;
        uint32_t mac_row = w[1] & 0xFFu, ilu_row = (w[1] >> 8) & 0xFFu;
        uint32_t o_kind = (w[1] >> 16) & 0xFFu, o_index = w[1] >> 24;
        if ((mac_row > 29 && mac_row != 255) || (ilu_row > 29 && ilu_row != 255))
            return false;
        if ((o_kind == 1 && o_index > 29) || (o_kind == 2 && o_index > 15))
            return false;
        for (int k = 0; k < 3; k++)
            if (!((w[3 + k] >> 8) & 1u) && (w[3 + k] & 0xFFu) > 29)
                return false;
    }
    return count > 0;
}

/* ── Fragment setups (ff.frag with one draw's setup as constants) ────── */

/* The blob's words: [0] pixel shader (combiners) or texture stages; the
 * stages' color_op, alpha_op, stage_misc (4 x 4 each); alpha_test enable,
 * func, specular; fog_mode enable, mode; the pixel shader's rgb in, rgb
 * out, alpha in, alpha out (8 each) and final (4). What the setup does not
 * use is 0, so that setups differing only there share a shader: the other
 * kind's fields, stages after the first disabled one, combiner stages past
 * the count, the alpha function with the test off, the fog mode with fog
 * off. */
#define B_COMB    0
#define B_COP     1
#define B_AOP     17
#define B_MISC    33
#define B_AT      49
#define B_FOG     52
#define B_RGBIN   54
#define B_RGBOUT  62
#define B_AIN     70
#define B_AOUT    78
#define B_FINAL   86

void pdo_fs_spec_blob(const pdo_ff_uniforms *u, bool combiners, int32_t out[PDO_FS_SPEC_WORDS])
{
    memset(out, 0, PDO_FS_SPEC_WORDS * sizeof *out);
    out[B_COMB] = combiners;
    if (combiners) {
        int count = u->ps_final[2] & 0xFF;
        if (count > 8)
            count = 8;
        for (int i = 0; i < count; i++) {
            out[B_RGBIN + i] = u->ps_rgb_in[i];
            out[B_RGBOUT + i] = u->ps_rgb_out[i];
            out[B_AIN + i] = u->ps_a_in[i];
            out[B_AOUT + i] = u->ps_a_out[i];
        }
        memcpy(&out[B_FINAL], u->ps_final, 4 * sizeof(int32_t));
        for (int s = 0; s < 4; s++) {
            out[B_MISC + s * 4 + 1] = u->stage_misc[s][1];      /* has texture */
            out[B_MISC + s * 4 + 3] = u->stage_misc[s][3];      /* shadow map */
        }
    } else {
        for (int s = 0; s < 4 && u->color_op[s][0] > 1; s++) {
            memcpy(&out[B_COP + s * 4], u->color_op[s], 4 * sizeof(int32_t));
            memcpy(&out[B_AOP + s * 4], u->alpha_op[s], 4 * sizeof(int32_t));
            memcpy(&out[B_MISC + s * 4], u->stage_misc[s], 4 * sizeof(int32_t));
        }
        out[B_AT + 2] = u->alpha_test[3];                       /* specular */
    }
    out[B_AT] = u->alpha_test[0] != 0;
    out[B_AT + 1] = u->alpha_test[0] ? u->alpha_test[1] : 0;
    out[B_FOG] = u->fog_mode[0] != 0;
    out[B_FOG + 1] = u->fog_mode[0] ? u->fog_mode[1] : 0;
}

static void put_ivec4s(sbuf *b, const char *name, const int32_t *w, int n)
{
    put(b, "const ivec4 %s[%d] = ivec4[%d](", name, n, n);
    for (int i = 0; i < n; i++)
        put(b, "%sivec4(%d, %d, %d, %d)", i ? ", " : "", w[i * 4], w[i * 4 + 1], w[i * 4 + 2], w[i * 4 + 3]);
    put(b, ");\n");
}

char *pdo_fs_spec_glsl(const int32_t *w)
{
    sbuf b = { 0 };
    const char *src = ff_frag_src, *nl = strchr(src, '\n');

    if (!nl || strncmp(src, "#version", 8) != 0)
        return NULL;
    put(&b, "%.*s", (int)(nl - src + 1), src);          /* #version */
    put(&b, "#define PDO_FF_SPEC\n");
    if (w[B_COMB])
        put(&b, "#define PIXEL_SHADER\n");
    put_ivec4s(&b, "color_op", &w[B_COP], 4);
    put_ivec4s(&b, "alpha_op", &w[B_AOP], 4);
    put_ivec4s(&b, "stage_misc", &w[B_MISC], 4);
    put_ivec4s(&b, "ps_rgb_in", &w[B_RGBIN], 2);
    put_ivec4s(&b, "ps_rgb_out", &w[B_RGBOUT], 2);
    put_ivec4s(&b, "ps_a_in", &w[B_AIN], 2);
    put_ivec4s(&b, "ps_a_out", &w[B_AOUT], 2);
    put(&b, "const ivec4 ps_final = ivec4(%d, %d, %d, %d);\n", w[B_FINAL], w[B_FINAL + 1], w[B_FINAL + 2],
        w[B_FINAL + 3]);
    put(&b, "const ivec4 fog_mode = ivec4(%d, %d, 0, 0);\n", w[B_FOG], w[B_FOG + 1]);
    put(&b, "#define alpha_test ivec4(%d, %d, u_alpha_test.z, %d)\n", w[B_AT], w[B_AT + 1], w[B_AT + 2]);
    put(&b, "#line 2\n%s", nl + 1);
    return b.p;
}

char *pdo_vsh_spec_glsl(const uint32_t *code, uint32_t count)
{
    return code_valid(code, count) ? gen(code, count, false) : NULL;
}

/* The program's compute variant (vsh.vert's PDO_VSH_COMPUTE): run once a
 * vertex, its outputs written to a buffer (renderer.c, the vertex pre-pass). */
static char *comp_glsl(const uint32_t *code, uint32_t count)
{
    return code_valid(code, count) ? gen(code, count, true) : NULL;
}

uint64_t pdo_vsh_comp_key(uint64_t vkey)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 8; i++)
        h = (h ^ ((vkey >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
    h = (h ^ 'C') * 0x100000001B3ull;
    return h ? h : 1;
}

/* FNV-1a, 64 bits. */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *s = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ s[i]) * 0x100000001B3ull;
    return h;
}

uint64_t pdo_fs_spec_key(const int32_t *w)
{
    static uint64_t base;
    uint64_t h;

    if (!base)
        base = fnv(fnv(0xCBF29CE484222325ull, "ff", 2), ff_frag_src, sizeof ff_frag_src);
    h = fnv(base, w, PDO_FS_SPEC_WORDS * sizeof *w);
    return h ? h : 1;
}

/* What shaders/ holds is for this build of vsh.vert and ff.frag. */
static uint64_t build_key(void)
{
    return fnv(pdo_vsh_spec_key(NULL, 0), ff_frag_src, sizeof ff_frag_src);
}

uint64_t pdo_vsh_spec_key(const uint32_t *code, uint32_t count)
{
    static uint64_t base;
    uint64_t h;

    if (!base)
        base = fnv(0xCBF29CE484222325ull, vsh_vert_src, sizeof vsh_vert_src);
    if (!code)
        return base;
    h = fnv(base, code, (size_t)count * 32);
    return h ? h : 1;
}

/* ── Compiling, and the cache ─────────────────────────────────────────── */

#ifdef PDO_HAVE_GLSLANG

typedef struct {
    uint64_t key;
    int      state;             /* 0 waiting, 1 ready, -1 failed */
    int      kind;              /* 0 a vertex program, 1 a fragment setup, 2 a program's compute variant */
    uint32_t *code, count;      /* while waiting: the program (count instructions), or the setup */
    uint32_t *spv;
    size_t   bytes;
} entry;

static SDL_Mutex *lock;
static SDL_Condition *wake;
static SDL_Thread *worker;
static bool quitting, enabled_known, enabled;
static entry *entries;
static uint32_t nentries, cap_entries;
static uint32_t n_made, n_failed, n_pending, n_logged;
static double ms_made;

#define FILE_MAGIC 0x50535650u  /* "PVSP" */

static void cache_path(char *out, size_t n, uint64_t key)
{
    snprintf(out, n, "%sshaders%cvsh-%016llx.spv", pdo_data_dir(),
#ifdef _WIN32
             '\\',
#else
             '/',
#endif
             (unsigned long long)key);
}

static bool load(uint64_t key, uint32_t **spv, size_t *bytes)
{
    char path[1024];
    size_t size = 0;
    uint32_t *d;

    cache_path(path, sizeof path, key);
    d = SDL_LoadFile(path, &size);
    if (!d)
        return false;
    if (size < 16 + 20 || (size & 3) || d[0] != FILE_MAGIC || d[1] != (uint32_t)key
            || d[2] != (uint32_t)(key >> 32) || d[4] != 0x07230203u) {
        SDL_free(d);
        SDL_RemovePath(path);
        return false;
    }
    *bytes = size - 16;
    *spv = malloc(*bytes);
    if (*spv)
        memcpy(*spv, d + 4, *bytes);
    SDL_free(d);
    return *spv != NULL;
}

static void save(uint64_t key, const uint32_t *spv, size_t bytes)
{
    char path[1024], dir[1024], tmp[1100];
    uint32_t head[4] = { FILE_MAGIC, (uint32_t)key, (uint32_t)(key >> 32), 0 };
    SDL_IOStream *io;
    bool ok;

    snprintf(dir, sizeof dir, "%sshaders", pdo_data_dir());
    SDL_CreateDirectory(dir);
    cache_path(path, sizeof path, key);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    io = SDL_IOFromFile(tmp, "wb");
    if (!io)
        return;
    ok = SDL_WriteIO(io, head, sizeof head) == sizeof head && SDL_WriteIO(io, spv, bytes) == bytes;
    ok = SDL_CloseIO(io) && ok;
    if (!ok || !SDL_RenamePath(tmp, path))
        SDL_RemovePath(tmp);
}

/* Shaders from another build of vsh.vert are never asked for again: the
 * folder keeps one build's, named in shaders/vsh-build. */
static void tidy_cache(void)
{
    char dir[1024], stamp[1100], want[32];
    size_t size = 0;
    char *have;
    int n = 0;

    snprintf(dir, sizeof dir, "%sshaders", pdo_data_dir());
    snprintf(stamp, sizeof stamp, "%s%cvsh-build", dir,
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
             );
    snprintf(want, sizeof want, "%016llx", (unsigned long long)build_key());
    have = SDL_LoadFile(stamp, &size);
    if (have && size == strlen(want) && !memcmp(have, want, size)) {
        SDL_free(have);
        return;
    }
    SDL_free(have);
    {
        char **old = SDL_GlobDirectory(dir, "vsh-*.spv", 0, &n);
        for (int i = 0; old && i < n; i++) {
            char path[1200];
            snprintf(path, sizeof path, "%s%c%s", dir,
#ifdef _WIN32
                     '\\',
#else
                     '/',
#endif
                     old[i]);
            SDL_RemovePath(path);
        }
        SDL_free(old);
    }
    SDL_CreateDirectory(dir);
    SDL_SaveFile(stamp, want, strlen(want));
    if (n)
        SDL_Log("gfx: %d shaders of another build removed", n);
}

static bool compile(const char *glsl, uint32_t **spv, size_t *bytes, uint64_t key, int kind)
{
    const char *what = kind == 1 ? "fragment setup" : kind == 2 ? "vertex program (compute)" : "vertex program";
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *prog = NULL;
    bool ok = false;

    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = kind == 1 ? GLSLANG_STAGE_FRAGMENT : kind == 2 ? GLSLANG_STAGE_COMPUTE : GLSLANG_STAGE_VERTEX;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_0;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_0;
    in.code = glsl;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();
    sh = glslang_shader_create(&in);
    if (!sh)
        return false;
    if (!glslang_shader_preprocess(sh, &in) || !glslang_shader_parse(sh, &in)) {
        SDL_Log("gfx: %s %016llx: %s", what, (unsigned long long)key, glslang_shader_get_info_log(sh));
        goto out;
    }
    prog = glslang_program_create();
    glslang_program_add_shader(prog, sh);
    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        SDL_Log("gfx: %s %016llx: %s", what, (unsigned long long)key, glslang_program_get_info_log(prog));
        goto out;
    }
    glslang_program_SPIRV_generate(prog, in.stage);
    *bytes = glslang_program_SPIRV_get_size(prog) * 4;
    *spv = malloc(*bytes);
    if (*spv) {
        glslang_program_SPIRV_get(prog, *spv);
        ok = true;
    }
out:
    if (prog)
        glslang_program_delete(prog);
    glslang_shader_delete(sh);
    return ok;
}

static entry *find(uint64_t key)
{
    for (uint32_t i = 0; i < nentries; i++)
        if (entries[i].key == key)
            return &entries[i];
    return NULL;
}

static int SDLCALL work(void *unused)
{
    (void)unused;
    glslang_initialize_process();
    tidy_cache();
    SDL_LockMutex(lock);
    while (!quitting) {
        entry *e = NULL;
        for (uint32_t i = 0; i < nentries && !e; i++)
            if (entries[i].state == 0)
                e = &entries[i];
        if (!e) {
            SDL_WaitCondition(wake, lock);
            continue;
        }
        {
            uint64_t key = e->key, t0 = SDL_GetTicksNS();
            uint32_t *code = e->code, count = e->count, *spv = NULL;
            int kind = e->kind;
            size_t bytes = 0;
            bool ok;
            char *glsl;

            e->code = NULL;     /* ours now; e may move while unlocked */
            SDL_UnlockMutex(lock);
            ok = load(key, &spv, &bytes);
            glsl = ok ? NULL : kind == 1 ? pdo_fs_spec_glsl((const int32_t *)code)
                 : kind == 2 ? comp_glsl(code, count) : pdo_vsh_spec_glsl(code, count);
            if (glsl) {
                ok = compile(glsl, &spv, &bytes, key, kind);
                free(glsl);
                if (ok)
                    save(key, spv, bytes);
                if (n_logged++ < 96) {
                    if (kind == 2)
                        SDL_Log("gfx: vertex program %016llx (%u instructions) %s in %.0f ms",
                                (unsigned long long)key, count, ok ? "made into its compute shader" : "left without one",
                                (double)(SDL_GetTicksNS() - t0) / 1e6);
                    else if (kind)
                        SDL_Log("gfx: fragment setup %016llx (%s) %s in %.0f ms", (unsigned long long)key,
                                code[B_COMB] ? "pixel shader" : "texture stages",
                                ok ? "made into its own shader" : "left to the interpreter",
                                (double)(SDL_GetTicksNS() - t0) / 1e6);
                    else
                        SDL_Log("gfx: vertex program %016llx (%u instructions) %s in %.0f ms",
                                (unsigned long long)key, count,
                                ok ? "made into its own shader" : "left to the interpreter",
                                (double)(SDL_GetTicksNS() - t0) / 1e6);
                }
            }
            free(code);
            SDL_LockMutex(lock);
            e = find(key);
            e->state = ok ? 1 : -1;
            e->spv = spv;
            e->bytes = bytes;
            n_pending--;
            if (ok)
                n_made++;
            else
                n_failed++;
            ms_made += (double)(SDL_GetTicksNS() - t0) / 1e6;
        }
    }
    SDL_UnlockMutex(lock);
    glslang_finalize_process();
    return 0;
}

bool pdo_vsh_spec_enabled(void)
{
    if (!enabled_known) {
        const char *e = getenv("PDO_VSH_SPEC");
        enabled = !(e && e[0] == '0');
        enabled_known = true;
        if (!enabled)
            SDL_Log("gfx: vertex programs interpreted on the GPU (PDO_VSH_SPEC=0)");
    }
    return enabled;
}

static bool start(void)
{
    if (worker)
        return true;
    if (!lock)
        lock = SDL_CreateMutex();
    if (!wake)
        wake = SDL_CreateCondition();
    if (!lock || !wake)
        return false;
    worker = SDL_CreateThread(work, "vsh_spec", NULL);
    return worker != NULL;
}

static entry *add(uint64_t key)
{
    if (nentries == cap_entries) {
        uint32_t cap = cap_entries ? cap_entries * 2 : 64;
        entry *n = realloc(entries, cap * sizeof *n);
        if (!n)
            return NULL;
        entries = n;
        cap_entries = cap;
    }
    memset(&entries[nentries], 0, sizeof *entries);
    entries[nentries].key = key;
    return &entries[nentries++];
}

int pdo_vsh_spec_get(uint64_t key, const uint32_t *code, uint32_t count,
                     const uint32_t **spv, size_t *bytes)
{
    entry *e;
    int state;

    if (!pdo_vsh_spec_enabled() || !start())
        return -1;
    SDL_LockMutex(lock);
    e = find(key);
    if (!e && (e = add(key)) != NULL) {
        e->code = malloc((size_t)count * 32);
        if (e->code) {
            memcpy(e->code, code, (size_t)count * 32);
            e->count = count;
            n_pending++;
            SDL_SignalCondition(wake);
        } else {
            e->state = -1;
        }
    }
    state = e ? e->state : -1;
    if (state == 1) {
        *spv = e->spv;
        *bytes = e->bytes;
    }
    SDL_UnlockMutex(lock);
    return state;
}

int pdo_vsh_comp_get(uint64_t vkey, const uint32_t *code, uint32_t count, const uint32_t **spv, size_t *bytes)
{
    uint64_t key = pdo_vsh_comp_key(vkey);
    entry *e;
    int state;

    if (!pdo_vsh_spec_enabled() || !start())
        return -1;
    SDL_LockMutex(lock);
    e = find(key);
    if (!e && (e = add(key)) != NULL) {
        e->kind = 2;
        e->code = malloc((size_t)count * 32);
        if (e->code) {
            memcpy(e->code, code, (size_t)count * 32);
            e->count = count;
            n_pending++;
            SDL_SignalCondition(wake);
        } else {
            e->state = -1;
        }
    }
    state = e ? e->state : -1;
    if (state == 1) {
        *spv = e->spv;
        *bytes = e->bytes;
    }
    SDL_UnlockMutex(lock);
    return state;
}

int pdo_fs_spec_get(uint64_t key, const int32_t *w, const uint32_t **spv, size_t *bytes)
{
    entry *e;
    int state;

    if (!pdo_fs_spec_enabled() || !start())
        return -1;
    SDL_LockMutex(lock);
    e = find(key);
    if (!e && (e = add(key)) != NULL) {
        e->kind = 1;
        e->code = malloc(PDO_FS_SPEC_WORDS * sizeof *w);
        if (e->code) {
            memcpy(e->code, w, PDO_FS_SPEC_WORDS * sizeof *w);
            n_pending++;
            SDL_SignalCondition(wake);
        } else {
            e->state = -1;
        }
    }
    state = e ? e->state : -1;
    if (state == 1) {
        *spv = e->spv;
        *bytes = e->bytes;
    }
    SDL_UnlockMutex(lock);
    return state;
}

bool pdo_fs_spec_enabled(void)
{
    static int known = -1;
    if (known < 0) {
        const char *e = getenv("PDO_FS_SPEC");
        known = !(e && e[0] == '0');
        if (!known)
            SDL_Log("gfx: fragment setups interpreted on the GPU (PDO_FS_SPEC=0)");
    }
    return known && pdo_vsh_spec_enabled();
}

bool pdo_vsh_spec_find(uint64_t key, const uint32_t **spv, size_t *bytes)
{
    entry *e;
    bool ok = false;

    if (!pdo_vsh_spec_enabled() || !start())
        return false;
    SDL_LockMutex(lock);
    e = find(key);
    if (!e) {
        uint32_t *s;
        size_t n;
        SDL_UnlockMutex(lock);
        if (!load(key, &s, &n))
            return false;
        SDL_LockMutex(lock);
        e = find(key);
        if (!e && (e = add(key)) != NULL) {
            e->state = 1;
            e->spv = s;
            e->bytes = n;
        } else {
            free(s);
        }
    }
    if (e && e->state == 1) {
        *spv = e->spv;
        *bytes = e->bytes;
        ok = true;
    }
    SDL_UnlockMutex(lock);
    return ok;
}

void pdo_vsh_spec_counts(uint32_t *made, uint32_t *failed, uint32_t *pending, double *ms)
{
    if (!lock) {
        *made = *failed = *pending = 0;
        *ms = 0;
        return;
    }
    SDL_LockMutex(lock);
    *made = n_made;
    *failed = n_failed;
    *pending = n_pending;
    *ms = ms_made;
    SDL_UnlockMutex(lock);
}

void pdo_vsh_spec_quit(void)
{
    if (!worker)
        return;
    SDL_LockMutex(lock);
    quitting = true;
    SDL_SignalCondition(wake);
    SDL_UnlockMutex(lock);
    SDL_WaitThread(worker, NULL);
    worker = NULL;
}

#else   /* without glslang: the interpreter only */

bool pdo_vsh_spec_enabled(void) { return false; }

int pdo_vsh_spec_get(uint64_t key, const uint32_t *code, uint32_t count,
                     const uint32_t **spv, size_t *bytes)
{
    (void)key; (void)code; (void)count; (void)spv; (void)bytes;
    return -1;
}

bool pdo_vsh_spec_find(uint64_t key, const uint32_t **spv, size_t *bytes)
{
    (void)key; (void)spv; (void)bytes;
    return false;
}

void pdo_vsh_spec_counts(uint32_t *made, uint32_t *failed, uint32_t *pending, double *ms)
{
    *made = *failed = *pending = 0;
    *ms = 0;
}

void pdo_vsh_spec_quit(void) {}

bool pdo_fs_spec_enabled(void) { return false; }

uint64_t pdo_vsh_comp_key(uint64_t vkey) { return vkey ^ 1; }

int pdo_vsh_comp_get(uint64_t vkey, const uint32_t *code, uint32_t count, const uint32_t **spv, size_t *bytes)
{
    (void)vkey; (void)code; (void)count; (void)spv; (void)bytes;
    return -1;
}

int pdo_fs_spec_get(uint64_t key, const int32_t *w, const uint32_t **spv, size_t *bytes)
{
    (void)key; (void)w; (void)spv; (void)bytes;
    return -1;
}

#endif

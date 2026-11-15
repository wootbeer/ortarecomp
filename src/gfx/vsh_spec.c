/* Vertex programs as their own GPU shaders (vsh_spec.h). The generated
 * run_spec does what vsh.vert's run() does with the same code, instruction
 * by instruction; run() is the reference, and a change there is a change
 * here. */
#include "gfx/vsh_spec.h"
#include "gfx/shaders_spv.h"
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

static char *gen(const uint32_t *code, uint32_t count)
{
    sbuf b = {0};
    bool v_used[16] = {0}, rel_any = false;
    const char *src = vsh_vert_src;
    const char *nl = strchr(src, '\n');

    /* #version stays first. */
    put(&b, "%.*s", (int)(nl - src + 1), src);
    put(&b, "#define PDO_VSH_SPEC 1\n");
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

char *pdo_vsh_spec_glsl(const uint32_t *code, uint32_t count)
{
    return code_valid(code, count) ? gen(code, count) : NULL;
}

/* FNV-1a, 64 bits. */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *s = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ s[i]) * 0x100000001B3ull;
    return h;
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
    uint32_t *code, count;      /* while waiting */
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
    snprintf(want, sizeof want, "%016llx", (unsigned long long)pdo_vsh_spec_key(NULL, 0));
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
        SDL_Log("gfx: %d vertex program shaders of another build removed", n);
}

static bool compile(const char *glsl, uint32_t **spv, size_t *bytes, uint64_t key)
{
    glslang_input_t in;
    glslang_shader_t *sh;
    glslang_program_t *prog = NULL;
    bool ok = false;

    memset(&in, 0, sizeof in);
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = GLSLANG_STAGE_VERTEX;
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
        SDL_Log("gfx: vertex program %016llx: %s", (unsigned long long)key, glslang_shader_get_info_log(sh));
        goto out;
    }
    prog = glslang_program_create();
    glslang_program_add_shader(prog, sh);
    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        SDL_Log("gfx: vertex program %016llx: %s", (unsigned long long)key, glslang_program_get_info_log(prog));
        goto out;
    }
    glslang_program_SPIRV_generate(prog, GLSLANG_STAGE_VERTEX);
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
            size_t bytes = 0;
            bool ok;
            char *glsl;

            e->code = NULL;     /* ours now; e may move while unlocked */
            SDL_UnlockMutex(lock);
            ok = load(key, &spv, &bytes);
            if (!ok && (glsl = pdo_vsh_spec_glsl(code, count)) != NULL) {
                ok = compile(glsl, &spv, &bytes, key);
                free(glsl);
                if (ok)
                    save(key, spv, bytes);
                if (n_logged++ < 64)
                    SDL_Log("gfx: vertex program %016llx (%u instructions) %s in %.0f ms", (unsigned long long)key,
                            count, ok ? "made into its own shader" : "left to the interpreter",
                            (double)(SDL_GetTicksNS() - t0) / 1e6);
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

#endif

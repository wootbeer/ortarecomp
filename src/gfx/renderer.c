/* SDL3 GPU renderer for the Direct3D replacement. See renderer.h. */

#include "gfx/renderer.h"

#include "audio/mixer.h"
#include "config.h"
#include "input_map.h"
#include "gfx/overlay.h"

#include "gfx/shaders_spv.h"
#include "gfx/xtex.h"
#ifdef PDO_HAVE_LSFG
#include "gfx/lsfg_present.h"
#endif
#include "guest/vsh.h"      /* pdo_parallel */

#include <SDL3/SDL.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#if defined(_MSC_VER)
#define PDO_TLS __declspec(thread)
#else
#define PDO_TLS _Thread_local
#endif

#define WINDOW_TITLE   "Orta Recomp"
#define MAX_PIPELINES  1024
#define MAX_SAMPLERS   64
#define MAX_TEXTURES   4096

extern ptrdiff_t g_xbox_mem_offset;
extern void pdo_dump_threads(void);   /* src/guest/crash_report.c: F11 */
extern void pdo_dump_guest_waits(void);  /* src/guest/wait_trace.c */

static SDL_GPUDevice *dev;
static SDL_Window *window;
static SDL_ThreadID window_thread;
static bool have_swapchain;
/* Lossless Scaling frame generation (lsfg_present.cpp): each game frame's
 * window pass is drawn into ls_tex, and the frames generated between it
 * and the last are shown before it. */
static bool lsfg_on;
static SDL_GPUTexture *ls_tex;
static Uint32 ls_w, ls_h;
static bool ls_draw;            /* present_to_window: into ls_tex, not the window */
static bool ls_drawn;           /* ... and it drew a picture there */
static SDL_GPUTexture *frame_color, *frame_depth, *white;
static SDL_GPUTextureFormat depth_format;
static int frame_w, frame_h;
static uint64_t frames;
static SDL_GPUShader *vs, *fs, *fs_ps, *vsh_vs, *fvf_vs;

/* Overlay settings in force: every render target is rscale times the game's
 * size (viewports and copies scaled to match), drawn with nsamples samples
 * into a multisampled buffer (ms) that resolves into the texture the game
 * samples. A target whose resolved texture was written directly (a copy, a
 * new target) is stale: its multisampled buffer is seeded from it before the
 * next draw. */
static float rscale = 1.0f;         /* 1, 1.5, 2 .. 8 */
static int nsamples = 1;

/* A length in the game's pixels, in ours. */
static int sc(int v)
{
    return (int)lroundf((float)v * rscale);
}

/* Shadows (Options > Display): Low keeps shadow maps (depth-only targets)
 * at the game's size, lighter on the GPU at high scales; High scales them
 * with everything else. Read by choose_quality. */
static bool shadow_native;

/* A target's size in our pixels. */
static uint32_t target_px(uint32_t v, bool depth_only)
{
    return depth_only && shadow_native ? v : (uint32_t)sc((int)v);
}
static SDL_GPUSampleCount sample_count = SDL_GPU_SAMPLECOUNT_1;
static SDL_GPUTexture *frame_ms;
static bool frame_stale;
static volatile bool paused;        /* the overlay menu holds the game */
/* The overlay's holds, for the vblank clock (d3d_hle.c) to stop with the
 * game: when the current one began (ms, | 1 so never 0; 0: none), and the
 * ones before, in all. */
/* Time the menu held the game, taken off the game's vblank clock. Read and
 * written under pause_lock, so a reader never sees a start that is after
 * its own "now" (once a -1 ms that wrapped to 49 days and sent the clock to
 * before vblank 0, which hung the game's Swap for good) nor a pause counted
 * twice, in the total and still as running. */
static SDL_SpinLock pause_lock;
static Uint64 pause_start_ns, paused_total_ns;
static SDL_AtomicInt pause_on;

/* Window pass: the frame (aspect, softening) and the overlay over it; and the
 * seed pass that copies a resolved target back into its multisampled one. */
static SDL_GPUGraphicsPipeline *screen_pipeline, *overlay_pipeline, *seed_pipeline;
static SDL_GPUSampler *linear;
static SDL_GPUTexture *overlay_tex;

/* Present pass: frame -> gamma ramp -> output, then output -> window. */
static SDL_GPUTexture *output, *gamma_lut;
static SDL_GPUGraphicsPipeline *present_pipeline;
static SDL_GPUSampler *nearest;
static uint8_t gamma_rgba[256 * 4];
static bool gamma_dirty = true;

static const char *shot_dir;
static int shot_every = 1;

/* F11 dumps every thread's stack (hangs). F12 (or PDO_CAPTURE=frame): log every command of the next frame
 * ("capture:" lines) and save it as capture_<frame>.bmp in the working
 * directory. */
static uint64_t capture_frame = UINT64_MAX;
/* PDO_CAPTURE_SEQ=n: PDO_CAPTURE_INTERP's pictures for n frames from the
 * capture frame on (none of them dropped), to see motion. */
static uint64_t capture_seq = 1;
static bool programs_switched;      /* match_frames: many draws on other programs (boost or brake end) */
static bool viewport_changed;       /* match_frames: the scene's render size changed (boost or brake end) */

/* ── Frame recording ──────────────────────────────────────────────────── */

enum { CMD_DRAW, CMD_CLEAR, CMD_TARGET, CMD_COPY };

typedef struct command {
    uint8_t  kind;
    /* target (surf), copy (surf to surf2, rect: x, y, w, h, dx, dy) */
    pdo_surface surf, surf2;
    int      rect[6];
    /* clear */
    uint32_t clear_flags, clear_color, clear_stencil;
    float    clear_z;
    /* draw */
    pdo_draw draw;
    uint32_t first_index, index_count;
    /* a vertex-program draw on the GPU (pdo_gfx_draw_vsh); has_cpu: the CPU
     * interpreter's vertices are recorded too, at first_index */
    bool     vsh, has_cpu;
    int32_t  fvf;               /* or a fixed-function one: its uniforms in fvf_us, else -1 */
    uint32_t vsh_first_index, vsh_index_count;
    struct vsh_uniforms {
        int32_t info[4];                /* program base, count, constant base, - */
        int32_t inputs[16][4];          /* byte base in the stream blob, stride, offset, type */
        float   scale[4], offset[4];
        /* frame interpolation (match_frames): see shaders/vsh.vert */
        int32_t interp[4];
        int32_t prev_in[16];
        float   blend_t[4];
        /* paired with a draw on another program (count 0: the same): its
         * program base and count. Then each runs as drawn -- the earlier
         * program on the earlier frame's constants, this one on this
         * frame's -- and what they output is blended (shaders/vsh.vert). */
        int32_t prev_prog[4];
        /* the paired draw's viewport transform (its scale, offset): the
         * game renders a boost's and a brake's end at another size (426x320
         * after 512x360 at 0,12), and the earlier frame's positions are in
         * its own pixels */
        float   prev_scale[4], prev_offset[4];
    } vu;
    /* What the draw drew from, to find it again in the next frame: the
     * host address of its (first) vertex data and its vertex count. */
    uintptr_t id_src;
    uint32_t id_nv;
    /* CPU-made vertices: where the draw's start in verts, and the paired
     * draw's in the previous frame (-1: none); unpaired: for the tint. */
    uint32_t vbase;
    int32_t prev_vbase;
    bool unpaired;
    /* Fixed-function: how its matrices (wvp, then world) moved since its
     * partner in the frame before (has_mvel), to guess where it is next
     * (draw_distance). */
    bool has_mvel;
    float mvel[32];
    uint32_t pair_prev;     /* the paired draw in the frame before (match_frames) */
    uint8_t  consts_read[24]; /* vertex programs: a bit per c register read (pdo_gpu_vsh) */
    float    anchor[8];       /* vertex programs: oPos of the first and last vertex (pdo_gpu_vsh) */
    bool     anchored;
    bool strayed;           /* its vertices strayed over a fifth of its size from its partner's */
    bool no_partner;        /* match_frames: its group had none left for it (not a test's refusal) */
} command;

/* ── Threads ──────────────────────────────────────────────────────────────
 * The game runs on a thread of its own; this file's GPU work all happens on
 * the main thread, which owns the window. The game thread records a frame
 * into a frame_data and, at Swap, resolves its textures (while guest memory
 * still holds that frame's) and hands it over; the main thread uploads,
 * replays and presents it. Five frame_datas go round: the one being
 * recorded, the latest handed over, the two the main thread holds (the
 * newest it took, and the one before for frame interpolation), and a spare.
 * The game never waits for drawing: a frame handed over while the one
 * before was not taken yet replaces it. F is the one this thread works on. */

typedef struct gtex gtex;
typedef struct pending_upload pending_upload;

/* A draw's texture stage as resolved on the game thread: a texture, or a
 * render target (target_w not 0) that the main thread looks up by its
 * memory and size, where it lives. */
typedef struct tex_slot {
    SDL_GPUTexture *tex;
    uint32_t target_data, target_w, target_h;
} tex_slot;

typedef struct frame_data {
    uint64_t frame;
    command *cmds;
    uint32_t ncmds, cap_cmds;
    pdo_vertex *verts;
    uint32_t nverts, cap_verts;
    uint32_t *indices;
    uint32_t nindices, cap_indices;
    /* Vertex-program and fixed-function draws on the GPU: their stream
     * bytes, constant sets and uniforms. */
    uint8_t *blob;
    uint32_t nblob, cap_blob;
    float (*vconsts)[4];
    uint32_t nvconsts, cap_vconsts;
    int32_t last_consts;
    pdo_fvf_uniforms *fvf_us;
    uint32_t nfvf_us, cap_fvf_us;
    /* Resolved at Swap: four slots a command, texels to upload. */
    tex_slot *tex;
    uint32_t cap_tex;
    uint8_t *staging;
    uint32_t staging_len, staging_cap;
    pending_upload *ups;
    int nups;
    /* Every vertex program so far, when one was added since the last
     * frame handed over; the gamma ramp, when it changed. */
    uint32_t *progs;
    uint32_t nprogs, cap_progs;
    bool progs_new, gamma_new;
    uint8_t gamma[256 * 4];
    Uint64 stamp;           /* when it was handed over */
    /* Main thread: the frame's own GPU copies, uploaded once (on_gpu) and
     * used for every frame shown from it. */
    SDL_GPUBuffer *gvb, *gib, *gblob, *gconst;
    Uint32 gvb_size, gib_size, gblob_size, gconst_size;
    bool on_gpu;
} frame_data;

static frame_data pool[5];
/* F: the frame being recorded (rec_F) -- one for all the game's threads,
 * which take turns under the guest CPU lock: kept per thread, a second
 * game thread that draws went on with a slot of its own from long before,
 * one the main thread had drawn since, and that frame's texture uploads
 * were skipped as already done (the black dragon, the rainbow sky) -- or,
 * while the main thread draws (draw_frame, capture_interp), the frame it
 * draws (draw_F). */
static frame_data *rec_F, *draw_F;
static PDO_TLS bool tls_drawing;
#define F (*(tls_drawing ? &draw_F : &rec_F))

static SDL_Mutex *q_lock;
static SDL_Condition *q_cond;
static frame_data *ready;           /* handed over, not drawn yet */
static uint32_t frames_dropped;     /* replaced before they were drawn */
static uint32_t drops_total;        /* the same, never reset (main loop's check) */
static uint32_t frames_drawn;       /* by the main thread */

void pdo_gfx_take_frame_counts(uint32_t *drawn, uint32_t *dropped)
{
    SDL_LockMutex(q_lock);
    *drawn = frames_drawn;
    *dropped = frames_dropped;
    frames_drawn = frames_dropped = 0;
    SDL_UnlockMutex(q_lock);
}
static frame_data *held_cur;        /* the main thread's newest */
static frame_data *held_prev;       /* and the one before it */
static bool game_done;              /* the game thread has ended */
static int init_w, init_h, init_state;  /* pdo_gfx_init asked of the main thread:
                                         * 1 asked, 2 done, 3 failed */
static SDL_ThreadID main_thread;

static bool on_main_thread(void)
{
    return !main_thread || SDL_GetCurrentThreadID() == main_thread;
}

static bool init_here(int width, int height);

/* Every program compiled so far (game thread). */
static uint32_t *progs;
static uint32_t nprogs, cap_progs;
static bool progs_dirty;
static bool cpu_vsh;        /* captures only: GPU-vertex draws drawn from the CPU's vertices */

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return p;
    while (*cap < need)
        *cap = *cap ? *cap * 2 : 1024;
    return SDL_realloc(p, (size_t)*cap * elem);
}

/* Forget a frame's commands and what they recorded. */
static void reset_frame(frame_data *f)
{
    f->nverts = f->nindices = f->ncmds = 0;
    f->nblob = f->nvconsts = f->nfvf_us = 0;
    f->last_consts = -1;
    f->staging_len = 0;
    f->nups = 0;
    f->progs_new = f->gamma_new = false;
    f->on_gpu = false;
}

/* The game thread records into pool[0] first. */
#define RECORDING() do { if (!F) { F = &pool[0]; reset_frame(F); } } while (0)

static command *new_command(void)
{
    F->cmds = grow(F->cmds, &F->cap_cmds, F->ncmds + 1, sizeof *F->cmds);
    memset(&F->cmds[F->ncmds], 0, sizeof *F->cmds);
    F->cmds[F->ncmds].fvf = -1;
    return &F->cmds[F->ncmds++];
}

/* Bytes the vertex shaders read, 4-aligned with 8 bytes of slack past the
 * end for their word reads. Returns where they start. */
static uint32_t add_blob(const uint8_t *data, uint32_t n)
{
    uint32_t at;

    F->nblob = (F->nblob + 3) & ~3u;
    F->blob = grow(F->blob, &F->cap_blob, F->nblob + n + 8, 1);
    memcpy(F->blob + F->nblob, data, n);
    at = F->nblob;
    F->nblob += n;
    memset(F->blob + F->nblob, 0, 8);
    return at;
}

void pdo_gfx_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
    RECORDING();
    command *c = new_command();
    c->kind = CMD_CLEAR;
    c->clear_flags = flags;
    c->clear_color = color;
    c->clear_z = z;
    c->clear_stencil = stencil;
}

static uintptr_t draw_src;     /* pdo_gfx_set_draw_source, for the next draw */

void pdo_gfx_draw(const pdo_draw *d, const pdo_vertex *v, uint32_t nv,
                  const uint32_t *idx, uint32_t ni)
{
    command *c;

    RECORDING();

    if (!nv || !ni)
        return;
    F->verts = grow(F->verts, &F->cap_verts, F->nverts + nv, sizeof *F->verts);
    F->indices = grow(F->indices, &F->cap_indices, F->nindices + ni, sizeof *F->indices);
    memcpy(F->verts + F->nverts, v, nv * sizeof *v);
    for (uint32_t i = 0; i < ni; i++)
        F->indices[F->nindices + i] = F->nverts + idx[i];

    {
        /* PDO_GFX_DEBUG=n: describe the draws of frame n. */
        static long debug_frame = -2;
        static long debug_every;
        if (debug_frame == -2) {
            /* PDO_GFX_DEBUG=n, or n/every to repeat every 'every' frames */
            const char *v = getenv("PDO_GFX_DEBUG");
            debug_frame = v ? atol(v) : -1;
            debug_every = v && strchr(v, '/') ? atol(strchr(v, '/') + 1) : 0;
        }
        if ((long)frames == debug_frame ||
            (debug_every > 0 && debug_frame >= 0 && (long)frames > debug_frame &&
             ((long)frames - debug_frame) % debug_every == 0)) {
            pdo_xtex_info ti;
            pdo_xtex_describe(d->tex[0].format_word, d->tex[0].size_word, &ti);
            SDL_Log("gfx draw @%llu: %u F->verts %u idx topo %u blend %u %x/%x z %u tex0 %08x %s %ux%u "
                    "cop0 %d/%x/%x aop0 %d/%x/%x vp %.0f,%.0f %.0fx%.0f",
                    (unsigned long long)frames, nv, ni, d->ps.topology, d->ps.blend_enable, d->ps.src_blend, d->ps.dst_blend,
                    d->ps.z_enable, d->tex[0].data, pdo_xtex_format_name(ti.format), ti.width, ti.height,
                    d->ff.color_op[0][0], d->ff.color_op[0][1], d->ff.color_op[0][2],
                    d->ff.alpha_op[0][0], d->ff.alpha_op[0][1], d->ff.alpha_op[0][2],
                    d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3]);
            for (uint32_t i = 0; i < nv && i < 4; i++)
                SDL_Log("gfx   v%u pos %.3f %.3f %.3f %.3f diffuse %08x tc0 %.3f %.3f", i,
                        v[i].pos[0], v[i].pos[1], v[i].pos[2], v[i].pos[3], v[i].diffuse,
                        v[i].tc[0][0], v[i].tc[0][1]);
        }
    }
    c = new_command();
    c->draw = *d;
    c->first_index = F->nindices;
    c->index_count = ni;
    c->vbase = F->nverts;
    c->id_src = draw_src;
    c->id_nv = nv;
    draw_src = 0;
    F->nverts += nv;
    F->nindices += ni;
}

void pdo_gfx_set_draw_source(const void *src)
{
    draw_src = (uintptr_t)src;
}

int32_t pdo_gfx_vsh_program(const uint32_t *code, uint32_t count)
{
    uint32_t base = nprogs / 4;     /* in uvec4 */

    if (!count || !vsh_vs)
        return -1;
    progs = grow(progs, &cap_progs, nprogs + count * 8, sizeof *progs);
    memcpy(progs + nprogs, code, (size_t)count * 32);
    nprogs += count * 8;
    progs_dirty = true;
    return (int32_t)base;
}

void pdo_gfx_draw_vsh(const pdo_draw *d, const pdo_gpu_vsh *g, uint32_t nv,
                      const uint32_t *idx, uint32_t ni, const pdo_vertex *cpu)
{
    uint32_t sbase[16] = { 0 };
    int32_t consts;
    command *c;

    RECORDING();

    if (!nv || !ni || g->program < 0)
        return;
    if (cpu) {
        pdo_gfx_draw(d, cpu, nv, idx, ni);
        c = &F->cmds[F->ncmds - 1];
        c->has_cpu = true;
    } else {
        c = new_command();
        c->draw = *d;
    }
    c->vsh = true;
    c->id_nv = nv;
    c->anchored = g->anchored;
    memcpy(c->anchor, g->anchor, sizeof c->anchor);
    if (g->consts_read)
        memcpy(c->consts_read, g->consts_read, sizeof c->consts_read);
    else
        memset(c->consts_read, 0xFF, sizeof c->consts_read);

    /* The streams, 4-aligned; 8 bytes of slack past the end for the
     * shader's word reads. */
    for (int k = 0; k < 16; k++)
        if (g->stream_bytes[k] && g->stream_data[k]) {
            sbase[k] = add_blob(g->stream_data[k], g->stream_bytes[k]);
            if (!c->id_src)
                c->id_src = (uintptr_t)g->stream_data[k];
        }

    /* The constants, shared with the draw before when they are the same. */
    if (F->last_consts >= 0 && !memcmp(F->vconsts[F->last_consts], g->constants, 192 * 16)) {
        consts = F->last_consts;
    } else {
        F->vconsts = grow(F->vconsts, &F->cap_vconsts, F->nvconsts + 192, sizeof *F->vconsts);
        memcpy(F->vconsts[F->nvconsts], g->constants, 192 * 16);
        consts = (int32_t)F->nvconsts;
        F->last_consts = consts;
        F->nvconsts += 192;
    }

    c->vu.info[0] = g->program;
    c->vu.info[1] = (int32_t)g->count;
    c->vu.info[2] = consts;
    for (int r = 0; r < 16; r++) {
        int32_t st = g->input[r][0];
        if (st < 0 || st > 15 || !g->input[r][3] || !g->stream_bytes[st]) {
            memset(c->vu.inputs[r], 0, sizeof c->vu.inputs[r]);
            continue;
        }
        c->vu.inputs[r][0] = (int32_t)sbase[st];
        c->vu.inputs[r][1] = g->input[r][1];
        c->vu.inputs[r][2] = g->input[r][2];
        c->vu.inputs[r][3] = g->input[r][3];
    }
    memcpy(c->vu.scale, g->vp_scale, sizeof c->vu.scale);
    memcpy(c->vu.offset, g->vp_offset, sizeof c->vu.offset);

    F->indices = grow(F->indices, &F->cap_indices, F->nindices + ni, sizeof *F->indices);
    memcpy(F->indices + F->nindices, idx, ni * sizeof *idx);
    c->vsh_first_index = F->nindices;
    c->vsh_index_count = ni;
    F->nindices += ni;
}

bool pdo_gfx_draw_fvf(const pdo_draw *d, const pdo_gpu_fvf *g, uint32_t nv,
                      const uint32_t *idx, uint32_t ni, const pdo_vertex *cpu)
{
    command *c;

    RECORDING();

    if (!fvf_vs || !nv || !ni || !g->data)
        return false;
    if (cpu) {
        pdo_gfx_draw(d, cpu, nv, idx, ni);
        c = &F->cmds[F->ncmds - 1];
        c->has_cpu = true;
    } else {
        c = new_command();
        c->draw = *d;
    }
    F->fvf_us = grow(F->fvf_us, &F->cap_fvf_us, F->nfvf_us + 1, sizeof *F->fvf_us);
    /* An unlit draw's lights and material are never read: leave them. */
    memcpy(&F->fvf_us[F->nfvf_us], &g->u, (g->u.layout0[2] & PDO_FVF_LIT) ? sizeof g->u
                                    : offsetof(pdo_fvf_uniforms, material));
    F->fvf_us[F->nfvf_us].layout0[0] = (int32_t)add_blob(g->data, g->bytes);
    c->fvf = (int32_t)F->nfvf_us++;
    c->id_src = (uintptr_t)g->data;
    c->id_nv = nv;
    F->indices = grow(F->indices, &F->cap_indices, F->nindices + ni, sizeof *F->indices);
    memcpy(F->indices + F->nindices, idx, ni * sizeof *idx);
    c->vsh_first_index = F->nindices;
    c->vsh_index_count = ni;
    F->nindices += ni;
    return true;
}

bool pdo_gfx_capturing(void)
{
    return frames + 1 == capture_frame;
}

/* ── Pipelines and samplers ───────────────────────────────────────────── */

static SDL_GPUBlendFactor blend_factor(uint16_t gl)
{
    switch (gl) {
    case 0x000: return SDL_GPU_BLENDFACTOR_ZERO;
    case 0x001: return SDL_GPU_BLENDFACTOR_ONE;
    case 0x300: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 0x301: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 0x303: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case 0x305: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 0x307: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case 0x8002: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    default:    return SDL_GPU_BLENDFACTOR_ONE;
    }
}

static SDL_GPUBlendOp blend_op(uint16_t gl)
{
    switch (gl) {
    case 0x800A: return SDL_GPU_BLENDOP_SUBTRACT;
    case 0x800B: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case 0x8007: return SDL_GPU_BLENDOP_MIN;
    case 0x8008: return SDL_GPU_BLENDOP_MAX;
    default:     return SDL_GPU_BLENDOP_ADD;
    }
}

static SDL_GPUStencilOp stencil_op(uint16_t gl)
{
    switch (gl) {
    case 0x0000: return SDL_GPU_STENCILOP_ZERO;
    case 0x1E01: return SDL_GPU_STENCILOP_REPLACE;
    case 0x1E02: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
    case 0x1E03: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
    case 0x150A: return SDL_GPU_STENCILOP_INVERT;
    case 0x8507: return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
    case 0x8508: return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
    default:     return SDL_GPU_STENCILOP_KEEP;     /* 0x1E00 */
    }
}

static SDL_GPUCompareOp compare_op(uint16_t gl)
{
    switch (gl) {
    case 0x200: return SDL_GPU_COMPAREOP_NEVER;
    case 0x201: return SDL_GPU_COMPAREOP_LESS;
    case 0x202: return SDL_GPU_COMPAREOP_EQUAL;
    case 0x203: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case 0x204: return SDL_GPU_COMPAREOP_GREATER;
    case 0x205: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case 0x206: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default:    return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

static struct { pdo_pipeline_state key; SDL_GPUGraphicsPipeline *p; } pipelines[MAX_PIPELINES];
static int npipelines;

/* Making a pipeline mid-frame can take the driver tens of milliseconds, a
 * visible hitch the first time an effect appears. Every state combination
 * met is appended to pipelines.bin in the data folder, and the next start
 * (or a change of anti-aliasing) makes them all up front. The file is
 * only a list of our own keys: a different build or a bad file is ignored. */
#define PIPELINE_FILE_MAGIC 0x4C505044u /* "DPPL" */
static bool warming;                     /* making the saved ones: do not save again */
static Uint64 pipeline_ns;               /* time making them mid-game, for the timing log */
static uint32_t pipelines_made;

static void pipeline_file(char *path, size_t n)
{
    snprintf(path, n, "%spipelines.bin", pdo_data_dir());
}

static void save_pipeline_key(const pdo_pipeline_state *ps)
{
    char path[PDO_PATH_MAX];
    SDL_PathInfo info;
    SDL_IOStream *io;

    pipeline_file(path, sizeof path);
    if (!SDL_GetPathInfo(path, &info) || info.size == 0) {
        uint32_t head[2] = { PIPELINE_FILE_MAGIC, (uint32_t)sizeof *ps };
        io = SDL_IOFromFile(path, "wb");
        if (!io)
            return;
        SDL_WriteIO(io, head, sizeof head);
    } else {
        io = SDL_IOFromFile(path, "ab");
        if (!io)
            return;
    }
    SDL_WriteIO(io, ps, sizeof *ps);
    SDL_CloseIO(io);
}

/* Reversed depth (depth_reversed in ortarecomp.ini, read at start; Concept
 * of Love): the depth buffer holds 1 - z, so the far end keeps float
 * precision instead of crowding against 1. Float depth near z = 1 rounds
 * about as coarsely as one 24-bit step, and layers the game puts a step
 * apart lose their ties (JSRF: a menu icon's fill beat its picture, and
 * distant rails z-fought). Everything that makes or reads depth follows:
 * the shaders write the reversed value (computed without losing
 * precision), compares, clears, bias and the viewport's depth range are
 * mirrored, and shadow maps are read back un-reversed (ff.frag). */
static bool depth_rev;

bool pdo_gfx_depth_reversed(void)
{
    return depth_rev;
}

static SDL_GPUGraphicsPipeline *get_pipeline(const pdo_pipeline_state *ps);

static void warm_pipelines(void)
{
    char path[PDO_PATH_MAX];
    size_t size = 0;
    uint8_t *data;
    Uint64 t0 = SDL_GetTicksNS();
    int before = npipelines;

    pipeline_file(path, sizeof path);
    data = SDL_LoadFile(path, &size);
    if (!data)
        return;
    if (size >= 8 && ((uint32_t *)data)[0] == PIPELINE_FILE_MAGIC
            && ((uint32_t *)data)[1] == sizeof(pdo_pipeline_state)) {
        warming = true;
        for (size_t o = 8; o + sizeof(pdo_pipeline_state) <= size && npipelines < MAX_PIPELINES;
             o += sizeof(pdo_pipeline_state)) {
            pdo_pipeline_state ps;
            memcpy(&ps, data + o, sizeof ps);
            get_pipeline(&ps);
        }
        warming = false;
        SDL_Log("gfx: made %d saved pipelines in %.0f ms", npipelines - before,
                (double)(SDL_GetTicksNS() - t0) / 1e6);
    } else {
        SDL_Log("gfx: %s is from another build; starting it again", path);
        SDL_RemovePath(path);
    }
    SDL_free(data);
}

/* Per-frame GPU work counts for the timing log: render passes begun,
 * multisample seeds (the resolved image drawn back into the samples),
 * snapshots (copies of the target for draws that sample it), copies. */
static uint32_t n_passes, n_seeds, n_snapshots, n_copies;

void pdo_gfx_take_pass_counts(uint32_t out[4])
{
    out[0] = n_passes; out[1] = n_seeds; out[2] = n_snapshots; out[3] = n_copies;
    n_passes = n_seeds = n_snapshots = n_copies = 0;
}

static Uint64 swap_wait_ns;              /* waiting for a swapchain image, for the timing log */
static Uint64 main_wait_ns, main_record_ns, main_match_ns;   /* ... and pairing frames (match_frames) */    /* the same, and recording frames: the main loop's log */

uint64_t pdo_gfx_take_swap_wait_ns(void)
{
    Uint64 t = swap_wait_ns;
    swap_wait_ns = 0;
    return t;
}

uint64_t pdo_gfx_take_pipeline_ns(uint32_t *made)
{
    Uint64 t = pipeline_ns;
    *made = pipelines_made;
    pipeline_ns = 0;
    pipelines_made = 0;
    return t;
}

static SDL_GPUGraphicsPipeline *get_pipeline(const pdo_pipeline_state *ps)
{
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUVertexBufferDescription vb[2];
    SDL_GPUVertexAttribute attrs[9];
    SDL_GPUColorTargetDescription ct;
    SDL_GPUGraphicsPipeline *p;
    uint32_t cw = ps->color_write;
    Uint64 t0;

    for (int i = 0; i < npipelines; i++)
        if (!memcmp(&pipelines[i].key, ps, sizeof *ps))
            return pipelines[i].p;
    if (npipelines == MAX_PIPELINES)
        return pipelines[0].p;
    t0 = SDL_GetTicksNS();

    /* Slot 1: the previous game frame's vertices, for interpolation. */
    SDL_zero(vb);
    for (int i = 0; i < 2; i++) {
        vb[i].slot = (Uint32)i;
        vb[i].pitch = sizeof(pdo_vertex);
        vb[i].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    }
    for (int i = 0; i < 8; i++) {
        attrs[i].location = (Uint32)i;
        attrs[i].buffer_slot = 0;
    }
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attrs[0].offset = offsetof(pdo_vertex, pos);
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    attrs[1].offset = offsetof(pdo_vertex, diffuse);
    attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    attrs[2].offset = offsetof(pdo_vertex, specular);
    for (int i = 0; i < 4; i++) {
        attrs[3 + i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[3 + i].offset = (Uint32)(offsetof(pdo_vertex, tc) + (size_t)i * 16);
    }
    attrs[7].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    attrs[7].offset = offsetof(pdo_vertex, fog);
    attrs[8].location = 8;
    attrs[8].buffer_slot = 1;
    attrs[8].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attrs[8].offset = offsetof(pdo_vertex, pos);

    SDL_zero(ct);
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ct.blend_state.enable_blend = ps->blend_enable != 0;
    ct.blend_state.src_color_blendfactor = blend_factor(ps->src_blend);
    ct.blend_state.dst_color_blendfactor = blend_factor(ps->dst_blend);
    ct.blend_state.color_blend_op = blend_op(ps->blend_op);
    ct.blend_state.src_alpha_blendfactor = blend_factor(ps->src_blend);
    ct.blend_state.dst_alpha_blendfactor = blend_factor(ps->dst_blend);
    ct.blend_state.alpha_blend_op = blend_op(ps->blend_op);
    ct.blend_state.enable_color_write_mask = true;
    /* Xbox D3DCOLORWRITEENABLE: red bit 16, green 8, blue 0, alpha 24. */
    ct.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)(
        ((cw & 0x00FF0000u) ? SDL_GPU_COLORCOMPONENT_R : 0) |
        ((cw & 0x0000FF00u) ? SDL_GPU_COLORCOMPONENT_G : 0) |
        ((cw & 0x000000FFu) ? SDL_GPU_COLORCOMPONENT_B : 0) |
        ((cw & 0xFF000000u) ? SDL_GPU_COLORCOMPONENT_A : 0));

    SDL_zero(pi);
    pi.vertex_shader = ps->gpu_vsh == 1 ? vsh_vs : ps->gpu_vsh == 2 ? fvf_vs : vs;
    pi.fragment_shader = ps->combiners && fs_ps ? fs_ps : fs;
    if (!ps->gpu_vsh) {         /* vsh.vert reads its streams from storage buffers */
        pi.vertex_input_state.vertex_buffer_descriptions = vb;
        pi.vertex_input_state.num_vertex_buffers = 2;
        pi.vertex_input_state.vertex_attributes = attrs;
        pi.vertex_input_state.num_vertex_attributes = 9;
    }
    pi.primitive_type = ps->topology == 1 ? SDL_GPU_PRIMITIVETYPE_LINELIST
                      : ps->topology == 2 ? SDL_GPU_PRIMITIVETYPE_POINTLIST
                      : ps->topology == 3 ? SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP
                      : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    /* D3D culls by screen-space winding, which ours matches: clip space is
     * D3D's (y up) and SDL presents it the same way up. JSRF's outlines are
     * the mesh drawn again, inflated, in one color, with the other side
     * culled; without culling that shell covers the whole character. */
    pi.rasterizer_state.cull_mode = ps->cull ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.front_face = ps->cull == 1 ? SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE
                                                   : SDL_GPU_FRONTFACE_CLOCKWISE;
    if (ps->depth_bias != 0.0f || ps->depth_slope != 0.0f) {
        pi.rasterizer_state.enable_depth_bias = true;
        pi.rasterizer_state.depth_bias_constant_factor = depth_rev ? -ps->depth_bias : ps->depth_bias;
        pi.rasterizer_state.depth_bias_slope_factor = depth_rev ? -ps->depth_slope : ps->depth_slope;
    }
    pi.depth_stencil_state.enable_depth_test = ps->z_enable != 0;
    pi.depth_stencil_state.enable_depth_write = ps->z_enable && ps->z_write;
    {   /* Reversed depth: the comparison mirrored (LESS reads GREATER). */
        uint16_t f = ps->z_func;
        static const uint16_t mirror[8] = { 0x200, 0x204, 0x202, 0x206, 0x201, 0x205, 0x203, 0x207 };
        if (depth_rev && f >= 0x200 && f <= 0x207)
            f = mirror[f - 0x200];
        pi.depth_stencil_state.compare_op = compare_op(f);
    }
    if (ps->stencil_enable) {
        SDL_GPUStencilOpState so;
        so.fail_op = stencil_op(ps->stencil_fail);
        so.depth_fail_op = stencil_op(ps->stencil_zfail);
        so.pass_op = stencil_op(ps->stencil_pass);
        so.compare_op = compare_op(ps->stencil_func);
        pi.depth_stencil_state.enable_stencil_test = true;
        pi.depth_stencil_state.front_stencil_state = so;
        pi.depth_stencil_state.back_stencil_state = so;
        pi.depth_stencil_state.compare_mask = ps->stencil_mask;
        pi.depth_stencil_state.write_mask = ps->stencil_write_mask;
    }
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = depth_format;
    pi.target_info.has_depth_stencil_target = true;
    pi.multisample_state.sample_count = ps->single_sample ? SDL_GPU_SAMPLECOUNT_1 : sample_count;

    p = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    if (!p) {
        SDL_Log("gfx: pipeline creation failed (%s)", SDL_GetError());
        return NULL;
    }
    pipelines[npipelines].key = *ps;
    pipelines[npipelines].p = p;
    npipelines++;
    if (!warming) {
        pipeline_ns += SDL_GetTicksNS() - t0;
        pipelines_made++;
        save_pipeline_key(ps);
    }
    return p;
}

static struct { uint32_t key; SDL_GPUSampler *s; } samplers[MAX_SAMPLERS];
static int nsamplers;

static SDL_GPUSamplerAddressMode address_mode(uint8_t a)
{
    switch (a) {
    case 2:  return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case 3: case 4: case 5: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    default: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    }
}

static SDL_GPUSampler *get_sampler(const pdo_tex_ref *t)
{
    /* The player's anisotropic filtering (Display > Texture filter), for
     * textures the game filters smoothly; point-filtered ones (menus, text)
     * stay as they are. Its level is part of the key: a change takes effect
     * with the next sampler looked up. */
    int aniso = t->min_filter >= 2 ? pdo_settings_get()->anisotropy : 1;
    uint32_t alevel = aniso >= 16 ? 4 : aniso >= 8 ? 3 : aniso >= 4 ? 2 : aniso >= 2 ? 1 : 0;
    uint32_t key = (uint32_t)t->min_filter | (uint32_t)t->mag_filter << 8 |
                   (uint32_t)t->address_u << 16 | (uint32_t)(t->address_v & 7) << 24 | alevel << 27 |
                   (uint32_t)(t->mip_filter & 3) << 30;
    SDL_GPUSamplerCreateInfo si;

    for (int i = 0; i < nsamplers; i++)
        if (samplers[i].key == key)
            return samplers[i].s;
    if (nsamplers == MAX_SAMPLERS)
        return samplers[0].s;
    SDL_zero(si);
    si.min_filter = t->min_filter >= 2 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mag_filter = t->mag_filter >= 2 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    /* D3DTSS_MIPFILTER: 0 none (level 0 only), 1 point, 2+ linear. */
    si.mipmap_mode = t->mip_filter >= 2 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.min_lod = 0.0f;
    si.max_lod = t->mip_filter ? 1000.0f : 0.0f;
    si.address_mode_u = address_mode(t->address_u);
    si.address_mode_v = address_mode(t->address_v);
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    if (alevel) {
        si.enable_anisotropy = true;
        si.max_anisotropy = (float)(1 << alevel);
    }
    samplers[nsamplers].key = key;
    samplers[nsamplers].s = SDL_CreateGPUSampler(dev, &si);
    return samplers[nsamplers++].s;
}

/* ── Texture cache ────────────────────────────────────────────────────────
 * Keyed by the guest data address and descriptor, validated by a hash of
 * the texel data each frame it is used, so a game that rewrites a texture
 * in place is seen. */

typedef struct gtex {
    uint32_t data, format_word, size_word;
    uint64_t hash;
    bool     uploaded;      /* the GPU copy holds data with that hash */
    uint64_t staged_in;     /* hand_over: number of the frame with an upload of it
                               (a number: frame_datas are reused, addresses repeat) */
    uint64_t last_used;
    uint32_t w, h;
    SDL_GPUTexture *tex;
} gtex;

static gtex textures[MAX_TEXTURES];
static int ntextures;

/* A texture's bytes, to notice when the game rewrites them: it runs over
 * every texture in use every frame, so it has to be fast more than strong.
 * Four independent 64-bit lanes take 32 bytes a step (byte-at-a-time
 * FNV-1a cost about 300 ms a second). */
static uint64_t rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    const uint64_t k = 0x9E3779B97F4A7C15ull;
    uint64_t a = k, b = k ^ 0x5bd1e995u, c = ~k, d = rotl64(k, 17), h;
    size_t i = 0;

    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        a = rotl64((a ^ w[0]) * k, 31);
        b = rotl64((b ^ w[1]) * k, 31);
        c = rotl64((c ^ w[2]) * k, 31);
        d = rotl64((d ^ w[3]) * k, 31);
    }
    h = rotl64(a, 1) ^ rotl64(b, 7) ^ rotl64(c, 12) ^ rotl64(d, 18) ^ (uint64_t)n;
    for (; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

/* ── Render targets ───────────────────────────────────────────────────────
 * A surface the game draws into (SetRenderTarget) or copies into
 * (CopyRects) lives on the GPU, keyed by its guest data address. A texture
 * over the same memory samples it from there: JSRF draws its save-menu and
 * map backgrounds and its boost blur this way. Guest memory is not written
 * back; nothing we know of reads it on the CPU. */

#define MAX_TARGETS 64

typedef struct rtarget {
    uint32_t data;            /* guest data address, 0x80000000 masked off */
    uint32_t w, h;            /* the game's size */
    uint32_t pw, ph;          /* ours: times rscale */
    SDL_GPUTexture *color, *depth, *ms;
    bool stale;
    bool depth_only;          /* a shadow map: keyed by its depth surface,
                                 single-sampled, depth readable by shaders */
} rtarget;

static rtarget targets[MAX_TARGETS];
static int ntargets;
static bool no_shadows;     /* captures only: shadow maps read as lit */
static Uint64 texture_ns;   /* time resolving textures, for the timing log */

uint64_t pdo_gfx_take_texture_ns(void)
{
    Uint64 t = texture_ns;
    texture_ns = 0;
    return t;
}

static uint32_t phys(uint32_t data)
{
    return data & 0x7FFFFFFFu;
}

void pdo_gfx_surface_size(const pdo_surface *s, int *w, int *h)
{
    pdo_xtex_info info;

    if (!s->data) {
        *w = frame_w;
        *h = frame_h;
        return;
    }
    pdo_xtex_describe(s->format_word, s->size_word, &info);
    *w = (int)info.width;
    *h = (int)info.height;
}

static rtarget *find_target(uint32_t data, uint32_t w, uint32_t h)
{
    for (int i = 0; i < ntargets; i++)
        if (targets[i].data == phys(data) && targets[i].w == w && targets[i].h == h)
            return &targets[i];
    return NULL;
}

/* A target's textures at our size: the sampled color, depth, and with
 * multisampling the multisampled color (depth is multisampled then too). */
static bool make_target_as(Uint32 w, Uint32 h, SDL_GPUTexture **color, SDL_GPUTexture **depth,
                           SDL_GPUTexture **ms, bool depth_only)
{
    SDL_GPUTextureCreateInfo ti;
    int n = depth_only ? 1 : nsamples;

    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    *color = SDL_CreateGPUTexture(dev, &ti);
    *ms = NULL;
    if (n > 1) {
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        ti.sample_count = sample_count;
        *ms = SDL_CreateGPUTexture(dev, &ti);
    }
    ti.format = depth_format;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    if (depth_only)
        ti.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.sample_count = n > 1 ? sample_count : SDL_GPU_SAMPLECOUNT_1;
    *depth = SDL_CreateGPUTexture(dev, &ti);
    if (*color && *depth && (n == 1 || *ms))
        return true;
    SDL_ReleaseGPUTexture(dev, *color);
    SDL_ReleaseGPUTexture(dev, *depth);
    SDL_ReleaseGPUTexture(dev, *ms);
    *color = *depth = *ms = NULL;
    return false;
}

static bool make_target(Uint32 w, Uint32 h, SDL_GPUTexture **color, SDL_GPUTexture **depth,
                        SDL_GPUTexture **ms)
{
    return make_target_as(w, h, color, depth, ms, false);
}

/* Clear a new target to black, its multisampled buffer too. */
static void clear_texture(SDL_GPUTexture *t, SDL_GPUTexture *ms)
{
    SDL_GPUCommandBuffer *c = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUColorTargetInfo ct;

    if (!c)
        return;
    SDL_zero(ct);
    ct.texture = ms ? ms : t;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = ms ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE;
    ct.resolve_texture = ms ? t : NULL;
    ct.clear_color.a = 1.0f;
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(c, &ct, 1, NULL));
    SDL_SubmitGPUCommandBuffer(c);
}

/* The GPU target for a guest surface, created on first use. */
/* A color surface without memory (data 0, with a format and size): the
 * game draws depth only into it, e.g. a shadow map whose depth buffer is a
 * texture. Its color goes to a scratch target of that size, not the frame
 * (data 0 with no format is the back buffer). */
#define NO_COLOR_DATA 0x7FFFF000u

static rtarget *target_for_surface_as(const pdo_surface *s, bool depth_only);

static rtarget *target_for_surface(const pdo_surface *s)
{
    return target_for_surface_as(s, false);
}

/* With a depth surface (z data not 0), a depth-only pass gets a target of
 * its own, keyed by that surface's memory: two shadow maps of one size
 * stay apart, and a texture over the depth surface finds it. */
static rtarget *target_of(const pdo_surface *s, const pdo_surface *z)
{
    pdo_surface scratch;

    if (s->data)
        return target_for_surface(s);
    if (!s->format_word)
        return NULL;
    scratch = *s;
    if (z && z->data) {
        scratch.data = z->data;
        return target_for_surface_as(&scratch, true);
    }
    scratch.data = NO_COLOR_DATA;
    return target_for_surface(&scratch);
}

static rtarget *target_for_surface_as(const pdo_surface *s, bool depth_only)
{
    rtarget *r;
    int w, h;

    pdo_gfx_surface_size(s, &w, &h);
    if (!dev || !s->data || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return NULL;
    r = find_target(s->data, (uint32_t)w, (uint32_t)h);
    if (r)
        return r;
    /* Same memory, other size: the old surface is gone. */
    for (int i = 0; i < ntargets; i++)
        if (targets[i].data == phys(s->data)) {
            SDL_ReleaseGPUTexture(dev, targets[i].color);
            SDL_ReleaseGPUTexture(dev, targets[i].depth);
            SDL_ReleaseGPUTexture(dev, targets[i].ms);
            targets[i] = targets[--ntargets];
            break;
        }
    if (ntargets == MAX_TARGETS) {
        static bool warned;
        if (!warned) {
            warned = true;
            SDL_Log("gfx: more than %d render targets", MAX_TARGETS);
        }
        return NULL;
    }
    r = &targets[ntargets];
    memset(r, 0, sizeof *r);
    r->data = phys(s->data);
    r->w = (uint32_t)w;
    r->h = (uint32_t)h;
    r->pw = target_px(r->w, depth_only);
    r->ph = target_px(r->h, depth_only);
    r->depth_only = depth_only;
    if (!make_target_as(r->pw, r->ph, &r->color, &r->depth, &r->ms, depth_only)) {
        SDL_Log("gfx: cannot create a %ux%u render target (%s)", r->pw, r->ph, SDL_GetError());
        return NULL;
    }
    clear_texture(r->color, r->ms);
    SDL_Log("gfx: render target %ux%u at 0x%08x (%s)%s, drawn at %ux%u", r->w, r->h, r->data,
            pdo_xtex_format_name((s->format_word >> 8) & 0xFF),
            depth_only ? ", depth only: a shadow map" : "", r->pw, r->ph);
    ntargets++;
    return r;
}

/* The game thread's list of the render targets above, by key only, kept
 * the same way as they are made (target_for_surface_as, target_of): which
 * textures are targets has to be known at Swap, on the game thread, while
 * the targets themselves are made on the main thread as it replays. */
static struct { uint32_t data, w, h; bool depth_only; } tkeys[MAX_TARGETS];
static int ntkeys;

static int target_index(uint32_t data, uint32_t w, uint32_t h)
{
    for (int i = 0; i < ntkeys; i++)
        if (tkeys[i].data == phys(data) && tkeys[i].w == w && tkeys[i].h == h)
            return i;
    return -1;
}

static bool target_known(uint32_t data, uint32_t w, uint32_t h)
{
    return target_index(data, w, h) >= 0;
}

static void note_target(const pdo_surface *s, bool depth_only)
{
    int w, h, i;

    pdo_gfx_surface_size(s, &w, &h);
    if (!s->data || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return;
    i = target_index(s->data, (uint32_t)w, (uint32_t)h);
    if (i >= 0) {
        tkeys[i].depth_only = depth_only;       /* drawn now with color, or not */
        return;
    }
    for (i = 0; i < ntkeys; i++)        /* same memory, other size: the old one is gone */
        if (tkeys[i].data == phys(s->data)) {
            tkeys[i] = tkeys[--ntkeys];
            break;
        }
    if (ntkeys == MAX_TARGETS)
        return;
    tkeys[ntkeys].data = phys(s->data);
    tkeys[ntkeys].w = (uint32_t)w;
    tkeys[ntkeys].h = (uint32_t)h;
    tkeys[ntkeys].depth_only = depth_only;
    ntkeys++;
}

static void note_target_of(const pdo_surface *s, const pdo_surface *z)
{
    pdo_surface scratch;

    if (s->data) {
        note_target(s, false);
        return;
    }
    if (!s->format_word)
        return;
    scratch = *s;
    scratch.data = z && z->data ? z->data : NO_COLOR_DATA;
    note_target(&scratch, true);
}

static pdo_surface cur_depth_surface;

void pdo_gfx_set_depth(const pdo_surface *depth)
{
    cur_depth_surface = *depth;
}

void pdo_gfx_set_target(const pdo_surface *color)
{
    command *c;

    RECORDING();
    c = new_command();
    c->kind = CMD_TARGET;
    c->surf = *color;
    c->surf2 = cur_depth_surface;
    note_target_of(color, &cur_depth_surface);
}

void pdo_gfx_copy_rect(const pdo_surface *src, const pdo_surface *dst,
                       int x, int y, int w, int h, int dx, int dy)
{
    command *c;

    RECORDING();
    c = new_command();
    c->kind = CMD_COPY;
    c->surf = *src;
    c->surf2 = *dst;
    c->rect[0] = x; c->rect[1] = y; c->rect[2] = w; c->rect[3] = h;
    c->rect[4] = dx; c->rect[5] = dy;
    if (dst->data)
        note_target(dst, false);
}

typedef struct pending_upload {
    gtex    *t;
    uint32_t offset;   /* in the frame's texture transfer buffer */
    uint32_t level, w, h;
} pending_upload;

#define MAX_UPLOADS 2048

/* A texture used this frame, to hash and maybe upload (check_textures). */
static struct tex_check {
    gtex          *g;
    const uint8_t *src;
    pdo_xtex_info  info;
    uint32_t       levels, bytes;
    uint64_t       hash;
} *checks;
static uint32_t nchecks, cap_checks;

/* Find or create the GPU texture for t, and queue it for check_textures
 * the first time it is used in a frame. */
/* Tests for wrong textures (PDO_TEX_ALWAYS=1: decode and upload every
 * texture every frame, never trusting the hash; PDO_TEX_LEVEL0=1: the
 * first mip level only, as before they were all uploaded). */
static bool tex_always, tex_level0;

static tex_slot texture_for(const pdo_tex_ref *t)
{
    pdo_xtex_info info;
    const uint8_t *src;
    gtex *g = NULL;
    uint32_t levels;
    tex_slot slot = { white, 0, 0, 0 };

    if (!t->data)
        return slot;    /* white: untextured MODULATE quads show their color (menu highlights) */
    pdo_xtex_describe(t->format_word, t->size_word, &info);
    if (!info.width || !info.height || info.width > 4096 || info.height > 4096)
        return slot;
    /* A depth-only target (a shadow map) sampled as a color texture: the
     * game has put a texture in that memory since -- the title screen's
     * menu bands sit where a shadow map was, and drew as solid red, the
     * shadow map's depth read as color. The texture is in memory. */
    if (target_known(t->data, info.width, info.height) &&
        !(tkeys[target_index(t->data, info.width, info.height)].depth_only &&
          !(info.format >= 0x2A && info.format <= 0x31))) {
        slot.tex = NULL;
        slot.target_data = t->data;
        slot.target_w = info.width;
        slot.target_h = info.height;
        return slot;
    }
    if (info.format >= 0x2A && info.format <= 0x31) {
        /* Depth textures (D24S8, F24S8, D16, F16, swizzled and linear) are
         * shadow maps, compared in the shader (ff.frag, shadow_stage). One
         * that no depth-only pass drew reads as fully lit. */
        static bool noted;
        if (!noted) {
            noted = true;
            SDL_Log("gfx: depth texture (format 0x%02x) at 0x%08x sampled before any pass drew it: lit",
                    info.format, t->data);
        }
        return slot;
    }
    if (!info.known) {
        static uint32_t warned[256];
        if (!warned[info.format]++)
            SDL_Log("gfx: texture format 0x%02x not decoded yet", info.format);
    }

    for (int i = 0; i < ntextures; i++) {
        if (textures[i].data == t->data && textures[i].format_word == t->format_word &&
            textures[i].size_word == t->size_word) {
            g = &textures[i];
            break;
        }
    }
    if (g && g->last_used == frames)
        { slot.tex = g->tex; return slot; }      /* already checked this frame */
    /* The CPU view of a resource is Data | 0x80000000, as D3D computes it. */
    src = (const uint8_t *)((uintptr_t)(t->data | 0x80000000u) + g_xbox_mem_offset);
    levels = info.known && !tex_level0 ? pdo_xtex_level_count(&info) : 1;
    if (!g) {
        SDL_GPUTextureCreateInfo ti;
        if (ntextures == MAX_TEXTURES)
            return slot;
        g = &textures[ntextures++];
        memset(g, 0, sizeof *g);
        g->data = t->data;
        g->format_word = t->format_word;
        g->size_word = t->size_word;
        g->w = info.width;
        g->h = info.height;
        SDL_zero(ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = info.width;
        ti.height = info.height;
        ti.layer_count_or_depth = 1;
        ti.num_levels = levels;
        g->tex = SDL_CreateGPUTexture(dev, &ti);
    }
    g->last_used = frames;
    /* Hashed with the rest of the frame's textures, on the worker threads
     * (check_textures); decoded after, if it changed. */
    checks = grow(checks, &cap_checks, nchecks + 1, sizeof *checks);
    checks[nchecks].g = g;
    checks[nchecks].src = src;
    checks[nchecks].info = info;
    checks[nchecks].levels = levels;
    {   /* the whole mip chain, so a rewrite of any level is seen */
        pdo_xtex_info last;
        uint32_t off;
        pdo_xtex_level(&info, levels - 1, &last, &off);
        checks[nchecks].bytes = info.bytes ? off + last.bytes : 0;
    }
    nchecks++;
    slot.tex = g->tex;
    return slot;
}

/* Hashed in blocks, so one large texture spreads over the workers too. */
#define HASH_BLOCK (256u * 1024u)
static struct { uint32_t check, offset, bytes; uint64_t hash; } *blocks;
static uint32_t nblocks, cap_blocks;

static void hash_blocks(void *ctx, uint32_t first, uint32_t end)
{
    (void)ctx;
    for (uint32_t i = first; i < end; i++)
        blocks[i].hash = hash_bytes(checks[blocks[i].check].src + blocks[i].offset, blocks[i].bytes);
}

/* Hash this frame's textures (in parallel: on the game thread it was a
 * large share of the frame), then decode the ones that changed into
 * *staging and note their uploads. */
static void check_textures(uint8_t **staging, uint32_t *staging_len, uint32_t *staging_cap,
                           pending_upload *ups, int *nups)
{
    nblocks = 0;
    for (uint32_t i = 0; i < nchecks; i++)
        for (uint32_t o = 0; o < checks[i].bytes; o += HASH_BLOCK) {
            blocks = grow(blocks, &cap_blocks, nblocks + 1, sizeof *blocks);
            blocks[nblocks].check = i;
            blocks[nblocks].offset = o;
            blocks[nblocks].bytes = checks[i].bytes - o < HASH_BLOCK ? checks[i].bytes - o : HASH_BLOCK;
            nblocks++;
        }
    pdo_parallel(nblocks, 1, 4, hash_blocks, NULL);
    for (uint32_t i = 0; i < nchecks; i++)
        checks[i].hash = 0;
    for (uint32_t b = 0; b < nblocks; b++) {     /* in order, per texture */
        uint64_t *h = &checks[blocks[b].check].hash;
        *h = rotl64(*h, 23) * 0x9E3779B97F4A7C15ull ^ blocks[b].hash;
    }
    for (uint32_t i = 0; i < nchecks; i++) {
        gtex *g = checks[i].g;
        const uint8_t *src = checks[i].src;
        pdo_xtex_info info = checks[i].info;
        uint32_t levels = checks[i].levels;
        uint64_t h = checks[i].hash;

        if ((g->uploaded && g->hash == h && !tex_always) || *nups + (int)levels > MAX_UPLOADS)
            continue;
        /* Every mip level: sampled with only level 0, distant textures
         * shimmered and looked grainy. */
        for (uint32_t l = 0; l < levels; l++) {
            pdo_xtex_info li;
            uint32_t off, need;
            pdo_xtex_level(&info, l, &li, &off);
            need = li.width * li.height * 4;
            if (*staging_len + need > *staging_cap) {
                *staging_cap = (*staging_len + need) * 2;
                *staging = SDL_realloc(*staging, *staging_cap);
            }
            pdo_xtex_decode(&li, src + off, *staging + *staging_len);
            ups[*nups].t = g;
            ups[*nups].offset = *staging_len;
            ups[*nups].level = l;
            ups[*nups].w = li.width;
            ups[*nups].h = li.height;
            (*nups)++;
            *staging_len += need;
        }
        g->hash = h;
        g->uploaded = true;
    }
    nchecks = 0;
}

/* ── Init ─────────────────────────────────────────────────────────────── */

static SDL_GPUShader *make_shader_sb(const uint8_t *code, size_t len, SDL_GPUShaderStage stage,
                                     Uint32 samplers_n, Uint32 uniforms_n, Uint32 storage_n)
{
    SDL_GPUShaderCreateInfo si;
    SDL_zero(si);
    si.code = code;
    si.code_size = len;
    si.entrypoint = "main";
    si.format = SDL_GPU_SHADERFORMAT_SPIRV;
    si.stage = stage;
    si.num_samplers = samplers_n;
    si.num_uniform_buffers = uniforms_n;
    si.num_storage_buffers = storage_n;
    return SDL_CreateGPUShader(dev, &si);
}

static SDL_GPUShader *make_shader(const uint8_t *code, size_t len, SDL_GPUShaderStage stage,
                                  Uint32 samplers_n, Uint32 uniforms_n)
{
    return make_shader_sb(code, len, stage, samplers_n, uniforms_n, 0);
}

static SDL_GPUShader *screen_vs, *screen_fs;

/* Render scale and samples from the settings, the samples as many as both
 * the color and the depth format take. */
static void choose_quality(void)
{
    static const SDL_GPUSampleCount counts[] = { SDL_GPU_SAMPLECOUNT_1, SDL_GPU_SAMPLECOUNT_2,
                                                 SDL_GPU_SAMPLECOUNT_4, SDL_GPU_SAMPLECOUNT_8 };
    const pdo_settings *st = pdo_settings_get();
    int want = st->msaa >= 8 ? 3 : st->msaa >= 4 ? 2 : st->msaa >= 2 ? 1 : 0;

    while (want > 0 && !(SDL_GPUTextureSupportsSampleCount(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, counts[want])
                         && SDL_GPUTextureSupportsSampleCount(dev, depth_format, counts[want])))
        want--;
    rscale = (float)SDL_clamp(st->render_scale2, 2, 16) / 2.0f;
    shadow_native = st->shadow_original;
    nsamples = 1 << want;
    sample_count = counts[want];
}

/* VSync (Options > Display): the swapchain's present mode, whenever the
 * setting changes. Off falls back to Fast, Fast to On, if the display
 * cannot; benchmarks and Lossless Scaling keep their own. */
static void apply_vsync(void)
{
    static int applied = -1;
    int want = pdo_settings_get()->vsync;
    static const SDL_GPUPresentMode modes[] = { SDL_GPU_PRESENTMODE_VSYNC, SDL_GPU_PRESENTMODE_MAILBOX,
                                                SDL_GPU_PRESENTMODE_IMMEDIATE };
    static const char *names[] = { "on", "fast (mailbox)", "off (immediate)" };

    if (want == applied || !have_swapchain || lsfg_on || getenv("PDO_BENCHMARK"))
        return;
    applied = want;
    for (int m = SDL_clamp(want, 0, 2); m >= 0; m--)
        if (m == 0 || SDL_WindowSupportsGPUPresentMode(dev, window, modes[m])) {
            if (SDL_SetGPUSwapchainParameters(dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, modes[m])) {
                SDL_Log("gfx: vsync %s", names[m]);
                return;
            }
        }
    SDL_Log("gfx: vsync stays as it was (%s)", SDL_GetError());
}

/* The frame after the gamma ramp, at our size. */
static SDL_GPUTexture *make_output(void)
{
    SDL_GPUTextureCreateInfo ti;

    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = (Uint32)sc(frame_w);
    ti.height = (Uint32)sc(frame_h);
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    return SDL_CreateGPUTexture(dev, &ti);
}

/* Draws a resolved target into its multisampled buffer: inside a target's
 * pass, so with its depth buffer and sample count, depth left alone. */
static void make_seed_pipeline(void)
{
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUColorTargetDescription ct;

    if (seed_pipeline)
        SDL_ReleaseGPUGraphicsPipeline(dev, seed_pipeline);
    seed_pipeline = NULL;
    if (nsamples == 1 || !screen_vs || !screen_fs)
        return;
    SDL_zero(ct);
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_zero(pi);
    pi.vertex_shader = screen_vs;
    pi.fragment_shader = screen_fs;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = depth_format;
    pi.target_info.has_depth_stencil_target = true;
    pi.multisample_state.sample_count = sample_count;
    seed_pipeline = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    if (!seed_pipeline)
        SDL_Log("gfx: cannot create the multisample seed pass (%s)", SDL_GetError());
}

/* The overlay changed the render scale or anti-aliasing: every target is
 * made again at the new size (their contents go; the game redraws them),
 * and the pipelines for the new sample count. */
static void apply_quality(void)
{
    float old_scale = rscale;
    int old_samples = nsamples;
    bool old_shadow = shadow_native;

    choose_quality();
    if (rscale == old_scale && nsamples == old_samples && shadow_native == old_shadow)
        return;
    SDL_WaitForGPUIdle(dev);
    for (int i = 0; i < npipelines; i++)
        SDL_ReleaseGPUGraphicsPipeline(dev, pipelines[i].p);
    npipelines = 0;
    SDL_ReleaseGPUTexture(dev, frame_color);
    SDL_ReleaseGPUTexture(dev, frame_depth);
    SDL_ReleaseGPUTexture(dev, frame_ms);
    SDL_ReleaseGPUTexture(dev, output);
    if (!make_target((Uint32)sc(frame_w), (Uint32)sc(frame_h), &frame_color, &frame_depth,
                     &frame_ms)) {
        SDL_Log("gfx: cannot render at %gx with %d samples (%s); back to 1x, no anti-aliasing",
                (double)rscale, nsamples, SDL_GetError());
        rscale = 1.0f;
        nsamples = 1;
        sample_count = SDL_GPU_SAMPLECOUNT_1;
        make_target((Uint32)frame_w, (Uint32)frame_h, &frame_color, &frame_depth, &frame_ms);
    }
    clear_texture(frame_color, frame_ms);
    frame_stale = false;
    /* Each target again at the new size, kept in the list so this frame's
     * draws still find them as targets, not as textures in guest memory. */
    for (int i = 0; i < ntargets; i++) {
        rtarget *r = &targets[i];
        SDL_ReleaseGPUTexture(dev, r->color);
        SDL_ReleaseGPUTexture(dev, r->depth);
        SDL_ReleaseGPUTexture(dev, r->ms);
        r->pw = target_px(r->w, r->depth_only);
        r->ph = target_px(r->h, r->depth_only);
        r->stale = false;
        if (!make_target_as(r->pw, r->ph, &r->color, &r->depth, &r->ms, r->depth_only)) {
            targets[i--] = targets[--ntargets];
            continue;
        }
        clear_texture(r->color, r->ms);
    }
    output = make_output();
    make_seed_pipeline();
    warm_pipelines();
    SDL_Log("gfx: rendering at %gx (%dx%d), %d sample%s", (double)rscale, sc(frame_w), sc(frame_h),
            nsamples, nsamples > 1 ? "s" : "");
}

static bool init_present(void)
{
    SDL_GPUTextureCreateInfo ti;
    SDL_GPUSamplerCreateInfo si;
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUColorTargetDescription ct;
    SDL_GPUShader *pvs, *pfs;

    output = make_output();
    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = PDO_OVERLAY_W;
    ti.height = PDO_OVERLAY_H;
    overlay_tex = SDL_CreateGPUTexture(dev, &ti);
    ti.width = 256;
    ti.height = 1;
    gamma_lut = SDL_CreateGPUTexture(dev, &ti);

    SDL_zero(si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    nearest = SDL_CreateGPUSampler(dev, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    linear = SDL_CreateGPUSampler(dev, &si);

    pvs = make_shader(present_vert_spv, sizeof present_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    pfs = make_shader(present_frag_spv, sizeof present_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
    SDL_zero(ct);
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_zero(pi);
    pi.vertex_shader = pvs;
    pi.fragment_shader = pfs;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    present_pipeline = pvs && pfs ? SDL_CreateGPUGraphicsPipeline(dev, &pi) : NULL;
    SDL_ReleaseGPUShader(dev, pfs);
    screen_fs = make_shader(screen_frag_spv, sizeof screen_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    screen_vs = pvs;
    if (have_swapchain && screen_fs) {
        ct.format = SDL_GetGPUSwapchainTextureFormat(dev, window);
        pi.fragment_shader = screen_fs;
        screen_pipeline = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        overlay_pipeline = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        if (!screen_pipeline || !overlay_pipeline)
            SDL_Log("gfx: cannot create the window pass (%s)", SDL_GetError());
    }
    make_seed_pipeline();

    for (int i = 0; i < 256; i++) {     /* identity until the game sets one */
        gamma_rgba[i * 4 + 0] = gamma_rgba[i * 4 + 1] = gamma_rgba[i * 4 + 2] = (uint8_t)i;
        gamma_rgba[i * 4 + 3] = 255;
    }
    if (!output || !gamma_lut || !nearest || !present_pipeline) {
        SDL_Log("gfx: cannot create the present pass (%s)", SDL_GetError());
        return false;
    }
    return true;
}

/* The ramp as the game last set it (game thread); it goes over with the
 * next frame. */
static uint8_t game_gamma[256 * 4];
static bool game_gamma_dirty;

void pdo_gfx_set_gamma(const uint8_t ramp[768])
{
    if (getenv("PDO_GAMMA_TRACE"))
        SDL_Log("gfx: frame %llu gamma r[64]=%u r[128]=%u r[255]=%u g[128]=%u b[128]=%u",
                (unsigned long long)frames, ramp[64], ramp[128], ramp[255], ramp[256 + 128], ramp[512 + 128]);
    for (int i = 0; i < 256; i++) {
        game_gamma[i * 4 + 0] = ramp[i];
        game_gamma[i * 4 + 1] = ramp[256 + i];
        game_gamma[i * 4 + 2] = ramp[512 + i];
        game_gamma[i * 4 + 3] = 255;
    }
    game_gamma_dirty = true;
}

/* frame -> (gamma) -> output. */
static void present_pass(SDL_GPUCommandBuffer *cmd)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUTextureSamplerBinding tsb[2];
    SDL_GPURenderPass *pass;

    if (gamma_dirty) {
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, sizeof gamma_rgba, 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        SDL_GPUTextureTransferInfo src;
        SDL_GPUTextureRegion dst;
        SDL_GPUCopyPass *copy;

        memcpy(SDL_MapGPUTransferBuffer(dev, tb, false), gamma_rgba, sizeof gamma_rgba);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        copy = SDL_BeginGPUCopyPass(cmd);
        SDL_zero(src);
        src.transfer_buffer = tb;
        SDL_zero(dst);
        dst.texture = gamma_lut;
        dst.w = 256;
        dst.h = dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        gamma_dirty = false;
    }

    SDL_zero(ct);
    ct.texture = output;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!pass)
        return;
    SDL_BindGPUGraphicsPipeline(pass, present_pipeline);
    tsb[0].texture = frame_color;
    tsb[0].sampler = nearest;
    tsb[1].texture = gamma_lut;
    tsb[1].sampler = nearest;
    SDL_BindGPUFragmentSamplers(pass, 0, tsb, 2);
    {   /* the player's picture settings (present.frag) */
        const pdo_settings *s = pdo_settings_get();
        float pic[8] = { s->picture_crt ? 1.0f : 0.0f, (float)s->pic_brightness / 100.0f,
                         (float)s->pic_contrast / 100.0f, 100.0f / (float)SDL_max(s->pic_gamma, 1),
                         (float)s->pic_saturation / 100.0f, 0.0f, 0.0f, 0.0f };
        SDL_PushGPUFragmentUniformData(cmd, 0, pic, sizeof pic);
    }
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

/* A hang where the game thread never comes back to pump window events
 * ignores F11, so a watchdog takes the thread dump itself once no frame has
 * come for 3 s, and again at 15 s (players close a frozen game quickly; a
 * long load also gets a report, which does no harm). */
static int SDLCALL watchdog(void *unused)
{
    uint64_t seen = 0;
    int still = 0;

    (void)unused;
    for (;;) {
        uint64_t now = *(volatile uint64_t *)&frames;
        SDL_Delay(1000);
        if (now != seen || paused) {
            seen = now;
            still = 0;
        } else if ((++still == 3 || still == 15) && seen > 0) {
            SDL_Log("watchdog: no frame for %d s (frame %llu), dumping threads", still,
                    (unsigned long long)seen);
            pdo_dump_threads();
            pdo_dump_guest_waits();
        }
    }
    return 0;
}

/* The game thread asks (Direct3D_CreateDevice); the main thread, which
 * owns the window, does it in pdo_gfx_main_loop. */
bool pdo_gfx_init(int width, int height)
{
    bool ok;

    if (on_main_thread())
        return init_here(width, height);
    if (dev)
        return true;
    SDL_LockMutex(q_lock);
    init_w = width;
    init_h = height;
    init_state = 1;
    SDL_BroadcastCondition(q_cond);
    while (init_state == 1)
        SDL_WaitCondition(q_cond, q_lock);
    ok = init_state == 2;
    SDL_UnlockMutex(q_lock);
    return ok;
}

static bool init_here(int width, int height)
{
    SDL_GPUTextureCreateInfo ti;
    const char *every;

    if (dev)
        return true;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        SDL_Log("gfx: no video (%s)", SDL_GetError());
        return false;
    }
#ifdef PDO_HAVE_LSFG
    /* Frame generation's shaders need two Vulkan features SDL does not ask
     * for by itself; a GPU without them gets the usual device. */
    if (pdo_settings_get()->lsfg_multiplier >= 2 && !getenv("PDO_BENCHMARK"))
        dev = pdo_lsfg_create_device();
#endif
    if (!dev)
        dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, NULL);
    if (!dev) {
        SDL_Log("gfx: no GPU device (%s)", SDL_GetError());
        return false;
    }
    {
        /* Which GPU and driver: performance reports name a laptop, not its
         * graphics. */
        SDL_PropertiesID pr = SDL_GetGPUDeviceProperties(dev);
        SDL_Log("gfx: GPU driver %s: %s (%s %s)", SDL_GetGPUDeviceDriver(dev),
                SDL_GetStringProperty(pr, SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown GPU"),
                SDL_GetStringProperty(pr, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, "driver"),
                SDL_GetStringProperty(pr, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "?"));
    }

    window_thread = SDL_GetCurrentThreadID();
    {   /* Where the window was last time (pdo_gfx_pump_events), if it fits
         * on a display still: else twice the game's size, centred. */
        const pdo_settings *st = pdo_settings_get();
        SDL_PropertiesID wp = SDL_CreateProperties();
        int w = st->win_w >= 320 ? st->win_w : width * 2, h = st->win_h >= 240 ? st->win_h : height * 2;
        bool placed = false;
        if (st->win_w >= 320) {
            SDL_Point c = { st->win_x + w / 2, st->win_y + h / 2 };
            SDL_DisplayID d = SDL_GetDisplayForPoint(&c);
            SDL_Rect r;
            placed = d && SDL_GetDisplayUsableBounds(d, &r) && SDL_PointInRect(&c, &r);
        }
        SDL_SetStringProperty(wp, SDL_PROP_WINDOW_CREATE_TITLE_STRING, WINDOW_TITLE);
        SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
        SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
        SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_X_NUMBER, placed ? st->win_x : (int)SDL_WINDOWPOS_CENTERED);
        SDL_SetNumberProperty(wp, SDL_PROP_WINDOW_CREATE_Y_NUMBER, placed ? st->win_y : (int)SDL_WINDOWPOS_CENTERED);
        SDL_SetBooleanProperty(wp, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
        SDL_SetBooleanProperty(wp, SDL_PROP_WINDOW_CREATE_MAXIMIZED_BOOLEAN, st->win_maximized);
        SDL_SetBooleanProperty(wp, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, st->fullscreen);
        window = SDL_CreateWindowWithProperties(wp);
        SDL_DestroyProperties(wp);
    }
    if (window && SDL_ClaimWindowForGPUDevice(dev, window))
        have_swapchain = true;
    else
        SDL_Log("gfx: rendering without a window (%s)", SDL_GetError());
#ifdef PDO_HAVE_LSFG
    if (have_swapchain && pdo_settings_get()->lsfg_multiplier >= 2 && !getenv("PDO_BENCHMARK")) {
        const pdo_settings *st = pdo_settings_get();
        char dll[PDO_PATH_MAX], cache[PDO_PATH_MAX];
        if (st->lsfg_dll[0])
            SDL_strlcpy(dll, st->lsfg_dll, sizeof dll);
        else if (!pdo_lsfg_find_dll(pdo_data_dir(), dll, sizeof dll))
            dll[0] = 0;
        snprintf(cache, sizeof cache, "%slossless_scaling/shaders.bin", pdo_data_dir());
        pdo_lsfg_configure(st->lsfg_multiplier, st->lsfg_performance);
        lsfg_on = pdo_lsfg_start(dev, dll, cache);
        SDL_Log("gfx: Lossless Scaling frame generation %s", lsfg_on ? "on" : "off, it cannot run here");
        /* Frame generation shows two to four pictures a game frame: with
         * SDL's two frames in flight they queued up ahead of the screen and
         * the mouse felt late. One is enough: each game frame already waits
         * for its generation (lsfg_show). */
        if (lsfg_on && !SDL_SetGPUAllowedFramesInFlight(dev, 1))
            SDL_Log("gfx: frames in flight stay at SDL's default (%s)", SDL_GetError());
    }
#endif
    if (have_swapchain && getenv("PDO_BENCHMARK") && atoi(getenv("PDO_BENCHMARK"))) {
        /* Not tied to the monitor's refresh, so the frame rate is the PC's. */
        SDL_GPUPresentMode m = SDL_WindowSupportsGPUPresentMode(dev, window, SDL_GPU_PRESENTMODE_IMMEDIATE)
                               ? SDL_GPU_PRESENTMODE_IMMEDIATE : SDL_GPU_PRESENTMODE_MAILBOX;
        if (SDL_WindowSupportsGPUPresentMode(dev, window, m)
                && SDL_SetGPUSwapchainParameters(dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, m))
            SDL_Log("gfx: benchmark: presenting %s", m == SDL_GPU_PRESENTMODE_IMMEDIATE ? "immediately" : "by mailbox");
        else
            SDL_Log("gfx: benchmark: presentation stays synchronised to the display");
    }

    vs = make_shader(ff_vert_spv, sizeof ff_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    fs = make_shader(ff_frag_spv, sizeof ff_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    fs_ps = make_shader(ff_ps_frag_spv, sizeof ff_ps_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    vsh_vs = make_shader_sb(vsh_vert_spv, sizeof vsh_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 5);
    if (!vsh_vs)
        SDL_Log("gfx: no GPU vertex programs (%s); they run on the CPU", SDL_GetError());
    fvf_vs = make_shader_sb(fvf_vert_spv, sizeof fvf_vert_spv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);
    if (!fvf_vs)
        SDL_Log("gfx: no GPU fixed-function vertices (%s); they run on the CPU", SDL_GetError());
    if (!vs || !fs) {
        SDL_Log("gfx: shader creation failed (%s)", SDL_GetError());
        return false;
    }

    frame_w = width;
    frame_h = height;
    /* Reversed depth wants a float buffer (its precision gathers at 0, where
     * the far end now is); otherwise the 24-bit one. */
    depth_rev = pdo_settings_get()->depth_reversed;
    {
        bool d32 = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        bool d24 = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        depth_format = (depth_rev && d32) || !d24 ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT
                                                  : SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
        if (depth_rev && !d32)
            depth_rev = false;      /* no float depth: reversing gains nothing */
        SDL_Log("gfx: depth %s, %s", depth_format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT ? "32-bit float" : "24-bit",
                depth_rev ? "reversed" : "as the game writes it");
    }
    choose_quality();
    if (!make_target((Uint32)sc(width), (Uint32)sc(height), &frame_color, &frame_depth, &frame_ms))
        frame_color = NULL;

    SDL_zero(ti);
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    white = SDL_CreateGPUTexture(dev, &ti);
    tex_always = getenv("PDO_TEX_ALWAYS") && atoi(getenv("PDO_TEX_ALWAYS"));
    tex_level0 = getenv("PDO_TEX_LEVEL0") && atoi(getenv("PDO_TEX_LEVEL0"));
    if (tex_always || tex_level0)
        SDL_Log("gfx: texture test:%s%s", tex_always ? " every texture uploaded every frame" : "",
                tex_level0 ? " first mip level only" : "");
    if (!frame_color || !frame_depth || !white) {
        SDL_Log("gfx: cannot create the frame targets (%s)", SDL_GetError());
        return false;
    }
    {
        /* Fill the 1x1 stand-in for an unbound stage: white, so untextured
         * MODULATE quads show their color (Orta's menu highlights). */
        static const uint8_t px[4] = { 255, 255, 255, 255 };
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 4, 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        SDL_GPUCommandBuffer *c = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUTextureTransferInfo src;
        SDL_GPUTextureRegion dst;
        SDL_GPUCopyPass *copy;

        memcpy(SDL_MapGPUTransferBuffer(dev, tb, false), px, 4);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        copy = SDL_BeginGPUCopyPass(c);
        SDL_zero(src);
        src.transfer_buffer = tb;
        SDL_zero(dst);
        dst.texture = white;
        dst.w = dst.h = dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(c);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }

    if (!init_present())
        return false;
    SDL_Log("gfx: rendering at %gx (%dx%d), %d sample%s", (double)rscale, sc(frame_w), sc(frame_h),
            nsamples, nsamples == 1 ? "" : "s");
    warm_pipelines();

    if (getenv("PDO_CAPTURE_SEQ") && atoi(getenv("PDO_CAPTURE_SEQ")) > 0)
        capture_seq = (uint64_t)atoi(getenv("PDO_CAPTURE_SEQ"));
    if (getenv("PDO_CAPTURE"))      /* same as F12 at that frame */
        capture_frame = (uint64_t)atoll(getenv("PDO_CAPTURE"));
    SDL_DetachThread(SDL_CreateThread(watchdog, "watchdog", NULL));
    shot_dir = getenv("PDO_SCREENSHOT");
    every = getenv("PDO_SCREENSHOT_EVERY");
    if (every && atoi(every) > 0)
        shot_every = atoi(every);
    return true;
}

uint64_t pdo_gfx_frame_count(void)
{
    return frames;
}

/* ── Replay ───────────────────────────────────────────────────────────── */

static SDL_GPUBuffer *prog_buf;
static Uint32 prog_buf_size;

/* A draw's vertices: the vertex buffer, or for a vertex program on the GPU
 * its streams, programs and constants. */
/* Frame interpolation (main thread): the previous game frame, when the
 * frame shown blends towards F from it, and how far (1: F as it is). */
static frame_data *P;
static float blend;
static bool interp_tint;    /* PDO_INTERP_TINT=1: unpaired draws magenta when blending */

static void bind_geometry(SDL_GPURenderPass *pass, int gpu, const command *c)
{
    SDL_GPUBufferBinding ib = { F->gib, 0 };
    SDL_GPUBuffer *pblob = P && P->gblob ? P->gblob : F->gblob;
    SDL_GPUBuffer *pconst = P && P->gconst ? P->gconst : F->gconst;

    if (gpu == 1) {
        SDL_GPUBuffer *sb[5] = { F->gblob, prog_buf, F->gconst, pblob, pconst };
        SDL_BindGPUVertexStorageBuffers(pass, 0, sb, 5);
    } else if (gpu == 2) {
        SDL_GPUBuffer *sb[2] = { F->gblob, pblob };
        SDL_BindGPUVertexStorageBuffers(pass, 0, sb, 2);
    } else {
        /* From the draw's first vertex (drawn with a vertex offset taking
         * it back off the indices); slot 1 the paired draw's, or its own. */
        bool paired = P && P->gvb && c->prev_vbase >= 0;
        SDL_GPUBufferBinding vb[2] = {
            { F->gvb, c->vbase * (Uint32)sizeof(pdo_vertex) },
            { paired ? P->gvb : F->gvb, (paired ? (Uint32)c->prev_vbase : c->vbase) * (Uint32)sizeof(pdo_vertex) },
        };
        SDL_BindGPUVertexBuffers(pass, 0, vb, 2);
    }
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
}

static SDL_GPUBuffer *ensure_buffer(SDL_GPUBuffer *b, Uint32 *size, Uint32 need, SDL_GPUBufferUsageFlags usage)
{
    SDL_GPUBufferCreateInfo bi;
    if (b && *size >= need)
        return b;
    if (b)
        SDL_ReleaseGPUBuffer(dev, b);
    *size = need * 2;
    SDL_zero(bi);
    bi.usage = usage;
    bi.size = *size;
    return SDL_CreateGPUBuffer(dev, &bi);
}

/* The target being replayed into: NULL is the frame. */
static rtarget *cur_target;

static SDL_GPUTexture *cur_color(void)
{
    return cur_target ? cur_target->color : frame_color;
}

static void cur_size(Uint32 *w, Uint32 *h)
{
    *w = cur_target ? cur_target->pw : (Uint32)sc(frame_w);
    *h = cur_target ? cur_target->ph : (Uint32)sc(frame_h);
}

static SDL_GPUTexture *snapshot(SDL_GPUCommandBuffer *cmd);

static SDL_GPURenderPass *begin_pass(SDL_GPUCommandBuffer *cmd, const command *clear)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUDepthStencilTargetInfo dt;
    SDL_GPURenderPass *pass;
    SDL_GPUTexture *ms = cur_target ? cur_target->ms : frame_ms, *seed = NULL;
    bool *stale = cur_target ? &cur_target->stale : &frame_stale;

    /* The resolved texture holds what the multisampled buffer does not:
     * draw it in first, unless this pass clears the color anyway. */
    if (ms && *stale && !(clear && (clear->clear_flags & 0xF0)))
        seed = snapshot(cmd);
    *stale = false;

    SDL_zero(ct);
    ct.texture = ms ? ms : cur_color();
    ct.load_op = SDL_GPU_LOADOP_LOAD;
    ct.store_op = ms ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE;
    ct.resolve_texture = ms ? cur_color() : NULL;
    SDL_zero(dt);
    dt.texture = cur_target ? cur_target->depth : frame_depth;
    dt.load_op = SDL_GPU_LOADOP_LOAD;
    dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_LOAD;
    dt.stencil_store_op = SDL_GPU_STOREOP_STORE;
    if (clear) {
        uint32_t c = clear->clear_color;
        if (clear->clear_flags & 0xF0) {
            ct.load_op = SDL_GPU_LOADOP_CLEAR;
            ct.clear_color.r = (float)((c >> 16) & 0xFF) / 255.0f;
            ct.clear_color.g = (float)((c >> 8) & 0xFF) / 255.0f;
            ct.clear_color.b = (float)(c & 0xFF) / 255.0f;
            ct.clear_color.a = (float)(c >> 24) / 255.0f;
        }
        if (clear->clear_flags & 1) {
            dt.load_op = SDL_GPU_LOADOP_CLEAR;
            dt.clear_depth = depth_rev ? 1.0f - clear->clear_z : clear->clear_z;
        }
        if (clear->clear_flags & 2) {
            dt.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
            dt.clear_stencil = (Uint8)clear->clear_stencil;
        }
    }
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
    n_passes++;
    n_seeds += seed != NULL;
    if (pass && seed && seed_pipeline) {
        SDL_GPUTextureSamplerBinding tsb = { seed, nearest };
        float params[4] = { 0, 0, 0, 0 };
        SDL_BindGPUGraphicsPipeline(pass, seed_pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, params, sizeof params);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }
    return pass;
}

static bool is_3d(const frame_data *f, const command *c);
static bool draw_corners(const frame_data *f, const command *c, float out[8]);

/* ── Bloom ────────────────────────────────────────────────────────────────
 * Picture > Bloom (Low, High): the bright parts of the scene, blurred and
 * added back, run right after the frame's last 3D draw on the target it
 * went to. Orta draws its scene into a target of its own and copies that
 * onto the screen before the HUD, so the HUD, menus, text and our overlay
 * come after it and stay as they are. Off, nothing here is made or run:
 * the textures, shader and pipelines appear the first time it is used. */
static SDL_GPUShader *bloom_fs;
static SDL_GPUGraphicsPipeline *bloom_pipe;          /* modes 0 and 1: into a bloom texture */
static SDL_GPUGraphicsPipeline *bloom_add[4];        /* mode 2, by log2 of the target's samples */
static SDL_GPUTexture *bloom_tex[2];
static Uint32 bloom_w, bloom_h;
static bool bloom_failed;

static SDL_GPUGraphicsPipeline *bloom_pipeline(int samples_log2, bool add)
{
    SDL_GPUGraphicsPipelineCreateInfo pi;
    SDL_GPUColorTargetDescription ct;

    SDL_zero(ct);
    ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    if (add) {
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_zero(pi);
    pi.vertex_shader = screen_vs;
    pi.fragment_shader = bloom_fs;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.multisample_state.sample_count = (SDL_GPUSampleCount)samples_log2;
    return SDL_CreateGPUGraphicsPipeline(dev, &pi);
}

/* One full-screen draw of the bloom shader into dst. */
static void bloom_draw(SDL_GPUCommandBuffer *cmd, SDL_GPUGraphicsPipeline *p, SDL_GPUTexture *dst,
                       SDL_GPUTexture *resolve, SDL_GPUTexture *src, const float params[4], bool load)
{
    SDL_GPUColorTargetInfo ct;
    SDL_GPUTextureSamplerBinding tsb = { src, linear };
    SDL_GPURenderPass *pass;

    SDL_zero(ct);
    ct.texture = dst;
    ct.load_op = load ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = resolve ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE;
    ct.resolve_texture = resolve;
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!pass)
        return;
    SDL_BindGPUGraphicsPipeline(pass, p);
    SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
    SDL_PushGPUFragmentUniformData(cmd, 0, params, 4 * sizeof(float));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

/* On the current target, between passes. */
static void bloom_pass(SDL_GPUCommandBuffer *cmd, int level)
{
    SDL_GPUTexture *color = cur_color(), *ms = cur_target ? cur_target->ms : frame_ms;
    Uint32 w, h, bw, bh;
    int samples_log2 = ms ? (nsamples >= 8 ? 3 : nsamples >= 4 ? 2 : nsamples >= 2 ? 1 : 0) : 0;
    /* The same glow at two strengths, around the bright lights (Orta's
     * picture is dark: half of an episode 1 frame is under 20 of 255).
     * High 0.4, Low 0.2, by the player's eye. */
    float threshold = 0.60f, strength = level >= 2 ? 0.4f : 0.2f;
    int rounds = 1;

    if (bloom_failed || !color || !screen_vs || (cur_target && cur_target->depth_only))
        return;
    if (!bloom_fs) {
        bloom_fs = make_shader(bloom_frag_spv, sizeof bloom_frag_spv, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        bloom_pipe = bloom_fs ? bloom_pipeline(0, false) : NULL;
        if (!bloom_pipe) {
            SDL_Log("gfx: no bloom (%s)", SDL_GetError());
            bloom_failed = true;
            return;
        }
    }
    if (!bloom_add[samples_log2] && !(bloom_add[samples_log2] = bloom_pipeline(samples_log2, true))) {
        SDL_Log("gfx: no bloom at %d samples (%s)", 1 << samples_log2, SDL_GetError());
        bloom_failed = true;
        return;
    }
    cur_size(&w, &h);
    bw = SDL_max(w / 4, 1u);
    bh = SDL_max(h / 4, 1u);
    if (!bloom_tex[0] || bloom_w != bw || bloom_h != bh) {
        SDL_GPUTextureCreateInfo ti;
        SDL_zero(ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = bw;
        ti.height = bh;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        for (int i = 0; i < 2; i++) {
            if (bloom_tex[i])
                SDL_ReleaseGPUTexture(dev, bloom_tex[i]);
            bloom_tex[i] = SDL_CreateGPUTexture(dev, &ti);
        }
        bloom_w = bw;
        bloom_h = bh;
        if (!bloom_tex[0] || !bloom_tex[1])
            return;
    }
    {
        float bright[4] = { 0.0f, threshold, 0.0f, 0.0f };
        float across[4] = { 1.0f, 0.0f, 1.0f / (float)bw, 0.0f };
        float down[4] = { 1.0f, 0.0f, 0.0f, 1.0f / (float)bh };
        float add[4] = { 2.0f, strength, 0.0f, 0.0f };

        bloom_draw(cmd, bloom_pipe, bloom_tex[0], NULL, color, bright, false);
        for (int r = 0; r < rounds; r++) {
            if (r) {        /* wider: the second round takes every other texel */
                across[2] *= 2.0f;
                down[3] *= 2.0f;
            }
            bloom_draw(cmd, bloom_pipe, bloom_tex[1], NULL, bloom_tex[0], across, false);
            bloom_draw(cmd, bloom_pipe, bloom_tex[0], NULL, bloom_tex[1], down, false);
        }
        /* Added into what the game's next draws there continue on: the
         * multisampled buffer (resolved again here), else the target. */
        bloom_draw(cmd, bloom_add[samples_log2], ms ? ms : color, ms ? color : NULL, bloom_tex[0], add, true);
    }
}

static void log_command(uint32_t i, const command *c, SDL_GPUTexture *const *tex)
{
    const pdo_draw *d = &c->draw;

    if (c->kind == CMD_CLEAR) {
        SDL_Log("capture: %u clear flags %x color %08x", i, c->clear_flags, c->clear_color);
        return;
    }
    if (c->kind == CMD_TARGET) {
        SDL_Log("capture: %u target data %08x format %08x size %08x", i, c->surf.data,
                c->surf.format_word, c->surf.size_word);
        return;
    }
    if (c->kind == CMD_COPY) {
        SDL_Log("capture: %u copy %08x -> %08x (%d,%d %dx%d) at %d,%d", i, c->surf.data, c->surf2.data,
                c->rect[0], c->rect[1], c->rect[2], c->rect[3], c->rect[4], c->rect[5]);
        return;
    }
    SDL_Log("capture: %u draw %u idx topo %u blend %u %x/%x op %x z %u/%u/%x cull %u bias %g "
            "vp %.0f,%.0f %.0fx%.0f ps %d final %08x %08x atest %d/%x/%d spec %d fog %d/%d %g-%g/%g %.2f,%.2f,%.2f",
            i, c->index_count, d->ps.topology, d->ps.blend_enable, d->ps.src_blend, d->ps.dst_blend,
            d->ps.blend_op, d->ps.z_enable, d->ps.z_write, d->ps.z_func, d->ps.cull, d->ps.depth_bias,
            d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3],
            d->ff.ps_final[2] & 0xFF, d->ff.ps_final[0], d->ff.ps_final[1],
            d->ff.alpha_test[0], d->ff.alpha_test[1], d->ff.alpha_test[2], d->ff.alpha_test[3],
            d->ff.fog_mode[0], d->ff.fog_mode[1], d->ff.fog_params[0], d->ff.fog_params[1], d->ff.fog_params[2],
            d->ff.fog_color[0], d->ff.fog_color[1], d->ff.fog_color[2]);
    SDL_Log("capture:     drawn from 0x%08x, 0x%08x, 0x%08x; color write %08x", d->caller[0], d->caller[1],
            d->caller[2], d->ps.color_write);
    if (c->fvf >= 0) {  /* its matrices: world, and world * view * projection */
        const pdo_fvf_uniforms *u = &F->fvf_us[c->fvf];
        SDL_Log("capture:     world %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f",
                u->world[0], u->world[1], u->world[2], u->world[3], u->world[4], u->world[5], u->world[6],
                u->world[7], u->world[8], u->world[9], u->world[10], u->world[11], u->world[12], u->world[13],
                u->world[14], u->world[15]);
        SDL_Log("capture:     wvp %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f",
                u->wvp[0], u->wvp[1], u->wvp[2], u->wvp[3], u->wvp[4], u->wvp[5], u->wvp[6], u->wvp[7], u->wvp[8],
                u->wvp[9], u->wvp[10], u->wvp[11], u->wvp[12], u->wvp[13], u->wvp[14], u->wvp[15]);
    }
    {   /* how it interpolates: kind, vertices, paired with which of the frame before, its corners */
        float b4[8];
        bool corners = draw_corners(F, c, b4);
        char prog[48] = "";
        if (c->vsh)     /* its program, and the paired one's when another */
            snprintf(prog, sizeof prog, " program %x%s", (unsigned)c->vu.info[0],
                     !c->unpaired && P && c->pair_prev < P->ncmds && P->cmds[c->pair_prev].vsh &&
                     P->cmds[c->pair_prev].vu.info[0] != c->vu.info[0] ? " (was another)" : "");
        SDL_Log("capture:     interp %s%s nv %u %s %s %u, tex0 %08x%s, corners %.1f,%.1f,%.1f,%.1f - %.1f,%.1f,%.1f,%.1f",
                c->vsh ? "vsh" : c->fvf >= 0 ? "fvf" : "cpu", prog, c->id_nv, is_3d(F, c) ? "3d" : "2d",
                c->unpaired ? "unpaired" : "paired with", c->unpaired ? 0u : c->pair_prev, (unsigned)d->tex[0].data,
                F->tex[i * 4].target_data ? " (a render target)" : "",
                corners ? b4[0] : 0.0f, corners ? b4[1] : 0.0f, corners ? b4[2] : 0.0f, corners ? b4[3] : 0.0f,
                corners ? b4[4] : 0.0f, corners ? b4[5] : 0.0f, corners ? b4[6] : 0.0f, corners ? b4[7] : 0.0f);
    }
    SDL_Log("capture:     vs %08x lighting %u colorvertex %u sources %04x ambient %08x "
            "material %08x/%08x/%08x lights %u", d->vertex_info[0], d->vertex_info[1], d->vertex_info[2] & 0xFF,
            d->vertex_info[3], d->vertex_info[4], d->vertex_info[5], d->vertex_info[6], d->vertex_info[7],
            d->vertex_info[2] >> 8);
    if (d->ff.ps_final[2] & 0xFF) {
        /* The pixel shader in full: texture modes, then each combiner stage's
         * RGB and alpha inputs and outputs with its two constants. */
        SDL_Log("capture:     ps modes %08x flags %08x fc0 %.2f,%.2f,%.2f,%.2f fc1 %.2f,%.2f,%.2f,%.2f",
                d->ff.ps_final[3], d->ff.ps_final[2], d->ff.ps_fc0[0], d->ff.ps_fc0[1], d->ff.ps_fc0[2],
                d->ff.ps_fc0[3], d->ff.ps_fc1[0], d->ff.ps_fc1[1], d->ff.ps_fc1[2], d->ff.ps_fc1[3]);
        for (int k = 0; k < (d->ff.ps_final[2] & 0xFF) && k < 8; k++)
            SDL_Log("capture:     ps %d rgb %08x>%08x a %08x>%08x c0 %.2f,%.2f,%.2f,%.2f c1 %.2f,%.2f,%.2f,%.2f",
                    k, d->ff.ps_rgb_in[k], d->ff.ps_rgb_out[k], d->ff.ps_a_in[k], d->ff.ps_a_out[k],
                    d->ff.ps_c0[k][0], d->ff.ps_c0[k][1], d->ff.ps_c0[k][2], d->ff.ps_c0[k][3],
                    d->ff.ps_c1[k][0], d->ff.ps_c1[k][1], d->ff.ps_c1[k][2], d->ff.ps_c1[k][3]);
    }
    if (d->ps.stencil_enable)
        SDL_Log("capture:     stencil func %x ref %u mask %x/%x ops %x %x %x", d->ps.stencil_func, d->stencil_ref,
                d->ps.stencil_mask, d->ps.stencil_write_mask, d->ps.stencil_fail, d->ps.stencil_zfail,
                d->ps.stencil_pass);
    for (int s = 0; s < 4; s++) {
        const pdo_tex_ref *t = &d->tex[s];
        pdo_xtex_info ti;
        const char *what = "";

        if (!t->data && d->ff.color_op[s][0] <= 1)
            continue;
        pdo_xtex_describe(t->format_word, t->size_word, &ti);
        if (tex[s] == white)
            what = t->data ? " (WHITE: no usable texture)" : " (none)";
        else if (find_target(t->data, ti.width, ti.height))
            what = " (render target)";
        SDL_Log("capture:     stage %d cop %d %x %x %x aop %d %x %x %x tex %08x fmt %08x size %08x %s %ux%u%s",
                s, d->ff.color_op[s][0], d->ff.color_op[s][1], d->ff.color_op[s][2], d->ff.color_op[s][3],
                d->ff.alpha_op[s][0], d->ff.alpha_op[s][1], d->ff.alpha_op[s][2], d->ff.alpha_op[s][3],
                t->data, t->format_word, t->size_word, pdo_xtex_format_name(ti.format), ti.width, ti.height,
                what);
    }
    if (c->vsh)
        SDL_Log("capture:     vertex program on the GPU: %d instructions at %d, constants at %d%s",
                c->vu.info[1], c->vu.info[0], c->vu.info[2], c->has_cpu ? "; the CPU's vertices:" : "");
    if (c->fvf >= 0) {
        const pdo_fvf_uniforms *u = &F->fvf_us[c->fvf];
        SDL_Log("capture:     fixed-function vertices on the GPU: flags %x, %d lights%s",
                u->layout0[2], u->light_info[0], c->has_cpu ? "; the CPU's vertices:" : "");
        /* The first vertex as the game wrote it: position, colors. */
        if ((uint32_t)u->layout0[0] + (uint32_t)u->layout0[1] <= F->nblob && u->layout0[1] >= 16) {
            const uint8_t *v = F->blob + u->layout0[0];
            float p[4];
            uint32_t dif = 0, spe = 0;
            memcpy(p, v, 16);
            if (u->layout0[2] & PDO_FVF_DIFFUSE)
                memcpy(&dif, v + u->offs[1], 4);
            if (u->layout0[2] & PDO_FVF_SPECULAR)
                memcpy(&spe, v + u->offs[2], 4);
            SDL_Log("capture:     v0 raw %.3f %.3f %.3f %.3f diffuse %08x specular %08x, %u vertices",
                    p[0], p[1], p[2], p[3], dif, spe, c->id_nv);
        }
    }
    for (uint32_t k = 0; k < 3 && k < c->index_count && ((!c->vsh && c->fvf < 0) || c->has_cpu); k++) {
        const pdo_vertex *v = &F->verts[F->indices[c->first_index + k]];
        SDL_Log("capture:     v pos %.3f %.3f %.3f %.3f d %08x s %08x t0 %.3f %.3f %.3f %.3f "
                "t1 %.3f %.3f %.3f %.3f fog %.2f",
                v->pos[0], v->pos[1], v->pos[2], v->pos[3], v->diffuse, v->specular, v->tc[0][0], v->tc[0][1],
                v->tc[0][2], v->tc[0][3], v->tc[1][0], v->tc[1][1], v->tc[1][2], v->tc[1][3], v->fog);
    }
    /* Each distinct guest texture once, as decoded. */
    for (int s = 0; s < 4; s++) {
        static uint32_t dumped[512];
        static int ndumped;
        const pdo_tex_ref *t = &d->tex[s];
        pdo_xtex_info ti;
        uint8_t *rgba;
        int k;

        if (!t->data || tex[s] == white)
            continue;
        for (k = 0; k < ndumped && dumped[k] != t->data; k++)
            ;
        if (k < ndumped || ndumped == 512)
            continue;
        dumped[ndumped++] = t->data;
        pdo_xtex_describe(t->format_word, t->size_word, &ti);
        if (find_target(t->data, ti.width, ti.height) || !ti.known)
            continue;
        rgba = SDL_malloc((size_t)ti.width * ti.height * 4);
        if (rgba) {
            char path[64];
            SDL_Surface *sf;
            pdo_xtex_decode(&ti, (const uint8_t *)((uintptr_t)(t->data | 0x80000000u) + g_xbox_mem_offset), rgba);
            sf = SDL_CreateSurfaceFrom((int)ti.width, (int)ti.height, SDL_PIXELFORMAT_ABGR8888, rgba,
                                       (int)ti.width * 4);
            snprintf(path, sizeof path, "capture_tex_%08x.bmp", t->data);
            if (sf) {
                SDL_SaveBMP(sf, path);
                SDL_DestroySurface(sf);
            }
            SDL_free(rgba);
        }
    }
}

/* A copy of the current target, for draws that sample it. */
static SDL_GPUTexture *snapshot(SDL_GPUCommandBuffer *cmd)
{
    static SDL_GPUTexture *tex;
    static Uint32 tw, th;
    SDL_GPUTextureLocation src, dst;
    SDL_GPUCopyPass *copy;
    Uint32 w, h;

    cur_size(&w, &h);
    if (!tex || tw != w || th != h) {
        SDL_GPUTextureCreateInfo ti;
        if (tex)
            SDL_ReleaseGPUTexture(dev, tex);
        SDL_zero(ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = w;
        ti.height = h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        tex = SDL_CreateGPUTexture(dev, &ti);
        tw = w;
        th = h;
    }
    if (!tex)
        return white;
    n_snapshots++;
    SDL_zero(src);
    src.texture = cur_color();
    SDL_zero(dst);
    dst.texture = tex;
    copy = SDL_BeginGPUCopyPass(cmd);
    SDL_CopyGPUTextureToTexture(copy, &src, &dst, w, h, 1, false);
    SDL_EndGPUCopyPass(copy);
    return tex;
}

/* CopyRects: a rectangle from one surface to another, clipped to both. */
static void copy_rect(SDL_GPUCommandBuffer *cmd, const command *c)
{
    rtarget *rs = c->surf.data ? target_for_surface(&c->surf) : NULL;
    rtarget *rd = c->surf2.data ? target_for_surface(&c->surf2) : NULL;
    SDL_GPUTexture *ts = rs ? rs->color : frame_color, *td = rd ? rd->color : frame_color;
    int sw = rs ? (int)rs->pw : sc(frame_w), sh = rs ? (int)rs->ph : sc(frame_h);
    int dw = rd ? (int)rd->pw : sc(frame_w), dh = rd ? (int)rd->ph : sc(frame_h);
    /* Both edges scaled and rounded, so neighbouring copies still meet at
     * a fractional scale; each side by its own target's scale (a shadow map
     * may be at the game's size). */
    float ssx = rs ? (float)rs->pw / (float)rs->w : rscale, ssy = rs ? (float)rs->ph / (float)rs->h : rscale;
    float dsx = rd ? (float)rd->pw / (float)rd->w : rscale, dsy = rd ? (float)rd->ph / (float)rd->h : rscale;
    int x = (int)lroundf(c->rect[0] * ssx), y = (int)lroundf(c->rect[1] * ssy);
    int w = (int)lroundf((c->rect[0] + c->rect[2]) * ssx) - x, h = (int)lroundf((c->rect[1] + c->rect[3]) * ssy) - y;
    int dx = (int)lroundf(c->rect[4] * dsx), dy = (int)lroundf(c->rect[5] * dsy);
    SDL_GPUTextureLocation src, dst;
    SDL_GPUCopyPass *copy;

    if ((c->surf.data && !rs) || (c->surf2.data && !rd) || ts == td)
        return;
    if (x < 0) { dx -= x; w += x; x = 0; }
    if (y < 0) { dy -= y; h += y; y = 0; }
    if (dx < 0) { x -= dx; w += dx; dx = 0; }
    if (dy < 0) { y -= dy; h += dy; dy = 0; }
    w = SDL_min(w, SDL_min(sw - x, dw - dx));
    h = SDL_min(h, SDL_min(sh - y, dh - dy));
    if (w <= 0 || h <= 0)
        return;
    SDL_zero(src);
    src.texture = ts;
    src.x = (Uint32)x;
    src.y = (Uint32)y;
    SDL_zero(dst);
    dst.texture = td;
    dst.x = (Uint32)dx;
    dst.y = (Uint32)dy;
    copy = SDL_BeginGPUCopyPass(cmd);
    SDL_CopyGPUTextureToTexture(copy, &src, &dst, (Uint32)w, (Uint32)h, 1, false);
    SDL_EndGPUCopyPass(copy);
    n_copies++;
    if (rd)
        rd->stale = rd->ms != NULL;
    else
        frame_stale = frame_ms != NULL;
}

/* PDO_CAPTURE_STEP=n: at the capture frame, also save the target being
 * drawn to after every n commands (capture_<frame>_<count>.bmp), to find the
 * draw that goes wrong. The frame is replayed once per image. */
static uint32_t replay_limit = UINT32_MAX;

/* A stage's texture, as the game thread resolved it: render targets are
 * looked up here, where they are made. */
static SDL_GPUTexture *slot_texture(const tex_slot *t)
{
    rtarget *r;

    if (t->tex || !t->target_w)
        return t->tex ? t->tex : white;
    r = find_target(t->target_data, t->target_w, t->target_h);
    if (!r)
        return white;
    return r->depth_only ? (no_shadows ? white : r->depth) : r->color;
}

/* The frame's vertices, indices, streams, constants (and any new programs
 * and changed texels) to the GPU, once. */
static void upload_frame(SDL_GPUCommandBuffer *cmd)
{
    uint8_t *staging = F->staging;
    pending_upload *ups = F->ups;
    int nups = F->nups;
    uint32_t staging_len = F->staging_len;
    Uint32 vbytes = F->nverts * (Uint32)sizeof *F->verts, ibytes = F->nindices * 4;
    Uint32 bbytes = F->nblob ? (F->nblob + 8 + 3) & ~3u : 0, cbytes = F->nvconsts * 16, pbytes;

    if (F->on_gpu)
        return;
    F->on_gpu = true;
    /* All the programs again when one was added. */
    pbytes = F->progs_new ? F->nprogs * 4 : 0;
    if (vbytes || ibytes || staging_len || bbytes || cbytes || pbytes) {
        SDL_GPUTransferBufferCreateInfo tbi;
        SDL_GPUTransferBuffer *tb;
        SDL_GPUCopyPass *copy;
        uint8_t *map;
        Uint32 boff = vbytes + ibytes, coff = boff + bbytes, poff = coff + cbytes;
        Uint32 tex_off = poff + pbytes;

        if (vbytes)
            F->gvb = ensure_buffer(F->gvb, &F->gvb_size, vbytes, SDL_GPU_BUFFERUSAGE_VERTEX);
        if (ibytes)
            F->gib = ensure_buffer(F->gib, &F->gib_size, ibytes, SDL_GPU_BUFFERUSAGE_INDEX);
        if (bbytes)
            F->gblob = ensure_buffer(F->gblob, &F->gblob_size, bbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        if (cbytes)
            F->gconst = ensure_buffer(F->gconst, &F->gconst_size, cbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        if (pbytes)
            prog_buf = ensure_buffer(prog_buf, &prog_buf_size, pbytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        SDL_zero(tbi);
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = tex_off + staging_len;
        tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        map = SDL_MapGPUTransferBuffer(dev, tb, false);
        memcpy(map, F->verts, vbytes);
        memcpy(map + vbytes, F->indices, ibytes);
        memcpy(map + boff, F->blob, F->nblob + 8 < bbytes ? F->nblob + 8 : bbytes);
        memcpy(map + coff, F->vconsts, cbytes);
        memcpy(map + poff, F->progs, pbytes);
        memcpy(map + tex_off, staging, staging_len);
        SDL_UnmapGPUTransferBuffer(dev, tb);

        copy = SDL_BeginGPUCopyPass(cmd);
        {
            struct { SDL_GPUBuffer *b; Uint32 off, n; } up[5] = {
                { F->gvb, 0, vbytes }, { F->gib, vbytes, ibytes }, { F->gblob, boff, bbytes },
                { F->gconst, coff, cbytes }, { prog_buf, poff, pbytes },
            };
            for (int k = 0; k < 5; k++)
                if (up[k].n && up[k].b) {
                    SDL_GPUTransferBufferLocation src = { tb, up[k].off };
                    SDL_GPUBufferRegion dst = { up[k].b, 0, up[k].n };
                    SDL_UploadToGPUBuffer(copy, &src, &dst, true);
                }
        }
        for (int i = 0; i < nups; i++) {
            SDL_GPUTextureTransferInfo src;
            SDL_GPUTextureRegion dst;
            SDL_zero(src);
            src.transfer_buffer = tb;
            src.offset = tex_off + ups[i].offset;
            SDL_zero(dst);
            dst.texture = ups[i].t->tex;
            dst.mip_level = ups[i].level;
            dst.w = ups[i].w;
            dst.h = ups[i].h;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
        }
        SDL_EndGPUCopyPass(copy);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
    }
}

static void replay(SDL_GPUCommandBuffer *cmd)
{
    SDL_GPURenderPass *pass = NULL;
    int bloom = pdo_settings_get()->bloom;
    uint32_t bloom_at = UINT32_MAX;     /* the command after the frame's last 3D draw */

    upload_frame(cmd);
    cur_target = NULL;
    if (bloom)
        for (uint32_t i = F->ncmds; i-- > 0;)
            if (F->cmds[i].kind == CMD_DRAW && is_3d(F, &F->cmds[i])) {
                bloom_at = i + 1;
                break;
            }
    for (uint32_t i = 0; i < F->ncmds; i++) {
        if (i == bloom_at && i < replay_limit) {
            if (pass)
                SDL_EndGPURenderPass(pass);
            pass = NULL;
            bloom_pass(cmd, bloom);
        }
        command *c = &F->cmds[i];
        SDL_GPUGraphicsPipeline *p;
        SDL_GPUTextureSamplerBinding tsb[4];
        SDL_GPUViewport vp;
        int gpu;

        if (i >= replay_limit)
            break;
        SDL_GPUTexture *tex[4];

        if (c->kind == CMD_DRAW)
            for (int s = 0; s < 4; s++)
                tex[s] = slot_texture(&F->tex[i * 4 + s]);
        if (F->frame == capture_frame && replay_limit == UINT32_MAX)
            log_command(i, c, tex);

        if (c->kind == CMD_CLEAR) {
            if (pass)
                SDL_EndGPURenderPass(pass);
            pass = begin_pass(cmd, c);
            continue;
        }
        if (c->kind == CMD_TARGET) {
            if (pass)
                SDL_EndGPURenderPass(pass);
            pass = NULL;
            cur_target = target_of(&c->surf, &c->surf2);
            continue;
        }
        if (c->kind == CMD_COPY) {
            if (pass)
                SDL_EndGPURenderPass(pass);
            pass = NULL;
            copy_rect(cmd, c);
            continue;
        }
        gpu = 0;
        if (c->vsh && !(cpu_vsh && c->has_cpu) && vsh_vs && prog_buf && F->gblob && F->gconst)
            gpu = 1;
        else if (c->fvf >= 0 && !(cpu_vsh && c->has_cpu) && fvf_vs && F->gblob)
            gpu = 2;
        if ((c->vsh || c->fvf >= 0) && !gpu && !c->has_cpu)
            continue;           /* no vertices to draw it with */
        if (!pass)
            pass = begin_pass(cmd, NULL);
        {
            pdo_pipeline_state ps = c->draw.ps;
            ps.single_sample = cur_target && cur_target->depth_only;
            ps.gpu_vsh = gpu;
            ps.combiners = (c->draw.ff.ps_final[2] & 0xFF) != 0;
            p = get_pipeline(&ps);
        }
        if (!p)
            continue;
        SDL_BindGPUGraphicsPipeline(pass, p);
        /* The target's own scale: a shadow map may be at the game's size
         * (Shadows: Low) while the rest is scaled. */
        float vsx = cur_target ? (float)cur_target->pw / (float)cur_target->w : rscale;
        float vsy = cur_target ? (float)cur_target->ph / (float)cur_target->h : rscale;
        vp.x = c->draw.viewport[0] * vsx;
        vp.y = c->draw.viewport[1] * vsy;
        vp.w = c->draw.viewport[2] * vsx;
        vp.h = c->draw.viewport[3] * vsy;
        vp.min_depth = depth_rev ? 1.0f - c->draw.viewport[5] : c->draw.viewport[4];
        vp.max_depth = depth_rev ? 1.0f - c->draw.viewport[4] : c->draw.viewport[5];
        SDL_SetGPUViewport(pass, &vp);
        bind_geometry(pass, gpu, c);
        for (int s = 0; s < 4; s++) {
            tsb[s].texture = tex[s];
            tsb[s].sampler = get_sampler(&c->draw.tex[s]);
            if (tsb[s].texture == cur_color()) {
                /* Sampling the target being drawn to (feedback effects):
                 * the console reads what is there before the draw, so draw
                 * from a copy taken now. */
                SDL_EndGPURenderPass(pass);
                tsb[s].texture = snapshot(cmd);
                pass = begin_pass(cmd, NULL);
                SDL_BindGPUGraphicsPipeline(pass, p);
                SDL_SetGPUViewport(pass, &vp);
                bind_geometry(pass, gpu, c);
            }
        }
        SDL_BindGPUFragmentSamplers(pass, 0, tsb, 4);
        SDL_PushGPUFragmentUniformData(cmd, 0, &c->draw.ff, sizeof c->draw.ff);
        if (c->draw.ps.stencil_enable)
            SDL_SetGPUStencilReference(pass, c->draw.stencil_ref);
        if (gpu) {
            float t = P ? blend : 1.0f;
            /* Not screen-space draws (the HUD, full-screen passes): a
             * storm layer over the whole screen tinted everything. */
            int tint = P && interp_tint && c->unpaired &&
                       !(gpu != 1 && (F->fvf_us[c->fvf].layout0[2] & PDO_FVF_PRE));
            if (gpu == 1) {
                c->vu.blend_t[0] = t;
                c->vu.interp[2] = tint;
                c->vu.info[3] = depth_rev;
                SDL_PushGPUVertexUniformData(cmd, 0, &c->vu, sizeof c->vu);
            } else {
                F->fvf_us[c->fvf].blend_t[0] = t;
                F->fvf_us[c->fvf].blend_t[1] = depth_rev;
                F->fvf_us[c->fvf].interp[2] = tint;
                SDL_PushGPUVertexUniformData(cmd, 0, &F->fvf_us[c->fvf], sizeof F->fvf_us[0]);
            }
            SDL_DrawGPUIndexedPrimitives(pass, c->vsh_index_count, 1, c->vsh_first_index, 0, 0);
        } else {
            float b[4] = { P ? blend : 1.0f, P && c->prev_vbase >= 0 ? 1.0f : 0.0f,
                           0.0f, depth_rev ? 1.0f : 0.0f };   /* CPU-made: not tinted; w: reversed depth */
            SDL_PushGPUVertexUniformData(cmd, 0, b, sizeof b);
            SDL_DrawGPUIndexedPrimitives(pass, c->index_count, 1, c->first_index, -(Sint32)c->vbase, 0);
        }
    }
    if (pass)
        SDL_EndGPURenderPass(pass);
    if (bloom_at == F->ncmds && replay_limit >= F->ncmds)     /* the last 3D draw was the frame's last command */
        bloom_pass(cmd, bloom);
}

/* Read the frame target back and write it as a BMP. Slow; debugging only. */
static void save_texture(SDL_GPUCommandBuffer **cmdp, SDL_GPUTexture *tex, int w, int h, const char *path)
{
    SDL_GPUTransferBufferCreateInfo tbi;
    SDL_GPUTransferBuffer *tb;
    SDL_GPUTextureRegion src;
    SDL_GPUTextureTransferInfo dst;
    SDL_GPUCopyPass *copy;
    SDL_GPUFence *fence;
    void *pixels;

    SDL_zero(tbi);
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = (Uint32)(w * h * 4);
    tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!tb)
        return;
    copy = SDL_BeginGPUCopyPass(*cmdp);
    SDL_zero(src);
    src.texture = tex;
    src.w = (Uint32)w;
    src.h = (Uint32)h;
    src.d = 1;
    SDL_zero(dst);
    dst.transfer_buffer = tb;
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);

    fence = SDL_SubmitGPUCommandBufferAndAcquireFence(*cmdp);
    *cmdp = NULL;
    if (fence) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    pixels = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (pixels) {
        SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, pixels, w * 4);
        if (s) {
            SDL_SaveBMP(s, path);
            SDL_DestroySurface(s);
        }
        SDL_UnmapGPUTransferBuffer(dev, tb);
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
}

static void save_screenshot(SDL_GPUCommandBuffer **cmdp, const char *dir, const char *name)
{
    char path[1024];

    snprintf(path, sizeof path, "%s/%s_%05llu.bmp", dir, name, (unsigned long long)F->frame);
    save_texture(cmdp, output, sc(frame_w), sc(frame_h), path);
}

/* PDO_CAPTURE_NOSHADOW=1: at the capture frame, save the frame's last
 * target twice, with its shadow maps and with each read as lit
 * (capture_<frame>_shadow.bmp, capture_<frame>_noshadow.bmp). */
static void capture_shadows(void)
{
    const char *e = getenv("PDO_CAPTURE_NOSHADOW");

    if (!e || !atoi(e))
        return;
    for (int k = 0; k < 2; k++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        no_shadows = k == 1;
        replay(cmd);
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_%s.bmp", (unsigned long long)F->frame,
                 k ? "noshadow" : "shadow");
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    no_shadows = false;
}

/* PDO_CAPTURE_CPUVSH=1: at the capture frame, save the frame's last target
 * with its vertex programs run on the GPU and on the CPU
 * (capture_<frame>_gpuvsh.bmp, capture_<frame>_cpuvsh.bmp); d3d_hle records
 * both for that frame. */
static void capture_vsh(void)
{
    const char *e = getenv("PDO_CAPTURE_CPUVSH");

    if (!e || !atoi(e))
        return;
    for (int k = 0; k < 2; k++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        cpu_vsh = k == 1;
        replay(cmd);
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_%s.bmp", (unsigned long long)F->frame,
                 k ? "cpuvsh" : "gpuvsh");
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    cpu_vsh = false;
}

/* PDO_CAPTURE_INTERP=1: at the capture frame, save the frame between the
 * one before and it at t 0, 0.5 and 1 (capture_<frame>_t000.bmp, _t050,
 * _t100): matched draws should sit halfway in the middle one. */
static void match_frames(frame_data *prev, frame_data *cur);
static bool dbg_groups;
static bool is_3d(const frame_data *f, const command *c);
static bool draw_corners(const frame_data *f, const command *c, float out[8]);

static void capture_interp_main(frame_data *prev, frame_data *cur)
{
    dbg_groups = getenv("PDO_DBG_GROUPS") != NULL;
    const char *e = getenv("PDO_CAPTURE_INTERP");
    static const float ts[3] = { 0.0f, 0.5f, 1.0f };

    if (!e || !atoi(e))
        return;
    match_frames(prev, cur);
    dbg_groups = false;
    if (getenv("PDO_DBG_GROUPS"))
        for (uint32_t i = 0; i < cur->ncmds; i++) {
            const command *c = &cur->cmds[i];
            float b4[8], pa[8];
            if (c->kind != CMD_DRAW || c->unpaired || !draw_corners(cur, c, b4))
                continue;
            if (c->fvf >= 0 && !(cur->fvf_us[c->fvf].layout0[2] & PDO_FVF_PRE)) {
                const pdo_fvf_uniforms *u = &cur->fvf_us[c->fvf];
                float x0 = u->prev_wvp[12] / u->prev_wvp[15], y0 = u->prev_wvp[13] / u->prev_wvp[15];
                float x1 = u->wvp[12] / u->wvp[15], y1 = u->wvp[13] / u->wvp[15];
                if (SDL_fabsf(x1 - x0) + SDL_fabsf(y1 - y0) > 0.1f)
                    SDL_Log("moved: cmd %u kind 2 nv %u tex %08x ndc %.2f,%.2f -> %.2f,%.2f", i, c->id_nv,
                            (unsigned)c->draw.tex[0].data, x0, y0, x1, y1);
            } else if (!c->vsh && c->fvf < 0 && c->prev_vbase >= 0) {
                memcpy(pa, prev->verts[c->prev_vbase].pos, 16);
                if (SDL_fabsf(pa[0] / pa[3] - b4[0] / b4[3]) + SDL_fabsf(pa[1] / pa[3] - b4[1] / b4[3]) > 0.1f)
                    SDL_Log("moved: cmd %u kind 3 nv %u tex %08x clip %.2f,%.2f -> %.2f,%.2f", i, c->id_nv,
                            (unsigned)c->draw.tex[0].data, pa[0] / pa[3], pa[1] / pa[3], b4[0] / b4[3], b4[1] / b4[3]);
            }
        }
    if (getenv("PDO_DBG_GROUPS"))     /* paired 3D fixed-function draws whose texturing moved */
        for (uint32_t i = 0; i < cur->ncmds; i++) {
            const command *c = &cur->cmds[i];
            const pdo_fvf_uniforms *u, *pu;
            float dm = 0.0f, duv[2] = { 0.0f, 0.0f };
            int tts = 0;
            if (c->kind != CMD_DRAW || c->unpaired || c->fvf < 0 || !is_3d(cur, c))
                continue;
            u = &cur->fvf_us[c->fvf];
            pu = &prev->fvf_us[prev->cmds[c->pair_prev].fvf];
            for (int st = 0; st < 4; st++)
                if (u->stage_tt[st] & 0xFF) {
                    tts |= 1 << st;
                    for (int m = 0; m < 16; m++)
                        dm = SDL_max(dm, SDL_fabsf(u->tex[st][m] - pu->tex[st][m]));
                }
            if (u->layout0[3] > 0 && pu->layout0[3] > 0) {
                float a[2], b[2];
                uint32_t oa = (uint32_t)u->layout0[0] + (uint32_t)u->tc_off[0];
                uint32_t ob = (uint32_t)pu->layout0[0] + (uint32_t)pu->tc_off[0];
                if (oa + 8 <= cur->nblob && ob + 8 <= prev->nblob) {
                    memcpy(a, cur->blob + oa, 8);
                    memcpy(b, prev->blob + ob, 8);
                    duv[0] = a[0] - b[0];
                    duv[1] = a[1] - b[1];
                }
            }
            if (dm > 1e-5f || SDL_fabsf(duv[0]) > 1e-5f || SDL_fabsf(duv[1]) > 1e-5f)
                SDL_Log("uvmove: cmd %u nv %u tex %08x tt stages %x matrix moved %g; uv0 moved %g,%g", i, c->id_nv,
                        (unsigned)c->draw.tex[0].data, tts, dm, duv[0], duv[1]);
        }
    if (getenv("PDO_DBG_GROUPS"))
        for (uint32_t i = 0; i < cur->ncmds; i++) {
            const command *c = &cur->cmds[i];
            float a4[8];
            if (c->kind != CMD_DRAW || !c->unpaired)
                continue;
            if (!draw_corners(cur, c, a4))
                memset(a4, 0, sizeof a4);
            SDL_Log("unp: cmd %u kind %d pre %d src %d nv %u idx %u tex %08x blend %d v0 %.1f %.1f %.1f %.1f",
                    i, c->vsh ? 1 : c->fvf >= 0 ? 2 : 3,
                    c->fvf >= 0 ? !!(cur->fvf_us[c->fvf].layout0[2] & PDO_FVF_PRE) : 0, c->id_src != 0, c->id_nv,
                    c->vsh || c->fvf >= 0 ? c->vsh_index_count : c->index_count, (unsigned)c->draw.tex[0].data,
                    c->draw.ps.blend_enable, a4[0], a4[1], a4[2], a4[3]);
        }
    for (int k = 0; k < 3; k++) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        F = prev;           /* prev on the GPU first, as when interpolating */
        upload_frame(cmd);
        F = cur;
        P = prev;
        blend = ts[k];
        replay(cmd);
        cur_target = NULL;
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_t%03d.bmp", (unsigned long long)cur->frame,
                 (int)(ts[k] * 100));
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    {   /* and the frame before as it is: t 0 should match it */
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;
        if (cmd) {
            F = prev;
            P = NULL;
            blend = 1.0f;
            replay(cmd);
            cur_target = NULL;
            cur_size(&w, &h);
            snprintf(path, sizeof path, "capture_%05llu_prev.bmp", (unsigned long long)cur->frame);
            save_texture(&cmd, cur_color(), (int)w, (int)h, path);
            if (cmd)
                SDL_SubmitGPUCommandBuffer(cmd);
            F = cur;
        }
    }
    P = NULL;
    blend = 1.0f;
}

static void capture_interp(frame_data *prev, frame_data *cur)
{
    tls_drawing = true;
    capture_interp_main(prev, cur);
    tls_drawing = false;
}

/* At the capture frame: each texture's GPU copy, every mip level, against
 * what its guest memory decodes to now. A level that differs while its
 * texture is marked uploaded with the same hash lost an upload. */
static void capture_check_textures(void)
{
    uint32_t bad = 0, checked = 0;

    SDL_WaitForGPUIdle(dev);
    for (int i = 0; i < ntextures; i++) {
        gtex *g = &textures[i];
        pdo_xtex_info info;
        uint32_t levels;
        const uint8_t *src;

        if (!g->uploaded || g->last_used + 2 < F->frame)
            continue;
        pdo_xtex_describe(g->format_word, g->size_word, &info);
        if (!info.known)
            continue;
        levels = tex_level0 ? 1 : pdo_xtex_level_count(&info);
        src = (const uint8_t *)((uintptr_t)(g->data | 0x80000000u) + g_xbox_mem_offset);
        for (uint32_t l = 0; l < levels; l++) {
            pdo_xtex_info li;
            uint32_t off, need, diff = 0;
            uint8_t *want;
            SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, 0, 0 };
            SDL_GPUTransferBuffer *tb;
            SDL_GPUCommandBuffer *cmd;
            SDL_GPUCopyPass *copy;
            SDL_GPUTextureRegion r;
            SDL_GPUTextureTransferInfo dst;
            SDL_GPUFence *fence;
            const uint8_t *got;

            pdo_xtex_level(&info, l, &li, &off);
            need = li.width * li.height * 4;
            want = SDL_malloc(need);
            pdo_xtex_decode(&li, src + off, want);
            tbi.size = need;
            tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
            cmd = SDL_AcquireGPUCommandBuffer(dev);
            copy = SDL_BeginGPUCopyPass(cmd);
            SDL_zero(r);
            r.texture = g->tex;
            r.mip_level = l;
            r.w = li.width;
            r.h = li.height;
            r.d = 1;
            SDL_zero(dst);
            dst.transfer_buffer = tb;
            SDL_DownloadFromGPUTexture(copy, &r, &dst);
            SDL_EndGPUCopyPass(copy);
            fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            got = SDL_MapGPUTransferBuffer(dev, tb, false);
            for (uint32_t k = 0; k < need; k++)
                diff += got[k] != want[k];
            SDL_UnmapGPUTransferBuffer(dev, tb);
            SDL_ReleaseGPUTransferBuffer(dev, tb);
            SDL_free(want);
            checked++;
            if (diff) {
                pdo_xtex_info last;
                uint32_t lo;
                pdo_xtex_level(&info, levels - 1, &last, &lo);
                bad++;
                SDL_Log("capture: texture %08x fmt %08x level %u (%ux%u of %u levels): %u of %u bytes differ on the GPU"
                        " (hash kept %016llx, now %016llx, last used %llu, staged in %llu, frame %llu)",
                        g->data, g->format_word, l, li.width, li.height, levels, diff, need,
                        (unsigned long long)g->hash, (unsigned long long)hash_bytes(src, lo + last.bytes),
                        (unsigned long long)g->last_used, (unsigned long long)g->staged_in,
                        (unsigned long long)F->frame);
            }
        }
    }
    SDL_Log("capture: %u texture levels checked against guest memory, %u differ", checked, bad);
}

static void capture_steps(void)
{
    const char *e = getenv("PDO_CAPTURE_STEP");
    uint32_t step = e ? (uint32_t)atol(e) : 0, total = F->ncmds;

    if (!step)
        return;
    for (uint32_t lim = step; lim < total; lim += step) {
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        char path[64];
        Uint32 w, h;

        if (!cmd)
            break;
        replay_limit = lim;
        replay(cmd);
        cur_size(&w, &h);
        snprintf(path, sizeof path, "capture_%05llu_%04u.bmp", (unsigned long long)F->frame, lim);
        save_texture(&cmd, cur_color(), (int)w, (int)h, path);
        if (cmd)
            SDL_SubmitGPUCommandBuffer(cmd);
    }
    replay_limit = UINT32_MAX;
}

/* Handles window and key events. Only the thread that made the window may
 * (Windows sends its messages there); elsewhere this does nothing. */
/* Escape gave the mouse back to the desktop (pdo_gfx_pump_events). */
static bool mouse_released;

/* Mouse motion only, the rest left queued: while frame generation spaces
 * its pictures (lsfg_show) the main loop is not pumping, and the game would
 * read the mouse up to a game frame late. */
static void take_mouse_motion(void)
{
    SDL_Event ev[16];
    int n;

    SDL_PumpEvents();
    while ((n = SDL_PeepEvents(ev, 16, SDL_GETEVENT, SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_MOTION)) > 0)
        for (int i = 0; i < n; i++)
            if (window && SDL_GetWindowRelativeMouseMode(window))
                pdo_mouse_add(ev[i].motion.xrel, ev[i].motion.yrel);
}

bool pdo_gfx_pump_events(void)
{
    SDL_Event ev;
    bool keep_going = true;

    if (window_thread && SDL_GetCurrentThreadID() != window_thread)
        return true;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT)
            keep_going = false;
        if (ev.type == SDL_EVENT_MOUSE_MOTION && window && SDL_GetWindowRelativeMouseMode(window))
            pdo_mouse_add(ev.motion.xrel, ev.motion.yrel);
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE && !pdo_overlay_open())
            mouse_released = true;      /* Escape gives the mouse back */
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_WINDOW_FOCUS_LOST)
            mouse_released = false;     /* a click (or coming back) takes it again */
        /* Remember the window (saved when the game closes, see note_quit):
         * its place and size when neither fullscreen nor maximized. */
        if ((ev.type == SDL_EVENT_WINDOW_MOVED || ev.type == SDL_EVENT_WINDOW_RESIZED ||
             ev.type == SDL_EVENT_WINDOW_MAXIMIZED || ev.type == SDL_EVENT_WINDOW_RESTORED) && window) {
            pdo_settings *st = pdo_settings_get();
            SDL_WindowFlags fl = SDL_GetWindowFlags(window);
            if (!(fl & SDL_WINDOW_FULLSCREEN)) {
                st->win_maximized = (fl & SDL_WINDOW_MAXIMIZED) != 0;
                if (!st->win_maximized) {
                    SDL_GetWindowPosition(window, &st->win_x, &st->win_y);
                    SDL_GetWindowSize(window, &st->win_w, &st->win_h);
                }
            }
        }
        if (pdo_overlay_event(&ev))
            continue;
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F11 && !ev.key.repeat) {
            pdo_dump_threads();
            pdo_dump_guest_waits();
        }
        /* Alt+Enter: borderless fullscreen (the desktop's mode) or back. */
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT) &&
            !ev.key.repeat && window) {
            pdo_settings *st = pdo_settings_get();
            st->fullscreen = !st->fullscreen;
            SDL_SetWindowFullscreen(window, st->fullscreen);
            pdo_settings_save();
            continue;
        }
        if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F12 && !ev.key.repeat) {
            /* The next whole frame: frames + 1 is often being recorded
             * already, and draws recorded before the key missed what a
             * capture records (the CPU's vertices beside GPU vertex-program
             * draws: PDO_CAPTURE_CPUVSH drew a frame twice the same way). */
            capture_frame = frames + 2;
            SDL_Log("capture: frame %llu", (unsigned long long)capture_frame);
        }
    }
    if (window) {
        /* The game has the mouse (hidden, held in the window) for mouse
         * aim while it is the window in front and the options are shut. */
        bool want = pdo_settings_get()->mouse_aim && !mouse_released && !pdo_overlay_open() &&
                    (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
        if (want != SDL_GetWindowRelativeMouseMode(window))
            SDL_SetWindowRelativeMouseMode(window, want);
        pdo_mouse_set_captured(want);
    }
    return keep_going;
}

/* The finished frame onto the window, 4:3, 16:9 (the game's widescreen
 * mode) or stretched, softened if asked,
 * with the overlay over it. */
/* The window's image, taken before the frame was drawn (draw_frame_main,
 * interpolating): present_to_window then uses it. */
static SDL_GPUTexture *pre_swap;
static Uint32 pre_sw, pre_sh;

static SDL_GPUTexture *lsfg_target(Uint32 *w, Uint32 *h);

static void present_to_window(SDL_GPUCommandBuffer *cmd)
{
    const pdo_settings *st = pdo_settings_get();
    SDL_GPUTexture *swap = NULL;
    Uint32 sw = 0, sh = 0;
    SDL_Surface *img;
    bool changed;
    SDL_GPUColorTargetInfo ct;
    SDL_GPURenderPass *pass;
    SDL_GPUViewport vp, fitv;

    if (!have_swapchain || !screen_pipeline)
        return;
    img = pdo_overlay_image(&changed);
    if (img && changed && overlay_tex) {
        Uint32 bytes = PDO_OVERLAY_W * PDO_OVERLAY_H * 4;
        SDL_GPUTransferBufferCreateInfo tbi = { SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes, 0 };
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        if (tb) {
            uint8_t *map = SDL_MapGPUTransferBuffer(dev, tb, false);
            SDL_GPUTextureTransferInfo src;
            SDL_GPUTextureRegion dst;
            SDL_GPUCopyPass *copy;

            for (int y = 0; y < PDO_OVERLAY_H; y++)
                memcpy(map + (size_t)y * PDO_OVERLAY_W * 4, (uint8_t *)img->pixels + (size_t)y * img->pitch,
                       PDO_OVERLAY_W * 4);
            SDL_UnmapGPUTransferBuffer(dev, tb);
            copy = SDL_BeginGPUCopyPass(cmd);
            SDL_zero(src);
            src.transfer_buffer = tb;
            SDL_zero(dst);
            dst.texture = overlay_tex;
            dst.w = PDO_OVERLAY_W;
            dst.h = PDO_OVERLAY_H;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
            SDL_EndGPUCopyPass(copy);
            SDL_ReleaseGPUTransferBuffer(dev, tb);
        }
    }
    {   /* Blocks while the GPU still has the frames in flight: GPU-bound time. */
        Uint64 t0 = SDL_GetTicksNS();
        bool ok = pre_swap || ls_draw ? true : SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh);
        if (ls_draw)
            swap = lsfg_target(&sw, &sh);
        if (pre_swap) {
            swap = pre_swap;
            sw = pre_sw;
            sh = pre_sh;
            pre_swap = NULL;
        }
        swap_wait_ns += SDL_GetTicksNS() - t0;
        main_wait_ns += SDL_GetTicksNS() - t0;
        if (!ok || !swap)
            return;
    }
    {   /* The picture: the game's 4:3, or 16:9 in its widescreen mode. fit:
         * all of it, bars elsewhere -- also where the overlay goes, so it is
         * whole in every mode; zoomed to fill, the overflow cut off;
         * stretched to the window. */
        float aw = pdo_widescreen_active() ? frame_h * 16.0f / 9.0f : (float)frame_w;
        float kf = SDL_min((float)sw / aw, (float)sh / frame_h);
        float kz = SDL_max((float)sw / aw, (float)sh / frame_h);
        float k = st->fit == PDO_FIT_ZOOM ? kz : kf;
        SDL_zero(fitv);
        fitv.w = SDL_floorf(aw * kf);
        fitv.h = SDL_floorf(frame_h * kf);
        fitv.x = SDL_floorf((sw - fitv.w) / 2);
        fitv.y = SDL_floorf((sh - fitv.h) / 2);
        fitv.max_depth = 1.0f;
        SDL_zero(vp);
        if (st->fit == PDO_FIT_STRETCH) {
            vp.w = (float)sw;
            vp.h = (float)sh;
        } else {
            vp.w = SDL_floorf(aw * k);
            vp.h = SDL_floorf(frame_h * k);
            vp.x = SDL_floorf((sw - vp.w) / 2);
            vp.y = SDL_floorf((sh - vp.h) / 2);
        }
        vp.max_depth = 1.0f;
    }
    SDL_zero(ct);
    ct.texture = swap;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.clear_color.a = 1.0f;
    pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!pass)
        return;
    {
        SDL_GPUTextureSamplerBinding tsb = { output, linear };
        float params[4] = { st->soft_filter ? 1.0f : 0.0f, 1.0f / (float)frame_h, st->fxaa && st->msaa == 1 ? 1.0f : 0.0f, 0 };
        SDL_SetGPUViewport(pass, &vp);
        SDL_BindGPUGraphicsPipeline(pass, screen_pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, params, sizeof params);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }
    if (img && overlay_tex && overlay_pipeline) {
        SDL_GPUTextureSamplerBinding tsb = { overlay_tex, linear };
        float params[4] = { 0, 0, 0, 0 };
        SDL_SetGPUViewport(pass, &fitv);
        SDL_BindGPUGraphicsPipeline(pass, overlay_pipeline);
        SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, params, sizeof params);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    ls_drawn = ls_draw;
}

/* Frame generation's picture of the window: the window's size and format. */
static SDL_GPUTexture *lsfg_target(Uint32 *w, Uint32 *h)
{
    int ww = 0, wh = 0;

    SDL_GetWindowSizeInPixels(window, &ww, &wh);
    if (ww <= 0 || wh <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
        return NULL;
    if (!ls_tex || ls_w != (Uint32)ww || ls_h != (Uint32)wh) {
        SDL_GPUTextureCreateInfo ti;
        if (ls_tex) {
            SDL_WaitForGPUIdle(dev);
            SDL_ReleaseGPUTexture(dev, ls_tex);
        }
        SDL_zero(ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GetGPUSwapchainTextureFormat(dev, window);
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;     /* kept as a color target between passes */
        ti.width = (Uint32)ww;
        ti.height = (Uint32)wh;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        ls_tex = SDL_CreateGPUTexture(dev, &ti);
        ls_w = ls_tex ? (Uint32)ww : 0;
        ls_h = ls_tex ? (Uint32)wh : 0;
    }
    *w = ls_w;
    *h = ls_h;
    return ls_tex;
}

/* The game frame in ls_tex once the GPU has done the generating: the
 * frames generated before it, spaced over the time between game frames,
 * then the frame itself, each into a swap chain image of its own. A cut to
 * another scene shows the frame alone. The number of pictures shown. */
static int lsfg_show(uint32_t generated)
{
    int shown = 0;
#ifdef PDO_HAVE_LSFG
    Uint64 spacing, first = 0;

    if (pdo_lsfg_scene_cut())
        generated = 0;
    spacing = pdo_lsfg_spacing(generated + 1);
    for (uint32_t f = 0; f <= generated; f++) {
        SDL_GPUCommandBuffer *c;
        SDL_GPUTexture *swap = NULL;
        Uint32 sw = 0, sh = 0;
        Uint64 t0;

        if (f > 0 && spacing) {
            Uint64 due = first + spacing * f, now;
            take_mouse_motion();
            now = SDL_GetTicksNS();
            if (due > now)
                SDL_DelayPrecise(due - now);
        }
        c = SDL_AcquireGPUCommandBuffer(dev);
        if (!c)
            break;
        t0 = SDL_GetTicksNS();
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(c, window, &swap, &sw, &sh))
            swap = NULL;
        swap_wait_ns += SDL_GetTicksNS() - t0;
        main_wait_ns += SDL_GetTicksNS() - t0;
        if (swap)
            pdo_lsfg_record_output(c, f < generated ? (int)f : -1, ls_tex, ls_w, ls_h, swap, sw, sh);
        SDL_SubmitGPUCommandBuffer(c);
        if (!swap)
            break;
        if (f == 0)
            first = SDL_GetTicksNS();
        shown++;
    }
#else
    (void)generated;
#endif
    return shown;
}

bool pdo_gfx_lsfg_active(void)
{
    return lsfg_on;
}

/* ── Frame interpolation ─────────────────────────────────────────────────
 * The game runs at 60; frames between two of its frames are drawn by
 * blending each draw of the newer frame with the same draw in the frame
 * before. "The same draw" is a guess from what the draw drew from: the kind
 * of draw, its program (or fixed-function layout), the host address of its
 * vertex data, vertex and index counts and first texture. Draws alike in
 * all of that (a flock of one enemy) pair up nearest first: by how little
 * their matrices, or their program constants, differ between the frames.
 * The game's draw order is no guide -- it sorts see-through things by
 * depth, and pairing in order swapped them as they flew. A group whose
 * size changed (a pack losing and gaining members) still pairs as far as
 * it goes: refusing the whole group made it jump between the blended past
 * and the unblended newer frame as members came and went. A draw whose
 * partner is far beyond its own nearest is not blended either. A
 * fixed-function draw whose position on screen jumps (a teleport, a new
 * object recycling a buffer) is not blended either, and when over half of
 * those on screen do, the camera cut and nothing is. Unblended draws show
 * the newer frame. */

typedef struct draw_key {
    uint64_t h;
    uint32_t cmd;
} draw_key;

/* A screen-space draw's place in its texture: the first vertex's stage-0
 * coordinates. Glyphs and icons cut from one atlas are
 * alike in every other way, and games draw them from the same buffer slots
 * whichever they are: JSRF's custom-graffiti keyboard's "3" paired with the
 * "4" beside it when the selection moved and showed between them for a
 * frame. 0 for 3D draws and vertex programs (their texture coordinates move
 * with animation, and Orta's world draws have no view of their own, so a
 * "no view" UI plane cannot be told from them) and when unknown. */
static uint64_t uv_sig(const frame_data *f, const command *c)
{
    float uv[2];

    if (c->vsh)
        return 0;
    if (c->fvf >= 0) {
        const pdo_fvf_uniforms *u = &f->fvf_us[c->fvf];
        int set = u->stage_set[0];
        if (!(u->layout0[2] & PDO_FVF_PRE) || set < 0 || set >= u->layout0[3] || set >= 4 || u->tc_n[set] < 2)
            return 0;
        if ((uint32_t)u->layout0[0] + (uint32_t)u->tc_off[set] + 8u > f->nblob)
            return 0;
        memcpy(uv, f->blob + u->layout0[0] + u->tc_off[set], 8);
    } else {
        if (!c->index_count || c->first_index >= f->nindices || f->indices[c->first_index] >= f->nverts)
            return 0;
        memcpy(uv, f->verts[f->indices[c->first_index]].tc[0], 8);
    }
    if (!(SDL_fabsf(uv[0]) < 1e6f && SDL_fabsf(uv[1]) < 1e6f))
        return 0;                       /* NaN or huge: unknown */
    /* to 1/4096: the same place in the atlas, whatever float noise */
    return ((uint64_t)(uint32_t)(int32_t)lroundf(uv[0] * 4096.0f) << 32) | (uint32_t)(int32_t)lroundf(uv[1] * 4096.0f);
}

/* any_src: whatever vertex data it drew from (match_frames' second pass). */
static uint64_t key_of(const frame_data *f, const command *c, bool any_src, bool any_prog)
{
    uint64_t h = 1469598103934665603ull;
    uint64_t v[8];

    v[0] = c->vsh ? 1 : c->fvf >= 0 ? 2 : 3;
    /* Vertex programs: their program, but for any_prog (match_frames' third
     * pass). Episode 4's battleships draw each part twice in one place, a
     * pass per program: keyed without it, the passes of alike ships paired
     * with each other's and the ships swapped. As a boost or a brake ends the
     * game puts every model on other programs (0x9E -> 0x20E for an enemy's
     * body): those find no partner by program and pair in the third pass. */
    v[1] = c->vsh ? (any_prog ? 0 : (uint64_t)(uint32_t)c->vu.info[0])
         : c->fvf >= 0 ? ((uint64_t)(uint32_t)f->fvf_us[c->fvf].layout0[2] << 32) | (uint32_t)f->fvf_us[c->fvf].layout0[1]
         : c->draw.ps.topology;
    v[2] = any_src ? 0 : c->id_src;
    v[3] = c->id_nv;
    v[4] = c->vsh || c->fvf >= 0 ? c->vsh_index_count : c->index_count;
    v[5] = c->draw.tex[0].data;
    v[6] = c->vsh && !any_prog ? (uint64_t)(uint32_t)c->vu.info[1] : 0;
    v[7] = uv_sig(f, c);    /* screen space: the atlas cell */
    for (int i = 0; i < 8; i++)
        h = (h ^ v[i]) * 1099511628211ull;
    return h;
}

static int cmp_key(const void *a, const void *b)
{
    const draw_key *x = a, *y = b;
    if (x->h != y->h)
        return x->h < y->h ? -1 : 1;
    return x->cmd < y->cmd ? -1 : x->cmd > y->cmd;
}

/* Draws on the GPU from the game's bytes, or from CPU-made vertices with
 * their source known (pdo_gfx_set_draw_source). */
static bool blendable(const command *c)
{
    return c->kind == CMD_DRAW && c->id_src;
}

/* A 3D draw: by a vertex program, or fixed-function not pretransformed --
 * not one placed on the screen by its vertices (HUD, menus, CPU-made). */
static bool is_3d(const frame_data *f, const command *c)
{
    return c->vsh || (c->fvf >= 0 && !(f->fvf_us[c->fvf].layout0[2] & PDO_FVF_PRE));
}

/* any_src: only the 3D draws skip[] does not mark, keyed without their
 * data. Screen-space ones stay out: menu words drawn alike (EASY, NORMAL,
 * HARD) each looked like the lone partner of another and jittered. */
/* The render target a draw goes to, for its key: its memory and its size
 * word (the back buffer and Orta's 512x512 target both read data 0). */
static uint64_t target_key(const command *c)
{
    return ((uint64_t)c->surf.size_word << 32) | c->surf.data;
}

static uint32_t keys_of(const frame_data *f, draw_key **keys, uint32_t *cap, bool any_src, bool any_prog,
                        const uint8_t *skip)
{
    uint32_t n = 0;
    uint64_t target = 0;
    for (uint32_t i = 0; i < f->ncmds; i++) {
        /* Draws pair only within the same render target (Concept of
         * Love: JSRF's boost draws the scene into two copies on alternate
         * frames, and paired across them the copies went out of step).
         * Orta draws its scene into a 512x384 target and some of it into a
         * 512x512 one too. */
        if (f->cmds[i].kind == CMD_TARGET)
            target = target_key(&f->cmds[i]);
        if (blendable(&f->cmds[i]) && !(any_src && (skip[i] || !is_3d(f, &f->cmds[i]))) &&
            !(any_prog && !f->cmds[i].vsh)) {
            *keys = grow(*keys, cap, n + 1, sizeof **keys);
            /* 3D draws always without their data's address: the game hands
             * vertex buffers out from a pool, so the same buffer is not
             * the same object (fighters in a pack warped between each
             * other's places); they pair by where they are. Screen-space
             * ones (menu words drawn alike) keep it. */
            (*keys)[n].h = (key_of(f, &f->cmds[i], any_src || is_3d(f, &f->cmds[i]), any_prog) ^ target) *
                           1099511628211ull;
            (*keys)[n].cmd = i;
            n++;
        }
    }
    qsort(*keys, n, sizeof **keys, cmp_key);
    return n;
}

/* Where a fixed-function draw's origin lands on screen (NDC), and whether
 * it is in front of the eye. */
static bool origin_ndc(const float *wvp, float *x, float *y)
{
    float w = wvp[15];
    if (w <= 1e-4f)
        return false;
    *x = wvp[12] / w;
    *y = wvp[13] / w;
    return true;
}

/* Where a draw's first and last vertices are, as the raw floats of its
 * position (fixed-function), or of its first float input (programs):
 * screen pixels for pretransformed sprites, so that HUD pieces drawn alike
 * (one shared vertex area, four vertices) tell apart. False: none read. */
static bool draw_corners(const frame_data *f, const command *c, float out[8])
{
    uint32_t base, stride, n = c->id_nv ? c->id_nv : 1;

    if (!c->vsh && c->fvf < 0) {           /* CPU-made: clip-space positions */
        if (c->vbase + n > f->nverts)
            return false;
        memcpy(out, f->verts[c->vbase].pos, 16);
        memcpy(out + 4, f->verts[c->vbase + n - 1].pos, 16);
        return true;
    }
    if (c->vsh) {
        int r;
        for (r = 0; r < 16; r++)
            if ((c->vu.inputs[r][3] & 15) == 2 && (c->vu.inputs[r][3] >> 4) >= 3)
                break;
        if (r == 16)
            return false;
        base = (uint32_t)c->vu.inputs[r][0] + (uint32_t)c->vu.inputs[r][2];
        stride = (uint32_t)c->vu.inputs[r][1];
    } else {
        base = (uint32_t)f->fvf_us[c->fvf].layout0[0];
        stride = (uint32_t)f->fvf_us[c->fvf].layout0[1];
    }
    for (int k = 0; k < 2; k++) {
        uint32_t at = base + (k ? (n - 1) * stride : 0);
        if (at + 16 > f->nblob)
            return false;
        memcpy(out + k * 4, f->blob + at, 16);
    }
    return true;
}

/* Where two alike draws are, cheaply: the screen and world translation of
 * a fixed-function draw, a program draw's anchors, else its corners. To
 * pick a big group's few nearest before the full distance. */
static float near_of(const frame_data *pf, const command *pc, const frame_data *cf, const command *c)
{
    float d = 0.0f;
    if (c->fvf >= 0 && pc->fvf >= 0) {
        const pdo_fvf_uniforms *u = &cf->fvf_us[c->fvf], *pu = &pf->fvf_us[pc->fvf];
        for (int i = 12; i < 16; i++)
            d += SDL_fabsf(u->wvp[i] - pu->wvp[i]);
        for (int i = 12; i < 15; i++)
            d += SDL_fabsf(u->world[i] - pu->world[i]);
    } else if (c->vsh && c->anchored && pc->anchored) {
        for (int i = 0; i < 2; i++)
            d += SDL_fabsf(c->anchor[i] - pc->anchor[i]) + SDL_fabsf(c->anchor[4 + i] - pc->anchor[4 + i]);
    } else {
        float a4[8], b4[8];
        if (draw_corners(pf, pc, a4) && draw_corners(cf, c, b4))
            for (int i = 0; i < 8; i++)
                if (a4[i] == a4[i] && b4[i] == b4[i])
                    d += SDL_fabsf(a4[i] - b4[i]);
    }
    return d == d ? d : 3.4e38f;
}

/* How far apart two alike draws are, to pair a group nearest first: where
 * their vertices are, and their matrices (fixed-function) or constant sets
 * (programs; what all objects share is equal and adds nothing). */
static float draw_distance(const frame_data *pf, const command *pc, const frame_data *cf, const command *c)
{
    float d = pc->id_src == c->id_src ? 0.0f : 1e-3f, a4[8], b4[8];   /* the same data: only a tie-break */

    if (draw_corners(pf, pc, a4) && draw_corners(cf, c, b4))
        for (int i = 0; i < 8; i++)
            if (a4[i] == a4[i] && b4[i] == b4[i])     /* not NaN */
                d += SDL_fabsf(a4[i] - b4[i]);

    if (!c->vsh && c->fvf < 0)
        return d;                           /* CPU-made: positions only */
    /* Another vertex program (key_of): only a tie-break, as the data. */
    float vsh_other = c->vsh && pc->vsh && (pc->vu.info[0] != c->vu.info[0] || pc->vu.info[1] != c->vu.info[1])
        ? 1e-3f : 0.0f;
    d += vsh_other;
    if (c->vsh && c->anchored && pc->anchored) {
        /* Where the draw is: its first and last vertex through the program
         * (d3d_hle.c). Constants told episode 4's alike battleships apart
         * no better than chance (all 192 read, through a0). */
        /* oPos ends in screen pixels (the program's own viewport transform,
         * which shaders/vsh.vert undoes); z and w are depth, and summed in
         * they swamped the position (a ship's own move measured 396 against
         * 453 for another ship, and packs swapped). x, y only. */
        float e = (pc->id_src == c->id_src ? 0.0f : 1e-3f) + vsh_other;
        for (int k = 0; k < 2; k++) {
            const float *a = pc->anchor + 4 * k, *b = c->anchor + 4 * k;
            float dx = a[0] - b[0], dy = a[1] - b[1];
            if (dx == dx && dy == dy)       /* not NaN */
                e += SDL_fabsf(dx) + SDL_fabsf(dy);
        }
        return e;
    }
    if (c->vsh) {
        /* Only the registers the program reads: the file is shared, and the
         * rest hold whatever the draws before left (episode 4's four alike
         * battleships paired worse with their own last frame than with
         * another, and their constants "jumped"). */
        const float *a = pf->vconsts[pc->vu.info[2]], *b = cf->vconsts[c->vu.info[2]];
        for (int i = 0; i < 192 * 4; i++)
            if ((c->consts_read[i >> 5] >> ((i >> 2) & 7) & 1) && SDL_fabsf(a[i]) < 1e6f &&
                SDL_fabsf(b[i]) < 1e6f)     /* not stale, huge or NaN registers */
                d += SDL_fabsf(a[i] - b[i]);
    } else {
        /* From where it was, or from where it was going (moved on by how it
         * moved the frame before), whichever is nearer: in a pack flying in
         * formation a fighter moves about as far in a frame as the gap to
         * the next, and nearest from where it was paired fighters with
         * their neighbours now and then -- they warped between each
         * other's places. Something that stopped is still found where it
         * was. */
        const float *a = pf->fvf_us[pc->fvf].wvp, *b = cf->fvf_us[c->fvf].wvp;
        const float *aw = pf->fvf_us[pc->fvf].world, *bw = cf->fvf_us[c->fvf].world;
        float ds = 0.0f, dp = 0.0f;
        for (int i = 0; i < 16; i++)
            ds += SDL_fabsf(a[i] - b[i]) + SDL_fabsf(aw[i] - bw[i]);
        if (pc->has_mvel) {
            for (int i = 0; i < 16; i++)
                dp += SDL_fabsf(a[i] + pc->mvel[i] - b[i]) + SDL_fabsf(aw[i] + pc->mvel[16 + i] - bw[i]);
            if (dp < ds)
                ds = dp;
        }
        d += ds;
    }
    return d;
}

/* How far a draw's vertices strayed from their average move since the
 * frame before (a draw carried along whole blends well), as a share of
 * the draw's own size (0: together). The ring of arcs round the
 * glide wing's reticle is one draw rebuilt each frame in another order --
 * a vertex moved 7.6 in a draw 9.8 across -- and blended vertex by vertex
 * its arcs scattered over the screen. Positions: the first three floats
 * of each vertex, stride apart; ncur, nprev: bytes there to read. */
static float stray_n(const uint8_t *cur, const uint8_t *prev, uint32_t stride, uint32_t nv, uint32_t ncur,
                     uint32_t nprev, int nc)
{
    float lo[3] = { 3e38f, 3e38f, 3e38f }, hi[3] = { -3e38f, -3e38f, -3e38f }, dmax = 0.0f, size = 0.0f;
    float mean[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t bytes = 4u * (uint32_t)nc;

    if (!nv || !stride || nc < 1 || nc > 3 || (uint64_t)(nv - 1) * stride + bytes > ncur ||
        (uint64_t)(nv - 1) * stride + bytes > nprev)
        return 0.0f;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t v = 0; v < nv; v++) {
            float p1[3] = { 0.0f, 0.0f, 0.0f }, p0[3] = { 0.0f, 0.0f, 0.0f }, d = 0.0f;
            memcpy(p1, cur + (size_t)v * stride, bytes);
            memcpy(p0, prev + (size_t)v * stride, bytes);
            for (int k = 0; k < nc; k++) {
                if (pass == 0) {
                    lo[k] = SDL_min(lo[k], p1[k]);
                    hi[k] = SDL_max(hi[k], p1[k]);
                    mean[k] += (p1[k] - p0[k]) / (float)nv;
                } else {
                    d += SDL_fabsf(p1[k] - p0[k] - mean[k]);
                }
            }
            if (d > dmax)
                dmax = d;
        }
    for (int k = 0; k < nc; k++)
        size += hi[k] - lo[k];
    return dmax > 1e-3f && size > 0.0f ? dmax / size : 0.0f;  /* NaN: 0, coherent as before */
}

/* A particle system (see-through quads the game builds itself each frame,
 * four vertices a particle) whose particles changed places in the vertex
 * data: one quad moved by over its own size from where the rest moved
 * (their median). Smoke is re-sorted back to front as it drifts, and fire
 * respawns particles in freed slots; blended vertex by vertex, a particle
 * slid toward another one's place, and flames and puffs flashed, swapped
 * and vanished for an in-between picture (episode 8). stray() measures
 * against the whole cloud, so a swap of neighbours passed it. */
static bool quads_resorted(const uint8_t *cur, const uint8_t *prev, uint32_t stride, uint32_t nv, uint32_t ncur,
                           uint32_t nprev)
{
    static float mv[1024];
    uint32_t nq = nv / 4, n = 0;
    float med;

    if (nq < 2 || nq > 1024 || stride < 12 || (uint64_t)(nv - 1) * stride + 12 > ncur ||
        (uint64_t)(nv - 1) * stride + 12 > nprev)
        return false;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t q = 0; q < nq; q++) {
            float c1[3] = { 0, 0, 0 }, c0[3] = { 0, 0, 0 }, lo[3], hi[3], size = 0.0f, d = 0.0f;
            for (int k = 0; k < 3; k++)
                lo[k] = 3.4e38f, hi[k] = -3.4e38f;
            for (uint32_t v = 4 * q; v < 4 * q + 4; v++) {
                float a[3], b[3];
                memcpy(a, cur + (size_t)v * stride, 12);
                memcpy(b, prev + (size_t)v * stride, 12);
                for (int k = 0; k < 3; k++) {
                    c1[k] += a[k] * 0.25f;
                    c0[k] += b[k] * 0.25f;
                    lo[k] = SDL_min(lo[k], a[k]);
                    hi[k] = SDL_max(hi[k], a[k]);
                }
            }
            for (int k = 0; k < 3; k++) {
                size = SDL_max(size, hi[k] - lo[k]);
                d += (c1[k] - c0[k]) * (c1[k] - c0[k]);
            }
            d = SDL_sqrtf(d);
            if (!(d == d) || !(size == size))
                return false;   /* NaN: not positions */
            if (pass == 0) {
                mv[n++] = d;
            } else if (size > 0.0f && d - med > size) {
                return true;
            }
        }
        if (pass == 0) {        /* the median move, by a partial selection */
            for (uint32_t i = 0; i <= n / 2; i++)
                for (uint32_t j = i + 1; j < n; j++)
                    if (mv[j] < mv[i]) {
                        float t = mv[i];
                        mv[i] = mv[j];
                        mv[j] = t;
                    }
            med = mv[n / 2];
        }
    }
    return false;
}

static float stray(const uint8_t *cur, const uint8_t *prev, uint32_t stride, uint32_t nv, uint32_t ncur,
                     uint32_t nprev)
{
    return stray_n(cur, prev, stride, nv, ncur, nprev, 3);
}

typedef struct pair_cand {
    float d;
    uint16_t a, b;      /* index in the group: previous frame, this frame */
} pair_cand;

static int cmp_cand(const void *x, const void *y)
{
    const pair_cand *a = x, *b = y;
    return a->d < b->d ? -1 : a->d > b->d;
}

/* Pair cur's draws with prev's (prev may be NULL: nothing pairs). */
static uint32_t st_close;   /* alike draws paired with a runner-up near as good (log) */
static uint32_t st_swapped; /* pairs undone: another draw was far nearer (log) */
static uint32_t st_rebuilt; /* pairs not blended: vertices rebuilt in another order (log) */

/* Whether a draw samples the picture: a texture that is a render target
 * (texture_for found the memory is one -- the back buffer too, whose
 * target reads data 0 in the frame's own commands), or memory this frame
 * drew to or copied to or from (a copy of the picture made into a
 * texture). A copy of the picture pasted back through a quad. */
static uint32_t rt_addr[32];      /* the frame's targets and copies, phys (rt_gather) */
static int rt_n;
static const frame_data *rt_of;
static uint64_t rt_frame;           /* frame data is pooled: the frame it was for */

static void rt_gather(const frame_data *f)
{
    rt_n = 0;
    rt_of = f;
    rt_frame = f->frame;
    for (uint32_t q = 0; q < f->ncmds && rt_n < 30; q++) {
        const command *t = &f->cmds[q];
        if (t->kind != CMD_TARGET && t->kind != CMD_COPY)
            continue;
        if (t->surf.data)
            rt_addr[rt_n++] = phys(t->surf.data);
        if (t->kind == CMD_COPY && t->surf2.data)
            rt_addr[rt_n++] = phys(t->surf2.data);
    }
}

static bool reads_target(const frame_data *f, const command *c)
{
    uint32_t k = (uint32_t)(c - f->cmds);
    if (rt_of != f || rt_frame != f->frame)
        rt_gather(f);
    for (int st = 0; st < 4; st++) {
        uint32_t d = c->draw.tex[st].data;
        if (f->tex[k * 4 + st].target_data)
            return true;
        if (d)
            for (int i = 0; i < rt_n; i++)
                if (phys(d) == rt_addr[i])
                    return true;
    }
    return false;
}

/* match_frames' last call, by stage (the slow-pairing log): the key
 * passes, the cross-model pass, the wrong-pair check, the picture's move,
 * the diagnostics, the cut test. */
static Uint64 mf_ns[6];
#define MF_MARK(i) do { Uint64 _t = SDL_GetTicksNS(); mf_ns[i] = _t - mf_t0; mf_t0 = _t; } while (0)

static void match_frames(frame_data *prev, frame_data *cur)
{
    Uint64 mf_t0 = SDL_GetTicksNS();
    static draw_key *pk, *ck;
    static uint8_t *taken, *grouped;    /* prev draws paired; cur draws that had alike ones */
    static uint32_t cap_pk, cap_ck, cap_taken, cap_grouped;
    uint32_t np = 0, nc, i = 0, j = 0, fvf_pairs = 0, fvf_jumps = 0;
    /* Groups of alike 3D draws whose size changed from the frame before
     * (the log, below): how many, and the first few as before -> now x nv. */
    uint32_t resized = 0;
    float cam_step = -1.0f, cam_avg = -1.0f;    /* the picture's move, and the eight before (log) */
    static float moves[512];                     /* on-screen moves of paired draws (log) */
    uint32_t n_moves = 0;
    /* Why 3D draws were not blended, and paired fixed-function draws whose
     * matrices did not change at all (a frame the game held still). */
    uint32_t n_forced = 0, n_jumped = 0, n_rebuilt = 0, n_swapped = 0, n_still = 0, n_fvf_paired = 0;
    uint32_t n_lod = 0;     /* vertex-program draws paired across models (below) */
    uint32_t n_progswitch = 0;
    uint32_t n_outblend = 0;    /* same program, a read register jumped: outputs blended */
    uint32_t n_screen = 0;  /* copies of the picture pasted on screen, not blended */  /* paired with the same draw on another program (boost, brake end) */
    char resized_txt[96] = "";

    for (uint32_t k = 0; k < cur->ncmds; k++) {
        command *c = &cur->cmds[k];
        if (c->kind != CMD_DRAW)
            continue;
        c->prev_vbase = -1;
        c->unpaired = true;
        c->has_mvel = false;
        c->strayed = false;
        c->no_partner = false;
        if (c->vsh) {
            c->vu.interp[0] = -1;
            c->vu.interp[1] = 0;
            memset(c->vu.prev_prog, 0, sizeof c->vu.prev_prog);
            memcpy(c->vu.prev_scale, c->vu.scale, sizeof c->vu.prev_scale);
            memcpy(c->vu.prev_offset, c->vu.offset, sizeof c->vu.prev_offset);
        }
        if (c->fvf >= 0) {
            cur->fvf_us[c->fvf].interp[0] = -1;
            cur->fvf_us[c->fvf].interp2[0] = 0;
        }
    }
    if (!prev)
        return;
    taken = grow(taken, &cap_taken, prev->ncmds + 1, 1);
    grouped = grow(grouped, &cap_grouped, cur->ncmds + 1, 1);
    memset(taken, 0, prev->ncmds);
    memset(grouped, 0, cur->ncmds);
    /* Two passes: draws from the same vertex data; then a draw that found
     * none, with the one left in the previous frame that is alike but for
     * its data -- only when it is the one such in both frames. The game
     * builds some vertices (the dragon's) into a buffer that alternates
     * between two places: those parts never paired and showed a frame
     * ahead of the rest of the dragon. A third: vertex-program draws that
     * had nothing alike on their own program, with those left on another
     * (a boost's or a brake's end), nearest first. */
    memset(mf_ns, 0, sizeof mf_ns);
    mf_t0 = SDL_GetTicksNS();
    for (int pass = 0; pass < 3; pass++) {
    np = keys_of(prev, &pk, &cap_pk, pass >= 1, pass == 2, taken);
    nc = keys_of(cur, &ck, &cap_ck, pass >= 1, pass == 2, grouped);
    i = j = 0;
    while (i < np && j < nc) {
        uint32_t i2 = i, j2 = j;
        if (pk[i].h < ck[j].h) { i++; continue; }
        if (pk[i].h > ck[j].h) { j++; continue; }
        while (i2 < np && pk[i2].h == pk[i].h) i2++;
        while (j2 < nc && ck[j2].h == ck[j].h) j2++;
        for (uint32_t k = j; k < j2; k++)
            grouped[ck[k].cmd] = 1;
        if (pass == 1 && (i2 - i != 1 || j2 - j != 1)) {
            i = i2;
            j = j2;
            continue;               /* not the one such in both */
        }
        if (pass == 0 && i2 - i != j2 - j && is_3d(cur, &cur->cmds[ck[j].cmd])) {
            if (resized++ < 6) {
                size_t len = strlen(resized_txt);
                snprintf(resized_txt + len, sizeof resized_txt - len, " %u->%ux%u", i2 - i, j2 - j,
                         cur->cmds[ck[j].cmd].id_nv);
            }
        }
        if (i2 - i <= 256 && j2 - j <= 256) {
            uint32_t na = i2 - i, n = j2 - j, order[256];
            bool forced[256] = { false };
            static pair_cand *cand;
            static uint32_t cap_cand;

            /* Which previous draw each one pairs with: alone in both, the
             * one; otherwise nearest first, as many as pair (a pack whose
             * size changed: the rest of it stays blended, the new ones
             * show the newer frame). */
            for (uint32_t b = 0; b < n; b++)
                forced[b] = true;           /* until paired */
            if (n == 1 && na == 1) {
                order[0] = 0;
                forced[0] = false;
            } else {
                bool used_a[256] = { false }, used_b[256] = { false };
                uint32_t nc2 = 0;
                cand = grow(cand, &cap_cand, na * n, sizeof *cand);
                /* A big group (episode 10's boss: 247 alike crystal spikes, 61,000
                 * pairs a frame, most of the main thread): each draw's full
                 * distance only to its few nearest by where it is (near_of),
                 * not to every one. Groups up to 64 x 64 as before. */
                bool big = na * n > 4096;
                enum { NEAR_K = 6 };
                static uint16_t near_a[256][NEAR_K];
                static uint8_t near_n[256];
                if (big)
                    for (uint32_t b = 0; b < n; b++) {
                        float nd[NEAR_K];
                        const command *cb = &cur->cmds[ck[j + b].cmd];
                        near_n[b] = 0;
                        for (uint32_t a = 0; a < na; a++) {
                            float d = near_of(prev, &prev->cmds[pk[i + a].cmd], cur, cb);
                            int m = near_n[b];
                            if (m == NEAR_K && d >= nd[m - 1])
                                continue;
                            if (m < NEAR_K)
                                near_n[b]++;
                            else
                                m--;
                            while (m > 0 && nd[m - 1] > d) {    /* kept in order, nearest first */
                                nd[m] = nd[m - 1];
                                near_a[b][m] = near_a[b][m - 1];
                                m--;
                            }
                            nd[m] = d;
                            near_a[b][m] = (uint16_t)a;
                        }
                    }
                for (uint32_t a = 0; a < na; a++)
                    for (uint32_t b = 0; b < n; b++) {
                        if (big) {
                            bool keep = false;
                            for (int q = 0; q < near_n[b] && !keep; q++)
                                keep = near_a[b][q] == a;
                            if (!keep)
                                continue;
                        }
                        cand[nc2].d = draw_distance(prev, &prev->cmds[pk[i + a].cmd], cur, &cur->cmds[ck[j + b].cmd]);
                        /* Ties keep the draw order: a mesh drawn twice in one
                         * place (the dragon's passes, on other programs each)
                         * pairs first with first, second with second. As a
                         * boost or a brake ends every program changes, the
                         * other-program tie-break counted alike for all, and
                         * a pass blended toward another pass's constants: the
                         * dragon came out brighter for that frame. */
                        cand[nc2].d += 1e-5f * SDL_fabsf((float)a - (float)b);
                        cand[nc2].a = (uint16_t)a;
                        cand[nc2].b = (uint16_t)b;
                        nc2++;
                    }
                float best[256], second[256];
                for (uint32_t b = 0; b < n; b++)
                    best[b] = second[b] = 3.4e38f;
                for (uint32_t k = 0; k < nc2; k++)
                    if (cand[k].d < best[cand[k].b]) {
                        second[cand[k].b] = best[cand[k].b];
                        best[cand[k].b] = cand[k].d;
                    } else if (cand[k].d < second[cand[k].b]) {
                        second[cand[k].b] = cand[k].d;
                    }
                qsort(cand, nc2, sizeof *cand, cmp_cand);
                for (uint32_t k = 0; k < nc2; k++)
                    if (!used_a[cand[k].a] && !used_b[cand[k].b]) {
                        used_a[cand[k].a] = used_b[cand[k].b] = true;
                        order[cand[k].b] = cand[k].a;
                        /* Its nearest went to another: what is left can be
                         * far (the end of a chain of alike segments that
                         * each moved one along). Not blended. */
                        forced[cand[k].b] = cand[k].d > best[cand[k].b] * 2.0f + 1e-3f;
                        /* A close call (the log): the runner-up within 25%,
                         * and not the same (a draw made twice in one place). */
                        if (cand[k].d > 1e-2f && second[cand[k].b] < cand[k].d * 1.25f &&
                            second[cand[k].b] > cand[k].d * 1.02f) {
                            st_close++;
                        }
                        if (dbg_groups) {
                            const command *c0 = &cur->cmds[ck[j + cand[k].b].cmd];
                            float sec = 3.4e38f;
                            for (uint32_t q = 0; q < nc2; q++)
                                if (cand[q].b == cand[k].b && q != k && cand[q].d < sec)
                                    sec = cand[q].d;
                            SDL_Log("grp: cmd %u kind %d na %u n %u nv %u b %u -> a %u d %g best %g second %g%s",
                                    ck[j + cand[k].b].cmd, c0->vsh ? 1 : c0->fvf >= 0 ? 2 : 3, na, n, c0->id_nv,
                                    cand[k].b, cand[k].a, cand[k].d, best[cand[k].b], sec,
                                    forced[cand[k].b] ? " FORCED" : "");
                        }
                    }
            }
            for (uint32_t k = 0; k < n; k++) {
                const command *pc;
                command *c = &cur->cmds[ck[j + k].cmd];
                if (forced[k]) {
                    c->no_partner = true;
                    n_forced += is_3d(cur, c);
                    continue;           /* no partner (order[k] unset) */
                }
                pc = &prev->cmds[pk[i + order[k]].cmd];
                /* A copy of the picture pasted back on screen (the boost's
                 * and the brake's swirl lays one over the scene through a
                 * quad whose corners move each frame): not blended. Blended,
                 * the quad slid between its two places while what it shows
                 * is this moment's picture, and for that frame a shifted
                 * copy of the whole scene, the dragon in it, ghosted over
                 * the screen. Shown as the newer frame, it sits where the
                 * game puts it. */
                if (!is_3d(cur, c) && reads_target(cur, c)) {
                    n_screen++;
                    continue;
                }
                if (!c->vsh && c->fvf < 0 && !pc->vsh && pc->fvf < 0) {
                    /* Rebuilt in another order (below, fixed-function), on
                     * CPU-made vertices too (JSRF's
                     * lightning, a new zigzag each frame, blended point by
                     * point was all over the screen). */
                    uint32_t nb = (uint32_t)sizeof(pdo_vertex);
                    float sv = c->vbase + c->id_nv <= cur->nverts && pc->vbase + c->id_nv <= prev->nverts
                        ? stray((const uint8_t *)&cur->verts[c->vbase], (const uint8_t *)&prev->verts[pc->vbase], nb,
                                c->id_nv, (cur->nverts - c->vbase) * nb, (prev->nverts - pc->vbase) * nb)
                        : 0.0f;
                    c->strayed = sv > 0.2f;
                    if (sv > 0.5f || (c->strayed && pc->strayed)) {
                        st_rebuilt++;
                        n_rebuilt++;
                        continue;
                    }
                    c->prev_vbase = (int32_t)pc->vbase;
                    c->unpaired = false;
                    taken[pk[i + order[k]].cmd] = 1;
                    c->pair_prev = pk[i + order[k]].cmd;
                } else if (c->vsh && pc->vsh) {
                    bool same_in = true, regs_jumped = false;
                    {   /* Rebuilt in another order: as below, on every input of
                         * floats of a vertex program (JSRF's
                         * lightning keeps its positions and makes each zigzag
                         * from another input); r: the worst one. */
                        int r = -1;
                        float sv = 0.0f;
                        for (int q = 0; q < 16; q++) {
                            int nc = c->vu.inputs[q][3] >> 4;
                            if ((c->vu.inputs[q][3] & 15) != 2 || nc < 1 || pc->vu.inputs[q][1] != c->vu.inputs[q][1] ||
                                pc->vu.inputs[q][3] != c->vu.inputs[q][3])
                                continue;
                            uint32_t a = (uint32_t)c->vu.inputs[q][0] + (uint32_t)c->vu.inputs[q][2];
                            uint32_t b = (uint32_t)pc->vu.inputs[q][0] + (uint32_t)pc->vu.inputs[q][2];
                            if (a >= cur->nblob || b >= prev->nblob)
                                continue;
                            float v = stray_n(cur->blob + a, prev->blob + b, (uint32_t)c->vu.inputs[q][1], c->id_nv,
                                              cur->nblob - a, prev->nblob - b, nc > 3 ? 3 : nc);
                            if (v > sv)
                                sv = v, r = q;
                        }
                        /* The constants rebuilt: registers whose four values
                         * changed by over half their size in one frame
                         * (JSRF's lightning keeps its
                         * vertices and draws each zigzag through new random
                         * constants; blended, the mixed transforms flung
                         * the bolts over the screen). Things animated
                         * through constants (skinned characters, the
                         * camera) change a little a frame. */
                        int jumped_regs = 0, read_regs = 0;
                        {
                            const float *ca = prev->vconsts[pc->vu.info[2]], *cb = cur->vconsts[c->vu.info[2]];
                            for (int q = 0; q < 192; q++) {
                                float d = 0.0f, m = 0.0f;
                                if (!(c->consts_read[q >> 3] >> (q & 7) & 1))
                                    continue;   /* not read: another draw's leftovers */
                                read_regs++;
                                for (int e = 0; e < 4; e++) {
                                    float x = ca[4 * q + e], y = cb[4 * q + e];
                                    if (!(SDL_fabsf(x) < 1e6f && SDL_fabsf(y) < 1e6f))
                                        break;          /* stale, huge or NaN */
                                    d += SDL_fabsf(x - y);
                                    m += SDL_fabsf(x) + SDL_fabsf(y);
                                }
                                if (d > 0.5f * m && d > 1e-3f)
                                    jumped_regs++;
                            }
                        }
                        /* Rebuilt: four or more, and half of those it reads
                         * (JSRF's lightning: nearly all). Orta's creatures
                         * flap through their bone registers, and five or six
                         * of them swinging past half their size left a
                         * flying creature unblended at the game's rate. */
                        regs_jumped = jumped_regs > 0;
                        bool consts_rebuilt = jumped_regs >= 4 && jumped_regs * 2 >= read_regs &&
                                              pc->vu.info[0] == c->vu.info[0];  /* another program: its own constants */
                        c->strayed = sv > 0.2f;
                        if (dbg_groups)
                            SDL_Log("vsh pair: cmd %u nv %u worst input %d (stride %d, prev %d) stray %.2f, "
                                    "%d of %d read constant registers jumped%s",
                                    ck[j + k].cmd, c->id_nv, r, r >= 0 ? c->vu.inputs[r][1] : 0,
                                    r >= 0 ? pc->vu.inputs[r][1] : 0, sv, jumped_regs, read_regs,
                                    sv > 0.5f || (c->strayed && pc->strayed) || consts_rebuilt ? " -> not blended"
                                                                                                  : "");
                        /* Big programs (episode 4's battleships, 500+ vertices):
                         * a line when one is left unblended or pairs narrowly,
                         * at most ten in 300 frames. */
                        if (c->id_nv >= 500) {
                            static uint64_t win;
                            static int logs;
                            bool notb = sv > 0.5f || (c->strayed && pc->strayed) || consts_rebuilt;
                            float dch = draw_distance(prev, pc, cur, c), dmin = 3.4e38f;
                            int nread = 0;
                            for (uint32_t a = i; a < i2; a++)
                                if (pk[a].cmd != pk[i + order[k]].cmd)
                                    dmin = SDL_min(dmin, draw_distance(prev, &prev->cmds[pk[a].cmd], cur, c));
                            for (int q = 0; q < 192; q++)
                                nread += c->consts_read[q >> 3] >> (q & 7) & 1;
                            if (cur->frame >= win + 300) {
                                win = cur->frame;
                                logs = 0;
                            }
                            if ((notb || dmin < dch * 1.3f) && logs++ < 10)
                                SDL_Log("interp: big program draw: frame %llu cmd %u nv %u group %u->%u, own %.1f "
                                        "other %.1f, stray %.2f, %d of %d read registers jumped%s",
                                        (unsigned long long)cur->frame, ck[j + k].cmd, c->id_nv, i2 - i, j2 - j,
                                        dch, dmin, sv, jumped_regs, nread, notb ? " -> not blended" : "");
                        }
                        if (sv > 0.5f || (c->strayed && pc->strayed) || consts_rebuilt) {
                            /* PDO_DBG_NOBLEND=1: each program
                             * draw refused here, and by which test -- the stray,
                             * the registers that jumped and the one that
                             * changed most (in JSRF a hand left unblended was
                             * the game snapping a bone ~100 degrees in a frame,
                             * which blended would squash). */
                            static int dbg_noblend = -1;
                            static uint32_t nb_lines;
                            if (dbg_noblend < 0)
                                dbg_noblend = getenv("PDO_DBG_NOBLEND") && atoi(getenv("PDO_DBG_NOBLEND"));
                            if (dbg_noblend && nb_lines < 20000) {
                                int worst = -1;
                                float wd = 0.0f;
                                const float *ca = prev->vconsts[pc->vu.info[2]], *cb = cur->vconsts[c->vu.info[2]];
                                for (int q = 0; q < 192; q++) {
                                    float d = 0.0f;
                                    for (int e = 0; e < 4; e++)
                                        d += SDL_fabsf(ca[4 * q + e] - cb[4 * q + e]);
                                    if (d > wd && d < 1e6f)
                                        wd = d, worst = q;
                                }
                                nb_lines++;
                                SDL_Log("noblend: frame %llu program %x nv %u idx %u stray %.2f strayed %d/%d "
                                        "jumped %d of %d (most changed c%d by %.3f: %.3f %.3f %.3f %.3f -> %.3f %.3f "
                                        "%.3f %.3f)",
                                        (unsigned long long)cur->frame, (unsigned)c->vu.info[0], c->id_nv,
                                        c->vsh_index_count, sv, c->strayed, pc->strayed, jumped_regs, read_regs, worst,
                                        wd, worst >= 0 ? ca[4 * worst] : 0.0f, worst >= 0 ? ca[4 * worst + 1] : 0.0f,
                                        worst >= 0 ? ca[4 * worst + 2] : 0.0f, worst >= 0 ? ca[4 * worst + 3] : 0.0f,
                                        worst >= 0 ? cb[4 * worst] : 0.0f, worst >= 0 ? cb[4 * worst + 1] : 0.0f,
                                        worst >= 0 ? cb[4 * worst + 2] : 0.0f, worst >= 0 ? cb[4 * worst + 3] : 0.0f);
                            }
                            st_rebuilt++;
                            n_rebuilt++;
                            continue;
                        }
                    }
                    for (int r = 0; r < 16; r++) {
                        if (c->vu.inputs[r][1] != pc->vu.inputs[r][1] || c->vu.inputs[r][2] != pc->vu.inputs[r][2] ||
                            c->vu.inputs[r][3] != pc->vu.inputs[r][3])
                            same_in = false;
                        c->vu.prev_in[r] = pc->vu.inputs[r][0];
                    }
                    c->vu.interp[0] = pc->vu.info[2];
                    c->vu.interp[1] = same_in;
                    memcpy(c->vu.prev_scale, pc->vu.scale, sizeof c->vu.prev_scale);
                    memcpy(c->vu.prev_offset, pc->vu.offset, sizeof c->vu.prev_offset);
                    c->unpaired = false;
                    if (pc->vu.info[0] != c->vu.info[0] || regs_jumped) {
                        n_progswitch += pc->vu.info[0] != c->vu.info[0];
                        n_outblend += regs_jumped;
                        /* A boost's or a brake's end: every model on
                         * screen goes back to its usual program. One
                         * program run on a mix of two programs' constants
                         * came out neither frame (the dragon brighter,
                         * everything a little off): each runs its own.
                         * The same when a register it reads jumped (by
                         * over half its size): the game moved what it
                         * holds -- a skinned model's bones to other
                         * registers, as the boost's end hands the dragon
                         * to its usual pose -- and blended register by
                         * register, bones mixed with other bones (the
                         * dragon with a second pair of wings for that
                         * frame, capture of a boost's end). Each frame's
                         * run is as the game drew it, and the outputs
                         * blend. */
                        c->vu.prev_prog[0] = pc->vu.info[0];
                        c->vu.prev_prog[1] = pc->vu.info[1];
                    }
                    taken[pk[i + order[k]].cmd] = 1;
                    c->pair_prev = pk[i + order[k]].cmd;
                } else if (c->fvf >= 0 && pc->fvf >= 0) {
                    pdo_fvf_uniforms *u = &cur->fvf_us[c->fvf];
                    const pdo_fvf_uniforms *pu = &prev->fvf_us[pc->fvf];
                    float x0, y0, x1, y1, a4[8], b4[8];
                    if ((u->layout0[2] & PDO_FVF_PRE) && draw_corners(prev, pc, a4) && draw_corners(cur, c, b4)) {
                        /* A sprite that jumped: not blended. A quarter of the
                         * screen, or over half its own size (Concept of
                         * Love: JSRF's character select swaps its icons a
                         * slot at a time, and the in-between frames slid and
                         * ghosted them); things that really slide move a few
                         * pixels a frame. Size: across its first and last
                         * vertices. */
                        float dx = SDL_fabsf(a4[0] - b4[0]), dy = SDL_fabsf(a4[1] - b4[1]);
                        float size = SDL_max(SDL_fabsf(b4[4] - b4[0]), SDL_fabsf(b4[5] - b4[1]));
                        float moved = SDL_max(dx, dy);
                        if (dx > u->rt[0] * 0.25f || dy > u->rt[1] * 0.25f || (moved > 2.0f && moved > size * 0.5f))
                            continue;
                    }
                    /* Jumped: only judged with the origin on screen in both
                     * frames -- near the eye, behind it or off screen it
                     * swings far while the object hardly moves, and judged
                     * there it made ordinary frames look like camera cuts:
                     * the whole frame then showed unblended, and flying
                     * packs flashed and jittered as frames went in and out
                     * of blending. */
                    if (!(u->layout0[2] & PDO_FVF_PRE) && origin_ndc(pu->wvp, &x0, &y0) &&
                        origin_ndc(u->wvp, &x1, &y1) && SDL_fabsf(x0) < 1.0f && SDL_fabsf(y0) < 1.0f &&
                        SDL_fabsf(x1) < 1.0f && SDL_fabsf(y1) < 1.0f) {
                        fvf_pairs++;
                        if (n_moves < SDL_arraysize(moves))
                            moves[n_moves++] = SDL_fabsf(x1 - x0) + SDL_fabsf(y1 - y0);
                        if (SDL_fabsf(x1 - x0) > 0.5f || SDL_fabsf(y1 - y0) > 0.5f) {
                            fvf_jumps++;
                            n_jumped++;
                            continue;   /* jumped: not blended */
                        }
                    }
                    /* How far its vertices strayed from their common move, in
                     * the draw's size: over half, rebuilt in another order
                     * (the reticle, 78%); over a fifth twice running, too --
                     * a scramble lasts (reticle, packs), while animated
                     * parts (an episode 5 dragon's wings) only now and then
                     * pass a fifth, and left unblended for that one frame
                     * they flickered. */
                    float sv = stray(cur->blob + u->layout0[0], prev->blob + pu->layout0[0], (uint32_t)u->layout0[1],
                                     c->id_nv, cur->nblob - (uint32_t)u->layout0[0],
                                     prev->nblob - (uint32_t)pu->layout0[0]);
                    c->strayed = sv > 0.2f;
                    if (sv > 0.5f || (c->strayed && pc->strayed)) {
                        st_rebuilt++;
                        n_rebuilt++;
                        if (dbg_groups)
                            SDL_Log("rebuilt: cmd %u nv %u tex %08x", ck[j + k].cmd, c->id_nv,
                                    (unsigned)c->draw.tex[0].data);
                        continue;       /* rebuilt in another order: not blended */
                    }
                    /* see-through, no depth written, quads: a particle system */
                    if (c->draw.ps.blend_enable && !c->draw.ps.z_write && c->id_nv % 4 == 0 &&
                        quads_resorted(cur->blob + u->layout0[0], prev->blob + pu->layout0[0],
                                       (uint32_t)u->layout0[1], c->id_nv, cur->nblob - (uint32_t)u->layout0[0],
                                       prev->nblob - (uint32_t)pu->layout0[0])) {
                        st_rebuilt++;
                        n_rebuilt++;
                        if (dbg_groups)
                            SDL_Log("resorted: cmd %u nv %u tex %08x", ck[j + k].cmd, c->id_nv,
                                    (unsigned)c->draw.tex[0].data);
                        continue;       /* particles changed places: not blended */
                    }
                    u->interp[0] = pu->layout0[0];
                    u->interp[1] = 1;
                    /* Texture matrices (projected textures follow the
                     * camera) blend too, stage by stage -- unless one
                     * jumped by over half its size (a flipbook or random
                     * offset: blended it would slide). */
                    u->interp2[0] = 0;
                    for (int st = 0; st < 4; st++) {
                        float dmax = 0.0f, mag = 0.0f;
                        if (!(u->stage_tt[st] & 0xFF) || (u->stage_tt[st] & 0xFF) != (pu->stage_tt[st] & 0xFF))
                            continue;
                        for (int m = 0; m < 16; m++) {
                            dmax = SDL_max(dmax, SDL_fabsf(u->tex[st][m] - pu->tex[st][m]));
                            mag = SDL_max(mag, SDL_max(SDL_fabsf(u->tex[st][m]), SDL_fabsf(pu->tex[st][m])));
                        }
                        if (dmax <= mag * 0.5f)
                            u->interp2[0] |= 1 << st;
                    }
                    memcpy(u->prev_tex, pu->tex, sizeof u->prev_tex);
                    c->unpaired = false;
                    for (int m = 0; m < 16; m++) {
                        c->mvel[m] = u->wvp[m] - pu->wvp[m];
                        c->mvel[16 + m] = u->world[m] - pu->world[m];
                    }
                    c->has_mvel = true;
                    n_fvf_paired++;
                    n_still += memcmp(u->wvp, pu->wvp, sizeof u->wvp) == 0;
                    taken[pk[i + order[k]].cmd] = 1;
                    c->pair_prev = pk[i + order[k]].cmd;
                    memcpy(u->prev_wvp, pu->wvp, sizeof u->prev_wvp);
                    memcpy(u->prev_wv, pu->wv, sizeof u->prev_wv);
                    memcpy(u->prev_world, pu->world, sizeof u->prev_world);
                    memcpy(u->prev_rz, pu->rz, sizeof u->prev_rz);
                }
            }
        }
        i = i2;
        j = j2;
    }
    }
    MF_MARK(0);
    /* A vertex-program draw that found no partner, with an unused one of the
     * same program in the frame before, in the same render target, by the
     * constants the program reads alone -- its vertices may differ. Episode
     * 4's battleships switch between a detailed and simpler models as they
     * move (the detailed one's parts, 3131, 721, 862... vertices, going 6->5,
     * 2->3 while 1462- and 1184-vertex ones come and go): the switched ship
     * had no partner, showed a frame ahead, and jittered as it switched back
     * and forth. Paired, it blends its constants over this frame's vertices
     * (interp[1] 0: the streams are not the same). Only a clear choice: the
     * nearest at least twice as near as the next. */
    {
        static uint64_t *tp, *tc;
        static uint32_t cap_tp, cap_tc;
        uint64_t t = 0;
        tp = grow(tp, &cap_tp, prev->ncmds + 1, sizeof *tp);
        tc = grow(tc, &cap_tc, cur->ncmds + 1, sizeof *tc);
        for (uint32_t k = 0; k < prev->ncmds; k++) {
            if (prev->cmds[k].kind == CMD_TARGET)
                t = target_key(&prev->cmds[k]);
            tp[k] = t;
        }
        t = 0;
        for (uint32_t k = 0; k < cur->ncmds; k++) {
            if (cur->cmds[k].kind == CMD_TARGET)
                t = target_key(&cur->cmds[k]);
            tc[k] = t;
        }
        for (uint32_t k = 0; k < cur->ncmds; k++) {
            command *c = &cur->cmds[k];
            float best = 3.4e38f, next = 3.4e38f;
            int32_t bi = -1;
            if (c->kind != CMD_DRAW || !c->vsh || !c->unpaired || !c->id_src || (grouped[k] && !c->no_partner))
                continue;           /* its pair refused by a test (stray, constants jumped): on purpose */
            for (uint32_t q = 0; q < prev->ncmds; q++) {
                const command *pc = &prev->cmds[q];
                const float *a, *b;
                float d = 0.0f;
                if (pc->kind != CMD_DRAW || !pc->vsh || taken[q] || tp[q] != tc[k] ||
                    pc->vu.info[0] != c->vu.info[0] || pc->vu.info[1] != c->vu.info[1] ||
                    pc->draw.tex[0].data != c->draw.tex[0].data)
                    continue;
                a = prev->vconsts[pc->vu.info[2]];
                b = cur->vconsts[c->vu.info[2]];
                for (int e = 0; e < 192 * 4; e++)
                    if ((c->consts_read[e >> 5] >> ((e >> 2) & 7) & 1) && SDL_fabsf(a[e]) < 1e6f &&
                        SDL_fabsf(b[e]) < 1e6f)
                        d += SDL_fabsf(a[e] - b[e]);
                if (d < best) {
                    next = best;
                    best = d;
                    bi = (int32_t)q;
                } else if (d < next) {
                    next = d;
                }
            }
            if (bi < 0 || !(best * 2.0f < next))
                continue;
            for (int r = 0; r < 16; r++)
                c->vu.prev_in[r] = prev->cmds[bi].vu.inputs[r][0];
            c->vu.interp[0] = prev->cmds[bi].vu.info[2];
            c->vu.interp[1] = 0;
            memcpy(c->vu.prev_scale, prev->cmds[bi].vu.scale, sizeof c->vu.prev_scale);
            memcpy(c->vu.prev_offset, prev->cmds[bi].vu.offset, sizeof c->vu.prev_offset);
            c->unpaired = false;
            c->pair_prev = (uint32_t)bi;
            taken[bi] = 1;
            n_lod++;
        }
    }
    MF_MARK(1);
    /* A check on every 3D pair, data aside: paired by the address of its
     * vertex data, a draw is taken on trust when it alone has that address
     * in both frames -- but the game hands out vertex buffers from a pool,
     * and a fighter in a pack could be drawn from the buffer another used
     * the frame before: blended toward that one, the pack warped between
     * each other's places (episode 1). Among the alike draws of the frame
     * before (data aside), one under half as far as the partner means the
     * pair is wrong: not blended. */
    {
        static draw_key *lk;
        static uint32_t cap_lk;
        uint32_t nl;
        uint64_t target = 0;
        memset(taken, 0, prev->ncmds);      /* keys_of's skip: none */
        nl = keys_of(prev, &lk, &cap_lk, true, false, taken);
        for (uint32_t k = 0; k < cur->ncmds; k++) {
            command *c = &cur->cmds[k];
            uint64_t h;
            uint32_t lo = 0, hi = nl, e;
            float dc, dmin = 3.4e38f;
            if (c->kind == CMD_TARGET)
                target = target_key(c);
            if (c->kind != CMD_DRAW || c->unpaired || !is_3d(cur, c))
                continue;
            h = (key_of(cur, c, true, false) ^ target) * 1099511628211ull;   /* as keys_of */
            while (lo < hi) {               /* the first key >= h */
                uint32_t m = (lo + hi) / 2;
                if (lk[m].h < h) lo = m + 1; else hi = m;
            }
            for (e = lo; e < nl && lk[e].h == h; e++)
                ;
            if (e - lo < 2 || e - lo > 64)
                continue;                   /* alone, or too many to look at */
            dc = draw_distance(prev, &prev->cmds[c->pair_prev], cur, c);
            for (uint32_t m = lo; m < e; m++)
                if (lk[m].cmd != c->pair_prev) {
                    float d = draw_distance(prev, &prev->cmds[lk[m].cmd], cur, c);
                    if (d < dmin)
                        dmin = d;
                }
            if (dmin * 2.0f < dc) {
                c->unpaired = true;
                c->prev_vbase = -1;
                if (c->vsh) {
                    c->vu.interp[0] = -1;
                    c->vu.interp[1] = 0;
                }
                if (c->fvf >= 0) {
                    cur->fvf_us[c->fvf].interp[0] = -1;
                    cur->fvf_us[c->fvf].interp[1] = 0;
                }
                c->has_mvel = false;
                st_swapped++;
                n_swapped++;
            }
        }
    }
    MF_MARK(2);
    /* How far the picture moved this frame: the 75th percentile of the
     * on-screen moves of the paired fixed-function draws on screen (the
     * world is most of them, past the dragon's parts and afterimages; Orta places them round the eye, so the camera is not read
     * from a matrix). A frame moving far less than the eight before (the
     * freeze as the boost ends: the ground stood still while the dragon
     * moved) is logged. */
    {
        static float steps[8];
        static uint32_t nsteps, cam_logs, cam_frames;
        if (++cam_frames >= 300)
            cam_frames = cam_logs = 0;
        if (n_moves >= 8) {
            float step, avg = 0.0f;
            /* by a partial selection sort (n_moves <= 512) */
            for (uint32_t q = 0; q <= n_moves * 3 / 4; q++)
                for (uint32_t r = q + 1; r < n_moves; r++)
                    if (moves[r] < moves[q]) {
                        float t = moves[q];
                        moves[q] = moves[r];
                        moves[r] = t;
                    }
            step = moves[n_moves * 3 / 4];
            for (uint32_t q = 0; q < SDL_min(nsteps, 8u); q++)
                avg += steps[q] / (float)SDL_min(nsteps, 8u);
            if (nsteps >= 8 && avg > 0.002f && step < avg * 0.3f && cam_logs++ < 10)
                SDL_Log("interp: frame %llu: the picture moved %.4f, against %.4f a frame before (%u draws)",
                        (unsigned long long)cur->frame, step, avg, n_moves);
            steps[nsteps++ % 8] = step;
            cam_step = step;
            cam_avg = avg;
        } else {
            nsteps = 0;
        }
    }
    MF_MARK(3);
    /* What did not blend, when it is a lot or alike groups changed size:
     * a one-frame freeze with a ghost of the dragon as the boost ends
     * (episodes 2, 3) is still unexplained. At most ten lines in 300 frames. */
    {
        static uint32_t odd_frames, odd_logs;
        static bool was_still;
        bool still = n_fvf_paired >= 20 && n_still * 10 >= n_fvf_paired * 9;
        uint32_t unp3d = 0, all3d = 0;
        for (uint32_t k = 0; k < cur->ncmds; k++) {
            const command *c = &cur->cmds[k];
            if (c->kind == CMD_DRAW && blendable(c) && is_3d(cur, c)) {
                all3d++;
                unp3d += c->unpaired;
            }
        }
        if (++odd_frames >= 300)
            odd_frames = odd_logs = 0;
        if ((resized >= 3 || unp3d * 4 > all3d || (still && !was_still)) && odd_logs < 10) {
            /* Unblended draws that had nothing alike in the frame before (no
             * reason counted): which part of the key kept them apart, from a
             * draw of the frame before alike in kind, program or layout and
             * counts -- the render target, the first texture or the
             * program's other word. Boost end (episode 1): 43 of 127 such. */
            uint32_t nokey = 0, d_target = 0, d_tex = 0, d_info = 0, d_none = 0, alone = 0;
            uint64_t tc = 0, ptgt[8] = { 0 }, ctgt[8] = { 0 };
            uint32_t nptgt = 0, nctgt = 0, ptn[8] = { 0 }, ctn[8] = { 0 };
            for (uint32_t k = 0; k < prev->ncmds; k++)
                if (prev->cmds[k].kind == CMD_TARGET) {
                    uint64_t t = target_key(&prev->cmds[k]);
                    uint32_t q = 0;
                    while (q < nptgt && ptgt[q] != t) q++;
                    if (q == nptgt && nptgt < 8) ptgt[nptgt++] = t;
                    if (q < 8) ptn[q]++;
                }
            for (uint32_t k = 0; k < cur->ncmds; k++) {
                const command *c = &cur->cmds[k];
                if (c->kind == CMD_TARGET) {
                    uint32_t q = 0;
                    tc = target_key(c);
                    while (q < nctgt && ctgt[q] != tc) q++;
                    if (q == nctgt && nctgt < 8) ctgt[nctgt++] = tc;
                    if (q < 8) ctn[q]++;
                }
                if (c->kind != CMD_DRAW || !blendable(c) || !is_3d(cur, c) || !c->unpaired || grouped[k])
                    continue;
                nokey++;
                {
                    uint64_t tp = 0;
                    bool found = false, t_same = false, x_same = false, i_same = false;
                    for (uint32_t q = 0; q < prev->ncmds; q++) {
                        const command *pc = &prev->cmds[q];
                        if (pc->kind == CMD_TARGET)
                            tp = target_key(pc);
                        if (pc->kind != CMD_DRAW || !blendable(pc) || !is_3d(prev, pc) || pc->vsh != c->vsh ||
                            pc->id_nv != c->id_nv || pc->vsh_index_count != c->vsh_index_count)
                            continue;
                        if (c->vsh ? pc->vu.info[0] != c->vu.info[0]
                                   : prev->fvf_us[pc->fvf].layout0[1] != cur->fvf_us[c->fvf].layout0[1] ||
                                     prev->fvf_us[pc->fvf].layout0[2] != cur->fvf_us[c->fvf].layout0[2])
                            continue;
                        found = true;
                        t_same |= tp == tc;
                        x_same |= pc->draw.tex[0].data == c->draw.tex[0].data;
                        i_same |= !c->vsh || pc->vu.info[1] == c->vu.info[1];
                    }
                    if (!found) alone++;
                    else if (!t_same) d_target++;
                    else if (!x_same) d_tex++;
                    else if (!i_same) d_info++;
                    else d_none++;
                }
            }
            if (nokey) {
                char tt[160] = "";
                for (uint32_t q = 0; q < nptgt; q++) {
                    size_t len = strlen(tt);
                    snprintf(tt + len, sizeof tt - len, " %llx:%u", (unsigned long long)ptgt[q], ptn[q]);
                }
                {
                    size_t len = strlen(tt);
                    snprintf(tt + len, sizeof tt - len, " ->");
                }
                for (uint32_t q = 0; q < nctgt; q++) {
                    size_t len = strlen(tt);
                    snprintf(tt + len, sizeof tt - len, " %llx:%u", (unsigned long long)ctgt[q], ctn[q]);
                }
                SDL_Log("interp: frame %llu: %u unblended had nothing alike before: %u new, %u another target, "
                        "%u another texture, %u another program word, %u other; targets%s",
                        (unsigned long long)cur->frame, nokey, alone, d_target, d_tex, d_info, d_none, tt);
            }
            /* Many new (the boost end): which draws are new, and which of the
             * frame before have nothing alike now (gone): v/f, program or
             * layout, vertices, indices, first texture, where on screen. */
            if (alone >= 10) {
                for (int side = 0; side < 2; side++) {
                    const frame_data *f = side ? prev : cur, *o = side ? cur : prev;
                    char txt[1024] = "";
                    uint32_t n = 0;
                    for (uint32_t k = 0; k < f->ncmds; k++) {
                        const command *c = &f->cmds[k];
                        bool found = false;
                        float x = 9.0f, y = 9.0f;
                        if (c->kind != CMD_DRAW || !blendable(c) || !is_3d(f, c) || (!side && (!c->unpaired || grouped[k])))
                            continue;
                        for (uint32_t q = 0; q < o->ncmds && !found; q++) {
                            const command *pc = &o->cmds[q];
                            if (pc->kind != CMD_DRAW || !blendable(pc) || !is_3d(o, pc) || pc->vsh != c->vsh ||
                                pc->id_nv != c->id_nv || pc->vsh_index_count != c->vsh_index_count)
                                continue;
                            found = c->vsh ? pc->vu.info[0] == c->vu.info[0]
                                           : o->fvf_us[pc->fvf].layout0[1] == f->fvf_us[c->fvf].layout0[1] &&
                                             o->fvf_us[pc->fvf].layout0[2] == f->fvf_us[c->fvf].layout0[2];
                        }
                        if (found)
                            continue;
                        if (c->vsh && c->anchored)
                            x = c->anchor[0], y = c->anchor[1];
                        else if (!c->vsh && !origin_ndc(f->fvf_us[c->fvf].wvp, &x, &y))
                            x = y = 9.0f;
                        if (n++ < 16) {
                            size_t len = strlen(txt);
                            snprintf(txt + len, sizeof txt - len, " %c%x/%u/%u/%x@%.2f,%.2f", c->vsh ? 'v' : 'f',
                                     c->vsh ? (unsigned)c->vu.info[0] : (unsigned)f->fvf_us[c->fvf].layout0[1],
                                     c->id_nv, c->vsh_index_count, (unsigned)c->draw.tex[0].data, x, y);
                        }
                    }
                    SDL_Log("interp: frame %llu: %u %s:%s", (unsigned long long)cur->frame, n,
                            side ? "gone since the frame before" : "new", txt);
                }
            }
        }
        if ((resized >= 3 || unp3d * 4 > all3d || (still && !was_still)) && odd_logs++ < 10)
            SDL_Log("interp: frame %llu: %u of %u 3D draws not blended (%u no partner near, %u jumped, %u rebuilt, "
                    "%u undone, %u paired across models); %u of %u paired did not move; picture moved %.4f (%.4f before); "
                    "%u alike groups changed size:%s",
                    (unsigned long long)cur->frame, unp3d, all3d, n_forced, n_jumped, n_rebuilt, n_swapped, n_lod,
                    n_still, n_fvf_paired, cam_step, cam_avg, resized, resized_txt);
        was_still = still;
        /* The boost's and the brake's end: many draws on other programs. */
        programs_switched = n_progswitch >= 8;
        {   /* The scene's render size changed (a boost's or a brake's end:
             * 512x360 at 0,12 -> 426x320 at 0,0 in episode 1): the most
             * used viewport of the 3D draws, against the frame before's. */
            static float last_vp[4];
            static uint32_t vp_logs, vp_frames;
            float best[4] = { 0, 0, 0, 0 };
            uint32_t best_n = 0;
            if (++vp_frames >= 300)
                vp_frames = vp_logs = 0;
            for (uint32_t k = 0; k < cur->ncmds && best_n < 8; k++) {
                const command *c = &cur->cmds[k];
                uint32_t n = 0;
                if (c->kind != CMD_DRAW || !is_3d(cur, c))
                    continue;
                for (uint32_t q = k; q < cur->ncmds; q++)
                    if (cur->cmds[q].kind == CMD_DRAW && is_3d(cur, &cur->cmds[q]) &&
                        !memcmp(cur->cmds[q].draw.viewport, c->draw.viewport, 4 * sizeof(float)))
                        n++;
                if (n > best_n) {
                    best_n = n;
                    memcpy(best, c->draw.viewport, sizeof best);
                }
                if (best_n * 2 > all3d)
                    break;
            }
            viewport_changed = best_n && last_vp[2] > 0 && memcmp(best, last_vp, sizeof best) != 0;
            if (viewport_changed && vp_logs++ < 10)
                SDL_Log("interp: frame %llu: the scene's viewport changed, %.0f,%.0f %.0fx%.0f -> %.0f,%.0f %.0fx%.0f",
                        (unsigned long long)cur->frame, last_vp[0], last_vp[1], last_vp[2], last_vp[3], best[0],
                        best[1], best[2], best[3]);
            if (best_n)
                memcpy(last_vp, best, sizeof last_vp);
        }
        {   /* The draws that sample the picture, when they change (the swirl
             * of a boost's or a brake's end comes and goes): at most 20
             * lines in 300 frames. */
            static uint64_t last_sig;
            static uint32_t pic_logs, pic_frames;
            static int dbg_picture = -1;     /* PDO_DBG_PICTURE=1: this log (it looks at every draw) */
            if (dbg_picture < 0)
                dbg_picture = getenv("PDO_DBG_PICTURE") && atoi(getenv("PDO_DBG_PICTURE"));
            uint64_t sig = 1469598103934665603ull;
            char txt[600] = "";
            uint32_t n = 0;
            if (++pic_frames >= 300)
                pic_frames = pic_logs = 0;
            for (uint32_t k = 0; dbg_picture && k < cur->ncmds; k++) {
                const command *c = &cur->cmds[k];
                float b4[8];
                bool corners;
                if (c->kind != CMD_DRAW || !reads_target(cur, c))
                    continue;
                n++;
                sig = (sig ^ ((uint64_t)c->id_nv << 8 | (uint64_t)is_3d(cur, c) << 1 | (uint64_t)(c->fvf >= 0)))
                      * 1099511628211ull;
                corners = draw_corners(cur, c, b4);
                if (strlen(txt) < sizeof txt - 90) {
                    size_t len = strlen(txt);
                    snprintf(txt + len, sizeof txt - len, " %s/%u/%s%s/tex %08x@%.0f,%.0f-%.0f,%.0f",
                             c->vsh ? "v" : c->fvf >= 0 ? "f" : "c", c->id_nv, is_3d(cur, c) ? "3d" : "2d",
                             c->unpaired ? "" : "/blended", (unsigned)c->draw.tex[0].data,
                             corners ? b4[0] : -1.0f, corners ? b4[1] : -1.0f, corners ? b4[4] : -1.0f,
                             corners ? b4[5] : -1.0f);
                }
            }
            if (dbg_picture && sig != last_sig && pic_logs++ < 20)
                SDL_Log("interp: frame %llu: %u draws sample the picture:%s", (unsigned long long)cur->frame, n, txt);
            last_sig = sig;
        }
        static uint32_t switch_logs;    /* its own budget: the lines above use theirs up in fights */
        if (odd_frames == 0)
            switch_logs = 0;
        if (programs_switched && switch_logs++ < 10) {
            /* and every draw that samples the picture, 3D ones too: kind, vertices, screen corners */
            char txt[512] = "";
            uint32_t n3 = 0;
            for (uint32_t k = 0; k < cur->ncmds; k++) {
                const command *c = &cur->cmds[k];
                float b4[8];
                if (c->kind != CMD_DRAW || !reads_target(cur, c))
                    continue;
                n3 += is_3d(cur, c);
                if (strlen(txt) < sizeof txt - 80) {
                    size_t len = strlen(txt);
                    bool corners = draw_corners(cur, c, b4);
                    snprintf(txt + len, sizeof txt - len, " %s/%u/%s%s@%.0f,%.0f-%.0f,%.0f", c->vsh ? "v" : c->fvf >= 0 ? "f" : "c",
                             c->id_nv, is_3d(cur, c) ? "3d" : "2d", c->unpaired ? "" : "/blended",
                             corners ? b4[0] : -1.0f, corners ? b4[1] : -1.0f, corners ? b4[4] : -1.0f, corners ? b4[5] : -1.0f);
                }
            }
            SDL_Log("interp: frame %llu: %u draws paired with the same one on another program, %u whose read "
                    "registers jumped; %u copies of the picture on screen not blended; %u 3D draws sample the picture; "
                    "those sampling it:%s",
                    (unsigned long long)cur->frame, n_progswitch, n_outblend, n_screen, n3, txt);
        }
    }
    MF_MARK(4);
    /* A camera cut: most things jumped. Show the new frame as it is. */
    {   /* A camera cut: over half of those on screen jumped. */
        static uint32_t st_frames, st_cuts;
        /* The scene's render size changed (a boost's or a brake's end): the
         * frame is shown as the game drew it, as on a cut: blended across
         * the change, episode 4's dragon showed a frame in another pose at
         * the boost's end. At most a one-frame step, as at 60. */
        if (viewport_changed) {
            match_frames(NULL, cur);    /* returns before it looks at the viewport */
        } else if (fvf_pairs >= 8 && fvf_jumps * 2 > fvf_pairs) {
            match_frames(NULL, cur);
            st_cuts++;
            /* Rare, and a whole frame shown unblended: always logged. */
            SDL_Log("interp: camera cut at frame %llu (%u of %u on screen jumped)",
                    (unsigned long long)cur->frame, fvf_jumps, fvf_pairs);
        }
        if (++st_frames == 300) {
            SDL_Log("interp: %u camera cuts in the last 300 frames; %.1f close calls, %.1f wrong pairs undone, "
                    "%.1f rebuilt draws a frame", st_cuts, st_close / 300.0, st_swapped / 300.0, st_rebuilt / 300.0);
            st_frames = st_cuts = st_close = st_swapped = st_rebuilt = 0;
        }
    }
}

/* ── The two threads ──────────────────────────────────────────────────── */

/* A frame data no thread holds (with q_lock held): not recorded into, not
 * handed over, not the one last drawn. */
static frame_data *free_frame(void)
{
    for (int i = 0; i < 5; i++)
        if (&pool[i] != F && &pool[i] != ready && &pool[i] != held_cur && &pool[i] != held_prev)
            return &pool[i];
    return NULL;
}

/* Game thread, at Swap: resolve the frame's textures while guest memory
 * still holds them, then hand it over and go on recording into another. */
static uint64_t frame_time_ns;     /* pdo_gfx_set_frame_time, for the next hand_over */

static void hand_over(void)
{
    Uint64 tex_t0 = SDL_GetTicksNS();
    frame_data *next = NULL;

    F->frame = frames;
    /* On the vblank grid: stamped when recording ended, frames wobbled by
     * however long the game took, and blending with them did too. */
    F->stamp = frame_time_ns ? frame_time_ns : tex_t0;
    frame_time_ns = 0;
    F->tex = grow(F->tex, &F->cap_tex, F->ncmds * 4 + 4, sizeof *F->tex);
    if (!F->ups)
        F->ups = SDL_malloc(MAX_UPLOADS * sizeof *F->ups);
    for (uint32_t i = 0; i < F->ncmds; i++)
        if (F->cmds[i].kind == CMD_DRAW)
            for (int s = 0; s < 4; s++)
                F->tex[i * 4 + s] = texture_for(&F->cmds[i].draw.tex[s]);
    F->staging_len = 0;
    F->nups = 0;
    check_textures(&F->staging, &F->staging_len, &F->staging_cap, F->ups, &F->nups);
    texture_ns += SDL_GetTicksNS() - tex_t0;
    if (progs_dirty) {
        F->progs = grow(F->progs, &F->cap_progs, nprogs, sizeof *F->progs);
        memcpy(F->progs, progs, nprogs * sizeof *progs);
        F->nprogs = nprogs;
        F->progs_new = true;
        progs_dirty = false;
    }
    if (game_gamma_dirty) {
        memcpy(F->gamma, game_gamma, sizeof F->gamma);
        F->gamma_new = true;
        game_gamma_dirty = false;
    }

    SDL_LockMutex(q_lock);
    /* The options menu holds the game here. Otherwise no waiting: a frame
     * the main thread has not taken yet (it is behind, the display or the
     * GPU slower than the game) is dropped for this newer one. */
    while (paused)
        SDL_WaitCondition(q_cond, q_lock);
    while (ready && ready->frame >= capture_frame && ready->frame < capture_frame + capture_seq)
        SDL_WaitConditionTimeout(q_cond, q_lock, 2);   /* a capture is never dropped */
    next = ready ? ready : free_frame();
    if (ready) {
        /* What the dropped frame alone carried goes with this one: its
         * texture uploads (this frame skipped a texture the dropped one
         * uploaded, and would draw it never filled), programs and gamma. */
        frames_dropped++;
        drops_total++;
        for (int i = 0; i < F->nups; i++)
            F->ups[i].t->staged_in = F->frame;
        for (int i = 0; i < ready->nups; i++) {
            pending_upload *u = &ready->ups[i];
            uint32_t need = u->w * u->h * 4;
            if (u->t->staged_in == F->frame)
                continue;           /* this frame has a newer copy */
            if (F->nups == MAX_UPLOADS) {
                u->t->uploaded = false;     /* next time it is used */
                continue;
            }
            if (F->staging_len + need > F->staging_cap) {
                F->staging_cap = (F->staging_len + need) * 2;
                F->staging = SDL_realloc(F->staging, F->staging_cap);
            }
            memcpy(F->staging + F->staging_len, ready->staging + u->offset, need);
            F->ups[F->nups] = *u;
            F->ups[F->nups].offset = F->staging_len;
            F->nups++;
            F->staging_len += need;
        }
        if (ready->progs_new && !F->progs_new) {
            F->progs = grow(F->progs, &F->cap_progs, nprogs, sizeof *F->progs);
            memcpy(F->progs, progs, nprogs * sizeof *progs);
            F->nprogs = nprogs;
            F->progs_new = true;
        }
        if (ready->gamma_new && !F->gamma_new) {
            memcpy(F->gamma, ready->gamma, sizeof F->gamma);
            F->gamma_new = true;
        }
    }
    ready = F;
    F = next;
    SDL_BroadcastCondition(q_cond);
    SDL_UnlockMutex(q_lock);
    reset_frame(F);
}

void pdo_gfx_set_frame_time(uint64_t ns)
{
    frame_time_ns = ns;
}

bool pdo_gfx_present(void)
{
    RECORDING();
    frames++;
    if (!dev)
        return true;
    {
        /* PDO_NO_DRAW=1: drop the frame's commands unless it is captured or
         * screenshotted, so a debugging run reaches a stage at the game's
         * own pace on a slow (software) GPU. */
        static int no_draw = -1;
        bool shot = shot_dir && frames <= 300 * (uint64_t)shot_every && frames % (uint64_t)shot_every == 0;
        if (no_draw < 0)
            no_draw = getenv("PDO_NO_DRAW") && atoi(getenv("PDO_NO_DRAW")) != 0;
        if (no_draw && !shot && frames != capture_frame && frames + 1 != capture_frame) {
            reset_frame(F);
            return true;
        }
    }
    hand_over();
    return true;
}

static bool shot_frame(const frame_data *f)
{
    return shot_dir && f->frame <= 300 * (uint64_t)shot_every && f->frame % (uint64_t)shot_every == 0;
}

/* Main thread: draw a handed-over frame and present it; with prev, the
 * frame between prev and f at t (0 prev, 1 f). Captures and screenshots
 * are of frames as they are. */
static double frame_interval = 1e9 / 59.94;    /* between game frames, smoothed */

/* Frames in flight on the GPU, for the timing log (draw_frame_main). */
static struct { SDL_GPUFence *fence; Uint64 submitted; } gpu_q[8];
static int gpu_n;
static Uint64 gpu_busy_ns, gpu_last_end, main_submit_ns;
static uint32_t gpu_frames;

/* Take finished frames off the queue (polled: their end is known to the
 * time between polls, about a frame), then queue this one. */
static void gpu_note(SDL_GPUFence *fence, Uint64 now)
{
    int k = 0;
    for (int i = 0; i < gpu_n; i++) {
        if (SDL_QueryGPUFence(dev, gpu_q[i].fence)) {
            Uint64 start = gpu_q[i].submitted > gpu_last_end ? gpu_q[i].submitted : gpu_last_end;
            gpu_busy_ns += now > start ? now - start : 0;
            gpu_last_end = now;
            gpu_frames++;
            SDL_ReleaseGPUFence(dev, gpu_q[i].fence);
        } else {
            gpu_q[k++] = gpu_q[i];
        }
    }
    gpu_n = k;
    if (!fence)
        return;
    if (gpu_n == 8) {
        SDL_ReleaseGPUFence(dev, gpu_q[0].fence);
        memmove(gpu_q, gpu_q + 1, 7 * sizeof *gpu_q);
        gpu_n = 7;
    }
    gpu_q[gpu_n].fence = fence;
    gpu_q[gpu_n].submitted = now;
    gpu_n++;
}

/* t < 0: interpolating, work t out once the window's image is in hand. */
static uint32_t ls_generated;    /* frames frame generation made for this one */
static uint32_t ls_extra_shown;  /* its pictures beyond one a game frame, for the main loop's log */

static void draw_frame_main(frame_data *f, frame_data *prev, float t)
{
    SDL_GPUCommandBuffer *cmd;
    bool shot = !prev && shot_frame(f);

    F = f;
    P = prev;
    blend = t;
    if (f->gamma_new) {
        memcpy(gamma_rgba, f->gamma, sizeof gamma_rgba);
        gamma_dirty = true;
    }
    if (!prev && f->frame == capture_frame) {
        capture_steps();
        capture_shadows();
        if (getenv("PDO_CAPTURE_TEXCHECK"))
            capture_check_textures();
        capture_vsh();
    }
    apply_quality();
    apply_vsync();
    cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd)
        return;
    if (t < 0.0f) {
        /* Interpolating: where between the frames, worked out once the
         * window has an image for it -- that wait (up to a refresh, two
         * when one is missed) came after the blend was chosen, so what
         * reached the screen was late by a different amount each time
         * and fast movers (flying packs) jittered. */
        double d;
        pre_swap = NULL;
        if (have_swapchain && screen_pipeline) {
            Uint64 t0 = SDL_GetTicksNS();
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &pre_swap, &pre_sw, &pre_sh))
                pre_swap = NULL;
            swap_wait_ns += SDL_GetTicksNS() - t0;
            main_wait_ns += SDL_GetTicksNS() - t0;
        }
        {
            /* Over the gap between these two frames: stamped on the vblanks
             * their Swaps waited for, a frame that took the game two
             * vblanks (busy scenes) spans two -- over the usual one the
             * blend ran to its end early, stopped, and jumped. */
            double gap = prev ? (double)(f->stamp - prev->stamp) : 0;
            if (!(gap > 8e6 && gap < 70e6))
                gap = frame_interval;
            d = (double)(SDL_GetTicksNS() - f->stamp) / gap;
        }
        blend = t = (float)(d < 0 ? 0 : d > 1 ? 1 : d);
    }
    {
        Uint64 r0 = SDL_GetTicksNS();
        replay(cmd);
        present_pass(cmd);
        main_record_ns += SDL_GetTicksNS() - r0;
    }
    /* Frame generation: the window pass into ls_tex, taken in and the
     * frames before it generated in this command buffer. */
    ls_draw = lsfg_on;
    ls_drawn = false;
    present_to_window(cmd);
    ls_draw = false;
#ifdef PDO_HAVE_LSFG
    if (ls_drawn) {
        const pdo_settings *st = pdo_settings_get();
        pdo_lsfg_configure(st->lsfg_multiplier, st->lsfg_performance);
        ls_generated = pdo_lsfg_record(cmd, ls_tex, ls_w, ls_h);
    }
#endif
    if (shot)
        save_screenshot(&cmd, shot_dir, "frame");
    else if (!prev && f->frame == capture_frame) {
        save_screenshot(&cmd, ".", "capture");
        SDL_Log("capture: saved capture_%05llu.bmp", (unsigned long long)f->frame);
    }
    if (cmd) {
        /* Timing: how long submitting blocks, and when the GPU finishes
         * each frame (its fence, looked at on later frames): a frame's GPU
         * time is from its submit, or from the previous frame's end if
         * that was later. */
        Uint64 s0 = SDL_GetTicksNS();
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        Uint64 s1 = SDL_GetTicksNS();
        main_submit_ns += s1 - s0;
        /* Frame generation's scene-cut test reads what the GPU just did. */
        if (ls_drawn && fence)
            SDL_WaitForGPUFences(dev, true, &fence, 1);
        gpu_note(fence, s1);
    }
    if (ls_drawn) {
        /* The frame counter counts what reached the screen. */
        int shown = lsfg_show(ls_generated);
        for (int i = 1; i < shown; i++)
            pdo_overlay_frame();
        if (shown > 1)
            ls_extra_shown += (uint32_t)(shown - 1);
        ls_drawn = false;
    }
    pdo_overlay_frame();
}

static void draw_frame(frame_data *f, frame_data *prev, float t)
{
    tls_drawing = true;
    draw_frame_main(f, prev, t);
    tls_drawing = false;
}

/* The options menu is open: the game waits at its next Swap and the sound
 * holds, while the last frame is shown with the menu over it. */
static void hold_for_overlay(void)
{
    SDL_LockMutex(q_lock);
    paused = true;
    SDL_UnlockMutex(q_lock);
    SDL_LockSpinlock(&pause_lock);
    pause_start_ns = SDL_GetTicksNS();
    SDL_SetAtomicInt(&pause_on, 1);
    SDL_UnlockSpinlock(&pause_lock);
    pdo_mixer_set_paused(true);
    while (pdo_overlay_open()) {
        SDL_GPUCommandBuffer *cmd;

        if (!pdo_gfx_pump_events()) {
            SDL_Log("d3d: window closed");
            pdo_settings_save();        /* the window's place and size */
            exit(0);
        }
        cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (cmd) {
            present_to_window(cmd);
            SDL_SubmitGPUCommandBuffer(cmd);
        }
        if (!have_swapchain)
            SDL_Delay(16);
    }
    pdo_mixer_set_paused(false);
    SDL_LockSpinlock(&pause_lock);
    paused_total_ns += SDL_GetTicksNS() - pause_start_ns;
    SDL_SetAtomicInt(&pause_on, 0);
    SDL_UnlockSpinlock(&pause_lock);
    SDL_LockMutex(q_lock);
    paused = false;
    SDL_BroadcastCondition(q_cond);
    SDL_UnlockMutex(q_lock);
}

uint64_t pdo_gfx_paused_ns(void)
{
    Uint64 ns;

    SDL_LockSpinlock(&pause_lock);
    ns = paused_total_ns;
    if (SDL_GetAtomicInt(&pause_on))
        ns += SDL_GetTicksNS() - pause_start_ns;    /* now >= start: read after it */
    SDL_UnlockSpinlock(&pause_lock);
    return ns;
}

bool pdo_gfx_paused(void)
{
    return SDL_GetAtomicInt(&pause_on) != 0;
}

void pdo_gfx_prepare_threads(void)
{
    main_thread = SDL_GetCurrentThreadID();
    q_lock = SDL_CreateMutex();
    q_cond = SDL_CreateCondition();
}

void pdo_gfx_game_finished(void)
{
    SDL_LockMutex(q_lock);
    game_done = true;
    SDL_BroadcastCondition(q_cond);
    SDL_UnlockMutex(q_lock);
}

void pdo_gfx_main_loop(void)
{
    frame_data *cur = NULL, *prev = NULL;


    interp_tint = getenv("PDO_INTERP_TINT") && atoi(getenv("PDO_INTERP_TINT"));

    /* Interpolation only pays when the screen gets well over the game's
     * rate: near it, game frames are skipped and the blends land unevenly
     * -- worse than showing each frame once. Checked each second; off for
     * five seconds when the screen is under 1.5 times the game's rate,
     * then tried again. */
    bool interp_ok = true;
    int bad_windows = 0;    /* seconds in a row under 1.5 times */
    Uint64 win_start = SDL_GetTicksNS(), retry_at = 0;
    uint32_t win_games = 0, win_shown = 0, win_drops = drops_total, log_n = 0;

    for (;;) {
        frame_data *f;
        /* Frame generation makes the frames between the game's instead. */
        bool want = pdo_settings_get()->frame_interp && !lsfg_on && !getenv("PDO_BENCHMARK");
        bool interp = want && interp_ok;
        Uint64 now = SDL_GetTicksNS();

        if (now - win_start >= 1000000000ull) {
            double secs = (double)(now - win_start) / 1e9;
            double games = win_games / secs, shown = (win_shown + ls_extra_shown) / secs;
            uint32_t skipped = drops_total - win_drops;
            /* Skipped game frames alone do not count: in busy scenes the
             * game itself runs late, and holding blending off then made
             * it worse. */
            /* Two seconds in a row: one short stall (a load, a pipeline made
             * the first time, an effect starting) turned blending off for five
             * seconds, again and again through episode 9's and 10's bosses. */
            bad_windows = want && interp_ok && games > 30 && shown < games * 1.5 ? bad_windows + 1 : 0;
            if (bad_windows >= 2) {
                bad_windows = 0;
                interp_ok = false;
                retry_at = now + 5000000000ull;
                SDL_Log("interp: off for 5 s: %.0f fps on screen for %.0f game frames, %u skipped", shown, games,
                        skipped);
            } else if (!interp_ok && now >= retry_at) {
                interp_ok = true;
            }
            if (++log_n % 5 == 0)
                SDL_Log("main: %.0f fps on screen, %.0f game frames; per second %.0f ms pairing, %.0f ms recording, "
                        "%.0f ms waiting for the window, %.0f ms submitting; GPU about %.1f ms a frame; blending %s",
                        shown, games, main_match_ns / 1e6 / secs,
                        main_record_ns / 1e6 / secs, main_wait_ns / 1e6 / secs, main_submit_ns / 1e6 / secs,
                        gpu_frames ? gpu_busy_ns / 1e6 / gpu_frames : 0.0,
                        !want ? "off" : interp_ok ? "on" : "held off");
            main_record_ns = main_wait_ns = main_submit_ns = gpu_busy_ns = main_match_ns = 0;
            gpu_frames = 0;
            win_start = now;
            win_games = win_shown = ls_extra_shown = 0;
            win_drops = drops_total;
        }

        if (dev && !pdo_gfx_pump_events()) {
            SDL_Log("d3d: window closed");
            pdo_settings_save();        /* the window's place and size */
            exit(0);
        }
        if (dev && pdo_overlay_open())     /* opened while no frame comes (loading) */
            hold_for_overlay();
        SDL_LockMutex(q_lock);
        /* Interpolating, frames are drawn whether a new one came or not. */
        if (!ready && !game_done && init_state != 1 && !(interp && prev))
            SDL_WaitConditionTimeout(q_cond, q_lock, 4);
        if (init_state == 1) {
            SDL_UnlockMutex(q_lock);
            bool ok = init_here(init_w, init_h);
            SDL_LockMutex(q_lock);
            init_state = ok ? 2 : 3;
            SDL_BroadcastCondition(q_cond);
        }
        if (game_done && !ready) {
            SDL_UnlockMutex(q_lock);
            return;
        }
        f = ready;
        ready = NULL;
        win_games += f != NULL;
        if (f) {
            held_prev = cur;
            held_cur = f;
        }
        SDL_UnlockMutex(q_lock);

        if (f) {
            if (cur && interp) {
                /* Whole-screen hitches: a game frame over a vblank late
                 * (the game took longer), or one that reached this thread
                 * after the blend toward the one before had run out. */
                double gap = (double)(f->stamp - cur->stamp), late = (double)(SDL_GetTicksNS() - f->stamp);
                static Uint64 hitch_window;
                static uint32_t hitch_logs;
                if (now - hitch_window > 5000000000ull) {
                    hitch_window = now;
                    hitch_logs = 0;
                }
                if ((gap > frame_interval * 1.5 || late > frame_interval) && hitch_logs++ < 10)
                    SDL_Log("interp: frame %llu %.1f ms after the one before, here %.1f ms after its vblank",
                            (unsigned long long)f->frame, gap / 1e6, late / 1e6);
            }
            if (cur) {
                double d = (double)(f->stamp - cur->stamp);
                if (d > 8e6 && d < 50e6)
                    frame_interval += (d - frame_interval) * 0.1;
            }
            prev = cur;
            cur = f;
            programs_switched = viewport_changed = false;
            {
                Uint64 m0 = SDL_GetTicksNS(), dm;
                static uint32_t slow_logs;
                static Uint64 slow_window;
                match_frames(interp ? prev : NULL, cur);
                dm = SDL_GetTicksNS() - m0;
                main_match_ns += dm;
                if (now - slow_window > 5000000000ull) {
                    slow_window = now;
                    slow_logs = 0;
                }
                if (dm > 8000000 && slow_logs++ < 5) {     /* a frame's pairing over 8 ms */
                    uint32_t nd = 0;
                    for (uint32_t k = 0; k < cur->ncmds; k++)
                        nd += cur->cmds[k].kind == CMD_DRAW;
                    SDL_Log("interp: frame %llu: pairing took %.1f ms (%u draws): passes %.1f, across models %.1f, "
                            "wrong pairs %.1f, picture %.1f, logs %.1f", (unsigned long long)cur->frame, dm / 1e6, nd,
                            mf_ns[0] / 1e6, mf_ns[1] / 1e6, mf_ns[2] / 1e6, mf_ns[3] / 1e6, mf_ns[4] / 1e6);
                }
            }
            if (prev && f->frame >= capture_frame && f->frame < capture_frame + capture_seq)
                capture_interp(prev, cur);
            /* Captures and screenshots are of the frame as it is. */
            if (!interp || !prev || shot_frame(f) || f->frame == capture_frame) {
                draw_frame(f, NULL, 1.0f);
                win_shown++;
                SDL_LockMutex(q_lock);
                frames_drawn++;
                SDL_UnlockMutex(q_lock);
                if (!interp)
                    continue;
            }
        }
        if (!interp || !prev || !cur)
            continue;
        {
            /* One game frame behind: as the newest arrives, show the one
             * before; as the next is due, the newest (t worked out in
             * draw_frame_main, once the window's image is in hand). */
            draw_frame(cur, prev, -1.0f);
            win_shown++;
            SDL_LockMutex(q_lock);
            frames_drawn++;
            SDL_UnlockMutex(q_lock);
            if (!have_swapchain)
                SDL_Delay(1);
        }
    }
}

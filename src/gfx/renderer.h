#ifndef PDO_RENDERER_H
#define PDO_RENDERER_H

/*
 * The host side of our Direct3D replacement, on the SDL3 GPU API.
 *
 * Each frame is recorded as a list of commands (clears and draws, with all
 * the state they need copied in) and replayed when the game calls Swap:
 * vertices, indices and textures are uploaded once, then every command is
 * drawn in order. Recording rather than drawing immediately is what later
 * allows drawing the frame on another thread, interpolated frames, and
 * comparing frames against captures.
 *
 * The game renders into an internal target at its own resolution, presented
 * scaled with its aspect ratio kept. Debug switches (environment):
 *   PDO_SCREENSHOT=dir      save presented frames as BMP (first 300 saved)
 *   PDO_SCREENSHOT_EVERY=n  only every n-th frame
 */

#include <stdbool.h>
#include <stdint.h>

bool pdo_gfx_init(int width, int height);

/* Threads: the game runs on a thread of its own and the main thread, which
 * owns the window, does all the drawing. On the main thread, before the
 * game starts: pdo_gfx_prepare_threads; then pdo_gfx_main_loop, which draws
 * the frames the game hands over at Swap (and does pdo_gfx_init when the
 * game asks) until the game thread calls pdo_gfx_game_finished. */
void pdo_gfx_prepare_threads(void);
void pdo_gfx_main_loop(void);
void pdo_gfx_game_finished(void);
/* Frames the main thread drew, and frames it never took (a newer one came
 * first), since the last call. */
void pdo_gfx_take_frame_counts(uint32_t *drawn, uint32_t *dropped);

/* One vertex as the shaders see it. Positions are in clip space; colors are
 * D3DCOLORs; four texture coordinate sets of up to four components; fog is
 * the fog coordinate (eye distance, or the fog factor itself when the fog
 * table mode is NONE). psize is the point size in pixels, for point lists
 * (the shaders do not read it). */
typedef struct pdo_vertex {
    float    pos[4];
    uint32_t diffuse, specular;
    float    tc[4][4];
    float    fog;
    float    psize;
} pdo_vertex;

/* A texture as the game described it: a guest D3DPixelContainer's data
 * address and Format/Size words, plus how it is sampled (XDK D3DTEXF_* and
 * D3DTADDRESS_* values). data == 0 means no texture. */
typedef struct pdo_tex_ref {
    uint32_t data, format_word, size_word;
    uint8_t  min_filter, mag_filter, mip_filter;
    uint8_t  address_u, address_v;
} pdo_tex_ref;

/* Fixed-function stage setup, laid out as the fragment shader's std140
 * uniform block (src/gfx/shaders/ff.frag). */
typedef struct pdo_ff_uniforms {
    int32_t color_op[4][4];     /* op, arg1, arg2, arg0 */
    int32_t alpha_op[4][4];
    int32_t stage_misc[4][4];   /* result arg, has texture, texcoord set, shadow
                                 * (8 | D3DCMP - 0x200, + 16 for 24-bit depth) */
    float   tfactor[4];
    int32_t alpha_test[4];      /* enable, func, ref, specular enable */
    /* A pixel shader (NV2A register combiners), straight from the XDK's
     * D3DPIXELSHADERDEF. ps_final: final combiner ABCD inputs, EFG inputs,
     * PSCombinerCount (0 = no pixel shader: the stages above apply),
     * PSTextureModes. */
    int32_t ps_rgb_in[8], ps_rgb_out[8], ps_a_in[8], ps_a_out[8];
    float   ps_c0[8][4], ps_c1[8][4];
    int32_t ps_final[4];
    float   ps_fc0[4], ps_fc1[4];
    /* Per stage: texture coordinate scale (x, y). Linear (non-swizzled)
     * textures are addressed in texels on the NV2A, so 1/width, 1/height;
     * swizzled and compressed ones are normalised already, so 1.
     * z: 1 if the format has no alpha (alpha reads as 1).
     * w: 1 if red and green are signed (V8U8), stored offset by 128. */
    float   tc_scale[4][4];
    /* Fog: enable, D3DFOGMODE (0 NONE: the coordinate is the factor,
     * 1 EXP, 2 EXP2, 3 LINEAR); color; start, end, density. */
    int32_t fog_mode[4];
    float   fog_color[4];
    float   fog_params[4];
    /* Per stage, for texture mode BUMPENVMAP(_LUM): the bump matrix in
     * texture stage state order (00, 01, 11, 10) and luminance scale,
     * offset. */
    float   bump_mat[4][4];
    float   bump_lum[4][4];
} pdo_ff_uniforms;

/* Fixed-function output state, in the XDK's GL-valued enums. */
typedef struct pdo_pipeline_state {
    uint8_t  topology;          /* 0 triangles, 1 lines, 2 points, 3 triangle strip */
    uint8_t  blend_enable;
    uint16_t src_blend, dst_blend, blend_op;
    uint8_t  z_enable, z_write;
    uint8_t  cull;              /* 0 none, 1 cull clockwise, 2 counter-clockwise */
    uint8_t  single_sample;     /* renderer only: the target is a shadow map */
    uint16_t z_func;
    uint8_t  gpu_vsh;           /* renderer only: vertices from shaders/vsh.vert (1)
                                 * or shaders/fvf.vert (2) */
    uint8_t  combiners;         /* renderer only: a pixel shader (ff.frag built with
                                 * PIXEL_SHADER), not the texture stages */
    uint32_t color_write;       /* Xbox D3DCOLORWRITEENABLE_* bits */
    /* Polygon offset (D3DRS_ZBIAS sets these): constant in depth-buffer
     * units, and slope scale. Decals rely on it to win against the wall. */
    float    depth_bias, depth_slope;
    /* Stencil (GL-valued, as the XDK stores them): enable, func, ops for
     * stencil fail / depth fail / pass, read and write masks. */
    uint8_t  stencil_enable, stencil_mask, stencil_write_mask;
    uint16_t stencil_func, stencil_fail, stencil_zfail, stencil_pass;
} pdo_pipeline_state;

typedef struct pdo_draw {
    pdo_pipeline_state ps;
    pdo_tex_ref        tex[4];
    pdo_ff_uniforms    ff;
    float              viewport[6];  /* x, y, w, h, min z, max z */
    uint8_t            stencil_ref;
    /* For captures only: the vertex shader (FVF or program handle),
     * lighting enable, COLORVERTEX with the number of lights switched on in
 * bits 8-15, material sources (diffuse, ambient,
     * specular, emissive: a nibble each), AMBIENT, the material's diffuse,
     * ambient and emissive as D3DCOLORs. */
    uint32_t           vertex_info[8];
    /* For captures only: the game code that drew it -- the return address
     * of the D3D call, then two up the ebp chain (0 where it ends). */
    uint32_t           caller[3];
} pdo_draw;

/* Record a draw. idx are indices into v; both are copied. */
void pdo_gfx_draw(const pdo_draw *d, const pdo_vertex *v, uint32_t nv,
                  const uint32_t *idx, uint32_t ni);
/* What the next pdo_gfx_draw's vertices were made from (the host address
 * of the game's vertex data), so frame interpolation can find the same
 * draw in the next frame. */
void pdo_gfx_set_draw_source(const void *src);

/* D3DCLEAR_* flags: TARGET 0xF0, ZBUFFER 0x01, STENCIL 0x02. color is a
 * D3DCOLOR (ARGB). */
void pdo_gfx_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil);

/* A guest surface: a D3DPixelContainer's data address and Format/Size
 * words. data == 0 is the back buffer (the frame we present). */
typedef struct pdo_surface {
    uint32_t data, format_word, size_word;
} pdo_surface;

/* D3DDevice_SetRenderTarget: later clears and draws go to this surface. A
 * texture whose data address is a surface drawn to this way samples what was
 * drawn (the GPU copy), not guest memory. */
void pdo_gfx_set_target(const pdo_surface *color);

/* The depth-stencil surface set with it (pNewZStencil; data 0 for none).
 * A depth-only pass (a color surface without memory) renders into a
 * target keyed by this surface, and a depth-format texture over the same
 * memory samples that target's depth: a shadow map. */
void pdo_gfx_set_depth(const pdo_surface *depth);

/* ── Vertex programs on the GPU (shaders/vsh.vert) ───────────────────────
 * Instead of vertices, such a draw carries the program, its constants and
 * the game's vertex streams as raw bytes; the shader runs the program per
 * vertex as src/guest/vsh.c does on the CPU. */

/* A compiled program in the shader's form (pdo_vsh_gpu_code: 8 words an
 * instruction). Returns its place for pdo_gpu_vsh.program, or -1. Programs
 * are kept for the session. */
int32_t pdo_gfx_vsh_program(const uint32_t *code, uint32_t count);

typedef struct pdo_gpu_vsh {
    int32_t  program;                  /* pdo_gfx_vsh_program */
    uint32_t count;                    /* instructions */
    const float (*constants)[4];       /* the 192 the program reads */
    const uint8_t *consts_read;        /* a bit per one it really reads (NULL: all) */
    float    anchor[2][4];             /* oPos of the first and last vertex, run on the CPU */
    bool     anchored;                 /* (frame interpolation pairs alike draws by them) */
    const uint8_t *stream_data[16];    /* host pointer at the draw's first vertex */
    uint32_t stream_bytes[16];         /* 0: stream unused */
    int32_t  input[16][4];             /* per v register: stream, stride, offset,
                                        * D3DVSDT type (0: absent, reads 0, 0, 0, 1) */
    float    vp_scale[4], vp_offset[4];/* the program's viewport transform; offset w:
                                        * the depth scale */
} pdo_gpu_vsh;

/* A draw of nv vertices made by g; idx index them from 0. cpu, if not
 * NULL, is the same vertices from the CPU interpreter: a capture can then
 * draw the frame both ways (PDO_CAPTURE_CPUVSH=1). */
void pdo_gfx_draw_vsh(const pdo_draw *d, const pdo_gpu_vsh *g, uint32_t nv,
                      const uint32_t *idx, uint32_t ni, const pdo_vertex *cpu);

/* ── Fixed-function vertices on the GPU (shaders/fvf.vert) ──────────────
 * The same idea for FVF draws: the game's vertex bytes and the draw's
 * transform, texture-stage and lighting state; the shader does what
 * fetch_fvf and light_vertex (d3d_hle.c) do per vertex. The layout is the
 * shader's uniform block, std140: every member 16 bytes or a multiple. */
typedef struct pdo_fvf_uniforms {
    int32_t layout0[4];          /* byte base (the renderer's), stride, flags, coordinate sets */
    int32_t offs[4];             /* normal, diffuse, specular offsets */
    int32_t tc_off[4], tc_n[4];  /* per coordinate set */
    int32_t stage_set[4], stage_gen[4], stage_tt[4];
    int32_t light_info[4];       /* lights, COLORVERTEX */
    int32_t mat_src[4];          /* diffuse, ambient, specular, emissive material source */
    float   rt[4];               /* target width, height */
    float   wvp[16], wv[16], world[16];
    float   rz[4];               /* reversed depth: w - z of wvp, as a column (rows' entries) */
    float   tex[4][16];
    float   material[4][4];      /* diffuse, ambient, specular, emissive */
    float   ambient[4];
    float   normal[4];           /* the latched normal, for an FVF without one */
    float   light[8][7][4];      /* see shaders/fvf.vert */
    /* frame interpolation, filled by the renderer (match_frames) */
    int32_t interp[4];
    float   blend_t[4];
    float   prev_wvp[16], prev_wv[16], prev_world[16];
    int32_t interp2[4];          /* x: stages whose texture matrix blends from prev_tex */
    float   prev_tex[4][16];
    float   prev_rz[4];
} pdo_fvf_uniforms;

/* Reversed depth (renderer.c): the depth buffer holds 1 - z. */
bool pdo_gfx_depth_reversed(void);

enum { PDO_FVF_PRE = 1, PDO_FVF_DIFFUSE = 2, PDO_FVF_SPECULAR = 4, PDO_FVF_NORMAL = 8,
       PDO_FVF_LIT = 16, PDO_FVF_FOGTABLE = 32 };

typedef struct pdo_gpu_fvf {
    const uint8_t *data;         /* host pointer at the draw's first vertex */
    uint32_t bytes;
    pdo_fvf_uniforms u;
} pdo_gpu_fvf;

/* A draw of nv fixed-function vertices made by fvf.vert from g; idx index
 * them from 0. cpu: the CPU's vertices too, as for pdo_gfx_draw_vsh.
 * Returns false (nothing recorded) when the renderer cannot. */
bool pdo_gfx_draw_fvf(const pdo_draw *d, const pdo_gpu_fvf *g, uint32_t nv,
                      const uint32_t *idx, uint32_t ni, const pdo_vertex *cpu);

/* The frame being recorded will be captured (F12 or PDO_CAPTURE). */
bool pdo_gfx_capturing(void);

/* Nanoseconds spent finding, hashing and decoding textures since the last
 * call (the game thread's timing log, d3d_hle.c). */
uint64_t pdo_gfx_take_texture_ns(void);
/* Time spent making pipelines mid-game since the last call, and how many. */
uint64_t pdo_gfx_take_pipeline_ns(uint32_t *made);
/* Time spent waiting for a swapchain image (the GPU behind) since the last call. */
uint64_t pdo_gfx_take_swap_wait_ns(void);
/* Render passes, multisample seeds, snapshots and copies since the last call. */
void pdo_gfx_take_pass_counts(uint32_t out[4]);

/* D3DDevice_CopyRects, one rectangle: src (x, y, w, h) to dst at (dx, dy),
 * clipped to both surfaces. Either may be the back buffer. */
void pdo_gfx_copy_rect(const pdo_surface *src, const pdo_surface *dst,
                       int x, int y, int w, int h, int dx, int dy);

/* Width and height of a surface (the frame's for the back buffer). */
void pdo_gfx_surface_size(const pdo_surface *s, int *w, int *h);

/* The display gamma ramp: 256 red, then 256 green, then 256 blue entries
 * (Xbox D3DGAMMARAMP), applied when the frame is presented. */
void pdo_gfx_set_gamma(const uint8_t ramp[768]);

/* Replay the recorded frame and show it. Returns false if the player closed
 * the window. */
bool pdo_gfx_present(void);
/* When the frame about to be presented shows (SDL ticks, ns): the vblank
 * its Swap waited for, so frames sit on an even grid for interpolation. */
void pdo_gfx_set_frame_time(uint64_t ns);

/* The options overlay holds the game: the vblank clock (d3d_hle.c) stops
 * with it. How long it has held the game in all (ns), and whether it does
 * now. */
uint64_t pdo_gfx_paused_ns(void);
bool pdo_gfx_paused(void);
/* Window events between frames (loading, movies): false when the window was
 * closed. */
bool pdo_gfx_pump_events(void);

uint64_t pdo_gfx_frame_count(void);
/* Lossless Scaling frame generation shows this run's frames (lsfg_present.h). */
bool pdo_gfx_lsfg_active(void);

#endif

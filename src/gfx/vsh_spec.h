/* Vertex programs as their own GPU shaders: shaders/vsh.vert built, at run
 * time, with one program's code as straight-line GLSL (PDO_VSH_SPEC)
 * instead of interpreting it. Mobile GPUs (Adreno) run the interpreter's
 * loop over a register array slowly; the same program unrolled, its
 * registers plain variables, runs several times faster.
 *
 * Programs are compiled by glslang on a thread of their own, kept in
 * memory and on disk (shaders/ in the data folder), keyed by the program's
 * code and vsh.vert's source together. Until one is ready the renderer
 * draws with the interpreter, which stays for draws paired with another
 * program (frame interpolation) and when PDO_VSH_SPEC=0. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether programs are specialised at all (built with glslang, and not
 * turned off with PDO_VSH_SPEC=0). */
bool pdo_vsh_spec_enabled(void);

/* The key of a program's shader (code: pdo_vsh_gpu_code's 8 words an
 * instruction). Never 0. With code NULL, the build's: vsh.vert's alone. */
uint64_t pdo_vsh_spec_key(const uint32_t *code, uint32_t count);

/* The program's GLSL: vsh.vert with its run_spec. NULL-terminated, to free
 * with free(). */
char *pdo_vsh_spec_glsl(const uint32_t *code, uint32_t count);

/* The program's SPIR-V: 1 ready (*spv and *bytes stay valid until
 * pdo_vsh_spec_quit), 0 being made (the first call asks for it), -1 it
 * cannot be. */
int pdo_vsh_spec_get(uint64_t key, const uint32_t *code, uint32_t count,
                     const uint32_t **spv, size_t *bytes);

/* The same from what is made or on disk already, without asking for it:
 * for pipelines made up front. */
bool pdo_vsh_spec_find(uint64_t key, const uint32_t **spv, size_t *bytes);

/* Programs made (and failed) so far, for the log; ms spent making them. */
void pdo_vsh_spec_counts(uint32_t *made, uint32_t *failed, uint32_t *pending, double *ms);

void pdo_vsh_spec_quit(void);

/* Fragment setups: shaders/ff.frag built with one draw's setup (texture
 * stage operations, or pixel shader stages; alpha test; fog mode) as
 * constants, so the GPU's compiler leaves out what the setup does not do,
 * where the interpreter branches on it at every pixel. Made and kept like
 * the vertex programs, on the same thread and in the same folder.
 * PDO_FS_SPEC=0 turns them off. */
#define PDO_FS_SPEC_WORDS 90
struct pdo_ff_uniforms;
bool pdo_fs_spec_enabled(void);
/* The setup a draw's uniforms (and whether it uses a pixel shader) ask for. */
void pdo_fs_spec_blob(const struct pdo_ff_uniforms *u, bool combiners, int32_t out[PDO_FS_SPEC_WORDS]);
uint64_t pdo_fs_spec_key(const int32_t *w);
char *pdo_fs_spec_glsl(const int32_t *w);
/* As pdo_vsh_spec_get; ready shaders also come from pdo_vsh_spec_find. */
int pdo_fs_spec_get(uint64_t key, const int32_t *w, const uint32_t **spv, size_t *bytes);

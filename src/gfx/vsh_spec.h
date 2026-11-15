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

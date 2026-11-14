/*
 * pd1's frame pacing.
 *
 * pd1 counts vblanks with its vertical-blank callback: 0x001A39E0 adds 1 to
 * 0x0098D6C0 (registered with SetVerticalBlankCallback by 0x001A39F0), and
 * 0x001A3A20 reads it. Its frame limiter, 0x00124437 (cdecl, n), spins on
 * that read until n vblanks have passed since its last call (kept at
 * 0x0092D4A4); n = 0 only notes the count. On the console the vblank
 * interrupt runs the callback while it spins. Here nothing interrupts it,
 * and as the loop makes no kernel call it also keeps the guest CPU lock,
 * so no other guest thread would run either.
 *
 * Same logic, waiting for each vblank instead (the other threads run
 * meanwhile) and running the callback there (src/hle/d3d_hle.c).
 */

#include "recomp_types.h"

#include <SDL3/SDL.h>

#define VBLANK_COUNT    0x0098D6C0u     /* the callback's count */
#define LAST_FRAME      0x0092D4A4u     /* the count at the limiter's last call */

void pdo_d3d_vblank_wait(void);         /* src/hle/d3d_hle.c (pd1's copy) */

/* pd1's frame limiter (cdecl, n) */
void sub_00124437(void)
{
    uint32_t n = MEM32(g_esp + 4);
    uint32_t now = MEM32(VBLANK_COUNT);
    uint32_t waited = 0;
    static bool told;

    while (n && now - MEM32(LAST_FRAME) < n) {
        pdo_d3d_vblank_wait();
        now = MEM32(VBLANK_COUNT);
        /* The count stands still only with no callback registered, which
         * would hang the console too; say so rather than hang. */
        if (++waited >= n + 60 && now - MEM32(LAST_FRAME) < n) {
            if (!told) {
                told = true;
                SDL_Log("pd1: the vblank count stands still (no vblank callback?); "
                        "frame limiter going on after a second");
            }
            break;
        }
    }
    MEM32(LAST_FRAME) = now;
    g_eax = now;
    g_esp += 4;                 /* ret */
}

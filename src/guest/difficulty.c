/*
 * Orta's difficulty, logged when it changes (issue 8: Normal plays like Easy).
 *
 * The game keeps the chosen difficulty at 0x00342778 and an offset at
 * 0x00342788, and every difficulty-dependent value it reads (52 places) is
 * a table entry at max(difficulty - offset, 0). At boot 0x00011820 sets the
 * offset from the console's game region (XGetGameRegion, kept at
 * 0x0034278C): 0 for Japan (2), 1 for any other region. Facts in
 * docs/orta-facts.md.
 */

#include "recomp_types.h"
#include "guest/program.h"
#include <SDL3/SDL.h>

/* From Swap, once a frame. Orta's addresses: does nothing for pd1. */
void pdo_difficulty_frame(void)
{
    static uint32_t diff = ~0u, offset = ~0u, region = ~0u;
    uint32_t d, o, r;

    if (pdo_program_active() != PDO_PROGRAM_ORTA)
        return;
    d = MEM32(0x00342778);
    o = MEM32(0x00342788);
    r = MEM32(0x0034278C);
    if (d == diff && o == offset && r == region)
        return;
    diff = d;
    offset = o;
    region = r;
    SDL_Log("orta: difficulty %u, offset %u (game region %u): its tables are read at %d",
            d, o, r, (int32_t)(d - o) > 0 ? (int32_t)(d - o) : 0);
}

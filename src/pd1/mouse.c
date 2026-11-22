/*
 * Mouse aim in pd1: its cursor update, 0x0005EA44 (cdecl, one argument: an
 * object whose +0x1C is the cursor block: +0 the mode, 0 in flight; +0xA,
 * +0xC the cursor). pdo_pd1_cursor() (src/hle/input_hle.c, pd1's build)
 * says where the mouse puts the cursor; it is set before the game's update,
 * so this frame uses it, and again after it, as the update pulls the cursor
 * back towards the middle a little every frame (mouse aim felt like
 * molasses, a frame late and dragged back). Not aiming with the mouse, or
 * outside flight, the update is the game's alone.
 */

#include <stdbool.h>
#include "recomp_types.h"

bool pdo_pd1_cursor(uint32_t block, int16_t *cx, int16_t *cy);

extern void sub_0005EA44_gen(void);
void sub_0005EA44(void)
{
    uint32_t block = MEM32(g_esp + 4) + 0x1Cu;
    int16_t x, y;
    bool mine = block >= 0x10000u + 0x1Cu && block < 0x04000000u && MEM16(block) == 0 &&
                pdo_pd1_cursor(block, &x, &y);

    if (mine) {
        MEM16(block + 0xA) = (uint16_t)x;
        MEM16(block + 0xC) = (uint16_t)y;
    }
    sub_0005EA44_gen();
    if (mine && MEM16(block) == 0) {
        MEM16(block + 0xA) = (uint16_t)x;
        MEM16(block + 0xC) = (uint16_t)y;
    }
}

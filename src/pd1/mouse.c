/*
 * Mouse aim in pd1 (src/hle/input_hle.c, pd1's build: pd1_mouse() tilts the
 * stick towards where the mouse wants the cursor). Its cursor update,
 * 0x0005EA44 (cdecl, one argument: an object whose +0x1C is the cursor
 * block, +0 its mode, 0 in flight), runs as the game's; after it, in
 * flight, pdo_pd1_cursor() marks flight and, within a frame's travel of
 * the target, puts the cursor on it.
 */

#include "recomp_types.h"

void pdo_pd1_cursor(uint32_t block);

extern void sub_0005EA44_gen(void);
void sub_0005EA44(void)
{
    uint32_t block = MEM32(g_esp + 4) + 0x1Cu;

    sub_0005EA44_gen();
    if (block >= 0x10000u + 0x1Cu && block < 0x04000000u && MEM16(block) == 0)
        pdo_pd1_cursor(block);
}

/*
 * Mouse aim in pd1 (src/hle/input_hle.c, pd1's build). Its cursor update,
 * 0x0005EA44 (cdecl, one argument: an object whose +0x1C is the cursor
 * block, +0 its mode, 0 in flight), runs as the game's, with the direction
 * bits set towards where the mouse wants the cursor for that call
 * (pdo_pd1_cursor_before) and the button word put back after it, the cursor
 * put on the target after it (pdo_pd1_cursor_after).
 */

#include <stdbool.h>
#include "recomp_types.h"

void pdo_pd1_cursor_before(uint32_t block);
void pdo_pd1_cursor_after(uint32_t block);

extern void sub_0005EA44_gen(void);
void sub_0005EA44(void)
{
    uint32_t block = MEM32(g_esp + 4) + 0x1Cu;
    bool flight = block >= 0x10000u + 0x1Cu && block < 0x04000000u && MEM16(block) == 0;

    if (flight)
        pdo_pd1_cursor_before(block);
    sub_0005EA44_gen();
    if (flight)
        pdo_pd1_cursor_after(block);
}

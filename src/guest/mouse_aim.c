/*
 * Mouse aim on Orta's reticle (issue 51; input in src/hle/input_hle.c).
 *
 * The reticle's aim is a damped spring. Each frame the reticle update
 * (0x0007B5D0, thiscall, on [player+0x3D0], player at 0x00472030) pulls its
 * angles towards where the stick puts them: for each axis the pull is
 * +0x110/+0x114 plus [player+0x380] x the error, the velocity +0x130/+0x134
 * takes the pull and loses [player+0x384] of itself, and the angle
 * (+0xC0 pitch, clamped to 90 degrees; +0xC4 yaw) takes the velocity. With
 * a controller that is a glide of about a second; with the mouse it reads
 * as drift. While the mouse aims, the update runs with stiffness 1, no
 * damping and no velocity carried over, so the angles land where the stick
 * (the mouse's cursor) puts them in that frame. The two constants are the
 * player's and another spring reads them too (0x0007B2D0), so they are put
 * back as soon as the update returns.
 */

#include "recomp_types.h"
#include "input_map.h"

#define PLAYER 0x00472030u

extern void sub_0007B5D0_gen(void);
void sub_0007B5D0(void)
{
    uint32_t player = MEM32(PLAYER), self = g_ecx, stiffness, damping;

    if (!pdo_mouse_aim_active() || player < 0x10000u || self < 0x10000u) {
        sub_0007B5D0_gen();
        return;
    }
    stiffness = MEM32(player + 0x380);
    damping = MEM32(player + 0x384);
    MEMF(player + 0x380) = 1.0f;
    MEMF(player + 0x384) = 0.0f;
    MEMF(self + 0x130) = 0.0f;
    MEMF(self + 0x134) = 0.0f;
    sub_0007B5D0_gen();
    MEM32(player + 0x380) = stiffness;
    MEM32(player + 0x384) = damping;
}

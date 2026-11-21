/*
 * The camera's follow of the aim, for dual-stick aiming (Controls > Dual
 * stk; the input is src/hle/input_hle.c).
 *
 * 0x0007B5D0 (thiscall on [player+0x3D0], player at 0x00472030) is a damped
 * spring per axis turning the camera after the aim: the pull +0x120/+0x124
 * = +0x110/+0x114 + [player+0x380] x the error, the velocity +0x130/+0x134
 * takes the pull and loses [player+0x384] of itself, and the angles take the
 * velocity: +0xC0 pitch (clamped to 90), +0xC4 yaw (wrapping at 360). The
 * targets are the aim angles, player +0xF8/+0xFC.
 *
 * With dual-stick aiming on, it runs with stiffness 1, no damping and no
 * velocity carried over, so the camera is on the aim each frame and the
 * view turns with the right stick (the game clamps the aim around the
 * camera, so with the camera following it the aim goes all the way
 * round). The aim's yaw does not wrap as the camera's does: it is brought
 * to within 180 degrees of the camera's first (the same direction), or once
 * round the error is 360 and the camera shakes. The two constants are the
 * player's, read by another spring too (0x0007B2D0), so they are put back
 * as soon as the update returns. Off: the game's own.
 */

#include "recomp_types.h"
#include "config.h"

#define PLAYER 0x00472030u

extern void sub_0007B5D0_gen(void);
void sub_0007B5D0(void)
{
    uint32_t player = MEM32(PLAYER), self = g_ecx, stiffness, damping;
    float d;

    if (!pdo_settings_get()->dual_stick || player < 0x10000u || player >= 0x04000000u ||
        self < 0x10000u || self != MEM32(player + 0x3D0)) {
        sub_0007B5D0_gen();
        return;
    }
    d = MEMF(player + 0xFC) - MEMF(self + 0xC4);
    if (d == d && d > -1e5f && d < 1e5f) {
        while (d > 180.0f)
            d -= 360.0f;
        while (d < -180.0f)
            d += 360.0f;
        MEMF(player + 0xFC) = MEMF(self + 0xC4) + d;
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

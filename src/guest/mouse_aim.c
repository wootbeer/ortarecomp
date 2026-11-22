/*
 * The camera's follow of the aim, for dual-stick aiming (Controls > Dual
 * stk; the input is src/hle/input_hle.c): the camera on the aim, and the
 * aim free of the quarter it was held in.
 *
 * 0x0007B5D0 (thiscall on [player+0x3D0], player at 0x00472030) is a damped
 * spring per axis turning the camera after the aim: the pull +0x120/+0x124
 * = +0x110/+0x114 + [player+0x380] x the error, the velocity +0x130/+0x134
 * takes the pull and loses [player+0x384] of itself, and the angles take the
 * velocity: +0xC0 pitch (held to 40 either way), +0xC4 yaw (not wrapped). The
 * targets are the aim angles, player +0xF8/+0xFC.
 *
 * With dual-stick aiming on, it runs with stiffness 1, no damping and no
 * velocity carried over, so the camera is on the aim each frame and the
 * view turns with the right stick (the game clamps the aim around the
 * camera, so with the camera following it the aim goes all the way
 * round). The game wraps the aim's yaw to -180..180 and not the camera's:
 * the aim's is brought to within 180 degrees of the camera's (by whole
 * turns), before the update and after it, and after the aim update. The two constants are the
 * player's, read by another spring too (0x0007B2D0), so they are put back
 * as soon as the update returns. Off: the game's own.
 */

#include "recomp_types.h"
#include "config.h"
#include <SDL3/SDL.h>

#define PLAYER 0x00472030u

/* The aim's yaw brought within 180 degrees of the camera's by whole turns
 * (the same direction). The game wraps the aim's yaw to -180..180 and not
 * the camera's, and both its clamp of the aim around the camera and the
 * reticle take their difference. The camera's is left alone: what draws
 * the view follows it smoothly, and a camera moved by a turn swung round
 * (a shake at the back, the third try). */
static void aim_near_camera(uint32_t player, uint32_t cam)
{
    float d = MEMF(player + 0xFC) - MEMF(cam + 0xC4);

    if (d == d && d > -1e5f && d < 1e5f && (d > 180.0f || d < -180.0f)) {
        float turns = SDL_floorf((d + 180.0f) / 360.0f);
        MEMF(player + 0xFC) -= turns * 360.0f;
    }
}

extern void sub_0007B5D0_gen(void);
void sub_0007B5D0(void)
{
    uint32_t player = MEM32(PLAYER), self = g_ecx, stiffness, damping;

    if (!pdo_settings_get()->dual_stick || player < 0x10000u || player >= 0x04000000u ||
        self < 0x10000u || self != MEM32(player + 0x3D0)) {
        sub_0007B5D0_gen();
        return;
    }
    aim_near_camera(player, self);
    stiffness = MEM32(player + 0x380);
    damping = MEM32(player + 0x384);
    MEMF(player + 0x380) = 1.0f;
    MEMF(player + 0x384) = 0.0f;
    MEMF(self + 0x130) = 0.0f;
    MEMF(self + 0x134) = 0.0f;
    sub_0007B5D0_gen();
    MEM32(player + 0x380) = stiffness;
    MEM32(player + 0x384) = damping;
    aim_near_camera(player, self);      /* the camera turned in it */
}

/* The aim update (0x0007E6A0, thiscall on the aim block, player+0xB0)
 * clamps the aim's yaw (block +0x4C) between block +0x94 and +0x104: the
 * view's quarter, about 50 degrees either way of it (a trace: held at
 * 50, the camera at 46). With dual-stick aiming the right stick turns past
 * them, and the clamp pulled the aim back each frame while the stick
 * pushed it on: the camera, on the aim, shook. While it is on, the update
 * runs without that clamp (the limits wide open for the call, put back
 * after it); the pitch's is kept, and the quarter turns (L/R) are the
 * game's. */
extern void sub_0007E6A0_gen(void);
void sub_0007E6A0(void)
{
    uint32_t player = MEM32(PLAYER), block = g_ecx, lo, hi;

    if (!pdo_settings_get()->dual_stick || player < 0x10000u || player >= 0x04000000u ||
        block != player + 0xB0u) {
        sub_0007E6A0_gen();
        return;
    }
    {   /* PDO_DUAL_TRACE=1: frames where the aim and the camera part, or
         * the reticle leaves the picture (at most 300 lines) */
        static int trace = -1, lines;
        uint32_t cam = MEM32(player + 0x3D0);
        if (trace < 0)
            trace = SDL_getenv("PDO_DUAL_TRACE") && *SDL_getenv("PDO_DUAL_TRACE") == '1';
        if (trace && lines < 300 && cam >= 0x10000u && cam < 0x04000000u) {
            float dy = MEMF(player + 0xFC) - MEMF(cam + 0xC4), dp = MEMF(player + 0xF8) - MEMF(cam + 0xC0);
            float rx = MEMF(player + 0xB8), ry = MEMF(player + 0xBC);
            if (SDL_fabsf(dy) > 20.0f || SDL_fabsf(dp) > 20.0f || rx < 0.0f || rx > 640.0f || ry < 0.0f || ry > 480.0f) {
                lines++;
                SDL_Log("dual frame: aim %+.1f %+.1f camera %+.1f %+.1f (vel %+.2f %+.2f) reticle %.0f %.0f",
                        MEMF(player + 0xF8), MEMF(player + 0xFC), MEMF(cam + 0xC0), MEMF(cam + 0xC4),
                        MEMF(cam + 0x130), MEMF(cam + 0x134), rx, ry);
            }
        }
    }
    lo = MEM32(block + 0x94);
    hi = MEM32(block + 0x104);
    MEMF(block + 0x94) = -1e9f;
    MEMF(block + 0x104) = 1e9f;
    sub_0007E6A0_gen();
    MEM32(block + 0x94) = lo;
    MEM32(block + 0x104) = hi;
    {
        uint32_t cam = MEM32(player + 0x3D0);
        if (cam >= 0x10000u && cam < 0x04000000u)
            aim_near_camera(player, cam);   /* it may have wrapped the aim */
    }
}

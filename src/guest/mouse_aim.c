/*
 * The camera's follow of the aim, for dual-stick aiming (Controls > Dual
 * stk; the input is src/hle/input_hle.c): the camera on the aim, the aim
 * free of the quarter it was held in and of the left stick.
 *
 * 0x0007B5D0 (thiscall on [player+0x3D0], player at 0x00472030) is a damped
 * spring per axis turning the camera after the aim: the pull +0x120/+0x124
 * = +0x110/+0x114 + [player+0x380] x the error, the velocity +0x130/+0x134
 * takes the pull and loses [player+0x384] of itself, and the angles take the
 * velocity: +0xC0 pitch (held to 40 either way), +0xC4 yaw (not wrapped). The
 * targets are the aim angles, player +0xF8/+0xFC.
 *
 * With dual-stick aiming on, the camera's angles are put on the aim (below);
 * the springs themselves are the game's. Off: the game's own.
 */

#include "recomp_types.h"
#include "config.h"
#include <SDL3/SDL.h>

#define PLAYER 0x00472030u

/* With dual-stick aiming on, the camera's angles are set to the aim's
 * before its update and the yaw again after it, their velocities cleared:
 * the view points where the aim does, and the rest of the update (where
 * the camera sits behind the dragon, its own springs) runs as the game's.
 * Earlier tries changed the spring's stiffness and damping instead, which
 * the position springs read too: undamped, they swung, worst when turning
 * round (the shake). The pitch is left as the update clamps it (40 up,
 * about 32 down). The aim's yaw stays in -180..180, as the game keeps it
 * (past it the reticle went wild): the camera's takes the same value.
 *
 * The update also leans the camera sideways (+0x230, a spring towards
 * +0x140 = a constant x the camera's yaw: 0x0035F840, or 0x0035F838 in
 * another mode; zero while the game turns a quarter), and the aim update
 * builds the reticle's and the shots' direction from that lean. Within a
 * quarter (+-50) it is small; turned round with the right stick it grew
 * to +-10, flipped across the back (the view jerked) and turned the aim's
 * direction sideways and behind the camera (the reticle ran off one side
 * and came back from the other; a trace). With dual-stick aiming the yaw
 * lean's constants are 0 for the update, put back after it: the lean
 * settles at 0. The pitch's lean is the game's. */
#define LEAN_YAW_A 0x0035F840u
#define LEAN_YAW_B 0x0035F838u
extern void sub_0007B5D0_gen(void);
void sub_0007B5D0(void)
{
    static bool was_on;
    static Uint64 off_at;
    uint32_t player = MEM32(PLAYER), self = g_ecx, lean[2];
    float pitch, yaw;
    bool mine = player >= 0x10000u && player < 0x04000000u && self >= 0x10000u && self == MEM32(player + 0x3D0);

    if (!pdo_settings_get()->dual_stick || !mine) {
        if (was_on && mine) {
            /* Turned off in play: the aim and the camera back to the middle
             * of the quarter the game holds them in (its yaw limits, aim
             * block +0x94/+0x104), the pitch level, their velocities
             * cleared, so the game's clamps and springs start from where
             * they would be. */
            float lo = MEMF(player + 0xB0 + 0x94), hi = MEMF(player + 0xB0 + 0x104);
            float mid = lo == lo && hi == hi && hi > lo && hi - lo < 360.0f ? 0.5f * (lo + hi) : 0.0f;
            MEMF(player + 0xFC) = mid;
            MEMF(player + 0xF8) = 0.0f;
            MEMF(player + 0xD8) = 0.0f;
            MEMF(player + 0xDC) = 0.0f;
            MEMF(self + 0xC4) = mid;
            MEMF(self + 0xC0) = 0.0f;
            MEMF(self + 0x130) = 0.0f;
            MEMF(self + 0x134) = 0.0f;
            off_at = SDL_GetTicksNS();
            SDL_Log("dual-stick: off in play, the view back to %.1f", mid);
        }
        if (mine)
            was_on = false;
        sub_0007B5D0_gen();
        if (mine && off_at && SDL_GetTicksNS() - off_at < 4000000000ull && SDL_getenv("PDO_DUAL_TRACE")) {
            static Uint64 last;
            if (SDL_GetTicksNS() - last > 100000000ull) {   /* the first seconds after: what the game does */
                last = SDL_GetTicksNS();
                SDL_Log("dual off: aim %+.1f %+.1f camera %+.1f %+.1f (vel %+.2f %+.2f) lean %+.2f %+.2f "
                        "reticle %.0f %.0f turn %u", MEMF(player + 0xF8), MEMF(player + 0xFC), MEMF(self + 0xC0),
                        MEMF(self + 0xC4), MEMF(self + 0x130), MEMF(self + 0x134), MEMF(self + 0x230),
                        MEMF(self + 0x234), MEMF(player + 0xB8), MEMF(player + 0xBC), MEM32(player + 0x3D8));
            }
        }
        return;
    }
    was_on = true;
    pitch = MEMF(player + 0xF8);
    yaw = MEMF(player + 0xFC);
    if (pitch != pitch || yaw != yaw) {
        sub_0007B5D0_gen();
        return;
    }
    MEMF(self + 0xC0) = pitch;
    MEMF(self + 0xC4) = yaw;
    MEMF(self + 0x130) = 0.0f;
    MEMF(self + 0x134) = 0.0f;
    lean[0] = MEM32(LEAN_YAW_A);
    lean[1] = MEM32(LEAN_YAW_B);
    MEMF(LEAN_YAW_A) = 0.0f;
    MEMF(LEAN_YAW_B) = 0.0f;
    sub_0007B5D0_gen();
    MEM32(LEAN_YAW_A) = lean[0];
    MEM32(LEAN_YAW_B) = lean[1];
    MEMF(self + 0xC4) = yaw;
    MEMF(self + 0x130) = 0.0f;
    MEMF(self + 0x134) = 0.0f;
}

/* The aim update (0x0007E6A0, thiscall on the aim block, player+0xB0)
 * clamps the aim's yaw (block +0x4C) between block +0x94 and +0x104: the
 * view's quarter, about 50 degrees either way of it (a trace: held at
 * 50, the camera at 46). With dual-stick aiming the right stick turns past
 * them, and the clamp pulled the aim back each frame while the stick
 * pushed it on: the camera, on the aim, shook. While it is on, the update
 * runs without that clamp (the limits wide open for the call, put back
 * after it); the pitch's is kept, and the quarter turns (L/R) are the
 * game's.
 *
 * The left stick turns the aim there too: as direction bits in player
 * +0x3F8 (0x20000 right, 0x10000 left, 0x4000 and 0x8000 the pitch), which
 * ramp the turn rates (block +0x28 yaw, +0x2C pitch; player +0xD8/+0xDC).
 * With dual-stick aiming the left stick flies the dragon only: the bits are
 * cleared for the call and the rates zeroed, the bits put back after it
 * for whatever else reads them. */
#define AIM_DIRS 0x0003C000u
extern void sub_0007E6A0_gen(void);
void sub_0007E6A0(void)
{
    uint32_t player = MEM32(PLAYER), block = g_ecx, lo, hi, dirs;

    if (!pdo_settings_get()->dual_stick || player < 0x10000u || player >= 0x04000000u ||
        block != player + 0xB0u) {
        sub_0007E6A0_gen();
        return;
    }
    {   /* PDO_DUAL_TRACE=1: frames where the aim and the camera part, or
         * the reticle leaves the picture, and a few a second (at most 2000) */
        static int trace = -1, lines;
        uint32_t cam = MEM32(player + 0x3D0);
        if (trace < 0)
            trace = SDL_getenv("PDO_DUAL_TRACE") && *SDL_getenv("PDO_DUAL_TRACE") == '1';
        if (trace && lines < 2000 && cam >= 0x10000u && cam < 0x04000000u) {
            float dy = MEMF(player + 0xFC) - MEMF(cam + 0xC4), dp = MEMF(player + 0xF8) - MEMF(cam + 0xC0);
            float rx = MEMF(player + 0xB8), ry = MEMF(player + 0xBC);
            static Uint64 last;
            Uint64 now = SDL_GetTicksNS();
            bool off = SDL_fabsf(dy) > 20.0f || SDL_fabsf(dp) > 20.0f || rx < 0.0f || rx > 640.0f || ry < 0.0f ||
                       ry > 480.0f;
            if (off || now - last > 150000000ull) {
                /* also a few times a second: what may hold the view's
                 * quarter (the yaw limits, the turn state, the camera's
                 * own +0x10..+0x20 and +0x230..) */
                last = now;
                lines++;
                SDL_Log("dual frame%s: aim %+.1f %+.1f camera %+.1f %+.1f reticle %.0f %.0f | limits %+.1f %+.1f "
                        "turn %u flags %08x | cam10 %+.1f %+.1f %+.1f %+.1f %+.1f | cam230 %+.2f %+.2f %+.2f | "
                        "dragon %+.1f %+.1f %+.1f", off ? " OFF" : "",
                        MEMF(player + 0xF8), MEMF(player + 0xFC), MEMF(cam + 0xC0), MEMF(cam + 0xC4), rx, ry,
                        MEMF(block + 0x94), MEMF(block + 0x104), MEM32(player + 0x3D8), MEM32(player + 0x3F8),
                        MEMF(cam + 0x10), MEMF(cam + 0x14), MEMF(cam + 0x18), MEMF(cam + 0x1C), MEMF(cam + 0x20),
                        MEMF(cam + 0x230), MEMF(cam + 0x234), MEMF(cam + 0x238),
                        MEMF(player + 0x740), MEMF(player + 0x744), MEMF(player + 0x748));
            }
        }
    }
    lo = MEM32(block + 0x94);
    hi = MEM32(block + 0x104);
    dirs = MEM32(player + 0x3F8) & AIM_DIRS;
    MEMF(block + 0x94) = -1e9f;
    MEMF(block + 0x104) = 1e9f;
    MEM32(player + 0x3F8) &= ~AIM_DIRS;
    MEMF(block + 0x28) = 0.0f;
    MEMF(block + 0x2C) = 0.0f;
    sub_0007E6A0_gen();
    MEM32(block + 0x94) = lo;
    MEM32(block + 0x104) = hi;
    MEM32(player + 0x3F8) = (MEM32(player + 0x3F8) & ~AIM_DIRS) | dirs;
    MEMF(block + 0x28) = 0.0f;
    MEMF(block + 0x2C) = 0.0f;
}

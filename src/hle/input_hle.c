/*
 * Controller input, replaced at the XAPI level.
 *
 * The title links XAPI's XPP input stack, which talks to the Xbox's USB host
 * controller. There is none, so no controller ever appears. Its entry points
 * the game calls are replaced here and fed from SDL3: gamepads, plus keyboard
 * and mouse on port 0.
 *
 * Conventions as in dsound_hle.c: __stdcall, argument n at g_esp + 4 + 4n,
 * each pops what the original's `ret N` did (measured from the binary).
 *
 * Ports: port 0 is always connected (keyboard and mouse, and the first
 * gamepad); further gamepads take ports 1-3 in the order SDL reports them.
 *
 * Keyboard (port 0):
 *   WASD left stick     IJKL right stick     arrows d-pad
 *   Space A   Left Shift B   F X   R Y   Z White   C Black
 *   Q left trigger   E right trigger   Enter Start   Tab Back
 *   left mouse button A (so a click skips a logo)
 * Gamepad: Xbox layout by position; LB is White, RB is Black.
 */

#include "recomp_types.h"
#include "hle/program_va.h"
#include "input_map.h"
#include "config.h"

#include "guest/guest.h"
#include "xbox_memory_layout.h"
#include "gfx/overlay.h"
#include "gfx/renderer.h"
#ifdef __ANDROID__
#include "android/touch_controls.h"
#endif

#include <SDL3/SDL.h>
#include <string.h>

#define ARG(n)      MEM32(g_esp + 4 + 4 * (n))
#define RETURN(argbytes, v) do { g_eax = (uint32_t)(v); g_esp += 4 + (argbytes); return; } while (0)

/* XDEVICE_TYPE_GAMEPAD: the address of XAPI's gamepad device-type table in
 * this XBE. Every XGetDevices/XInputOpen for a controller passes it; other
 * types (memory units, voice) get no devices. The game also reads the
 * table's port masks itself (0x0029DA22), so they are kept current:
 * +0 connected now, +4 changed since the last XGetDevices, +8 connected at
 * the last XGetDevices. */
#define TYPE_GAMEPAD PROG_VA(0x003067CCu, 0x0027164Cu)
/* Orta's reticle object (docs/orta-facts.md, Input and the reticle); none in pd1 */
#define RETICLE PROG_VA(0x00472030u, 0)
/* The projection to the screen's focal lengths, x and y (0x00044C90); Orta's */
#define PROJ_FOCAL_X PROG_VA(0x0041A13Cu, 0)
#define PROJ_FOCAL_Y PROG_VA(0x0041A140u, 0)
#define TYPE_CURRENT  0
#define TYPE_CHANGED  4
#define TYPE_PREVIOUS 8

/* Controllers appear this long after XInitDevices, as a hot-plug insertion,
 * the way USB enumeration makes them appear on the console. The game picks
 * its player's controller from an insertion; one present from the very first
 * query is never taken up. */
#define ENUMERATION_MS 1000

#define PORTS 4

/* The handle XInputOpen returns is XPP's device record. Orta reaches it only
 * through these entry points, but it is guest memory laid out like the real
 * one all the same:
 *   +0x00 device object (its +0x04 bit 1: removed, in XDK 4928)
 *   +0x08 packet number   +0x14 last input report (id, length, XINPUT_GAMEPAD) */
#define REC_DEVICE   0x00
#define REC_PACKET   0x08
#define REC_REPORT   0x14
#define REC_SIZE     0xC0
#define DEV_FLAGS    0x04
#define DEV_REMOVED  0x02
#define DEV_SIZE     0x20

#define ERROR_SUCCESS            0u
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#define ERROR_IO_PENDING         997u

/* XINPUT_GAMEPAD.wButtons */
#define XB_DPAD_UP    0x0001
#define XB_DPAD_DOWN  0x0002
#define XB_DPAD_LEFT  0x0004
#define XB_DPAD_RIGHT 0x0008
#define XB_START      0x0010
#define XB_BACK       0x0020
#define XB_LTHUMB     0x0040
#define XB_RTHUMB     0x0080

/* XINPUT_GAMEPAD.bAnalogButtons[] */
enum { AB_A, AB_B, AB_X, AB_Y, AB_BLACK, AB_WHITE, AB_LTRIG, AB_RTRIG };

typedef struct pad_state {
    uint16_t buttons;
    uint8_t  analog[8];
    int16_t  thumb[4];      /* LX, LY, RX, RY; up is positive, as on Xbox */
} pad_state;

static SDL_Gamepad *pads[PORTS];
static uint32_t packet[PORTS];
static pad_state last[PORTS];
static uint32_t records[PORTS];     /* guest VA of each port's device record */
static uint64_t init_ms;            /* when the game called XInitDevices */
static bool ready;

/* ── SDL side ───────────────────────────────────────────────────────── */

static void input_init(void)
{
    if (ready)
        return;
    ready = true;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        SDL_Log("input: gamepads unavailable: %s", SDL_GetError());
}

/* Open gamepads that appeared and forget ones that went away. */
static void refresh_pads(void)
{
    SDL_JoystickID *ids;
    int count = 0;

    input_init();
    for (int p = 0; p < PORTS; p++)
        if (pads[p] && !SDL_GamepadConnected(pads[p])) {
            SDL_CloseGamepad(pads[p]);
            pads[p] = NULL;
        }
    ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) {
        bool open = false;
        for (int p = 0; p < PORTS; p++)
            if (pads[p] && SDL_GetGamepadID(pads[p]) == ids[i])
                open = true;
        for (int p = 0; !open && p < PORTS; p++)
            if (!pads[p]) {
                pads[p] = SDL_OpenGamepad(ids[i]);
                if (pads[p])
                    SDL_Log("input: %s on port %d", SDL_GetGamepadName(pads[p]), p + 1);
                open = true;
            }
    }
    SDL_free(ids);
}

static uint32_t connected_mask(void)
{
    uint32_t mask = 1u;     /* port 0: keyboard and mouse at least */
    for (int p = 1; p < PORTS; p++)
        if (pads[p])
            mask |= 1u << p;
    return mask;
}

/* PDO_INPUT_SCRIPT: scripted presses on port 1 for headless testing, as
 * "frame:button:frames,..." (frames counted from the first presented frame;
 * buttons a b x y black white lt rt start back up down left right). E.g.
 * "2700:start:6,2760:a:6" presses Start, then A a second later. */
#define SCRIPT_MAX 32
static struct { uint64_t at, len; char button[8]; } script[SCRIPT_MAX];
static int script_len = -1;

uint64_t pdo_gfx_frame_count(void);

static void load_script(void)
{
    const char *v = SDL_getenv("PDO_INPUT_SCRIPT");
    script_len = 0;
    while (v && *v && script_len < SCRIPT_MAX) {
        unsigned long long at, len;
        char button[8];
        if (SDL_sscanf(v, "%llu:%7[a-z]:%llu", &at, button, &len) != 3)
            break;
        script[script_len].at = at;
        script[script_len].len = len;
        SDL_strlcpy(script[script_len].button, button, sizeof button);
        script_len++;
        v = SDL_strchr(v, ',');
        if (v) v++;
    }
    if (script_len)
        SDL_Log("input: %d scripted presses", script_len);
}

static void apply_script(pad_state *s)
{
    static const struct { const char *name; uint16_t bit; int ab; } names[] = {
        { "a", 0, AB_A }, { "b", 0, AB_B }, { "x", 0, AB_X }, { "y", 0, AB_Y },
        { "black", 0, AB_BLACK }, { "white", 0, AB_WHITE },
        { "lt", 0, AB_LTRIG }, { "rt", 0, AB_RTRIG },
        { "start", XB_START, -1 }, { "back", XB_BACK, -1 },
        { "up", XB_DPAD_UP, -1 }, { "down", XB_DPAD_DOWN, -1 },
        { "left", XB_DPAD_LEFT, -1 }, { "right", XB_DPAD_RIGHT, -1 },
    };
    uint64_t now;

    if (script_len < 0)
        load_script();
    if (!script_len)
        return;
    now = pdo_gfx_frame_count();
    for (int i = 0; i < script_len; i++) {
        if (now < script[i].at || now >= script[i].at + script[i].len)
            continue;
        for (size_t j = 0; j < SDL_arraysize(names); j++)
            if (SDL_strcmp(names[j].name, script[i].button) == 0) {
                s->buttons |= names[j].bit;
                if (names[j].ab >= 0)
                    s->analog[names[j].ab] = 255;
            }
        /* The left stick, full over: lsl, lsr, lsu, lsd. */
        if (SDL_strncmp(script[i].button, "ls", 2) == 0) {
            char d = script[i].button[2];
            if (d == 'l') s->thumb[0] = -32767;
            if (d == 'r') s->thumb[0] = 32767;
            if (d == 'u') s->thumb[1] = 32767;
            if (d == 'd') s->thumb[1] = -32767;
        }
    }
}

static int16_t axis_from_keys(const bool *k, SDL_Scancode neg, SDL_Scancode pos)
{
    return (int16_t)((k[pos] ? 32767 : 0) - (k[neg] ? 32767 : 0));
}

/* PDO_AIM_SEARCH=1 (issue 51): which numbers the game keeps move with the
 * reticle. Each frame the floats of the player object (RETICLE's) and of
 * the objects it points to at +0x14, +0x3D4 and +0x3D0 (the reticle
 * update's own: 0x7B5D0, called from 0x41880) are sampled beside the
 * reticle's screen position (+0xB8/+0xBC); every 4 s those that follow it
 * most closely (correlation over the last 240 samples) are logged with
 * their scale and offset. The smoothed aim the reticle is drawn from should
 * follow it exactly. */
#define AIM_FIELDS 2560                 /* 0x1000 bytes of the player and of [+0x3D0], 0x400 of the others */
#define AIM_SAMPLES 240
static void aim_search(uint32_t obj)
{
    static float hist[AIM_SAMPLES][AIM_FIELDS], rx[AIM_SAMPLES], ry[AIM_SAMPLES];
    static uint32_t base[4];
    static int n, filled;
    static Uint64 last, last_report;
    Uint64 now = SDL_GetTicksNS();
    uint32_t b[4] = { obj, MEM32(obj + 0x14), MEM32(obj + 0x3D4), MEM32(obj + 0x3D0) };

    if (now - last < 15000000ull)       /* once a frame */
        return;
    last = now;
    for (int k = 0; k < 4; k++)
        if (b[k] != base[k]) {
            memcpy(base, b, sizeof base);
            filled = 0;
            n = 0;
        }
    for (int i = 0; i < AIM_FIELDS; i++) {
        uint32_t o = i < 1024 ? b[0] + i * 4u
                   : i < 1280 ? b[1] + (i - 1024) * 4u
                   : i < 1536 ? b[2] + (i - 1280) * 4u
                              : b[3] + (i - 1536) * 4u;
        float v = (o >= 0x10000u && o < 0x04000000u) ? MEMF(o) : 0.0f;
        hist[n][i] = v == v && v > -1e7f && v < 1e7f ? v : 0.0f;
    }
    rx[n] = MEMF(obj + 0xB8);
    ry[n] = MEMF(obj + 0xBC);
    n = (n + 1) % AIM_SAMPLES;
    if (filled < AIM_SAMPLES)
        filled++;
    if (filled < AIM_SAMPLES || now - last_report < 4000000000ull)
        return;
    last_report = now;
    for (int axis = 0; axis < 2; axis++) {
        const float *r = axis ? ry : rx;
        double mr = 0, vr = 0;
        struct { double c, slope, off; int i; } best[6] = { { 0 } };
        for (int t = 0; t < AIM_SAMPLES; t++)
            mr += r[t];
        mr /= AIM_SAMPLES;
        for (int t = 0; t < AIM_SAMPLES; t++)
            vr += (r[t] - mr) * (r[t] - mr);
        if (vr < 100.0)                 /* the reticle hardly moved */
            continue;
        for (int i = 0; i < AIM_FIELDS; i++) {
            double mf = 0, vf = 0, cov = 0, c;
            if ((i == 0xB8 / 4 || i == 0xBC / 4))
                continue;               /* the screen position itself */
            for (int t = 0; t < AIM_SAMPLES; t++)
                mf += hist[t][i];
            mf /= AIM_SAMPLES;
            for (int t = 0; t < AIM_SAMPLES; t++) {
                vf += (hist[t][i] - mf) * (hist[t][i] - mf);
                cov += (hist[t][i] - mf) * (r[t] - mr);
            }
            if (vf <= 1e-12)
                continue;
            c = cov / SDL_sqrt(vf * vr);
            for (int k = 0; k < 6; k++)
                if (SDL_fabs(c) > SDL_fabs(best[k].c)) {
                    memmove(&best[k + 1], &best[k], (5 - k) * sizeof best[0]);
                    best[k].c = c;
                    best[k].slope = cov / vf;           /* reticle pixels a unit of the field */
                    best[k].off = mr - best[k].slope * mf;
                    best[k].i = i;
                    break;
                }
        }
        for (int k = 0; k < 6 && best[k].c != 0; k++) {
            int i = best[k].i;
            SDL_Log("aim search: reticle %s follows %s+0x%03X (correlation %+.4f, %.3f px a unit, at 0: %.1f, now %.4f)",
                    axis ? "y" : "x",
                    i < 1024 ? "player" : i < 1280 ? "[player+0x14]" : i < 1536 ? "[player+0x3D4]" : "[player+0x3D0]",
                    (i < 1024 ? i : i < 1280 ? i - 1024 : i < 1536 ? i - 1280 : i - 1536) * 4, best[k].c,
                    best[k].slope, best[k].off,
                    hist[(n + AIM_SAMPLES - 1) % AIM_SAMPLES][i]);
        }
    }
}

/* Dual-stick aiming (Controls > Dual stk): in play the right stick turns
 * the aim angles (player +0xF8 pitch, +0xFC yaw, degrees; pitch grows
 * downwards) at up to YAW_RATE / PITCH_RATE degrees a second, and the
 * camera is held on the aim (src/guest/mouse_aim.c), so the view turns
 * with it all the way round, as in modern games. The right stick is not
 * passed on in play, the right trigger is A there (fire) and the left Y
 * (the dragon's form); the right stick turns the view, so the triggers'
 * quarter turns are not needed. The left stick
 * flies the dragon only (src/guest/mouse_aim.c). Pitch is held
 * to 40 degrees either way, where the camera stops (past it the reticle
 * left the screen). Outside play
 * (menus, pd1: no player object) nothing changes. Real time, not the
 * game's reads, so the speed is the same at any frame rate. */
static void dual_stick(pad_state *s)
{
    const float YAW_RATE = 200.0f, PITCH_RATE = 120.0f, DEAD = 0.15f, PITCH_MAX = 40.0f;
    static Uint64 last;
    Uint64 now = SDL_GetTicksNS();
    float dt = last ? (float)(now - last) * 1e-9f : 0.0f;
    uint32_t obj = RETICLE ? MEM32(RETICLE) : 0;
    float x, y, m, k, pitch, yaw;

    last = now;
    if (!pdo_settings_get()->dual_stick || obj < 0x10000u || obj >= 0x04000000u)
        return;
    if (s->analog[AB_RTRIG] > s->analog[AB_A])
        s->analog[AB_A] = s->analog[AB_RTRIG];  /* the right trigger shoots (A) */
    s->analog[AB_RTRIG] = 0;
    if (s->analog[AB_LTRIG] > s->analog[AB_Y])
        s->analog[AB_Y] = s->analog[AB_LTRIG];  /* the left trigger morphs (Y) */
    s->analog[AB_LTRIG] = 0;
    x = (float)s->thumb[2] / 32767.0f;
    y = (float)s->thumb[3] / 32767.0f;
    s->thumb[2] = s->thumb[3] = 0;
    {   /* PDO_DUAL_TRACE=1: the aim and the camera, a few lines a second */
        static int trace = -1;
        static Uint64 last_trace;
        uint32_t cam = MEM32(obj + 0x3D0);
        if (trace < 0)
            trace = SDL_getenv("PDO_DUAL_TRACE") && *SDL_getenv("PDO_DUAL_TRACE") == '1';
        if (trace && now - last_trace > 200000000ull && cam >= 0x10000u && cam < 0x04000000u) {
            last_trace = now;
            SDL_Log("dual: right %+.2f %+.2f left %+6d %+6d, aim pitch %+.1f yaw %+.1f, camera %+.1f %+.1f, "
                    "rates %+.2f %+.2f, reticle %.0f %.0f", x, y, s->thumb[0], s->thumb[1], MEMF(obj + 0xF8),
                    MEMF(obj + 0xFC), MEMF(cam + 0xC0), MEMF(cam + 0xC4), MEMF(obj + 0xDC), MEMF(obj + 0xD8),
                    MEMF(obj + 0xB8), MEMF(obj + 0xBC));
        }
    }
    m = SDL_sqrtf(x * x + y * y);
    if (m <= DEAD || dt <= 0.0f)
        return;
    if (dt > 0.05f)
        dt = 0.05f;                     /* after a pause: no jump */
    k = (m > 1.0f ? 1.0f : (m - DEAD) / (1.0f - DEAD));
    k = k * k / m;                      /* squared past the dead zone: fine aim near the centre */
    pitch = MEMF(obj + 0xF8);
    yaw = MEMF(obj + 0xFC);
    if (pitch != pitch || yaw != yaw || SDL_fabsf(yaw) > 1e5f)
        return;
    yaw += x * k * YAW_RATE * dt;
    pitch -= y * k * PITCH_RATE * dt;   /* stick up: look up */
    if (yaw > 180.0f || yaw < -180.0f)  /* -180..180, as the game keeps it */
        yaw -= SDL_floorf((yaw + 180.0f) / 360.0f) * 360.0f;
    MEMF(obj + 0xFC) = yaw;
    MEMF(obj + 0xF8) = SDL_clamp(pitch, -PITCH_MAX, PITCH_MAX);
}

static int16_t clamp16(int v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static void read_port(int port, pad_state *s)
{
    SDL_Gamepad *g = pads[port];

    memset(s, 0, sizeof *s);
    {   /* The Xbox buttons, from their bindings (input_map.c): the
         * controller on every port, the keyboard on the first. */
        static const struct { uint16_t bit; int ab; } xb[PDO_IN_COUNT] = {
            [PDO_IN_A] = { 0, AB_A }, [PDO_IN_B] = { 0, AB_B }, [PDO_IN_X] = { 0, AB_X },
            [PDO_IN_Y] = { 0, AB_Y }, [PDO_IN_BLACK] = { 0, AB_BLACK }, [PDO_IN_WHITE] = { 0, AB_WHITE },
            [PDO_IN_LTRIG] = { 0, AB_LTRIG }, [PDO_IN_RTRIG] = { 0, AB_RTRIG },
            [PDO_IN_START] = { XB_START, -1 }, [PDO_IN_BACK] = { XB_BACK, -1 },
            [PDO_IN_UP] = { XB_DPAD_UP, -1 }, [PDO_IN_DOWN] = { XB_DPAD_DOWN, -1 },
            [PDO_IN_LEFT] = { XB_DPAD_LEFT, -1 }, [PDO_IN_RIGHT] = { XB_DPAD_RIGHT, -1 },
            [PDO_IN_LTHUMB] = { XB_LTHUMB, -1 }, [PDO_IN_RTHUMB] = { XB_RTHUMB, -1 },
        };
        const pdo_binding *bind = pdo_input_bindings();
        const bool *k = port == 0 ? SDL_GetKeyboardState(NULL) : NULL;
        /* With Alt held, Enter is the fullscreen toggle (renderer.c), not a button. */
        bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
        SDL_MouseButtonFlags mb = k ? pdo_mouse_buttons() : 0;
        for (int i = 0; i < PDO_IN_COUNT; i++) {
            int level = pdo_input_pad_level(g, bind[i].pad);
            if (bind[i].mouse && (mb & SDL_BUTTON_MASK(bind[i].mouse)))
                level = 255;
            if (k && bind[i].key != SDL_SCANCODE_UNKNOWN && k[bind[i].key] &&
                !(alt && bind[i].key == SDL_SCANCODE_RETURN))
                level = 255;
            if (xb[i].ab >= 0) {
                if (level > s->analog[xb[i].ab])
                    s->analog[xb[i].ab] = (uint8_t)level;
            } else if (level > 128) {
                s->buttons |= xb[i].bit;
            }
        }
    }
    if (g) {
        /* SDL's Y axes grow downward, Xbox's upward. Negating -32768 would
         * overflow, so clamp. */
        s->thumb[0] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
        s->thumb[1] = clamp16(-(int)SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
        s->thumb[2] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
        s->thumb[3] = clamp16(-(int)SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
    }

    if (port == 0) {
        const bool *k = SDL_GetKeyboardState(NULL);
        int16_t lx, ly, rx, ry;

        /* Keys win over a resting stick, not over a held one. */
        lx = axis_from_keys(k, SDL_SCANCODE_A, SDL_SCANCODE_D);
        ly = axis_from_keys(k, SDL_SCANCODE_S, SDL_SCANCODE_W);
        rx = axis_from_keys(k, SDL_SCANCODE_J, SDL_SCANCODE_L);
        ry = axis_from_keys(k, SDL_SCANCODE_K, SDL_SCANCODE_I);
        if (lx) s->thumb[0] = lx;
        if (ly) s->thumb[1] = ly;
        {   /* Mouse aim (issue 51). Orta's aim is a pair of angles, player
             * +0xF8 (pitch) and +0xFC (yaw), in degrees: each frame the
             * reticle update (0x0007E6A0, on the aim block at player
             * +0xB0) ramps a rate (+0xDC pitch, +0xD8 yaw) towards what
             * the stick asks, adds it to the angles and clamps them around
             * where the camera points; the camera then turns after the aim,
             * and the reticle is drawn where the aim is from the camera
             * (docs/orta-facts.md). With a stick that is the console's
             * glide; with the mouse, as in any mouse-look, the mouse turns
             * the angles themselves: the stick is left at rest, the rates
             * are cleared, and the game's own clamp keeps the reticle on
             * the screen. The reticle moves as far as the mouse does and
             * stops with it; the camera follows as ever. Outside play (no
             * aim object: menus, pd1) the motion tilts the stick as it
             * moves, keeping its direction. A controller's stick, the
             * keys, Start or Back hand aiming back to them.
             *
             * Relative: each frame's motion is applied half then and half
             * the next frame (the motion arrives unevenly between the
             * game's 60 reads a second: steps of uneven size), all of it,
             * one frame late. Lightgun: the reticle is steered to where the
             * pointer is on the picture: each frame the aim turns by the
             * angle between the reticle and the pointer, from the game's
             * own projection (0x00044C90: its focal lengths), a share of it
             * so a few frames land it; the game's clamp keeps it within the
             * reticle's range, short of the picture's edges. */
            const float DEGREES_PER_COUNT = 0.05f;     /* at 100 %: about 0.2 reticle pixels */
            const float LIGHTGUN_SHARE = 0.6f;         /* of the angle to the pointer, a frame */
            static bool aiming;
            static float carry_x, carry_y;              /* the half of the last motion still to apply */
            static float last_gx = -1e9f, last_gy = -1e9f;
            int mode = pdo_settings_get()->mouse_aim;
            float gx = 0.0f, gy = 0.0f;
            bool pointer = mode == PDO_MOUSE_LIGHTGUN && pdo_gfx_pointer_on_game(&gx, &gy);
            float dx, dy, ex, ey, len, speed = (float)pdo_settings_get()->mouse_speed / 100.0f;
            bool stick = s->thumb[0] > 8000 || s->thumb[0] < -8000 || s->thumb[1] > 8000 || s->thumb[1] < -8000;
            uint32_t obj = RETICLE ? MEM32(RETICLE) : 0;
            bool play = obj >= 0x10000u && obj < 0x04000000u;

            pdo_mouse_take(&dx, &dy);
            if (!play || stick || lx || ly || (s->buttons & (XB_START | XB_BACK))) {
                aiming = false;
                carry_x = carry_y = 0.0f;
            }
            bool moved = dx != 0.0f || dy != 0.0f;
            if (pointer && (gx != last_gx || gy != last_gy)) {
                moved = last_gx > -1e8f;        /* the pointer moved: lightgun aiming */
                last_gx = gx;
                last_gy = gy;
            }
            if (play && !stick && !lx && !ly && mode != PDO_MOUSE_OFF) {
                if (moved)
                    aiming = true;
                if (aiming) {
                    float pitch = MEMF(obj + 0xF8), yaw = MEMF(obj + 0xFC);
                    if (pitch == pitch && yaw == yaw) {         /* not NaN */
                        if (mode == PDO_MOUSE_LIGHTGUN) {
                            float rx_ = MEMF(obj + 0xB8), ry_ = MEMF(obj + 0xBC);
                            float fx = SDL_fabsf(MEMF(PROJ_FOCAL_X)), fy = SDL_fabsf(MEMF(PROJ_FOCAL_Y));
                            if (!(fx > 1.0f && fx < 1e5f))
                                fx = 300.0f;            /* not read: about the game's */
                            if (!(fy > 1.0f && fy < 1e5f))
                                fy = 300.0f;
                            if (pointer && rx_ == rx_ && ry_ == ry_ && gx >= 0.0f && gy >= 0.0f &&
                                gx <= 1000.0f && gy <= 1000.0f) {
                                yaw += LIGHTGUN_SHARE * SDL_atanf((gx - rx_) / fx) * 57.29578f;
                                pitch += LIGHTGUN_SHARE * SDL_atanf((gy - ry_) / fy) * 57.29578f;
                            }
                        } else {
                            float ax = 0.5f * (dx + carry_x), ay = 0.5f * (dy + carry_y);
                            carry_x = dx;
                            carry_y = dy;
                            yaw += ax * speed * DEGREES_PER_COUNT;
                            pitch += ay * speed * DEGREES_PER_COUNT;
                        }
                        MEMF(obj + 0xFC) = yaw;
                        MEMF(obj + 0xF8) = pitch;
                        MEMF(obj + 0xD8) = 0.0f;                /* the stick's rates */
                        MEMF(obj + 0xDC) = 0.0f;
                    }
                    s->thumb[0] = 0;
                    s->thumb[1] = 0;
                }
            } else if (dx != 0.0f || dy != 0.0f) {
                ex = dx * speed / 15.0f;        /* full over at 15 pixels a frame at 100 % */
                ey = -dy * speed / 15.0f;
                len = SDL_sqrtf(ex * ex + ey * ey);
                if (len > 1.0f) {
                    ex /= len;
                    ey /= len;
                }
                s->thumb[0] = clamp16((int)(ex * 32767.0f));
                s->thumb[1] = clamp16((int)(ey * 32767.0f));
            }
            if (play && SDL_getenv("PDO_WATCH_RETICLE")) {
                /* the player object's 8 bytes at the given offset (1: the
                 * reticle's screen position, +0xB8; 0xF8: the aim angles) */
                uint32_t at = (uint32_t)SDL_strtoul(SDL_getenv("PDO_WATCH_RETICLE"), NULL, 0);
                pdo_debug_watch_writes((void *)XBOX_PTR(obj + (at >= 4 && at < 0x2000 ? at : 0xB8u)), 8);
            }
            if (play && SDL_getenv("PDO_AIM_SEARCH"))
                aim_search(obj);
            {   /* PDO_MOUSE_TRACE=1: what mouse aim does, a few lines a second */
                static int trace = -1;
                static Uint64 last_trace, rate_since;
                static int polls, rate;
                Uint64 tnow = SDL_GetTicksNS();
                polls++;                    /* how often the game reads its controller */
                if (tnow - rate_since >= 1000000000ull) {
                    rate = polls;
                    polls = 0;
                    rate_since = tnow;
                }
                if (trace < 0) {
                    const char *e = SDL_getenv("PDO_MOUSE_TRACE");
                    trace = e && *e == '1';
                }
                if (trace && (dx != 0.0f || dy != 0.0f || play) && SDL_GetTicksNS() - last_trace > 250000000ull) {
                    last_trace = SDL_GetTicksNS();
                    SDL_Log("mouse: %d reads a second, motion %+.0f %+.0f, stick %+6d %+6d, aim %+.2f %+.2f%s, "
                            "reticle %.1f %.1f%s", rate,
                            dx, dy, s->thumb[0], s->thumb[1], play ? MEMF(obj + 0xF8) : 0.0f,
                            play ? MEMF(obj + 0xFC) : 0.0f, aiming ? " (mouse)" : "", play ? MEMF(obj + 0xB8) : 0.0f,
                            play ? MEMF(obj + 0xBC) : 0.0f, play ? "" : " (no reticle: tilting as it moves)");
                }
            }
        }
        if (rx) s->thumb[2] = rx;
        if (ry) s->thumb[3] = ry;
#ifdef __ANDROID__
        /* The on-screen controls, as the first controller too (src/android/). */
        pdo_touch_add(&s->buttons, s->analog, s->thumb);
#endif
        dual_stick(s);
        apply_script(s);
    }
    {   /* The options overlay keeps the game's input while it is open. */
        bool any = s->buttons != 0;
        for (int i = 0; i < 8; i++)
            any |= s->analog[i] > 30;
        for (int i = 0; i < 4; i++)
            any |= s->thumb[i] > 8000 || s->thumb[i] < -8000;
        if (pdo_overlay_blocks_input(any))
            memset(s, 0, sizeof *s);
    }
}

/* A new press of A or Start on any port (keyboard and mouse on the first):
 * the logo skip (src/guest/intro_skip.c). Called once a frame while the
 * logos show; true only on the frame the press begins. */
bool pdo_input_skip_press(void)
{
    static bool held;
    bool now = false;

    input_init();
    for (int p = 0; p < PORTS; p++) {
        pad_state s;
        if (p > 0 && !pads[p])
            continue;
        read_port(p, &s);
        if ((s.buttons & XB_START) || s.analog[AB_A] > 0x40)
            now = true;
    }
    if (now && !held) {
        held = true;
        return true;
    }
    held = now;
    return false;
}

static uint32_t visible_mask(void)
{
    if (!init_ms)
        init_ms = SDL_GetTicks();
    return SDL_GetTicks() - init_ms >= ENUMERATION_MS ? connected_mask() : 0;
}

/* Bring the device-type table's masks up to date; returns the current one. */
static uint32_t update_type(void)
{
    uint32_t now, before;

    refresh_pads();
    now = visible_mask();
    before = MEM32(TYPE_GAMEPAD + TYPE_CURRENT);
    MEM32(TYPE_GAMEPAD + TYPE_CHANGED) |= before ^ now;
    MEM32(TYPE_GAMEPAD + TYPE_CURRENT) = now;
    for (int p = 0; p < PORTS; p++)
        if (records[p])
            MEM8(MEM32(records[p] + REC_DEVICE) + DEV_FLAGS) = (now & (1u << p)) ? 0 : DEV_REMOVED;
    return now;
}

static int port_of(uint32_t handle)
{
    for (int p = 0; p < PORTS; p++)
        if (handle && handle == records[p])
            return (MEM32(TYPE_GAMEPAD + TYPE_CURRENT) & (1u << p)) ? p : -1;
    return -1;
}

/* ── XAPI entry points ──────────────────────────────────────────────── */

/* VOID XInitDevices(DWORD, PXDEVICE_PREALLOC_TYPE): the original still runs
 * (it sets up XAPI's tables); we only note the time. */
void sub_003068D4_enter(void)
{
    init_ms = SDL_GetTicks();
}

/* DWORD XGetDevices(PXPP_DEVICE_TYPE DeviceType). Orta does not link
 * XGetDeviceChanges: it compares successive XGetDevices masks itself
 * (0x0029D4F0). */
void sub_003075B4(void)
{
    uint32_t now;

    if (ARG(0) != TYPE_GAMEPAD)
        RETURN(4, 0);
    now = update_type();
    MEM32(TYPE_GAMEPAD + TYPE_PREVIOUS) = now;
    MEM32(TYPE_GAMEPAD + TYPE_CHANGED) = 0;
    RETURN(4, now);
}

/* HANDLE XInputOpen(PXPP_DEVICE_TYPE, DWORD dwPort, DWORD dwSlot,
 *                   PXINPUT_POLLING_PARAMETERS) */
void sub_0030C55D(void)
{
    uint32_t type = ARG(0), port = ARG(1);

    if (type != TYPE_GAMEPAD || port >= PORTS || !(update_type() & (1u << port)))
        RETURN(16, 0);
    if (!records[port]) {
        uint32_t rec = xbox_HeapAlloc(REC_SIZE, 16), dev = xbox_HeapAlloc(DEV_SIZE, 16);
        if (!rec || !dev)
            RETURN(16, 0);
        memset((void *)XBOX_PTR(rec), 0, REC_SIZE);
        memset((void *)XBOX_PTR(dev), 0, DEV_SIZE);
        MEM32(rec + REC_DEVICE) = dev;
        records[port] = rec;
    }
    MEM8(MEM32(records[port] + REC_DEVICE) + DEV_FLAGS) = 0;
    SDL_Log("input: game opened controller port %u", port + 1);
    RETURN(16, records[port]);
}

/* VOID XInputClose(HANDLE) */
void sub_0030C5B3(void)
{
    RETURN(4, 0);
}

/* DWORD XInputGetCapabilities(HANDLE, PXINPUT_CAPABILITIES)
 * { BYTE SubType; WORD Reserved; XINPUT_GAMEPAD In; XINPUT_RUMBLE Out; },
 * byte-packed: 25 bytes, In at +3, Out at +0x15. Writing it with natural
 * alignment (26 bytes) runs into the next heap block. */
void sub_0030C5BF(void)
{
    uint32_t caps = ARG(1);
    int port = port_of(ARG(0));

    if (port < 0)
        RETURN(8, ERROR_DEVICE_NOT_CONNECTED);
    if (caps) {
        MEM8(caps + 0) = 1;                 /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
        MEM8(caps + 1) = 0;
        MEM8(caps + 2) = 0;
        MEM8(caps + 3) = 0xFF;              /* every digital button */
        MEM8(caps + 4) = 0x00;
        for (int i = 0; i < 8; i++)
            MEM8(caps + 5 + i) = 0xFF;      /* every analog button, full range */
        for (int i = 0; i < 8; i++)
            MEM8(caps + 13 + i) = 0xFF;     /* both sticks, full range */
        for (int i = 0; i < 4; i++)
            MEM8(caps + 0x15 + i) = 0xFF;   /* both rumble motors */
    }
    RETURN(8, ERROR_SUCCESS);
}

/* DWORD XInputGetState(HANDLE, PXINPUT_STATE)
 * { DWORD dwPacketNumber; WORD wButtons; BYTE bAnalogButtons[8];
 *   SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } */
void sub_0030C79D(void)
{
    uint32_t st = ARG(1);
    int port = port_of(ARG(0));
    pad_state s;

    update_type();
    if (port < 0)
        RETURN(8, ERROR_DEVICE_NOT_CONNECTED);
    read_port(port, &s);
    /* The packet number tells the game whether anything changed. */
    if (memcmp(&s, &last[port], sizeof s) != 0) {
        last[port] = s;
        packet[port]++;
    }
    {   /* The device record keeps the last report, as XPP's does. */
        uint32_t r = records[port] + REC_REPORT;
        MEM32(records[port] + REC_PACKET) = packet[port];
        MEM8(r + 0) = 0;
        MEM8(r + 1) = 20;
        MEM16(r + 2) = s.buttons;
        for (int i = 0; i < 8; i++)
            MEM8(r + 4 + i) = s.analog[i];
        for (int i = 0; i < 4; i++)
            MEM16(r + 12 + 2 * i) = (uint16_t)s.thumb[i];
    }
    if (st) {
        MEM32(st + 0) = packet[port];
        MEM16(st + 4) = s.buttons;
        for (int i = 0; i < 8; i++)
            MEM8(st + 6 + i) = s.analog[i];
        for (int i = 0; i < 4; i++)
            MEM16(st + 14 + 2 * i) = (uint16_t)s.thumb[i];
    }
    RETURN(8, ERROR_SUCCESS);
}

/* DWORD XInputSetState(HANDLE, PXINPUT_FEEDBACK)
 * Header { DWORD dwStatus; HANDLE hEvent; BYTE Reserved[58]; } then
 * XINPUT_RUMBLE { WORD wLeftMotorSpeed, wRightMotorSpeed; } at +66.
 * On hardware this completes later: it returns ERROR_IO_PENDING and the
 * status word moves off ERROR_IO_PENDING when the report is sent. Ours is
 * sent at once, so the status is final before we return. */
void sub_0030C809(void)
{
    uint32_t fb = ARG(1);
    int port = port_of(ARG(0));
    static bool first = true;

    if (first) {
        /* Once: that the game asks for vibration at all, and where it goes. */
        first = false;
        SDL_Log("input: first XInputSetState, handle 0x%08x -> %s%d (%u, %u)%s", ARG(0),
                port < 0 ? "no port " : "port ", port < 0 ? 0 : port + 1, fb ? MEM16(fb + 66) : 0,
                fb ? MEM16(fb + 68) : 0, port >= 0 && !pads[port] ? ", no controller open there" : "");
    }
    if (port < 0)
        RETURN(8, ERROR_DEVICE_NOT_CONNECTED);
    if (fb) {
        if (pads[port]) {
            /* Vibration strength (Options > Gameplay), 0-100 %. */
            uint32_t k = (uint32_t)pdo_settings_get()->rumble;
            uint16_t lo = (uint16_t)(MEM16(fb + 66) * k / 100), hi = (uint16_t)(MEM16(fb + 68) * k / 100);
            bool ok = SDL_RumbleGamepad(pads[port], lo, hi, 1000);
            static bool logged;
            if ((lo || hi || !ok) && !logged) {
                logged = true;
                SDL_Log("input: rumble on port %d (%u, %u)%s%s", port + 1, lo, hi,
                        ok ? "" : ": not supported, ", ok ? "" : SDL_GetError());
            }
        }
        MEM32(fb + 0) = ERROR_SUCCESS;
        static bool noted;
        if (MEM32(fb + 4) && !noted) {
            noted = true;
            SDL_Log("input: XInputSetState with a completion event (not signalled)");
        }
    }
    RETURN(8, ERROR_IO_PENDING);
}

/* DWORD XInputPoll(HANDLE): only for manual polling; state is always current. */
void sub_0030C83C(void)
{
    RETURN(4, port_of(ARG(0)) < 0 ? ERROR_DEVICE_NOT_CONNECTED : ERROR_SUCCESS);
}

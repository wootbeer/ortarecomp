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
#include "input_map.h"
#include "config.h"

#include "guest/guest.h"
#include "xbox_memory_layout.h"
#include "gfx/overlay.h"

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
#define TYPE_GAMEPAD 0x003067CCu
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
        {   /* Mouse aim: the mouse's motion this frame tilts the left stick,
             * full over at about 15 pixels a frame at speed 100 %, so the
             * game's own aiming (its speed, limits, camera follow) moves the
             * reticle. A still mouse leaves the stick to the controller. */
            float dx, dy, k = (float)pdo_settings_get()->mouse_speed / 100.0f * (32767.0f / 15.0f);
            pdo_mouse_take(&dx, &dy);
            if (dx != 0.0f)
                s->thumb[0] = clamp16((int)(dx * k));
            if (dy != 0.0f)
                s->thumb[1] = clamp16((int)(-dy * k));
        }
        if (rx) s->thumb[2] = rx;
        if (ry) s->thumb[3] = ry;
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

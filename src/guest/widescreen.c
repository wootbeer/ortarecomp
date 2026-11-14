/*
 * Widescreen: Panzer Dragoon Orta's own 16:9 mode, and 21:9.
 *
 * The display setup (0x0003D020) asks the game's display object (at
 * 0x0041A06C) for its video mode, query 15, and with bit 8 of the answer
 * renders anamorphic 16:9: the pixel aspect it keeps at 0x00342724 times
 * 4/3, and a flag at 0x00342754 that the camera and the screen layout read
 * (the HUD's round radar comes out narrow, to be stretched back). Our
 * kernel's video flags never set that bit, so the game stayed 4:3. With
 * the aspect setting at 16:9 or 21:9 we set it, and the window pass shows
 * the frame at that shape (renderer.c, present_to_window).
 *
 * 21:9 widens the view further after the setup (sub_0003D020 below); the
 * HUD is squeezed back and the cutscene bars dropped in d3d_hle.c
 * (hud_squeeze, cutscene_bars). 16:9 and 21:9 switch at the next frame;
 * 4:3 and widescreen only at the next start, as the game sets that up
 * once.
 */

#include <SDL3/SDL.h>
#include <string.h>

#include "recomp_types.h"
#include "config.h"

static bool active;     /* the game was told widescreen, at its setup */
static bool set_up;     /* its display setup has run: 4:3 or widescreen is fixed from here */
static float view_169;  /* the view's aspect value the game chose for 16:9 (0x00342724) */
static int view_aspect = PDO_ASPECT_16_9;   /* what the view is set to now: 16:9 or 21:9 */

bool pdo_widescreen_active(void)
{
    return active;
}

bool pdo_widescreen_needs_restart(void)
{
    return set_up && active != (pdo_settings_get()->aspect != PDO_ASPECT_4_3);
}

/* The shape the game's picture is meant to be shown at: what its view is
 * set to, not the menu's choice, which can run ahead of it until the next
 * frame (16:9 and 21:9) or the next start (4:3 and widescreen). */
float pdo_widescreen_ratio(void)
{
    if (!active)
        return 4.0f / 3.0f;
    return view_aspect == PDO_ASPECT_21_9 ? 64.0f / 27.0f : 16.0f / 9.0f;
}

/* The display object's query (this, id, arg; ret 0xC): id 15 returns its
 * video mode (low byte: 1 or 2, the pixel aspect; bit 8: widescreen).
 * Once the setup has run the answer stays what it was then, so a later
 * query cannot half-switch the game. */
extern void sub_00281330_gen(void);
void sub_00281330(void)
{
    uint32_t id = MEM32(g_esp + 8);

    sub_00281330_gen();
    if (id == 15 && (set_up ? active : pdo_settings_get()->aspect != PDO_ASPECT_4_3)) {
        g_eax |= 0x100;
        if (!active)
            SDL_Log("widescreen: the game renders 16:9 (anamorphic)");
        active = true;
    }
}

/* Sets the view's aspect value and hands it to the display object (method
 * +0x208, this and the value, which pops its arguments; 0x0003D184), which
 * sets the camera's projection; 0x00044380 and 0x000671E0 read the value
 * again later. Game thread only. */
static void set_view_aspect(float v)
{
    uint32_t disp = MEM32(0x0041A06C), fn_va;
    recomp_func_t fn;

    MEMF(0x00342724) = v;
    fn_va = disp ? MEM32(MEM32(disp) + 0x208) : 0;
    fn = fn_va ? recomp_lookup_manual(fn_va) : NULL;
    if (fn_va && !fn)
        fn = recomp_lookup(fn_va);
    if (fn) {
        uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
        uint32_t bits;
        memcpy(&bits, &v, 4);
        g_esp -= 4;
        MEM32(g_esp) = bits;
        g_esp -= 4;
        MEM32(g_esp) = disp;
        g_esp -= 4;
        MEM32(g_esp) = 0;       /* return address; the callee pops it and its arguments */
        fn();
        g_eax = eax, g_ecx = ecx, g_edx = edx;
    }
}

static void apply_view_aspect(int aspect)
{
    float v = view_169;

    if (aspect == PDO_ASPECT_21_9)
        v *= (64.0f / 27.0f) / (16.0f / 9.0f);
    set_view_aspect(v);
    view_aspect = aspect;
    SDL_Log("widescreen: %s, the view's aspect value %g", aspect == PDO_ASPECT_21_9 ? "21:9" : "16:9", v);
}

/* The display setup (0x0003D020, once at start): with bit 8 it keeps the
 * pixel aspect times 4/3 at 0x00342724 -- the 16:9 view's width over
 * 4:3's -- and hands it to the display object. For 21:9 the view is wider
 * by (64/27) / (16/9): the value is scaled after the setup and handed over
 * again. */
extern void sub_0003D020_gen(void);
void sub_0003D020(void)
{
    sub_0003D020_gen();
    set_up = true;
    if (!active)
        return;
    view_169 = MEMF(0x00342724);
    view_aspect = PDO_ASPECT_16_9;
    if (pdo_settings_get()->aspect == PDO_ASPECT_21_9)
        apply_view_aspect(PDO_ASPECT_21_9);
}

/* Each frame (from Swap, on the game thread): the menu's 16:9 or 21:9,
 * when it differs from the view's. 4:3 in a widescreen game (or the other
 * way) waits for a restart: the game lays its screen out for one or the
 * other at its setup. */
void pdo_widescreen_frame(void)
{
    int want = pdo_settings_get()->aspect;

    if (!set_up || !active || want == PDO_ASPECT_4_3 || want == view_aspect)
        return;
    apply_view_aspect(want);
}

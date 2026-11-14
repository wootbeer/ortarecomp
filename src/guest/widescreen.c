/*
 * Widescreen: Panzer Dragoon Orta's own 16:9 mode.
 *
 * The display setup (0x0003D020) asks the game's display object (at
 * 0x0041A06C) for its video mode, query 15, and with bit 8 of the answer
 * renders anamorphic 16:9: the pixel aspect it keeps at 0x00342724 times
 * 4/3, and a flag at 0x00342754 that the camera and the screen layout read
 * (the HUD's round radar comes out narrow, to be stretched back). Our
 * kernel's video flags never set that bit, so the game stayed 4:3. With
 * the aspect setting at 16:9 we set it, and the window pass shows the
 * frame at 16:9 (renderer.c, present_to_window).
 */

#include <SDL3/SDL.h>
#include <string.h>

#include "recomp_types.h"
#include "config.h"

static bool active;     /* the game was told widescreen, at its setup */

bool pdo_widescreen_active(void)
{
    return active;
}

float pdo_widescreen_ratio(void)
{
    if (!active)
        return 4.0f / 3.0f;
    return pdo_settings_get()->aspect == PDO_ASPECT_21_9 ? 64.0f / 27.0f : 16.0f / 9.0f;
}

/* The display object's query (this, id, arg; ret 0xC): id 15 returns its
 * video mode (low byte: 1 or 2, the pixel aspect; bit 8: widescreen). */
extern void sub_00281330_gen(void);
void sub_00281330(void)
{
    uint32_t id = MEM32(g_esp + 8);

    sub_00281330_gen();
    if (id == 15 && pdo_settings_get()->aspect != PDO_ASPECT_4_3) {
        g_eax |= 0x100;
        if (!active)
            SDL_Log("widescreen: the game renders 16:9 (anamorphic)");
        active = true;
    }
}

/* The display setup (0x0003D020, once at start): with bit 8 it keeps the
 * pixel aspect times 4/3 at 0x00342724 -- the 16:9 view's width over
 * 4:3's -- and hands it to the display object (method +0x208, this and the
 * value, which pops its arguments; 0x0003D184), which sets the camera's
 * projection; 0x00044380 and 0x000671E0 read it again later. For 21:9 the
 * view is wider by (64/27) / (16/9): the value is scaled after the setup
 * and handed over again. */
extern void sub_0003D020_gen(void);
void sub_0003D020(void)
{
    sub_0003D020_gen();
    if (active && pdo_settings_get()->aspect == PDO_ASPECT_21_9) {
        uint32_t disp = MEM32(0x0041A06C), fn_va;
        float v = MEMF(0x00342724) * (64.0f / 27.0f) / (16.0f / 9.0f);
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
        SDL_Log("widescreen: 21:9, the view's aspect value %g (16:9: 1.333)", v);
    }
}

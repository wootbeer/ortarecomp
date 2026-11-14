/*
 * In a captured frame (F12, PDO_CAPTURE), log every polygon pd1's HUD
 * drawing (0x0016A4ED: the gauge and the radar among them, the title's text)
 * hands its polygon routine 0x001321D8 (vertices, material, then two words),
 * with the data they point to. pd1 draws its frames in software; the same
 * frame from two builds shows whether the HUD's own numbers or the
 * rasterizer behind them differ.
 */

#include "recomp_types.h"

#include "gfx/renderer.h"

#include <SDL3/SDL.h>

#define HUD_FN      0x0016A4EDu
#define HUD_FN_END  (0x0016A4EDu + 0x4D5u)

/* pd1's polygon routine (cdecl: vertices, material, word, word) */
void sub_001321D8_enter(void)
{
    uint32_t ret = MEM32(g_esp), a[4], i;
    char line[512];
    int n;

    if (ret < HUD_FN || ret >= HUD_FN_END || !pdo_gfx_capturing())
        return;
    for (i = 0; i < 4; i++)
        a[i] = MEM32(g_esp + 4 + 4 * i);
    n = SDL_snprintf(line, sizeof line, "pd1 hud poly from %08x: %08x %08x %08x %08x | v",
                     ret, a[0], a[1], a[2], a[3]);
    for (i = 0; i < 12 && n < 400; i++)
        n += SDL_snprintf(line + n, sizeof line - n, " %08x", MEM32(a[0] + 4 * i));
    n += SDL_snprintf(line + n, sizeof line - n, " | m");
    for (i = 0; i < 4 && a[1] && n < 500; i++)
        n += SDL_snprintf(line + n, sizeof line - n, " %08x", MEM32(a[1] + 4 * i));
    SDL_Log("%s", line);
}

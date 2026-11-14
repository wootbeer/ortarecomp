/*
 * Pandora's Box's PANZER DRAGOON, which we cannot play yet.
 *
 * The original Panzer Dragoon on the Orta disc is another program,
 * d:\pd1.xbe. Leaving the Box (0x000B8080) takes the next scene from the
 * Box object (pointer at 0x00474058, +0x7D4); -2 means that program: it
 * stores -2 as the next scene (0x00342718) and writes the scene table at
 * that index, takes everything down, saves, and tail-jumps to 0x000120D0,
 * which shuts down sound and graphics and calls XLaunchNewImageA. Only
 * default.xbe is recompiled, so there is nothing to launch, and the game
 * cannot run on after that.
 *
 * So the Box leaves for the main menu instead: scene 9, where its EXIT goes
 * (0x000B8080 then starts it in mode 0x20003). Scene 10, the title as at
 * boot, came up black: nothing faded it in.
 *
 * pd1.xbe can now be recompiled as a second program (gen-pd1/, run alone
 * with --program pd1), but switching to it in-process is not written yet:
 * docs/two-programs.md.
 */

#include <SDL3/SDL.h>

#include "recomp_types.h"

#define BOX_OBJECT      0x00474058u
#define BOX_NEXT_SCENE  0x7D4u
#define SCENE_PD1       0xFFFFFFFEu     /* -2: launch d:\pd1.xbe */
#define SCENE_MENU      9u              /* where EXIT goes: the main menu */

/* Pandora's Box: leaving it (ret) */
void sub_000B8080_enter(void)
{
    uint32_t box = MEM32(BOX_OBJECT);

    if (box && MEM32(box + BOX_NEXT_SCENE) == SCENE_PD1) {
        SDL_Log("launch: PANZER DRAGOON (d:\\pd1.xbe) cannot be switched to yet; back to the main menu");
        MEM32(box + BOX_NEXT_SCENE) = SCENE_MENU;
    }
}

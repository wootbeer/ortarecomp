#ifndef PDO_GFX_OVERLAY_H
#define PDO_GFX_OVERLAY_H

/* The options overlay (F1, or Back+Start on a controller) and the frame
 * counter. Drawn on the CPU into a 640x480 RGBA image that the renderer lays
 * over the game picture. */

#include <SDL3/SDL.h>

#include <stdbool.h>

#define PDO_OVERLAY_W 640
#define PDO_OVERLAY_H 480

/* An event for the overlay: true when it took it (the menu is open, or the
 * key or buttons that open it). */
bool pdo_overlay_event(const SDL_Event *ev);

/* The menu is open: the game waits. */
bool pdo_overlay_open(void);
/* The Touch controls page is shown (in the game or the launcher): Android
 * shows its on-screen controls behind it as they are set. */
bool pdo_overlay_on_touch_page(void);

/* Where the layer (PDO_OVERLAY_W x PDO_OVERLAY_H) is shown, in the window's
 * pixels: the game's renderer and the launcher say so each frame, so that a
 * click or a tap (SDL makes touches clicks) finds the line under it. */
void pdo_overlay_set_rect(float x, float y, float w, float h);

/* Count a presented frame (for the frame counter). */
void pdo_overlay_frame(void);

/* Anything to draw: the menu or the frame counter. */
bool pdo_overlay_visible(void);

/* The overlay image, redrawn if it changed; *changed says whether it did.
 * NULL when nothing is visible. */
SDL_Surface *pdo_overlay_image(bool *changed);

/* Input for the game, from the input layer: true means hand the game an idle
 * pad. Blocks while the menu is open and, after it closes, until every key and
 * button the game reads is released, so the press that closed it does not
 * reach the game. any_pressed: the game's pad state has anything held. */
bool pdo_overlay_blocks_input(bool any_pressed);

/* The launcher (main.c, before the game starts): the same menu, with Start
 * game, the game files, graphics and fullscreen, and Exit; Close and F1 do
 * not close it. files: the game files to show (the folder), ok: usable. */
enum { PDO_LAUNCH_NONE, PDO_LAUNCH_START, PDO_LAUNCH_PICK, PDO_LAUNCH_EXIT };
void pdo_launcher_begin(const char *files, bool ok);
int pdo_launcher_poll(void);        /* what was chosen since the last poll (PDO_LAUNCH_*) */
void pdo_launcher_end(void);

#endif

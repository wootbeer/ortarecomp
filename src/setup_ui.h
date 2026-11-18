#ifndef PDO_SETUP_UI_H
#define PDO_SETUP_UI_H

#include "assets.h"
#include "config.h"

#include <SDL3/SDL.h>

#include <stdbool.h>

/* First-run setup: ask the player for their disc image or extracted game
 * folder, check it, import it if needed and save the choice to cfg.
 * Returns false if the player quits. Drawn in window and renderer if
 * given (the launcher's, left open), else in a window of its own. */
bool pdo_setup_run(pdo_config *cfg, pdo_game_check *check, SDL_Window *window, SDL_Renderer *renderer);

#endif

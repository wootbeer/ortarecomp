/*
 * Game-file picker. Uses SDL's native dialogs, which map to the system file
 * picker on Windows, macOS, Linux (portal/zenity) and Android (SAF), so the
 * same flow works everywhere.
 */

#include "setup_ui.h"

#include "sha256.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define WINDOW_TITLE "Orta Recomp"

enum { CHOICE_QUIT, CHOICE_ISO, CHOICE_FOLDER };

typedef struct dialog_result {
    bool done;
    char path[PDO_PATH_MAX];
} dialog_result;

typedef struct ui {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    Uint64        last_draw;
    bool          quit_requested;
    bool          borrowed;     /* the launcher's window: left open after */
} ui;

static void SDLCALL dialog_cb(void *userdata, const char *const *files, int filter)
{
    dialog_result *r = userdata;

    if (files && files[0])
        SDL_strlcpy(r->path, files[0], sizeof r->path);
    r->done = true;
}

static void pump(ui *u)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        if (ev.type == SDL_EVENT_QUIT)
            u->quit_requested = true;
}

static void draw(ui *u, double fraction)
{
    int w, h;
    SDL_FRect bar;

    if (!u->renderer)
        return;
    SDL_GetRenderOutputSize(u->renderer, &w, &h);
    SDL_SetRenderDrawColor(u->renderer, 6, 9, 16, 0xff);   /* the launcher's backdrop */
    SDL_RenderClear(u->renderer);
    if (fraction >= 0) {
        bar.x = w * 0.1f;
        bar.y = h * 0.5f - 12;
        bar.w = w * 0.8f;
        bar.h = 24;
        SDL_SetRenderDrawColor(u->renderer, 0x3a, 0x34, 0x48, 0xff);
        SDL_RenderFillRect(u->renderer, &bar);
        bar.w *= (float)(fraction > 1 ? 1 : fraction);
        SDL_SetRenderDrawColor(u->renderer, 0x9c, 0xe6, 0x2a, 0xff);
        SDL_RenderFillRect(u->renderer, &bar);
    }
    SDL_RenderPresent(u->renderer);
}

static int ask_source(ui *u, const char *extra)
{
    /* Android: a disc image only. Its picker hands a folder over as a
     * document tree, not a path the game can read files from. */
    static const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, CHOICE_QUIT, "Quit" },
#ifndef __ANDROID__
        { 0, CHOICE_FOLDER, "Game folder..." },
#endif
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, CHOICE_ISO, "Disc image..." },
    };
    char text[2048];
    SDL_MessageBoxData box;
    int choice = CHOICE_QUIT;

#ifdef __ANDROID__
    snprintf(text, sizeof text,
             "%s%s"
             "Orta Recomp needs your own copy of Panzer Dragoon Orta (USA).\n\n"
             "Choose its disc image (.iso / .xiso). Its files are copied into the "
             "app's own storage, so the image itself can be deleted afterwards.",
             extra ? extra : "", extra ? "\n\n" : "");
#else
    snprintf(text, sizeof text,
             "%s%s"
             "Orta Recomp needs your own copy of Panzer Dragoon Orta (USA).\n\n"
             "Choose either a disc image (.iso / .xiso) or a folder that already "
             "holds all of the game's files (default.xbe and the rest of the disc).\n\n"
             "A disc image is copied into:\n%s",
             extra ? extra : "", extra ? "\n\n" : "", pdo_data_dir());
#endif

    SDL_zero(box);
    box.flags = extra ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION;
    box.window = u->window;
    box.title = WINDOW_TITLE;
    box.message = text;
    box.numbuttons = SDL_arraysize(buttons);
    box.buttons = buttons;
    if (!SDL_ShowMessageBox(&box, &choice))
        return CHOICE_QUIT;
    return choice;
}

static bool pick(ui *u, int choice, char *out, size_t len)
{
#ifndef __ANDROID__
    static const SDL_DialogFileFilter filters[] = {
        { "Xbox disc images", "iso;xiso" },
        { "All files", "*" },
    };
#endif
    dialog_result r;

    SDL_zero(r);
    if (choice == CHOICE_ISO)
#ifdef __ANDROID__
        /* Android's picker filters by MIME type, and disc images have no
         * dependable one: every file is offered. */
        SDL_ShowOpenFileDialog(dialog_cb, &r, u->window, NULL, 0, NULL, false);
#else
        SDL_ShowOpenFileDialog(dialog_cb, &r, u->window, filters, SDL_arraysize(filters), NULL, false);
#endif
    else
        SDL_ShowOpenFolderDialog(dialog_cb, &r, u->window, NULL, false);

    while (!r.done && !u->quit_requested) {
        pump(u);
        draw(u, -1);
        SDL_Delay(16);
    }
    if (!r.path[0])
        return false;
    SDL_strlcpy(out, r.path, len);
    return true;
}

static void progress_cb(void *userdata, double fraction, const char *file)
{
    ui *u = userdata;
    Uint64 now = SDL_GetTicks();
    char title[256];

    if (now - u->last_draw < 33 && fraction < 1.0)
        return;
    u->last_draw = now;
    pump(u);
    snprintf(title, sizeof title, WINDOW_TITLE " - importing %d%%", (int)(fraction * 100));
    SDL_SetWindowTitle(u->window, title);
    draw(u, fraction);
}

static void warn_unverified(ui *u, const pdo_game_check *check)
{
    char hex[65], text[512];

    if (check->verdict != PDO_XBE_LAYOUT_MATCH)
        return;
    pdo_sha256_hex(check->xbe.file_sha256, hex);
    snprintf(text, sizeof text,
             "This copy of the game looks right, but its default.xbe is not one "
             "we have tested.\n\nIt will probably work. If it doesn't, please "
             "report this hash:\n%s", hex);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, WINDOW_TITLE, text, u->window);
}

bool pdo_setup_run(pdo_config *cfg, pdo_game_check *check, SDL_Window *window, SDL_Renderer *renderer)
{
    ui u;
    char problem[1024] = "";
    char title[256];
    bool ok = false;

    SDL_zero(u);
    if (window && renderer) {
        u.window = window;
        u.renderer = renderer;
        u.borrowed = true;
    } else
#ifdef __ANDROID__
    if (!SDL_CreateWindowAndRenderer(WINDOW_TITLE, 640, 360, SDL_WINDOW_FULLSCREEN, &u.window, &u.renderer))
#else
    if (!SDL_CreateWindowAndRenderer(WINDOW_TITLE, 640, 360, 0, &u.window, &u.renderer))
#endif
        SDL_Log("setup: no window (%s); dialogs will be parentless", SDL_GetError());
    SDL_strlcpy(title, u.window ? SDL_GetWindowTitle(u.window) : WINDOW_TITLE, sizeof title);
    draw(&u, -1);

    {
        char found[PDO_PATH_MAX], err[1024] = "";
        bool is_iso;

        if (pdo_assets_autodetect(found, sizeof found, &is_iso)) {
            SDL_Log("setup: found %s", found);
            if (is_iso) {
                char dst[PDO_PATH_MAX];
                pdo_assets_default_import_dir(dst, sizeof dst);
                ok = pdo_assets_import_iso(found, dst, progress_cb, &u, check, err, sizeof err);
                if (ok)
                    SDL_strlcpy(cfg->game_dir, dst, sizeof cfg->game_dir);
                SDL_SetWindowTitle(u.window, title);
            } else {
                ok = pdo_assets_check_dir(found, check, err, sizeof err);
                if (ok)
                    SDL_strlcpy(cfg->game_dir, found, sizeof cfg->game_dir);
            }
            if (!ok)
                snprintf(problem, sizeof problem, "Found %s, but it didn't work: %s", found, err);
        }
    }

    while (!ok && !u.quit_requested) {
        char path[PDO_PATH_MAX];
        char err[1024] = "";
        int choice = ask_source(&u, problem[0] ? problem : NULL);

        if (choice == CHOICE_QUIT)
            break;
        if (!pick(&u, choice, path, sizeof path))
            continue;

        if (choice == CHOICE_ISO) {
            char dst[PDO_PATH_MAX];
            pdo_assets_default_import_dir(dst, sizeof dst);
            ok = pdo_assets_import_iso(path, dst, progress_cb, &u, check, err, sizeof err);
            if (ok)
                SDL_strlcpy(cfg->game_dir, dst, sizeof cfg->game_dir);
            SDL_SetWindowTitle(u.window, title);
        } else {
            ok = pdo_assets_check_dir(path, check, err, sizeof err);
            if (ok)
                SDL_strlcpy(cfg->game_dir, path, sizeof cfg->game_dir);
        }

        if (!ok)
            snprintf(problem, sizeof problem, "That didn't work: %s", err);
    }

    if (ok) {
        warn_unverified(&u, check);
        if (!pdo_config_save(cfg))
            SDL_Log("setup: could not save settings: %s", SDL_GetError());
    }
    if (u.borrowed)
        return ok;
    if (u.renderer)
        SDL_DestroyRenderer(u.renderer);
    if (u.window)
        SDL_DestroyWindow(u.window);
    return ok;
}

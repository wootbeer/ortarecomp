/* Locating, checking and importing the player's own game files. */

#include "assets.h"

#include "config.h"
#include "xiso.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

void pdo_assets_default_import_dir(char *out, size_t len)
{
    snprintf(out, len, "%sgame", pdo_data_dir());
}

static void join(char *out, size_t len, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    bool sep = n && (dir[n - 1] == '/' || dir[n - 1] == '\\');
    snprintf(out, len, "%s%s%s", dir, sep ? "" : "/", name);
}

/* Orta reads its files from the disc root (D:\name), not from a Media
 * folder, so all we can ask of a game folder is that it holds more than
 * default.xbe. */
static SDL_EnumerationResult SDLCALL find_data_cb(void *userdata, const char *dirname, const char *fname)
{
    bool *found = userdata;

    if (SDL_strcasecmp(fname, "default.xbe") == 0)
        return SDL_ENUM_CONTINUE;
    *found = true;
    return SDL_ENUM_SUCCESS;
}

bool pdo_assets_check_dir(const char *dir, pdo_game_check *check, char *err, size_t err_len)
{
    char path[PDO_PATH_MAX];
    bool found = false;
    void *data;
    size_t size;
    bool ok;

    memset(check, 0, sizeof *check);
    check->verdict = PDO_XBE_INVALID;

    join(path, sizeof path, dir, "default.xbe");
    data = SDL_LoadFile(path, &size);
    if (!data) {
        snprintf(err, err_len, "No default.xbe in %s", dir);
        return false;
    }
    ok = pdo_xbe_parse(data, size, &check->xbe, err, err_len);
    SDL_free(data);
    if (!ok)
        return false;

    check->verdict = pdo_xbe_identify(&check->xbe);
    if (check->verdict != PDO_XBE_KNOWN && check->verdict != PDO_XBE_LAYOUT_MATCH) {
        snprintf(err, err_len, "default.xbe is %s.", pdo_xbe_verdict_text(check->verdict));
        return false;
    }

    SDL_EnumerateDirectory(dir, find_data_cb, &found);
    check->has_data = found;
    if (!found) {
        snprintf(err, err_len, "%s holds only default.xbe. Pick the whole game, "
                               "not just default.xbe.", dir);
        return false;
    }
    return true;
}

/* ── Finding the game without a dialog ─────────────────────────────── */

typedef struct iso_find {
    char path[PDO_PATH_MAX];
    int  count;
} iso_find;

static SDL_EnumerationResult SDLCALL iso_cb(void *userdata, const char *dirname, const char *fname)
{
    iso_find *f = userdata;
    size_t n = strlen(fname);

    if ((n > 4 && SDL_strcasecmp(fname + n - 4, ".iso") == 0) ||
        (n > 5 && SDL_strcasecmp(fname + n - 5, ".xiso") == 0)) {
        if (f->count++ == 0)
            join(f->path, sizeof f->path, dirname, fname);
    }
    return SDL_ENUM_CONTINUE;
}

bool pdo_assets_autodetect(char *out, size_t len, bool *is_iso)
{
#ifdef __ANDROID__
    /* SDL's base path there is "./", which SDL's own file calls take as the
     * app's storage but the kernel layer's open() as relative to "/": the
     * game then found none of its files on the disc. The app's storage
     * alone, by its full path. */
    const char *roots[2] = { pdo_data_dir(), NULL };
#else
    const char *roots[2] = { SDL_GetBasePath(), pdo_data_dir() };
#endif
    pdo_game_check check;
    char path[PDO_PATH_MAX], err[512];
    int i;

    for (i = 0; i < 2; i++) {
        if (!roots[i] || !roots[i][0])
            continue;
        join(path, sizeof path, roots[i], "game");
        if (pdo_assets_check_dir(path, &check, err, sizeof err)) {
            SDL_strlcpy(out, path, len);
            *is_iso = false;
            return true;
        }
    }
    for (i = 0; i < 2; i++) {
        iso_find f;

        if (!roots[i] || !roots[i][0])
            continue;
        SDL_zero(f);
        SDL_EnumerateDirectory(roots[i], iso_cb, &f);
        if (f.count == 1) {     /* more than one: ambiguous, let the player pick */
            SDL_strlcpy(out, f.path, len);
            *is_iso = true;
            return true;
        }
    }
    return false;
}

/* ── Disc image import ──────────────────────────────────────────────── */

static SDL_EnumerationResult SDLCALL remove_cb(void *userdata, const char *dirname, const char *fname)
{
    char path[PDO_PATH_MAX];
    SDL_PathInfo info;

    join(path, sizeof path, dirname, fname);
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY)
        SDL_EnumerateDirectory(path, remove_cb, NULL);
    SDL_RemovePath(path);
    return SDL_ENUM_CONTINUE;
}

static void remove_tree(const char *dir)
{
    if (!SDL_GetPathInfo(dir, NULL))
        return;
    SDL_EnumerateDirectory(dir, remove_cb, NULL);
    SDL_RemovePath(dir);
}

typedef struct import_ctx {
    pdo_xiso       *iso;
    const char     *dst;
    pdo_progress_fn progress;
    void           *userdata;
    uint64_t        total, done;
    const char     *current;
    char           *err;
    size_t          err_len;
    bool            failed;
    /* first pass */
    bool            have_xbe;
    uint32_t        xbe_sector, xbe_size;
} import_ctx;

static bool size_cb(void *userdata, const char *relpath, bool is_dir, uint32_t sector, uint32_t size)
{
    import_ctx *c = userdata;

    if (!is_dir)
        c->total += size;
    if (!is_dir && SDL_strcasecmp(relpath, "default.xbe") == 0) {
        c->have_xbe = true;
        c->xbe_sector = sector;
        c->xbe_size = size;
    }
    return true;
}

static void bytes_cb(void *userdata, uint64_t bytes)
{
    import_ctx *c = userdata;

    c->done += bytes;
    if (c->progress)
        c->progress(c->userdata, c->total ? (double)c->done / (double)c->total : 1.0, c->current);
}

static bool extract_cb(void *userdata, const char *relpath, bool is_dir, uint32_t sector, uint32_t size)
{
    import_ctx *c = userdata;
    char path[PDO_PATH_MAX];

    join(path, sizeof path, c->dst, relpath);
    if (is_dir) {
        if (!SDL_CreateDirectory(path)) {
            snprintf(c->err, c->err_len, "cannot create %s: %s", path, SDL_GetError());
            c->failed = true;
            return false;
        }
        return true;
    }
    if (SDL_strcasecmp(relpath, "default.xbe") == 0)
        return true;    /* already extracted and checked */
    c->current = relpath;
    if (!pdo_xiso_extract(c->iso, sector, size, path, bytes_cb, c, c->err, c->err_len)) {
        c->failed = true;
        return false;
    }
    return true;
}

bool pdo_assets_import_iso(const char *iso_path, const char *dst_dir,
                           pdo_progress_fn progress, void *userdata,
                           pdo_game_check *check, char *err, size_t err_len)
{
    import_ctx c;
    char staging[PDO_PATH_MAX], xbe_path[PDO_PATH_MAX];
    bool ok = false;

    memset(&c, 0, sizeof c);
    memset(check, 0, sizeof *check);
    c.progress = progress;
    c.userdata = userdata;
    c.err = err;
    c.err_len = err_len;

    c.iso = pdo_xiso_open(iso_path, err, err_len);
    if (!c.iso)
        return false;
    if (!pdo_xiso_walk(c.iso, size_cb, &c, err, err_len))
        goto done;
    if (!c.have_xbe) {
        snprintf(err, err_len, "This disc image has no default.xbe.");
        goto done;
    }

    /* Extract into a staging folder and only swap it in once complete, so a
     * cancelled or failed import never leaves a half-copied game behind. */
    snprintf(staging, sizeof staging, "%s.partial", dst_dir);
    remove_tree(staging);
    if (!SDL_CreateDirectory(staging)) {
        snprintf(err, err_len, "cannot create %s: %s", staging, SDL_GetError());
        goto done;
    }
    c.dst = staging;

    join(xbe_path, sizeof xbe_path, staging, "default.xbe");
    c.current = "default.xbe";
    if (!pdo_xiso_extract(c.iso, c.xbe_sector, c.xbe_size, xbe_path, bytes_cb, &c, err, err_len))
        goto done;
    {
        void *data;
        size_t size;
        bool parsed;

        data = SDL_LoadFile(xbe_path, &size);
        if (!data) {
            snprintf(err, err_len, "cannot read back default.xbe: %s", SDL_GetError());
            goto done;
        }
        parsed = pdo_xbe_parse(data, size, &check->xbe, err, err_len);
        SDL_free(data);
        if (!parsed)
            goto done;
        check->verdict = pdo_xbe_identify(&check->xbe);
        if (check->verdict != PDO_XBE_KNOWN && check->verdict != PDO_XBE_LAYOUT_MATCH) {
            snprintf(err, err_len, "This disc is %s.", pdo_xbe_verdict_text(check->verdict));
            goto done;
        }
    }

    if (!pdo_xiso_walk(c.iso, extract_cb, &c, err, err_len) || c.failed)
        goto done;

    remove_tree(dst_dir);
    if (!SDL_RenamePath(staging, dst_dir)) {
        snprintf(err, err_len, "cannot move the game into %s: %s", dst_dir, SDL_GetError());
        goto done;
    }
    ok = pdo_assets_check_dir(dst_dir, check, err, err_len);

done:
    pdo_xiso_close(c.iso);
    if (!ok && c.dst)
        remove_tree(staging);
    return ok;
}

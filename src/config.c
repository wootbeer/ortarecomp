/* Settings file: ortarecomp.ini, simple key=value lines. */

#include "config.h"
#include "input_map.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define CONFIG_NAME "ortarecomp.ini"

static char data_dir[PDO_PATH_MAX];
char pdo_log_file[PDO_PATH_MAX];

/* Depth: 32-bit float, reversed, for precision past the console's, on PC;
 * on Android the game's own 24 bits: with 4x MSAA a sample is half the
 * size in the tile memory, measured a few frames a second faster on the
 * RP6 at 2x (52 against 49), and no z-fighting seen. */
#ifdef __ANDROID__
#define PDO_DEPTH_REVERSED_DEFAULT false
#else
#define PDO_DEPTH_REVERSED_DEFAULT true
#endif

static pdo_settings settings = { .render_scale2 = 2, .msaa = 1, .aspect = PDO_ASPECT_4_3, .fit = PDO_FIT_BARS,
                                  .master_volume = 100, .gpu_vertex = true, .gpu_fixed = true,
                                  .lsfg_multiplier = 1, .anisotropy = 1, .mouse_aim = true, .mouse_speed = 100, .show_building = true,
                                  .skip_logos = true, .rumble = 100, .touch_size = 100, .touch_opacity = 30, .hrtf = true, .depth_reversed = PDO_DEPTH_REVERSED_DEFAULT, .pic_contrast = 100,
                                  .pic_gamma = 100, .pic_saturation = 100, .save_slot = 1 };

pdo_settings *pdo_settings_get(void)
{
    return &settings;
}

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Anything Orta Recomp keeps in a data folder: its settings, saves or
 * imported game files. */
static bool has_data(const char *dir)
{
    static const char *names[] = { "ortarecomp.ini", "saves", "game" };
    char path[PDO_PATH_MAX];

    for (size_t i = 0; i < SDL_arraysize(names); i++) {
        snprintf(path, sizeof path, "%s%s", dir, names[i]);
        if (SDL_GetPathInfo(path, NULL))
            return true;
    }
    return false;
}

const char *pdo_data_dir_note;

/* Portable by default: a new player's settings, saves and game files go
 * next to the executable, with a portable.txt saying so. A player who
 * already has them in the per-user folder keeps that folder, as does one
 * who deleted portable.txt (an ortarecomp.ini next to the executable
 * without it), and anyone whose executable's folder cannot be written to. */
const char *pdo_data_dir(void)
{
    const char *base;
    char *pref;
    char probe[PDO_PATH_MAX], ini[PDO_PATH_MAX];

    if (data_dir[0])
        return data_dir;

    base = SDL_GetBasePath();
#ifdef __ANDROID__
    base = NULL;        /* "./" there; no portable mode: the app's storage (SDL's pref path) */
#endif
    if (base) {
        snprintf(probe, sizeof probe, "%sportable.txt", base);
        if (SDL_GetPathInfo(probe, NULL)) {
            SDL_strlcpy(data_dir, base, sizeof data_dir);
            pdo_data_dir_note = "portable (portable.txt)";
            return data_dir;
        }
    }

    pref = SDL_GetPrefPath("OrtaRecomp", "OrtaRecomp");     /* made if missing */
    snprintf(ini, sizeof ini, "%sortarecomp.ini", base ? base : "");
    if (base && !SDL_GetPathInfo(ini, NULL) && (!pref || !has_data(pref))) {
        SDL_IOStream *f = SDL_IOFromFile(probe, "w");
        if (f) {
            static const char text[] =
                "Orta Recomp keeps its settings, saves and game files in this folder.\n"
                "Delete this file to keep them in your user folder instead.\n";
            SDL_WriteIO(f, text, sizeof text - 1);
            SDL_CloseIO(f);
            if (pref) {     /* the per-user folder SDL just made, if still empty */
                size_t n = strlen(pref);
                SDL_RemovePath(pref);
                while (n > 1 && (pref[n - 1] == '/' || pref[n - 1] == '\\'))
                    pref[--n] = 0;
                while (n > 0 && pref[n - 1] != '/' && pref[n - 1] != '\\')
                    pref[--n] = 0;
                if (n)
                    SDL_RemovePath(pref);       /* its "OrtaRecomp" parent, if empty */
                SDL_free(pref);
            }
            SDL_strlcpy(data_dir, base, sizeof data_dir);
            pdo_data_dir_note = "portable (new: portable.txt made)";
            return data_dir;
        }
    }
    if (pref) {
        SDL_strlcpy(data_dir, pref, sizeof data_dir);
        SDL_free(pref);
        pdo_data_dir_note = "the user folder";
    } else if (base) {
        SDL_strlcpy(data_dir, base, sizeof data_dir);
        pdo_data_dir_note = "the executable's folder (no user folder)";
    }
    return data_dir;
}

void pdo_save_dir(int slot, char *out, size_t len)
{
    const char *base = pdo_data_dir();
    char sep = base[0] ? base[strlen(base) - 1] : '/';     /* the system's, as the data folder ends */
    char e[4] = "E";

    if (slot >= 2 && slot <= 3)
        snprintf(e, sizeof e, "E%d", slot);
    snprintf(out, len, "%ssaves%c%s", base, sep, e);
}

/* The game's saves, as the dashboard lays them out: UDATA/<title ID>/<a
 * folder per save>, next to the title's own TitleMeta/TitleImage files. */
static SDL_EnumerationResult SDLCALL find_save(void *found, const char *dir, const char *name)
{
    char path[PDO_PATH_MAX];
    SDL_PathInfo info;

    snprintf(path, sizeof path, "%s%s", dir, name);
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
        *(bool *)found = true;
        return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

bool pdo_save_slot_used(int slot)
{
    char dir[PDO_PATH_MAX], udata[PDO_PATH_MAX];
    bool found = false;

    pdo_save_dir(slot, dir, sizeof dir);
    snprintf(udata, sizeof udata, "%s/UDATA/53450007", dir);
    SDL_EnumerateDirectory(udata, find_save, &found);
    return found;
}

static void config_path(char *out, size_t len)
{
    snprintf(out, len, "%s%s", pdo_data_dir(), CONFIG_NAME);
}

static bool load_setting(pdo_config *cfg, const char *line, const char *value);

bool pdo_config_load(pdo_config *cfg)
{
    char path[PDO_PATH_MAX];
    char *text, *line, *save = NULL;
    size_t size;

    memset(cfg, 0, sizeof *cfg);
    config_path(path, sizeof path);
    text = SDL_LoadFile(path, &size);
    if (!text)
        return false;

    for (line = SDL_strtok_r(text, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';')
            continue;
        *eq = '\0';
        if (!load_setting(cfg, line, eq + 1))
            pdo_input_load_line(line, eq + 1);
    }
    SDL_free(text);
    return true;
}

/* One line of the settings file, key and value: false if it is no setting
 * (a binding, say). */
static bool load_setting(pdo_config *cfg, const char *line, const char *value)
{
    {
        if (strcmp(line, "game_dir") == 0) {
            SDL_strlcpy(cfg->game_dir, value, sizeof cfg->game_dir);
#ifdef __ANDROID__
            /* An earlier build saved the app's game folder as "./game"
             * (assets.c): the same folder, by its full path. */
            if (cfg->game_dir[0] && cfg->game_dir[0] != '/') {
                const char *rel = cfg->game_dir[0] == '.' && cfg->game_dir[1] == '/' ? cfg->game_dir + 2 : cfg->game_dir;
                char full[PDO_PATH_MAX];
                snprintf(full, sizeof full, "%s%s", pdo_data_dir(), rel);
                SDL_strlcpy(cfg->game_dir, full, sizeof cfg->game_dir);
            }
#endif
        }
        else if (strcmp(line, "render_scale") == 0)
            settings.render_scale2 = clamp_int((int)SDL_lround(SDL_atof(value) * 2.0), 2, 16);
        else if (strcmp(line, "msaa") == 0) {
            int m = SDL_atoi(value);
            settings.msaa = m >= 8 ? 8 : m >= 4 ? 4 : m >= 2 ? 2 : 1;
        } else if (strcmp(line, "aspect") == 0) {
            settings.aspect = strcmp(value, "16:9") == 0   ? PDO_ASPECT_16_9
                            : strcmp(value, "21:9") == 0 ? PDO_ASPECT_21_9
                                                          : PDO_ASPECT_4_3;
            if (strcmp(value, "stretch") == 0)     /* a file from before the fit setting */
                settings.fit = PDO_FIT_STRETCH;
        } else if (strcmp(line, "hud_edges") == 0)
            settings.hud_edges = SDL_atoi(value) != 0;
        else if (strcmp(line, "movie_fit") == 0)
            settings.movie_fit = strcmp(value, "stretch") == 0    ? PDO_MOVIE_STRETCH
                               : strcmp(value, "zoom") == 0       ? PDO_MOVIE_ZOOM
                               : strcmp(value, "bars") == 0       ? PDO_MOVIE_BARS
                                                                   : PDO_MOVIE_ORIGINAL;
        else if (strcmp(line, "fit") == 0)
            settings.fit = strcmp(value, "stretch") == 0 ? PDO_FIT_STRETCH
                         : strcmp(value, "zoom") == 0    ? PDO_FIT_ZOOM
                                                          : PDO_FIT_BARS;
        else if (strcmp(line, "soft_filter") == 0)
            settings.soft_filter = SDL_atoi(value) != 0;
        else if (strcmp(line, "master_volume") == 0)
            settings.master_volume = clamp_int(SDL_atoi(value), 0, 100);
        else if (strcmp(line, "show_fps") == 0)
            settings.show_fps = SDL_atoi(value) != 0;
        else if (strcmp(line, "show_building") == 0)
            settings.show_building = SDL_atoi(value) != 0;
        else if (strcmp(line, "gpu_vertex") == 0)
            settings.gpu_vertex = SDL_atoi(value) != 0;
        else if (strcmp(line, "gpu_fixed") == 0)
            settings.gpu_fixed = SDL_atoi(value) != 0;
        else if (strcmp(line, "frame_interp") == 0)
            settings.frame_interp = SDL_atoi(value) != 0;
        else if (strcmp(line, "bloom") == 0)
            settings.bloom = strcmp(value, "high") == 0 ? 2 : strcmp(value, "low") == 0 ? 1 : 0;
        else if (strcmp(line, "anisotropy") == 0) {
            int a = clamp_int(SDL_atoi(value), 1, 16);
            settings.anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 1;
        } else if (strcmp(line, "lsfg_multiplier") == 0)
            settings.lsfg_multiplier = clamp_int(SDL_atoi(value), 1, 4);
        else if (strcmp(line, "lsfg_performance") == 0)
            settings.lsfg_performance = SDL_atoi(value) != 0;
        else if (strcmp(line, "lsfg_dll") == 0)
            SDL_strlcpy(settings.lsfg_dll, value, sizeof settings.lsfg_dll);
        else if (strcmp(line, "fullscreen") == 0)
            settings.fullscreen = SDL_atoi(value) != 0;
        else if (strcmp(line, "fxaa") == 0)
            settings.fxaa = SDL_atoi(value) != 0;
        else if (strcmp(line, "shadows") == 0)
            settings.shadow_original = strcmp(value, "low") == 0 || strcmp(value, "original") == 0;
        else if (strcmp(line, "vsync") == 0)
            settings.vsync = strcmp(value, "off") == 0 ? PDO_VSYNC_OFF
                           : strcmp(value, "fast") == 0 ? PDO_VSYNC_FAST : PDO_VSYNC_ON;
        else if (strcmp(line, "frame_rate") == 0)
            settings.fps30 = SDL_atoi(value) == 30;
        else if (strcmp(line, "window") == 0)        /* x,y,w,h */
            SDL_sscanf(value, "%d,%d,%d,%d", &settings.win_x, &settings.win_y, &settings.win_w, &settings.win_h);
        else if (strcmp(line, "window_maximized") == 0)
            settings.win_maximized = SDL_atoi(value) != 0;
        else if (strcmp(line, "mouse_aim") == 0)
            settings.mouse_aim = SDL_atoi(value) != 0;
        else if (strcmp(line, "mouse_speed") == 0)
            settings.mouse_speed = clamp_int(SDL_atoi(value), 10, 400);
        else if (strcmp(line, "unlock_box") == 0)
            settings.unlock_box = SDL_atoi(value) != 0;
        else if (strcmp(line, "skip_logos") == 0)
            settings.skip_logos = SDL_atoi(value) != 0;
        else if (strcmp(line, "skip_launcher") == 0)
            settings.skip_launcher = SDL_atoi(value) != 0;
        else if (strcmp(line, "rumble") == 0)
            settings.rumble = clamp_int(SDL_atoi(value), 0, 100);
        else if (strcmp(line, "touch_buttons") == 0)
            settings.touch_buttons = strcmp(value, "always") == 0 ? PDO_TOUCH_ALWAYS
                                   : strcmp(value, "off") == 0 ? PDO_TOUCH_OFF : PDO_TOUCH_AUTO;
        else if (strcmp(line, "touch_size") == 0)
            settings.touch_size = clamp_int(SDL_atoi(value), 90, 130);
        else if (strcmp(line, "touch_opacity") == 0)
            settings.touch_opacity = clamp_int(SDL_atoi(value), 10, 100);
        else if (strcmp(line, "gpu_driver") == 0)
            SDL_strlcpy(settings.gpu_driver, value, sizeof settings.gpu_driver);
        else if (strcmp(line, "depth_reversed") == 0)
            settings.depth_reversed = SDL_atoi(value) != 0;
        else if (strcmp(line, "picture_crt") == 0)
            settings.picture_crt = SDL_atoi(value) != 0;
        else if (strcmp(line, "picture_brightness") == 0)
            settings.pic_brightness = clamp_int(SDL_atoi(value), -50, 50);
        else if (strcmp(line, "picture_contrast") == 0)
            settings.pic_contrast = clamp_int(SDL_atoi(value), 50, 150);
        else if (strcmp(line, "picture_gamma") == 0)
            settings.pic_gamma = clamp_int(SDL_atoi(value), 50, 200);
        else if (strcmp(line, "picture_saturation") == 0)
            settings.pic_saturation = clamp_int(SDL_atoi(value), 0, 200);
        else if (strcmp(line, "audio_3d") == 0)    /* the key kept from when it was 3D only */
            settings.audio_out = strcmp(value, "headphones") == 0 ? PDO_AUDIO_HEADPHONES
                               : strcmp(value, "surround") == 0 ? PDO_AUDIO_SURROUND : PDO_AUDIO_STEREO;
        else if (strcmp(line, "audio_hrtf") == 0)
            settings.hrtf = SDL_atoi(value) != 0;
        else if (strcmp(line, "save_slot") == 0)
            settings.save_slot = clamp_int(SDL_atoi(value), 1, 3);
        else
            return false;
    }
    return true;
}

/* The settings file's text for cfg and the settings: its length, or -1. */
static int config_text(const pdo_config *cfg, char *text, size_t cap)
{
    int n;

    n = snprintf(text, cap,
                 "# Orta Recomp settings\n"
                 "game_dir=%s\n"
                 "render_scale=%g\n"
                 "msaa=%d\n"
                 "aspect=%s\n"
                 "fit=%s\n"
                 "movie_fit=%s\n"
                 "hud_edges=%d\n"
                 "soft_filter=%d\n"
                 "master_volume=%d\n"
                 "show_fps=%d\n"
                 "show_building=%d\n"
                 "gpu_vertex=%d\n"
                 "gpu_fixed=%d\n"
                 "frame_interp=%d\n"
                 "anisotropy=%d\n"
                 "bloom=%s\n"
                 "lsfg_multiplier=%d\n"
                 "lsfg_performance=%d\n"
                 "lsfg_dll=%s\n"
                 "fullscreen=%d\n"
                 "fxaa=%d\n"
                 "vsync=%s\n"
                 "frame_rate=%d\n"
                 "shadows=%s\n"
                 "window=%d,%d,%d,%d\n"
                 "window_maximized=%d\n"
                 "mouse_aim=%d\n"
                 "mouse_speed=%d\n"
                 "unlock_box=%d\n"
                 "skip_logos=%d\n"
                 "skip_launcher=%d\n"
                 "rumble=%d\n"
                 "touch_buttons=%s\n"
                 "touch_size=%d\n"
                 "touch_opacity=%d\n"
                 "gpu_driver=%s\n"
                 "depth_reversed=%d\n"
                 "picture_crt=%d\n"
                 "picture_brightness=%d\n"
                 "picture_contrast=%d\n"
                 "picture_gamma=%d\n"
                 "picture_saturation=%d\n"
                 "audio_3d=%s\n"
                 "audio_hrtf=%d\n"
                 "save_slot=%d\n",
                 cfg->game_dir, settings.render_scale2 / 2.0, settings.msaa,
                 settings.aspect == PDO_ASPECT_16_9 ? "16:9" : settings.aspect == PDO_ASPECT_21_9 ? "21:9" : "4:3",
                 settings.fit == PDO_FIT_STRETCH ? "stretch" : settings.fit == PDO_FIT_ZOOM ? "zoom" : "bars",
                 settings.movie_fit == PDO_MOVIE_STRETCH ? "stretch" : settings.movie_fit == PDO_MOVIE_ZOOM ? "zoom"
                 : settings.movie_fit == PDO_MOVIE_BARS ? "bars" : "original",
                 settings.hud_edges,
                 settings.soft_filter,
                 settings.master_volume, settings.show_fps, settings.show_building, settings.gpu_vertex,
                 settings.gpu_fixed, settings.frame_interp, settings.anisotropy, settings.bloom == 2 ? "high" : settings.bloom == 1 ? "low" : "off",
                 settings.lsfg_multiplier, settings.lsfg_performance,
                 settings.lsfg_dll, settings.fullscreen, settings.fxaa,
                 settings.vsync == PDO_VSYNC_OFF ? "off" : settings.vsync == PDO_VSYNC_FAST ? "fast" : "on",
                 settings.fps30 ? 30 : 60,
                 settings.shadow_original ? "low" : "high",
                 settings.win_x, settings.win_y, settings.win_w, settings.win_h, settings.win_maximized,
                 settings.mouse_aim, settings.mouse_speed, settings.unlock_box, settings.skip_logos, settings.skip_launcher, settings.rumble,
                 settings.touch_buttons == PDO_TOUCH_ALWAYS ? "always" : settings.touch_buttons == PDO_TOUCH_OFF ? "off" : "auto",
                 settings.touch_size, settings.touch_opacity, settings.gpu_driver, settings.depth_reversed,
                 settings.picture_crt, settings.pic_brightness, settings.pic_contrast, settings.pic_gamma,
                 settings.pic_saturation, settings.audio_out == PDO_AUDIO_HEADPHONES ? "headphones"
                 : settings.audio_out == PDO_AUDIO_SURROUND ? "surround" : "speakers",
                 settings.hrtf, settings.save_slot);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n + (int)pdo_input_save(text + n, cap - (size_t)n);
}

#define CONFIG_TEXT (2 * PDO_PATH_MAX + 8192)

/* Settings from the command line (pdo_settings_set): this run's only. Each
 * keeps its line in the file as it was and as the command line made it; a
 * save writes the file's line back while the setting is still the command
 * line's, so only a change made in the menu is kept. */
#define MAX_OVERRIDES 64
static struct { char key[40], before[PDO_PATH_MAX + 48], after[PDO_PATH_MAX + 48]; } overrides[MAX_OVERRIDES];
static int n_overrides;

/* key's line in text ("key=value"), into out; empty if there is none. */
static void line_of(const char *text, const char *key, char *out, size_t cap)
{
    size_t k = strlen(key);

    out[0] = 0;
    for (const char *l = text; *l; ) {
        const char *end = strchr(l, '\n');
        size_t len = end ? (size_t)(end - l) : strlen(l);
        if (len > k && strncmp(l, key, k) == 0 && l[k] == '=') {
            SDL_strlcpy(out, l, SDL_min(cap, len + 1));
            return;
        }
        l += len + (end ? 1 : 0);
    }
}

bool pdo_config_save(const pdo_config *cfg)
{
    char path[PDO_PATH_MAX];
    char *text = SDL_malloc(CONFIG_TEXT), *out = SDL_malloc(CONFIG_TEXT);
    int n;
    size_t o = 0;
    bool ok = false;

    if (!text || !out)
        goto done;
    config_path(path, sizeof path);
    n = config_text(cfg, text, CONFIG_TEXT);
    if (n < 0)
        goto done;
    /* Line by line, a command line's setting as the file had it. */
    for (const char *l = text; *l; ) {
        const char *end = strchr(l, '\n');
        size_t len = end ? (size_t)(end - l) : strlen(l);
        const char *put = l;
        size_t put_len = len;
        for (int i = 0; i < n_overrides; i++)
            if (strlen(overrides[i].after) == len && strncmp(l, overrides[i].after, len) == 0) {
                put = overrides[i].before;
                put_len = strlen(put);
                break;
            }
        if (o + put_len + 2 > CONFIG_TEXT)
            goto done;
        memcpy(out + o, put, put_len);
        o += put_len;
        out[o++] = '\n';
        l += len + (end ? 1 : 0);
    }
    ok = SDL_SaveFile(path, out, o);
done:
    SDL_free(text);
    SDL_free(out);
    return ok;
}

/* The names the command line also takes. */
static const struct { const char *alias, *key; } aliases[] = {
    { "scale", "render_scale" }, { "resolution", "render_scale" }, { "output", "audio_3d" },
    { "audio_out", "audio_3d" }, { "hrtf", "audio_hrtf" }, { "volume", "master_volume" },
    { "interp", "frame_interp" }, { "fps", "show_fps" }, { "shader_note", "show_building" }, { "save_file", "save_slot" },
};

/* A command line's setting name as the file's key: false if it is none of
 * those the command line takes. */
static bool setting_key(const char *name, char *key, size_t cap)
{
    size_t i;
    char *text, line[PDO_PATH_MAX + 48];
    pdo_config none;

    if (strlen(name) >= cap)
        return false;
    for (i = 0; name[i]; i++)
        key[i] = name[i] == '-' ? '_' : (char)SDL_tolower((unsigned char)name[i]);
    key[i] = 0;
    for (i = 0; i < SDL_arraysize(aliases); i++)
        if (strcmp(key, aliases[i].alias) == 0)
            SDL_strlcpy(key, aliases[i].key, cap);
    if (strcmp(key, "game_dir") == 0 || strcmp(key, "window") == 0)
        return false;               /* --game-dir; the window's place is the menu's */
    text = SDL_malloc(CONFIG_TEXT);
    if (!text)
        return false;
    SDL_zero(none);
    line[0] = 0;
    if (config_text(&none, text, CONFIG_TEXT) >= 0)
        line_of(text, key, line, sizeof line);
    SDL_free(text);
    if (!line[0])
        return false;
    /* the controls' bindings (key_, pad_, mouse_) are the Controls page's */
    if (!strncmp(key, "key_", 4) || !strncmp(key, "pad_", 4))
        return false;
    return strncmp(key, "mouse_", 6) != 0 || !strcmp(key, "mouse_aim") || !strcmp(key, "mouse_speed");
}

bool pdo_settings_known(const char *name)
{
    char key[40];
    return setting_key(name, key, sizeof key);
}

bool pdo_settings_set(const pdo_config *cfg, const char *name, const char *value)
{
    char key[40], *text, val[64];
    pdo_config scratch = *cfg;

    if (n_overrides >= MAX_OVERRIDES || !setting_key(name, key, sizeof key))
        return false;
    if (strcmp(key, "audio_3d") == 0) {     /* the values the menu shows, too */
        if (strcmp(value, "5.1") == 0)
            value = "surround";
        else if (strcmp(value, "stereo") == 0)
            value = "speakers";
    }
    SDL_strlcpy(val, value, sizeof val);
    if (SDL_strlen(val) > 2 && SDL_tolower((unsigned char)val[SDL_strlen(val) - 1]) == 'x')
        val[SDL_strlen(val) - 1] = 0;   /* "4x" */

    text = SDL_malloc(CONFIG_TEXT);
    if (!text)
        return false;
    if (config_text(cfg, text, CONFIG_TEXT) < 0) {
        SDL_free(text);
        return false;
    }
    line_of(text, key, overrides[n_overrides].before, sizeof overrides[n_overrides].before);
    if (!overrides[n_overrides].before[0] || !load_setting(&scratch, key, val)) {
        SDL_free(text);
        return false;               /* no such setting */
    }
    config_text(cfg, text, CONFIG_TEXT);
    line_of(text, key, overrides[n_overrides].after, sizeof overrides[n_overrides].after);
    SDL_strlcpy(overrides[n_overrides].key, key, sizeof overrides[n_overrides].key);
    SDL_Log("settings: %s from the command line (this run only)", overrides[n_overrides].after);
    n_overrides++;
    SDL_free(text);
    return true;
}

/* The settings in the log: at the start, and again when the menu closes
 * with any of them changed (a log's start alone left changes made in
 * play out of it). */
void pdo_settings_log(void)
{
    static char last[512];
    char line[512];
    const pdo_settings *s = pdo_settings_get();
    snprintf(line, sizeof line, "%gx, msaa %d, fxaa %d, anisotropy %d, bloom %d, frame interp %d, vertex programs on %s, "
             "fixed function on %s, aspect %s%s, frame rate %d, save file %d", s->render_scale2 / 2.0, s->msaa, s->fxaa,
             s->anisotropy, s->bloom, s->frame_interp, s->gpu_vertex ? "GPU" : "CPU", s->gpu_fixed ? "GPU" : "CPU",
             s->aspect == PDO_ASPECT_16_9 ? "16:9" : s->aspect == PDO_ASPECT_21_9 ? "21:9" : "4:3",
             s->aspect == PDO_ASPECT_21_9 ? (s->hud_edges ? " (HUD edges)" : " (HUD centered)") : "",
             s->fps30 ? 30 : 60, s->save_slot);
    if (strcmp(line, last)) {
        SDL_Log("settings%s: %s", last[0] ? " changed" : "", line);
        SDL_strlcpy(last, line, sizeof last);
    }
}

bool pdo_settings_save(void)
{
    /* The file's game folder, not this run's: --game-dir is not saved. The
     * load would also read the file's settings and its key_/pad_/mouse_
     * bindings over ours, so keep both: a rebinding made in Controls was
     * lost as the menu closed (shown, then gone, never working). */
    pdo_settings keep = settings;
    pdo_binding keep_bind[PDO_IN_COUNT];
    pdo_config cfg;
    bool ok;

    SDL_memcpy(keep_bind, pdo_input_bindings(), sizeof keep_bind);
    pdo_config_load(&cfg);
    settings = keep;
    SDL_memcpy(pdo_input_bindings(), keep_bind, sizeof keep_bind);
    ok = pdo_config_save(&cfg);
    if (!ok)
        SDL_Log("settings: could not save (%s)", SDL_GetError());
    return ok;
}

/* Settings file: ortarecomp.ini, simple key=value lines. */

#include "config.h"
#include "input_map.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define CONFIG_NAME "ortarecomp.ini"

static char data_dir[PDO_PATH_MAX];
char pdo_log_file[PDO_PATH_MAX];

static pdo_settings settings = { .render_scale2 = 2, .msaa = 1, .aspect = PDO_ASPECT_4_3, .fit = PDO_FIT_BARS,
                                  .master_volume = 100, .gpu_vertex = true, .gpu_fixed = true,
                                  .lsfg_multiplier = 1, .anisotropy = 1, .mouse_aim = true, .mouse_speed = 100,
                                  .skip_logos = true, .rumble = 100, .depth_reversed = true, .pic_contrast = 100,
                                  .pic_gamma = 100, .pic_saturation = 100, .save_slot = 1 };

pdo_settings *pdo_settings_get(void)
{
    return &settings;
}

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

const char *pdo_data_dir(void)
{
    const char *base;
    char *pref;

    if (data_dir[0])
        return data_dir;

    base = SDL_GetBasePath();
    if (base) {
        char probe[PDO_PATH_MAX];
        snprintf(probe, sizeof probe, "%sportable.txt", base);
        if (SDL_GetPathInfo(probe, NULL)) {
            SDL_strlcpy(data_dir, base, sizeof data_dir);
            return data_dir;
        }
    }

    pref = SDL_GetPrefPath("OrtaRecomp", "OrtaRecomp");
    if (pref) {
        SDL_strlcpy(data_dir, pref, sizeof data_dir);
        SDL_free(pref);
    } else if (base) {
        SDL_strlcpy(data_dir, base, sizeof data_dir);
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
        if (strcmp(line, "game_dir") == 0)
            SDL_strlcpy(cfg->game_dir, eq + 1, sizeof cfg->game_dir);
        else if (strcmp(line, "render_scale") == 0)
            settings.render_scale2 = clamp_int((int)SDL_lround(SDL_atof(eq + 1) * 2.0), 2, 16);
        else if (strcmp(line, "msaa") == 0) {
            int m = SDL_atoi(eq + 1);
            settings.msaa = m >= 8 ? 8 : m >= 4 ? 4 : m >= 2 ? 2 : 1;
        } else if (strcmp(line, "aspect") == 0) {
            settings.aspect = strcmp(eq + 1, "16:9") == 0 ? PDO_ASPECT_16_9 : PDO_ASPECT_4_3;
            if (strcmp(eq + 1, "stretch") == 0)     /* a file from before the fit setting */
                settings.fit = PDO_FIT_STRETCH;
        } else if (strcmp(line, "fit") == 0)
            settings.fit = strcmp(eq + 1, "stretch") == 0 ? PDO_FIT_STRETCH
                         : strcmp(eq + 1, "zoom") == 0    ? PDO_FIT_ZOOM
                                                          : PDO_FIT_BARS;
        else if (strcmp(line, "soft_filter") == 0)
            settings.soft_filter = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "master_volume") == 0)
            settings.master_volume = clamp_int(SDL_atoi(eq + 1), 0, 100);
        else if (strcmp(line, "show_fps") == 0)
            settings.show_fps = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "gpu_vertex") == 0)
            settings.gpu_vertex = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "gpu_fixed") == 0)
            settings.gpu_fixed = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "frame_interp") == 0)
            settings.frame_interp = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "bloom") == 0)
            settings.bloom = strcmp(eq + 1, "high") == 0 ? 2 : strcmp(eq + 1, "low") == 0 ? 1 : 0;
        else if (strcmp(line, "anisotropy") == 0) {
            int a = clamp_int(SDL_atoi(eq + 1), 1, 16);
            settings.anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 1;
        } else if (strcmp(line, "lsfg_multiplier") == 0)
            settings.lsfg_multiplier = clamp_int(SDL_atoi(eq + 1), 1, 4);
        else if (strcmp(line, "lsfg_performance") == 0)
            settings.lsfg_performance = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "lsfg_dll") == 0)
            SDL_strlcpy(settings.lsfg_dll, eq + 1, sizeof settings.lsfg_dll);
        else if (strcmp(line, "fullscreen") == 0)
            settings.fullscreen = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "fxaa") == 0)
            settings.fxaa = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "shadows") == 0)
            settings.shadow_original = strcmp(eq + 1, "low") == 0 || strcmp(eq + 1, "original") == 0;
        else if (strcmp(line, "vsync") == 0)
            settings.vsync = strcmp(eq + 1, "off") == 0 ? PDO_VSYNC_OFF
                           : strcmp(eq + 1, "fast") == 0 ? PDO_VSYNC_FAST : PDO_VSYNC_ON;
        else if (strcmp(line, "window") == 0)        /* x,y,w,h */
            SDL_sscanf(eq + 1, "%d,%d,%d,%d", &settings.win_x, &settings.win_y, &settings.win_w, &settings.win_h);
        else if (strcmp(line, "window_maximized") == 0)
            settings.win_maximized = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "mouse_aim") == 0)
            settings.mouse_aim = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "mouse_speed") == 0)
            settings.mouse_speed = clamp_int(SDL_atoi(eq + 1), 10, 400);
        else if (strcmp(line, "unlock_box") == 0)
            settings.unlock_box = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "skip_logos") == 0)
            settings.skip_logos = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "skip_launcher") == 0)
            settings.skip_launcher = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "rumble") == 0)
            settings.rumble = clamp_int(SDL_atoi(eq + 1), 0, 100);
        else if (strcmp(line, "depth_reversed") == 0)
            settings.depth_reversed = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "picture_crt") == 0)
            settings.picture_crt = SDL_atoi(eq + 1) != 0;
        else if (strcmp(line, "picture_brightness") == 0)
            settings.pic_brightness = clamp_int(SDL_atoi(eq + 1), -50, 50);
        else if (strcmp(line, "picture_contrast") == 0)
            settings.pic_contrast = clamp_int(SDL_atoi(eq + 1), 50, 150);
        else if (strcmp(line, "picture_gamma") == 0)
            settings.pic_gamma = clamp_int(SDL_atoi(eq + 1), 50, 200);
        else if (strcmp(line, "picture_saturation") == 0)
            settings.pic_saturation = clamp_int(SDL_atoi(eq + 1), 0, 200);
        else if (strcmp(line, "audio_3d") == 0)    /* the key kept from when it was 3D only */
            settings.audio_out = strcmp(eq + 1, "headphones") == 0 ? PDO_AUDIO_HEADPHONES
                               : strcmp(eq + 1, "surround") == 0 ? PDO_AUDIO_SURROUND : PDO_AUDIO_STEREO;
        else if (strcmp(line, "save_slot") == 0)
            settings.save_slot = clamp_int(SDL_atoi(eq + 1), 1, 3);
        else
            pdo_input_load_line(line, eq + 1);
    }
    SDL_free(text);
    return true;
}

bool pdo_config_save(const pdo_config *cfg)
{
    char path[PDO_PATH_MAX], text[2 * PDO_PATH_MAX + 2048];
    int n;

    config_path(path, sizeof path);
    n = snprintf(text, sizeof text,
                 "# Orta Recomp settings\n"
                 "game_dir=%s\n"
                 "render_scale=%g\n"
                 "msaa=%d\n"
                 "aspect=%s\n"
                 "fit=%s\n"
                 "soft_filter=%d\n"
                 "master_volume=%d\n"
                 "show_fps=%d\n"
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
                 "shadows=%s\n"
                 "window=%d,%d,%d,%d\n"
                 "window_maximized=%d\n"
                 "mouse_aim=%d\n"
                 "mouse_speed=%d\n"
                 "unlock_box=%d\n"
                 "skip_logos=%d\n"
                 "skip_launcher=%d\n"
                 "rumble=%d\n"
                 "depth_reversed=%d\n"
                 "picture_crt=%d\n"
                 "picture_brightness=%d\n"
                 "picture_contrast=%d\n"
                 "picture_gamma=%d\n"
                 "picture_saturation=%d\n"
                 "audio_3d=%s\n"
                 "save_slot=%d\n",
                 cfg->game_dir, settings.render_scale2 / 2.0, settings.msaa,
                 settings.aspect == PDO_ASPECT_16_9 ? "16:9" : "4:3",
                 settings.fit == PDO_FIT_STRETCH ? "stretch" : settings.fit == PDO_FIT_ZOOM ? "zoom" : "bars",
                 settings.soft_filter,
                 settings.master_volume, settings.show_fps, settings.gpu_vertex,
                 settings.gpu_fixed, settings.frame_interp, settings.anisotropy, settings.bloom == 2 ? "high" : settings.bloom == 1 ? "low" : "off",
                 settings.lsfg_multiplier, settings.lsfg_performance,
                 settings.lsfg_dll, settings.fullscreen, settings.fxaa,
                 settings.vsync == PDO_VSYNC_OFF ? "off" : settings.vsync == PDO_VSYNC_FAST ? "fast" : "on",
                 settings.shadow_original ? "low" : "high",
                 settings.win_x, settings.win_y, settings.win_w, settings.win_h, settings.win_maximized,
                 settings.mouse_aim, settings.mouse_speed, settings.unlock_box, settings.skip_logos, settings.skip_launcher, settings.rumble, settings.depth_reversed,
                 settings.picture_crt, settings.pic_brightness, settings.pic_contrast, settings.pic_gamma,
                 settings.pic_saturation, settings.audio_out == PDO_AUDIO_HEADPHONES ? "headphones"
                 : settings.audio_out == PDO_AUDIO_SURROUND ? "surround" : "speakers",
                 settings.save_slot);
    if (n < 0 || (size_t)n >= sizeof text)
        return false;
    n += (int)pdo_input_save(text + n, sizeof text - (size_t)n);
    return SDL_SaveFile(path, text, (size_t)n);
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

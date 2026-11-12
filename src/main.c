/*
 * Orta Recomp - entry point.
 *
 *   OrtaRecomp                  the launcher: Start game, game files, settings
 *   OrtaRecomp --setup          the launcher, on choosing the game files
 *   OrtaRecomp --game-dir DIR   use DIR for this run (not saved)
 *   OrtaRecomp --import-iso F   import a disc image without the dialogs
 *   OrtaRecomp --check          check the game files, print, exit
 *   OrtaRecomp --boot           play at once, without the launcher
 *   OrtaRecomp --no-boot        set the title up without running it
 *   OrtaRecomp --log FILE       write the log into FILE
 *
 * Release builds always log: to ortarecomp.log in the data folder, the
 * run before kept as ortarecomp-previous.log.
 */

#include "assets.h"
#include "config.h"
#include "setup_ui.h"
#include "gfx/overlay.h"
#include "sha256.h"

#ifdef PDO_HAVE_RECOMP
#include "guest/guest.h"
#include "gfx/renderer.h"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#define APP_NAME "Orta Recomp"

typedef struct options {
    bool        setup, check, boot, no_boot;
    const char *game_dir;
    const char *import_iso;
    const char *log;
} options;

static bool parse_args(int argc, char **argv, options *o)
{
    int i;

    SDL_zerop(o);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--setup") == 0)
            o->setup = true;
        else if (strcmp(argv[i], "--check") == 0)
            o->check = true;
        else if (strcmp(argv[i], "--boot") == 0)
            o->boot = true;
        else if (strcmp(argv[i], "--no-boot") == 0)
            o->no_boot = true;
        else if (strcmp(argv[i], "--game-dir") == 0 && i + 1 < argc)
            o->game_dir = argv[++i];
        else if (strcmp(argv[i], "--import-iso") == 0 && i + 1 < argc)
            o->import_iso = argv[++i];
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            o->log = argv[++i];
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

/* --log FILE: standard output and error go straight into FILE. Piping them
 * through PowerShell (2>&1 | Out-File) turns every line into an error
 * record, slowly; when it falls behind, the game's next write blocks, and
 * a guest thread blocked there can hold the lock the others need: the
 * game froze for seconds at a time, or for good, with the sound going
 * on. */
#ifdef _WIN32
static void SDLCALL log_to_stderr(void *user, int category, SDL_LogPriority priority, const char *message)
{
    (void)user; (void)category;
    fprintf(stderr, "%s%s\n", priority >= SDL_LOG_PRIORITY_ERROR ? "ERROR: "
                              : priority == SDL_LOG_PRIORITY_WARN ? "WARNING: " : "", message);
    fflush(stderr);
}
#endif

static bool open_log(const char *path)
{
#ifdef _WIN32
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif

    if (fd < 0) {
        fprintf(stderr, "cannot write the log %s\n", path);
        return false;
    }
    SDL_strlcpy(pdo_log_file, path, sizeof pdo_log_file);
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    /* A release build is a GUI program: its stdout and stderr start with
     * no file behind them (_fileno -2), and _dup2 onto descriptors 1 and 2
     * then leaves the C streams writing nowhere. Give them one first. */
    {
        FILE *f;
        if (_fileno(stdout) < 0)
            freopen_s(&f, "NUL", "w", stdout);
        if (_fileno(stderr) < 0)
            freopen_s(&f, "NUL", "w", stderr);
    }
    _dup2(fd, _fileno(stdout));
    _dup2(fd, _fileno(stderr));
    _close(fd);
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    /* SDL_Log looks for a parent console and, started from Explorer or
     * Steam, finds none and writes only to the debugger: the log came out
     * empty. Its lines go to stderr here instead. */
    SDL_SetLogOutputFunction(log_to_stderr, NULL);
#else
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    return true;
}

static void report(const char *game_dir, const pdo_game_check *check)
{
    char file_hex[65], image_hex[65];

    pdo_sha256_hex(check->xbe.file_sha256, file_hex);
    pdo_sha256_hex(check->xbe.image_sha256, image_hex);
    printf("game folder : %s\n", game_dir);
    printf("title       : %s (%08X)\n", check->xbe.title, check->xbe.title_id);
    printf("verdict     : %s\n", pdo_xbe_verdict_text(check->verdict));
    printf("xbe sha256  : %s\n", file_hex);
    printf("image sha256: %s\n", image_hex);
    printf("entry point : 0x%08X\n", check->xbe.entry);
    printf("XDK build   : %u\n", check->xbe.xdk_build);
}

static void cli_progress(void *userdata, double fraction, const char *file)
{
    static int last = -1;
    int pct = (int)(fraction * 100);

    if (pct != last) {
        last = pct;
        fprintf(stderr, "\rimporting %3d%%", pct);
        if (pct == 100)
            fputc('\n', stderr);
    }
}

#ifdef PDO_HAVE_RECOMP
/* The game's own thread. The address space is set up on the main thread
 * first (its fixed addresses must be free, and this thread's big stack
 * could land on them); the guest registers that set up are taken over
 * here. The main thread keeps the window and draws (pdo_gfx_main_loop). */
typedef struct game_run {
    pdo_guest_cpu cpu;
    uint32_t      entry;
} game_run;

static int SDLCALL game_thread(void *arg)
{
    game_run *g = arg;

    pdo_guest_cpu_load(&g->cpu);
    pdo_guest_run(g->entry);
    pdo_gfx_game_finished();
    return 0;
}

static int start_guest(const char *game_dir, const pdo_game_check *check, bool boot)
{
    char path[PDO_PATH_MAX], save_dir[PDO_PATH_MAX], image_hex[65];
    void *xbe;
    size_t size;
    int rc = 1;

    pdo_sha256_hex(check->xbe.image_sha256, image_hex);
    if (strcmp(image_hex, pdo_guest_built_from()) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME,
            "This build was recompiled from a different default.xbe than the "
            "one in your game files. Rebuild from your own copy.", NULL);
        return 1;
    }

    snprintf(path, sizeof path, "%s/default.xbe", game_dir);
    snprintf(save_dir, sizeof save_dir, "%ssaves", pdo_data_dir());
    SDL_CreateDirectory(save_dir);

    xbe = SDL_LoadFile(path, &size);
    if (!xbe) {
        SDL_Log("cannot read %s: %s", path, SDL_GetError());
        return 1;
    }
    if (boot && pdo_guest_init(xbe, size, game_dir, save_dir)) {
        game_run g;
        SDL_PropertiesID props;
        SDL_Thread *t;
        char list[PDO_PATH_MAX];

        snprintf(list, sizeof list, "%sopened_files.txt", pdo_data_dir());
        pdo_guest_log_opened_files(list);
        SDL_Log("the game files the title opens are listed in %s", list);
        pdo_guest_cpu_save(&g.cpu);
        g.entry = check->xbe.entry;
        pdo_gfx_prepare_threads();
        props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void *)game_thread);
        SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "game");
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &g);
        /* Generous: the recompiled code nests as deep as the game's calls. */
        SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 64 * 1024 * 1024);
        t = SDL_CreateThreadWithProperties(props);
        SDL_DestroyProperties(props);
        if (t) {
            pdo_gfx_main_loop();
            SDL_WaitThread(t, NULL);
            rc = 0;
        } else {
            SDL_Log("cannot start the game thread: %s", SDL_GetError());
        }
    } else if (!boot && pdo_guest_init(xbe, size, game_dir, save_dir)) {
        SDL_Log("game code is linked and the Xbox address space is set up; "
                "run without --no-boot to start the title");
        rc = 0;
    }
    pdo_guest_shutdown();
    SDL_free(xbe);
    return rc;
}
#endif

/* Shift held while starting: the launcher even with Skip launcher on. No
 * window exists yet, so SDL has no keyboard state to give on every
 * platform; Windows is asked directly. */
static bool shift_held(void)
{
#ifdef _WIN32
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    SDL_PumpEvents();
    return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
#endif
}

/* The launcher: the options menu in its own window before the game starts,
 * with Start game, the game files and Exit (gfx/overlay.c). Settings fixed
 * at start (widescreen, fullscreen; later the graphics driver) are chosen
 * here. True: start the game, with *have_game usable files in cfg. */
static bool run_launcher(pdo_config *cfg, pdo_game_check *check, bool *have_game)
{
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    const pdo_settings *st = pdo_settings_get();
    char shown[PDO_PATH_MAX];
    int choice = PDO_LAUNCH_NONE;

    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!SDL_CreateWindowAndRenderer(APP_NAME, st->win_w >= 320 ? st->win_w : 1280, st->win_h >= 240 ? st->win_h : 960,
                                     SDL_WINDOW_RESIZABLE, &win, &ren)) {
        SDL_Log("launcher: no window (%s); starting the game", SDL_GetError());
        return *have_game;
    }
    SDL_SetRenderVSync(ren, 1);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, PDO_OVERLAY_W, PDO_OVERLAY_H);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    for (;;) {
        const char *base = cfg->game_dir[0] ? SDL_strrchr(cfg->game_dir, '/') : NULL;
        const char *base2 = cfg->game_dir[0] ? SDL_strrchr(cfg->game_dir, '\\') : NULL;
        if (base2 > base)
            base = base2;
        SDL_strlcpy(shown, !cfg->game_dir[0] ? "none" : base && base[1] ? base + 1 : cfg->game_dir, sizeof shown);
        pdo_launcher_begin(shown, *have_game);
        choice = PDO_LAUNCH_NONE;
        while (choice == PDO_LAUNCH_NONE) {
            SDL_Event ev;
            bool changed;
            SDL_Surface *img;
            int w, h;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT)
                    choice = PDO_LAUNCH_EXIT;
                else if (ev.type == SDL_EVENT_GAMEPAD_ADDED)
                    SDL_OpenGamepad(ev.gdevice.which);
                else
                    pdo_overlay_event(&ev);
            }
            if (choice == PDO_LAUNCH_NONE)
                choice = pdo_launcher_poll();
            img = pdo_overlay_image(&changed);
            if (img && changed)
                SDL_UpdateTexture(tex, NULL, img->pixels, img->pitch);
            /* A dark backdrop, the menu over it at 4:3, as large as fits. */
            SDL_GetRenderOutputSize(ren, &w, &h);
            SDL_SetRenderDrawColor(ren, 6, 9, 16, 255);
            SDL_RenderClear(ren);
            {
                float k = SDL_min((float)w / PDO_OVERLAY_W, (float)h / PDO_OVERLAY_H);
                SDL_FRect r = { (w - PDO_OVERLAY_W * k) / 2, (h - PDO_OVERLAY_H * k) / 2, PDO_OVERLAY_W * k,
                                PDO_OVERLAY_H * k };
                SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
                SDL_RenderTexture(ren, tex, NULL, &r);
            }
            SDL_RenderPresent(ren);
        }
        pdo_launcher_end();
        if (choice != PDO_LAUNCH_PICK)
            break;
        /* The game file picker (setup_ui.c), its own window and dialogs. */
        SDL_HideWindow(win);
        {
            pdo_config pick = *cfg;
            pdo_game_check pc;
            if (pdo_setup_run(&pick, &pc)) {
                *cfg = pick;
                *check = pc;
                *have_game = true;
            }
        }
        SDL_ShowWindow(win);
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    return choice == PDO_LAUNCH_START && *have_game;
}

int main(int argc, char **argv)
{
    options opt;
    pdo_config cfg;
    pdo_game_check check;
    char err[1024];
    bool have_game = false;

    if (!parse_args(argc, argv, &opt))
        return 2;
    if (opt.log && !open_log(opt.log))
        return 2;
#ifdef PDO_RELEASE
    if (!opt.log) {
        char log[PDO_PATH_MAX], old[PDO_PATH_MAX];
        snprintf(log, sizeof log, "%sortarecomp.log", pdo_data_dir());
        snprintf(old, sizeof old, "%sortarecomp-previous.log", pdo_data_dir());
        SDL_RemovePath(old);
        SDL_RenamePath(log, old);
        open_log(log);
    }
#endif
    SDL_SetAppMetadata(APP_NAME, PDO_VERSION, "io.github.ortarecomp");
    SDL_Log("%s %s (built %s), %s, %d logical cores, %d MB RAM", APP_NAME, PDO_VERSION, __DATE__,
            SDL_GetPlatform(), SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM());

    if (!SDL_Init(opt.check || opt.import_iso ? 0 : SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    pdo_config_load(&cfg);
    {
        const pdo_settings *s = pdo_settings_get();
        SDL_Log("settings: %gx, msaa %d, fxaa %d, anisotropy %d, bloom %d, frame interp %d, vertex programs on %s, fixed function on %s, "
                "aspect %s, save file %d", s->render_scale2 / 2.0, s->msaa, s->fxaa, s->anisotropy, s->bloom, s->frame_interp,
                s->gpu_vertex ? "GPU" : "CPU", s->gpu_fixed ? "GPU" : "CPU",
                s->aspect == PDO_ASPECT_16_9 ? "16:9" : "4:3", s->save_slot);
    }

    if (opt.import_iso) {
        char dst[PDO_PATH_MAX];
        pdo_assets_default_import_dir(dst, sizeof dst);
        if (!pdo_assets_import_iso(opt.import_iso, dst, cli_progress, NULL, &check, err, sizeof err)) {
            fprintf(stderr, "import failed: %s\n", err);
            SDL_Quit();
            return 1;
        }
        SDL_strlcpy(cfg.game_dir, dst, sizeof cfg.game_dir);
        pdo_config_save(&cfg);
        report(cfg.game_dir, &check);
        SDL_Quit();
        return 0;
    }

    if (opt.game_dir)
        SDL_strlcpy(cfg.game_dir, opt.game_dir, sizeof cfg.game_dir);


    if (!opt.setup && cfg.game_dir[0]) {
        have_game = pdo_assets_check_dir(cfg.game_dir, &check, err, sizeof err);
        if (!have_game)
            SDL_Log("game files at %s are not usable: %s", cfg.game_dir, err);
    }

    if (opt.check) {
        if (have_game)
            report(cfg.game_dir, &check);
        else
            fprintf(stderr, "no usable game files%s%s\n", cfg.game_dir[0] ? ": " : "",
                    cfg.game_dir[0] ? err : "");
        SDL_Quit();
        return have_game ? 0 : 1;
    }

    /* --boot (or --no-boot) goes straight on, as before: the first-run
     * picker if there are no game files. Otherwise the launcher. */
    if (opt.boot || opt.no_boot) {
        if (!have_game && !pdo_setup_run(&cfg, &check)) {
            SDL_Quit();
            return 0;
        }
    } else if (pdo_settings_get()->skip_launcher && have_game && !shift_held()) {
        SDL_Log("launcher: skipped (Skip launcher is on; hold Shift while starting to see it)");
    } else if (!run_launcher(&cfg, &check, &have_game)) {
        SDL_Quit();
        return 0;
    }

#ifdef PDO_HAVE_RECOMP
    {
        int rc;
        rc = start_guest(cfg.game_dir, &check, !opt.no_boot);
        SDL_Quit();
        return rc;
    }
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, APP_NAME,
        "Your game files are set up.\n\nThis build has no recompiled game code "
        "in it yet. See BUILDING.md to generate it from your default.xbe.", NULL);
    SDL_Quit();
    return 0;
#endif
}

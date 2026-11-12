/*
 * Options overlay and frame counter.
 *
 * F1, or Back and Start together on a controller, opens the menu; the game
 * waits while it is open (the renderer holds it in Swap). Up/Down choose a
 * line, Left/Right (or A/Enter) change it, B/Escape/F1 close it. Changes take
 * effect at once (render scale and anti-aliasing from the next game frame)
 * and are saved when the menu closes.
 *
 * Text is SDL's built-in 8x8 debug font, drawn by a software renderer into a
 * surface, so no font or GPU code of our own is needed.
 */

#include "overlay.h"

#include "../config.h"
#include "../input_map.h"
#include "renderer.h"
#include "../audio/mixer.h"
#ifdef PDO_HAVE_LSFG
#include "lsfg_present.h"
#endif

#include <stdio.h>
#include <string.h>

enum { ITEM_SCALE, ITEM_MSAA, ITEM_ANISO, ITEM_ASPECT, ITEM_FIT, ITEM_SOFT, ITEM_VSYNC, ITEM_SHADOWS, ITEM_VOLUME,
       ITEM_FPS, ITEM_VSH, ITEM_FVF, ITEM_INTERP, ITEM_LSMODE, ITEM_LSDLL, ITEM_BOX, ITEM_LOGOS, ITEM_LAUNCHER, ITEM_RUMBLE, ITEM_3D,
       ITEM_CONTROLS, ITEM_CLOSE, ITEM_QUIT,
       /* the launcher's own */
       ITEM_START, ITEM_PICK, ITEM_SLOT, ITEM_GPU, ITEM_FULLSCREEN, ITEM_EXIT,
       /* Picture (present.frag) */
       ITEM_PIC_LOOK, ITEM_PIC_BLOOM, ITEM_PIC_BRIGHT, ITEM_PIC_CONTRAST, ITEM_PIC_GAMMA, ITEM_PIC_SAT, ITEM_PIC_RESET,
       /* links to the pages, and back */
       ITEM_GO_DISPLAY, ITEM_GO_PICTURE, ITEM_GO_SOUND, ITEM_GO_GAMEPLAY, ITEM_GO_ADVANCED, ITEM_BACK,
       ITEM_COUNT };

/* The menu is pages: a short main list (the game's, or the launcher's
 * before the game starts) of links, and a page per subject. */
enum { PAGE_MAIN, PAGE_DISPLAY, PAGE_PICTURE, PAGE_SOUND, PAGE_GAMEPLAY, PAGE_ADVANCED, PAGE_COUNT };
static const char *page_names[PAGE_COUNT] = { "OPTIONS", "DISPLAY", "PICTURE", "SOUND", "GAMEPLAY", "ADVANCED" };

static const int game_main[] = { ITEM_GO_DISPLAY, ITEM_GO_PICTURE, ITEM_GO_SOUND, ITEM_GO_GAMEPLAY,
                                 ITEM_GO_ADVANCED, ITEM_CONTROLS, ITEM_CLOSE, ITEM_QUIT };
static const int launcher_main[] = { ITEM_START, ITEM_PICK, ITEM_SLOT, ITEM_GO_DISPLAY, ITEM_GO_PICTURE, ITEM_GO_SOUND,
                                     ITEM_GO_GAMEPLAY, ITEM_GO_ADVANCED, ITEM_CONTROLS, ITEM_EXIT };
static const int display_game[] = { ITEM_SCALE, ITEM_MSAA, ITEM_ANISO, ITEM_ASPECT, ITEM_FIT, ITEM_SOFT, ITEM_VSYNC, ITEM_SHADOWS,
                                    ITEM_FPS, ITEM_BACK };
static const int display_launcher[] = { ITEM_SCALE, ITEM_MSAA, ITEM_ANISO, ITEM_ASPECT, ITEM_FIT, ITEM_FULLSCREEN, ITEM_SOFT,
                                        ITEM_VSYNC, ITEM_SHADOWS, ITEM_FPS, ITEM_BACK };
static const int picture_items[] = { ITEM_PIC_LOOK, ITEM_PIC_BLOOM, ITEM_PIC_BRIGHT, ITEM_PIC_CONTRAST, ITEM_PIC_GAMMA, ITEM_PIC_SAT,
                                     ITEM_PIC_RESET, ITEM_BACK };
static const int sound_items[] = { ITEM_VOLUME, ITEM_3D, ITEM_BACK };
static const int gameplay_items[] = { ITEM_BOX, ITEM_LOGOS, ITEM_LAUNCHER, ITEM_RUMBLE, ITEM_BACK };
static const int advanced_game[] = { ITEM_INTERP, ITEM_LSMODE, ITEM_LSDLL, ITEM_VSH, ITEM_FVF, ITEM_BACK };
static const int advanced_launcher[] = { ITEM_INTERP, ITEM_LSMODE, ITEM_LSDLL, ITEM_VSH, ITEM_FVF, ITEM_GPU,
                                         ITEM_BACK };

static bool launcher;               /* pdo_launcher_begin .. end */
static int launcher_choice;         /* PDO_LAUNCH_* picked, 0 none yet */
static const char *launcher_files;  /* the game files, as shown */
static bool launcher_files_ok;
static int page;                    /* PAGE_* shown */

static const int *page_items(int *n)
{
#define PAGE_OF(a) (*n = (int)SDL_arraysize(a), a)
    switch (page) {
    case PAGE_DISPLAY:  return launcher ? PAGE_OF(display_launcher) : PAGE_OF(display_game);
    case PAGE_PICTURE:  return PAGE_OF(picture_items);
    case PAGE_SOUND:    return PAGE_OF(sound_items);
    case PAGE_GAMEPLAY: return PAGE_OF(gameplay_items);
    case PAGE_ADVANCED: return launcher ? PAGE_OF(advanced_launcher) : PAGE_OF(advanced_game);
    default:            return launcher ? PAGE_OF(launcher_main) : PAGE_OF(game_main);
    }
#undef PAGE_OF
}

static int nitems(void)
{
    int n;
    page_items(&n);
    return n;
}
#define ITEMS (page_items(&(int){ 0 }))
#define NITEMS nitems()

/* The Controls page: a line per Xbox button, then these two. While a line
 * waits for its new key or controller button, capture is set. */
enum { CTL_MOUSE = PDO_IN_COUNT, CTL_MSPEED, CTL_RESET, CTL_BACK, CTL_COUNT };
static bool controls, capture;

static bool open_, dirty = true, swallow;
static int selected;
static bool start_held, back_held;
static SDL_Surface *image;
static SDL_Renderer *painter;

/* Frame counter: frames presented in the last whole second. */
static Uint64 count_start;
static int counted, fps = -1;

bool pdo_overlay_open(void)
{
    return open_;
}

void pdo_launcher_begin(const char *files, bool ok)
{
    launcher = open_ = dirty = true;
    controls = capture = false;
    launcher_files = files;
    launcher_files_ok = ok;
    launcher_choice = PDO_LAUNCH_NONE;
    page = PAGE_MAIN;
    selected = ok ? 0 : 1;          /* no usable files: on Game files */
}

int pdo_launcher_poll(void)
{
    int c = launcher_choice;
    launcher_choice = PDO_LAUNCH_NONE;
    return c;
}

void pdo_launcher_end(void)
{
    launcher = open_ = false;
    dirty = true;
    pdo_settings_save();
}

static bool quit_armed;
static void ask_quit(void);

static void set_open(bool on)
{
    if (open_ == on)
        return;
    open_ = on;
    dirty = true;
    quit_armed = false;
    if (on) {
        selected = 0;
        page = PAGE_MAIN;
        controls = capture = false;
    }
    else {
        swallow = true;
        pdo_settings_save();
    }
}

/* Frame generation steps: off, our blending (renderer.c), Lossless
 * Scaling's at 2x, 3x, 4x (lsfg_present.cpp). */
static int frame_gen_step(const pdo_settings *s)
{
    return s->lsfg_multiplier >= 2 ? s->lsfg_multiplier : s->frame_interp ? 1 : 0;
}

static void next_frame_gen(pdo_settings *s, int dir)
{
    int i = SDL_clamp(frame_gen_step(s) + dir, 0, 4);
    s->frame_interp = i == 1;
    s->lsfg_multiplier = i >= 2 ? i : 1;
}

/* Lossless.dll from a file dialog: its answer can come on another thread,
 * so it waits here until the menu takes it. */
static char picked_dll[PDO_PATH_MAX];
static SDL_AtomicInt dll_picked;

static void SDLCALL dll_dialog_cb(void *userdata, const char *const *files, int filter)
{
    (void)userdata;
    (void)filter;
    if (files && files[0]) {
        SDL_strlcpy(picked_dll, files[0], sizeof picked_dll);
        SDL_SetAtomicInt(&dll_picked, 1);
    }
}

static void pick_dll(void)
{
    static const SDL_DialogFileFilter filters[] = { { "Lossless.dll", "dll" }, { "All files", "*" } };
    SDL_ShowOpenFileDialog(dll_dialog_cb, NULL, NULL, filters, SDL_arraysize(filters), NULL, false);
}

static void take_picked_dll(void)
{
    if (SDL_GetAtomicInt(&dll_picked)) {
        SDL_SetAtomicInt(&dll_picked, 0);
        SDL_strlcpy(pdo_settings_get()->lsfg_dll, picked_dll, sizeof pdo_settings_get()->lsfg_dll);
        dirty = true;
        pdo_settings_save();
    }
}

/* Anti-aliasing steps: off, FXAA (cheap, on the finished frame), then
 * MSAA 2x, 4x, 8x. */
static void next_aa(pdo_settings *s, int dir)
{
    static const struct { int msaa; bool fxaa; } steps[] = { { 1, false }, { 1, true }, { 2, false }, { 4, false },
                                                              { 8, false } };
    int i = 0, n = (int)SDL_arraysize(steps);
    while (i < n - 1 && !(steps[i].msaa == s->msaa && steps[i].fxaa == (s->fxaa && s->msaa == 1)))
        i++;
    i = SDL_clamp(i + dir, 0, n - 1);
    s->msaa = steps[i].msaa;
    s->fxaa = steps[i].fxaa;
}

static int index_of(int item);

/* To a page, on its first line; back to the main list, on that page's link. */
static void go_page(int p)
{
    page = p;
    selected = 0;
    dirty = true;
}

static void page_back(void)
{
    static const int link[PAGE_COUNT] = { 0, ITEM_GO_DISPLAY, ITEM_GO_PICTURE, ITEM_GO_SOUND, ITEM_GO_GAMEPLAY,
                                          ITEM_GO_ADVANCED };
    int from = page;
    page = PAGE_MAIN;
    selected = index_of(link[from]);
    dirty = true;
}

/* Change the selected line by one step: dir -1 or +1. */
static void change(int dir)
{
    pdo_settings *s = pdo_settings_get();

    switch (ITEMS[selected]) {
    case ITEM_START:  if (dir > 0 && launcher_files_ok) launcher_choice = PDO_LAUNCH_START; break;
    case ITEM_PICK:   if (dir > 0) launcher_choice = PDO_LAUNCH_PICK; break;
    case ITEM_EXIT:   if (dir > 0) launcher_choice = PDO_LAUNCH_EXIT; break;
    case ITEM_SLOT:   s->save_slot = (s->save_slot - 1 + 3 + dir) % 3 + 1; break;
    case ITEM_FULLSCREEN: s->fullscreen = !s->fullscreen; break;
    case ITEM_GPU:    break;      /* Vulkan only, for now: the shaders are SPIR-V */
    case ITEM_SCALE: {
        /* 1x, 1.5x (for integrated graphics, between 1x and 2x's four
         * times the pixels), then whole steps to 8x; in halves. */
        static const int steps[] = { 2, 3, 4, 6, 8, 10, 12, 14, 16 };
        int i = 0, n = (int)SDL_arraysize(steps);
        while (i < n - 1 && steps[i] < s->render_scale2)
            i++;
        s->render_scale2 = steps[SDL_clamp(i + dir, 0, n - 1)];
        break;
    }
    case ITEM_MSAA:   next_aa(s, dir); break;
    case ITEM_ANISO: {      /* off, 2x, 4x, 8x, 16x */
        int i = s->anisotropy >= 16 ? 4 : s->anisotropy >= 8 ? 3 : s->anisotropy >= 4 ? 2 : s->anisotropy >= 2 ? 1 : 0;
        i = SDL_clamp(i + dir, 0, 4);
        s->anisotropy = 1 << i;
        break;
    }
    case ITEM_ASPECT: s->aspect = (s->aspect + PDO_ASPECT_COUNT + dir) % PDO_ASPECT_COUNT; break;
    case ITEM_FIT:    s->fit = (s->fit + PDO_FIT_COUNT + dir) % PDO_FIT_COUNT; break;
    case ITEM_SOFT:   s->soft_filter = !s->soft_filter; break;
    case ITEM_VOLUME: s->master_volume = SDL_clamp(s->master_volume + 5 * dir, 0, 100); break;
    case ITEM_BOX:    s->unlock_box = !s->unlock_box; break;
    case ITEM_LOGOS:  s->skip_logos = !s->skip_logos; break;
    case ITEM_LAUNCHER: s->skip_launcher = !s->skip_launcher; break;
    case ITEM_VSYNC:  s->vsync = (s->vsync + (dir < 0 ? 2 : 1)) % 3; break;
    case ITEM_SHADOWS: s->shadow_original = !s->shadow_original; break;
    case ITEM_RUMBLE: s->rumble = SDL_clamp((s->rumble + 10 * dir) / 10 * 10, 0, 100); break;
    case ITEM_3D:
        s->audio_out = (s->audio_out + PDO_AUDIO_COUNT + dir) % PDO_AUDIO_COUNT;
        pdo_mixer_apply_output();
        break;
    case ITEM_FPS:    s->show_fps = !s->show_fps; break;
    case ITEM_VSH:    s->gpu_vertex = !s->gpu_vertex; break;
    case ITEM_FVF:    s->gpu_fixed = !s->gpu_fixed; break;
    case ITEM_INTERP: next_frame_gen(s, dir); break;
    case ITEM_LSMODE: s->lsfg_performance = !s->lsfg_performance; break;
    case ITEM_LSDLL:  if (dir > 0) pick_dll(); else s->lsfg_dll[0] = 0; break;
    case ITEM_CONTROLS: if (dir > 0) { controls = true; selected = 0; } break;
    case ITEM_CLOSE:  if (dir > 0) set_open(false); break;
    case ITEM_QUIT:   if (dir > 0) ask_quit(); break;
    case ITEM_PIC_LOOK:     s->picture_crt = !s->picture_crt; break;
    case ITEM_PIC_BLOOM:    s->bloom = SDL_clamp(s->bloom + dir, 0, 2); break;
    case ITEM_PIC_BRIGHT:   s->pic_brightness = SDL_clamp(s->pic_brightness + 5 * dir, -50, 50); break;
    case ITEM_PIC_CONTRAST: s->pic_contrast = SDL_clamp(s->pic_contrast + 5 * dir, 50, 150); break;
    case ITEM_PIC_GAMMA:    s->pic_gamma = SDL_clamp(s->pic_gamma + 5 * dir, 50, 200); break;
    case ITEM_PIC_SAT:      s->pic_saturation = SDL_clamp(s->pic_saturation + 5 * dir, 0, 200); break;
    case ITEM_PIC_RESET:
        if (dir > 0) {
            s->picture_crt = false;
            s->bloom = 0;
            s->pic_brightness = 0;
            s->pic_contrast = s->pic_gamma = s->pic_saturation = 100;
        }
        break;
    case ITEM_GO_DISPLAY:  if (dir > 0) go_page(PAGE_DISPLAY); break;
    case ITEM_GO_PICTURE:  if (dir > 0) go_page(PAGE_PICTURE); break;
    case ITEM_GO_SOUND:    if (dir > 0) go_page(PAGE_SOUND); break;
    case ITEM_GO_GAMEPLAY: if (dir > 0) go_page(PAGE_GAMEPLAY); break;
    case ITEM_GO_ADVANCED: if (dir > 0) go_page(PAGE_ADVANCED); break;
    case ITEM_BACK:        if (dir > 0) page_back(); break;
    }
    dirty = true;
}

static int index_of(int item)
{
    for (int i = 0; i < NITEMS; i++)
        if (ITEMS[i] == item)
            return i;
    return 0;
}

/* Quit game asks twice (quit_armed): the game keeps only what it last
 * saved. Moving off the line, or closing the menu, takes the first press
 * back. */

static void ask_quit(void)
{
    SDL_Event ev;

    if (!quit_armed) {
        quit_armed = true;
        dirty = true;
        return;
    }
    /* As closing the window: settings saved, then the process ends. */
    SDL_zero(ev);
    ev.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&ev);
}

static void move(int dir)
{
    int n = controls ? CTL_COUNT : NITEMS;
    quit_armed = false;
    selected = (selected + n + dir) % n;
    dirty = true;
}

/* Controls page: OK on a button waits for its new key or controller input;
 * Left clears the line's key and controller input. */
static void control_act(int a, int dir)
{
    pdo_settings *st = pdo_settings_get();

    if (selected == CTL_MOUSE) {
        st->mouse_aim = !st->mouse_aim;
    } else if (selected == CTL_MSPEED) {
        st->mouse_speed = SDL_clamp(st->mouse_speed + 10 * dir, 10, 400);
    } else if (selected == CTL_BACK || (a < 0 && dir < 0 && selected >= PDO_IN_COUNT)) {
        controls = false;
        page = PAGE_MAIN;
        selected = index_of(ITEM_CONTROLS);
    } else if (selected == CTL_RESET) {
        if (dir > 0)
            pdo_input_reset();
    } else if (dir > 0) {
        capture = true;
    } else {
        pdo_input_bindings()[selected].key = SDL_SCANCODE_UNKNOWN;
        pdo_input_bindings()[selected].pad = -1;
        pdo_input_bindings()[selected].mouse = 0;
    }
    dirty = true;
}

/* A gamepad button or key, mapped to a menu action. */
enum { ACT_NONE, ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_OK, ACT_BACK };

static void act(int a)
{
    if (controls) {
        switch (a) {
        case ACT_UP:    move(-1); break;
        case ACT_DOWN:  move(+1); break;
        case ACT_LEFT:  control_act(-1, -1); break;
        case ACT_RIGHT: case ACT_OK: control_act(0, +1); break;
        case ACT_BACK:  controls = false; page = PAGE_MAIN; selected = index_of(ITEM_CONTROLS); dirty = true; break;
        }
        return;
    }
    switch (a) {
    case ACT_UP:    move(-1); break;
    case ACT_DOWN:  move(+1); break;
    case ACT_LEFT:  change(-1); break;
    case ACT_RIGHT: change(+1); break;
    case ACT_OK:    change(+1); break;
    case ACT_BACK:
        if (page != PAGE_MAIN)
            page_back();
        else if (!launcher)
            set_open(false);
        break;
    }
}

/* While a Controls line waits: the next key or controller input is its new
 * binding (Escape cancels). True: the event was taken. */
static bool capture_event(const SDL_Event *ev)
{
    pdo_binding *b = &pdo_input_bindings()[selected];

    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) {
        if (ev->key.key != SDLK_ESCAPE && ev->key.key != SDLK_F1)
            b->key = ev->key.scancode;
    } else if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        b->pad = ev->gbutton.button;
    } else if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.button >= 1 && ev->button.button <= 5) {
        b->mouse = ev->button.button;     /* left, middle, right, side buttons (x1, x2) */
    } else if (ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
               (ev->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || ev->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
               ev->gaxis.value > 16000) {
        b->pad = PDO_PAD_AXIS + ev->gaxis.axis;
    } else {
        return ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP || ev->type == SDL_EVENT_KEY_UP ||
               ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION || ev->type == SDL_EVENT_MOUSE_BUTTON_UP;
    }
    capture = false;
    dirty = true;
    return true;
}

bool pdo_overlay_event(const SDL_Event *ev)
{
    if (open_ && capture)
        return capture_event(ev);
    if (ev->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode k = ev->key.key;
        if (k == SDLK_F1 && !ev->key.repeat) {
            if (!launcher)
                set_open(!open_);
            return true;
        }
        if (!open_)
            return false;
        act(k == SDLK_UP ? ACT_UP : k == SDLK_DOWN ? ACT_DOWN : k == SDLK_LEFT ? ACT_LEFT
            : k == SDLK_RIGHT ? ACT_RIGHT : k == SDLK_RETURN || k == SDLK_SPACE ? ACT_OK
            : k == SDLK_ESCAPE || k == SDLK_BACKSPACE ? ACT_BACK : ACT_NONE);
        return true;
    }
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        bool down = ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        Uint8 b = ev->gbutton.button;

        if (b == SDL_GAMEPAD_BUTTON_START)
            start_held = down;
        if (b == SDL_GAMEPAD_BUTTON_BACK)
            back_held = down;
        if (down && start_held && back_held) {
            if (!launcher)
                set_open(!open_);
            return true;
        }
        if (!open_ || !down)
            return open_;
        act(b == SDL_GAMEPAD_BUTTON_DPAD_UP ? ACT_UP : b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? ACT_DOWN
            : b == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? ACT_LEFT : b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? ACT_RIGHT
            : b == SDL_GAMEPAD_BUTTON_SOUTH ? ACT_OK : b == SDL_GAMEPAD_BUTTON_EAST ? ACT_BACK : ACT_NONE);
        return true;
    }
    return false;
}

void pdo_overlay_frame(void)
{
    Uint64 now = SDL_GetTicks();

    take_picked_dll();

    if (!count_start)
        count_start = now;
    counted++;
    if (now - count_start >= 1000) {
        int f = (int)((Uint64)counted * 1000 / (now - count_start));
        if (f != fps)
            dirty = true;
        fps = f;
        counted = 0;
        count_start = now;
    }
}

bool pdo_overlay_visible(void)
{
    return open_ || pdo_settings_get()->show_fps;
}

bool pdo_overlay_blocks_input(bool any_pressed)
{
    if (open_)
        return true;
    if (swallow && any_pressed)
        return true;
    swallow = false;
    return false;
}

/* ── Drawing ─────────────────────────────────────────────────────────── */

#define SCALE 2.0f      /* 8x8 font at 16x16 on the 640x480 image */
#define CHAR  8.0f      /* in unscaled units: the layer is 40 x 30 of them */

/* The look: a dark panel, an amber for the title bar and what is chosen,
 * a sky blue for the rule and the bars of sliders. */
#define COL_PANEL    14, 18, 30, 238
#define COL_EDGE     70, 82, 110, 255
#define COL_TITLE    232, 150, 40, 255
#define COL_TITLE_TX 22, 20, 28, 255
#define COL_ACCENT   70, 160, 230, 255
#define COL_SEL      36, 56, 90, 255
#define COL_TEXT     206, 212, 224, 255
#define COL_TEXT_HI  255, 255, 255, 255
#define COL_DIM      138, 148, 166, 255
#define COL_OFF      96, 104, 120, 255
#define COL_TRACK    52, 60, 80, 255

static float cols = 40;     /* the layer's width in characters: more in 16:9 (drawn narrower) */

static void color(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_SetRenderDrawColor(painter, r, g, b, a);
}
#define COLOR(c) color(c)

static void text(float col, float row, const char *s)
{
    SDL_RenderDebugText(painter, col * CHAR, row * CHAR, s);
}

static void fill(float col, float row, float w, float h)
{
    SDL_FRect r = { col * CHAR, row * CHAR, w * CHAR, h * CHAR };
    SDL_RenderFillRect(painter, &r);
}

/* The page's frame: the game dimmed behind, the panel, its title bar with
 * the name on the left and the page on the right, and a blue rule. */
static void frame(float x0, float y0, float w, float h, const char *page_name)
{
    SDL_FRect r;

    SDL_SetRenderDrawBlendMode(painter, SDL_BLENDMODE_NONE);
    color(0, 0, 0, 110);
    fill(0, 0, cols, 30);
    COLOR(COL_PANEL);
    fill(x0, y0, w, h);
    COLOR(COL_EDGE);
    r = (SDL_FRect){ x0 * CHAR - 1, y0 * CHAR - 1, w * CHAR + 2, h * CHAR + 2 };
    SDL_RenderRect(painter, &r);
    COLOR(COL_TITLE);
    fill(x0, y0, w, 2.5f);
    COLOR(COL_ACCENT);
    fill(x0, y0 + 2.5f, w, 0.25f);
    COLOR(COL_TITLE_TX);
    text(x0 + 1.5f, y0 + 0.75f, "ORTA RECOMP");
    text(x0 + w - 1.5f - (float)strlen(page_name), y0 + 0.75f, page_name);
}

/* Up to two lines of wrapped help, cols wide. */
static void help(float col, float row, int cols, const char *s)
{
    char line[64];
    for (int l = 0; l < 2 && *s; l++) {
        int n = (int)strlen(s), cut = n;
        if (n > cols) {
            cut = cols;
            while (cut > 0 && s[cut] != ' ')
                cut--;
            if (cut == 0)
                cut = cols;
        }
        snprintf(line, sizeof line, "%.*s", cut, s);
        text(col, row + l * 1.25f, line);
        s += cut;
        while (*s == ' ')
            s++;
    }
}

static void draw_controls(void)
{
    const pdo_binding *b = pdo_input_bindings();
    const float w = 38, x0 = (cols - w) / 2, y0 = 0.75f, h = 28.5f;

    frame(x0, y0, w, h, "CONTROLS");
    COLOR(COL_DIM);
    text(x0 + 3, y0 + 3.5f, "button    key       pad       mouse");
    for (int i = 0; i < CTL_COUNT; i++) {
        float y = y0 + 5 + i * 1.0f + (i >= PDO_IN_COUNT ? 0.75f : 0);
        char line[64];
        if (i < PDO_IN_COUNT) {
            const char *k = b[i].key != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(b[i].key) : "-";
            snprintf(line, sizeof line, "%-9s %-9.9s %-9.9s %.6s", pdo_input_button_name(i), k[0] ? k : "?",
                     pdo_input_pad_label(b[i].pad), pdo_input_mouse_name(b[i].mouse));
            if (i == selected && capture)
                snprintf(line, sizeof line, "%-9s press a key/button/click", pdo_input_button_name(i));
        } else if (i == CTL_MOUSE) {
            snprintf(line, sizeof line, "%-9s %s", "Mouse aim", pdo_settings_get()->mouse_aim ? "On" : "Off");
        } else if (i == CTL_MSPEED) {
            snprintf(line, sizeof line, "%-9s %d%%", "Mouse spd", pdo_settings_get()->mouse_speed);
        } else {
            snprintf(line, sizeof line, "%s", i == CTL_RESET ? "Reset to defaults" : "Back");
        }
        if (i == selected) {
            COLOR(COL_SEL);
            fill(x0 + 0.5f, y - 0.0625f, w - 1, 1.125f);
            COLOR(COL_TITLE);
            fill(x0 + 0.5f, y - 0.0625f, 0.25f, 1.125f);
            COLOR(COL_TEXT_HI);
        } else
            COLOR(COL_TEXT);
        text(x0 + 2, y, line);
    }
    COLOR(COL_DIM);
    text(x0 + 1.5f, y0 + h - 2.25f, "A/Enter rebind  Left clear");
    text(x0 + 1.5f, y0 + h - 1.25f, "B/Esc back      Esc frees the mouse");
}

/* A line's value as shown, and for a slider its fill (0..1), else -1. */
static float value_of(int item, char *v, size_t n)
{
    const pdo_settings *s = pdo_settings_get();
    static const char *aspects[PDO_ASPECT_COUNT] = { "4:3", "16:9" };
    static const char *fits[PDO_FIT_COUNT] = { "Bars", "Stretch", "Zoom" };

    v[0] = 0;
    switch (item) {
    case ITEM_SCALE:  snprintf(v, n, "%gx", s->render_scale2 / 2.0); break;
    case ITEM_MSAA:
        if (s->msaa > 1) snprintf(v, n, "%dx MSAA", s->msaa);
        else snprintf(v, n, "%s", s->fxaa ? "FXAA" : "Off");
        break;
    case ITEM_ANISO:
        if (s->anisotropy > 1) snprintf(v, n, "%dx", s->anisotropy);
        else snprintf(v, n, "Off");
        break;
    case ITEM_ASPECT: snprintf(v, n, "%s", aspects[s->aspect]); break;
    case ITEM_FIT:    snprintf(v, n, "%s", fits[s->fit]); break;
    case ITEM_SOFT:   snprintf(v, n, "%s", s->soft_filter ? "On" : "Off"); break;
    case ITEM_FPS:    snprintf(v, n, "%s", s->show_fps ? "On" : "Off"); break;
    case ITEM_FULLSCREEN: snprintf(v, n, "%s", s->fullscreen ? "On" : "Off"); break;
    case ITEM_VOLUME:  snprintf(v, n, "%d%%", s->master_volume); return s->master_volume / 100.0f;
    case ITEM_PIC_LOOK:     snprintf(v, n, "%s", s->picture_crt ? "CRT" : "Original"); break;
    case ITEM_PIC_BLOOM: {
        static const char *levels[] = { "Off", "Low", "High" };
        snprintf(v, n, "%s", levels[SDL_clamp(s->bloom, 0, 2)]);
        break;
    }
    case ITEM_PIC_BRIGHT:   snprintf(v, n, "%+d", s->pic_brightness); return (s->pic_brightness + 50) / 100.0f;
    case ITEM_PIC_CONTRAST: snprintf(v, n, "%d%%", s->pic_contrast); return (s->pic_contrast - 50) / 100.0f;
    case ITEM_PIC_GAMMA:    snprintf(v, n, "%d.%02d", s->pic_gamma / 100, s->pic_gamma % 100);
                            return (s->pic_gamma - 50) / 150.0f;
    case ITEM_PIC_SAT:      snprintf(v, n, "%d%%", s->pic_saturation); return s->pic_saturation / 200.0f;
    case ITEM_BOX:    snprintf(v, n, "%s", s->unlock_box ? "All open" : "Normal"); break;
    case ITEM_LOGOS:  snprintf(v, n, "%s", s->skip_logos ? "On" : "Off"); break;
    case ITEM_LAUNCHER: snprintf(v, n, "%s", s->skip_launcher ? "On" : "Off"); break;
    case ITEM_VSYNC:  snprintf(v, n, "%s", s->vsync == PDO_VSYNC_OFF ? "Off" : s->vsync == PDO_VSYNC_FAST ? "Fast" : "On"); break;
    case ITEM_SHADOWS: snprintf(v, n, "%s", s->shadow_original ? "Low" : "High"); break;
    case ITEM_RUMBLE:
        if (s->rumble)
            snprintf(v, n, "%d%%", s->rumble);
        else
            snprintf(v, n, "Off");
        return s->rumble / 100.0f;
    case ITEM_3D: {
        static const char *outs[PDO_AUDIO_COUNT] = { "Stereo", "Headphones", "5.1" };
        snprintf(v, n, "%s", outs[s->audio_out]);
        break;
    }
    case ITEM_VSH:    snprintf(v, n, "%s", s->gpu_vertex ? "GPU" : "CPU"); break;
    case ITEM_FVF:    snprintf(v, n, "%s", s->gpu_fixed ? "GPU" : "CPU"); break;
    case ITEM_INTERP: {
        static const char *steps[] = { "Off", "Interp.", "LS 2x", "LS 3x", "LS 4x" };
        snprintf(v, n, "%s", steps[frame_gen_step(s)]);
        break;
    }
    case ITEM_LSMODE: snprintf(v, n, "%s", s->lsfg_performance ? "Performance" : "Quality"); break;
    case ITEM_LSDLL: {
#ifdef PDO_HAVE_LSFG
        char found[PDO_PATH_MAX];
        if (s->lsfg_dll[0]) {
            const char *f = s->lsfg_dll + strlen(s->lsfg_dll);
            while (f > s->lsfg_dll && f[-1] != '/' && f[-1] != '\\')
                f--;
            snprintf(v, n, "%.14s", f);
        } else {
            snprintf(v, n, "%s", pdo_lsfg_find_dll(pdo_data_dir(), found, sizeof found) ? "Found" : "Missing");
        }
#else
        snprintf(v, n, "Not built");
#endif
        break;
    }
    case ITEM_GPU:    snprintf(v, n, "Vulkan"); break;
    case ITEM_SLOT:   snprintf(v, n, "%d%s", s->save_slot, pdo_save_slot_used(s->save_slot) ? "" : " (empty)"); break;
    case ITEM_PICK: {   /* the end of the path: the file or folder's name */
        const char *f = launcher_files ? launcher_files : "-";
        size_t len = strlen(f);
        snprintf(v, n, "%s%s", len > 16 ? ".." : "", len > 16 ? f + len - 14 : f);
        break;
    }
    case ITEM_GO_DISPLAY: case ITEM_GO_PICTURE: case ITEM_GO_SOUND: case ITEM_GO_GAMEPLAY:
    case ITEM_GO_ADVANCED: case ITEM_CONTROLS:
        snprintf(v, n, ">"); break;
    }
    return -1.0f;
}

static const char *label_of(int item)
{
    switch (item) {
    case ITEM_SCALE:  return "Resolution";
    case ITEM_MSAA:   return "Anti-aliasing";
    case ITEM_ANISO:  return "Texture filter";
    case ITEM_ASPECT: return "Aspect ratio";
    case ITEM_FIT:    return "Fit to window";
    case ITEM_SOFT:   return "TV softening";
    case ITEM_FPS:    return "Show FPS";
    case ITEM_FULLSCREEN: return "Fullscreen";
    case ITEM_VOLUME:  return "Master";
    case ITEM_PIC_LOOK:     return "Look";
    case ITEM_PIC_BLOOM:    return "Bloom";
    case ITEM_PIC_BRIGHT:   return "Brightness";
    case ITEM_PIC_CONTRAST: return "Contrast";
    case ITEM_PIC_GAMMA:    return "Gamma";
    case ITEM_PIC_SAT:      return "Saturation";
    case ITEM_PIC_RESET:    return "Reset picture";
    case ITEM_BOX:    return "Pandora's Box";
    case ITEM_LOGOS:  return "Skip logos";
    case ITEM_LAUNCHER: return "Skip launcher";
    case ITEM_VSYNC:  return "VSync";
    case ITEM_SHADOWS: return "Shadows";
    case ITEM_RUMBLE: return "Vibration";
    case ITEM_3D:     return "Output";
    case ITEM_VSH:    return "Vertex programs";
    case ITEM_FVF:    return "Fixed function";
    case ITEM_INTERP: return "Frame gen.";
    case ITEM_LSMODE: return "LS mode";
    case ITEM_LSDLL:  return "Lossless.dll";
    case ITEM_GPU:    return "Graphics";
    case ITEM_START:  return launcher_files_ok ? "Start game" : "Start game (no files)";
    case ITEM_PICK:   return "Game files";
    case ITEM_SLOT:   return "Save file";
    case ITEM_EXIT:   return "Exit";
    case ITEM_CLOSE:  return "Close";
    case ITEM_QUIT:   return quit_armed ? "Quit? Press again" : "Quit game";
    case ITEM_BACK:   return "Back";
    case ITEM_CONTROLS:    return "Controls";
    case ITEM_GO_DISPLAY:  return "Display";
    case ITEM_GO_PICTURE:  return "Picture";
    case ITEM_GO_SOUND:    return "Sound";
    case ITEM_GO_GAMEPLAY: return "Gameplay";
    case ITEM_GO_ADVANCED: return "Advanced";
    }
    return "";
}

/* One line of help for the chosen setting: at most 31 characters, the
 * panel's width. */
static const char *help_of(int item)
{
    switch (item) {
    case ITEM_SCALE:  return "Multiples of 640x480.";
    case ITEM_MSAA:   return "Smoother edges. MSAA is best.";
    case ITEM_ANISO:  return "Anisotropic: sharper far away.";
    case ITEM_ASPECT: return "16:9 shows more to the sides.";
    case ITEM_FIT:    return "Bars, stretched or zoomed.";
    case ITEM_SOFT:   return "A slight TV-like blur.";
    case ITEM_FPS:    return "Frames per second, top left.";
    case ITEM_FULLSCREEN: return "Fullscreen or a window.";
    case ITEM_VOLUME:  return "Everything the game plays.";
    case ITEM_PIC_LOOK:     return "CRT: richer, like an old TV.";
    case ITEM_PIC_BLOOM:    return "A glow around bright lights.";
    case ITEM_PIC_BRIGHT:   return "Lighter or darker overall.";
    case ITEM_PIC_CONTRAST: return "Darks darker, lights lighter.";
    case ITEM_PIC_GAMMA:    return "Above 1 lifts the midtones.";
    case ITEM_PIC_SAT:      return "How strong the colors are.";
    case ITEM_PIC_RESET:    return "Back to the game's own picture.";
    case ITEM_BOX:    return "All open: every extra unlocked.";
    case ITEM_LOGOS:  return "A press skips the boot logos.";
    case ITEM_LAUNCHER: return "Start at once. Shift: launcher.";
    case ITEM_VSYNC:  return "Fast/Off: more fps. Off tears.";
    case ITEM_SHADOWS: return "Low: lighter on the GPU.";
    case ITEM_RUMBLE: return "Controller rumble strength.";
    case ITEM_3D: {
        static const char *helps[PDO_AUDIO_COUNT] = { "Two speakers or a TV.", "Headphones: sounds around you.",
                                                      "5.1 speakers: sound all around." };
        return helps[pdo_settings_get()->audio_out];
    }
    case ITEM_VSH:    return "GPU is faster.";
    case ITEM_FVF:    return "GPU is faster.";
    case ITEM_INTERP: {
        const pdo_settings *s = pdo_settings_get();
        bool ls = s->lsfg_multiplier >= 2;
        if (launcher)
            return ls ? "LS: Lossless Scaling's frames." : "Interp.: our own blending.";
#ifdef PDO_HAVE_LSFG
        if (ls && !pdo_gfx_lsfg_active() && pdo_lsfg_status()[0])
            return "Can't run here: see the log.";
#else
        if (ls)
            return "This build has no LS: BUILDING.";
#endif
        if (ls != pdo_gfx_lsfg_active())
            return "Takes effect at the next start.";
        return ls ? "60 fps x this; needs that Hz." : "Smooth motion above 60 fps.";
    }
    case ITEM_LSMODE: return "Performance: lighter, rougher.";
#ifdef PDO_HAVE_LSFG
    case ITEM_LSDLL:  return "OK: choose; Left: look for it.";
#else
    case ITEM_LSDLL:  return "This build has no LS: BUILDING.";
#endif
    case ITEM_GPU:    return "The graphics API.";
    case ITEM_START:  return launcher_files_ok ? "Play." : "Choose your game files first.";
    case ITEM_PICK:   return "A disc image or a folder.";
    case ITEM_EXIT:   return "Quit.";
    case ITEM_CLOSE:  return "Back to the game.";
    case ITEM_QUIT:   return "Unsaved progress is lost.";
    case ITEM_BACK:   return "Back to the main list.";
    case ITEM_CONTROLS:    return "Keys, pad and mouse.";
    case ITEM_GO_DISPLAY:  return "Resolution, aspect, window.";
    case ITEM_GO_PICTURE:  return "CRT look, brightness, color.";
    case ITEM_GO_SOUND:    return "Volume, stereo or 5.1.";
    case ITEM_GO_GAMEPLAY: return "Pandora's Box, boot logos.";
    case ITEM_GO_ADVANCED: return "Frame generation and more.";
    }
    return "";
}

static bool is_link(int item)
{
    return item == ITEM_START || item == ITEM_EXIT || item == ITEM_CLOSE || item == ITEM_QUIT || item == ITEM_BACK ||
           item == ITEM_LSDLL ||
           item == ITEM_PIC_RESET || item == ITEM_PICK || item == ITEM_GPU || item == ITEM_CONTROLS ||
           (item >= ITEM_GO_DISPLAY && item <= ITEM_GO_ADVANCED);
}

static void draw_menu(void)
{
    int n;
    const int *items = page_items(&n);
    const float w = 34, x0 = (cols - w) / 2, step = 2.0f;
    const float h = 4.0f + n * step + 6.0f, y0 = SDL_max((30 - h) / 2, 0.5f);
    char v[40];

    frame(x0, y0, w, h, page_names[page]);
    for (int i = 0; i < n; i++) {
        int item = items[i];
        float y = y0 + 4.0f + i * step;
        float fillv = value_of(item, v, sizeof v);
        bool sel = i == selected, off = item == ITEM_START && !launcher_files_ok;

        if (item == ITEM_BACK || item == ITEM_CLOSE || item == ITEM_EXIT) {
            COLOR(COL_TRACK);
            fill(x0 + 1, y - 0.6f, w - 2, 0.125f);       /* a rule before the way out */
        }
        if (sel) {
            COLOR(COL_SEL);
            fill(x0 + 0.5f, y - 0.4f, w - 1, 1.8f);
            COLOR(COL_TITLE);
            fill(x0 + 0.5f, y - 0.4f, 0.3f, 1.8f);
        }
        if (off)
            COLOR(COL_OFF);
        else if (sel)
            COLOR(COL_TEXT_HI);
        else
            COLOR(COL_TEXT);
        text(x0 + 2, y, label_of(item));
        if (fillv >= 0.0f) {        /* a slider, its value after it */
            float bx = x0 + w - 16, bw = 9;
            COLOR(COL_TRACK);
            fill(bx, y + 0.25f, bw, 0.5f);
            if (sel)
                COLOR(COL_TITLE);
            else
                COLOR(COL_ACCENT);
            fill(bx, y + 0.25f, bw * SDL_clamp(fillv, 0.0f, 1.0f), 0.5f);
            color(sel ? 255 : 206, sel ? 255 : 212, sel ? 255 : 224, 255);
            text(x0 + w - 1.5f - (float)strlen(v), y, v);
        } else if (v[0]) {
            float vx = x0 + w - 1.5f - (float)strlen(v);
            if (sel && !is_link(item)) {     /* < value > on what Left/Right change */
                vx -= 2;
                COLOR(COL_TITLE);
                text(vx - 2, y, "<");
                text(x0 + w - 2.5f, y, ">");
                COLOR(COL_TEXT_HI);
            } else if (is_link(item))
                color(sel ? 232 : 138, sel ? 150 : 148, sel ? 40 : 166, 255);
            text(vx, y, v);
        }
    }
    /* help for the chosen line, then the keys */
    COLOR(COL_TRACK);
    fill(x0 + 1, y0 + h - 5.5f, w - 2, 0.125f);
    COLOR(COL_DIM);
    help(x0 + 1.5f, y0 + h - 4.75f, (int)w - 3, help_of(items[selected < n ? selected : 0]));
    COLOR(COL_OFF);
    text(x0 + 1.5f, y0 + h - 1.5f, page != PAGE_MAIN ? "A select   </> change   B back"
                                   : launcher ? "A select   </> change" : "A select   </> change   B close");
}

SDL_Surface *pdo_overlay_image(bool *changed)
{
    *changed = false;
    if (!pdo_overlay_visible())
        return NULL;
    if (!image) {
        image = SDL_CreateSurface(PDO_OVERLAY_W, PDO_OVERLAY_H, SDL_PIXELFORMAT_RGBA32);
        painter = image ? SDL_CreateSoftwareRenderer(image) : NULL;
        if (!painter) {
            SDL_Log("overlay: cannot draw (%s)", SDL_GetError());
            return NULL;
        }
        SDL_SetRenderDrawBlendMode(painter, SDL_BLENDMODE_NONE);
        dirty = true;
    }
    {   /* The layer is shown with the game's frame: in its widescreen mode
         * that is stretched to 16:9, so draw a quarter narrower to come out
         * right (the FPS box and the menu were widened over the picture). */
        static int wide = -1;
        if (wide != (int)pdo_widescreen_active()) {
            wide = pdo_widescreen_active();
            SDL_SetRenderScale(painter, SCALE * (wide ? 0.75f : 1.0f), SCALE);
            cols = PDO_OVERLAY_W / (SCALE * (wide ? 0.75f : 1.0f) * CHAR);
            dirty = true;
        }
    }
    if (!dirty)
        return image;
    SDL_SetRenderDrawColor(painter, 0, 0, 0, 0);
    SDL_RenderClear(painter);
    if (open_ && controls)
        draw_controls();
    else if (open_)
        draw_menu();
    if (pdo_settings_get()->show_fps) {
        char buf[32];
        SDL_FRect bg = { 0, 0, 10 * CHAR, 2 * CHAR };
        snprintf(buf, sizeof buf, fps < 0 ? "-- fps" : "%d fps", fps);
        SDL_SetRenderDrawColor(painter, 0, 0, 0, 160);
        SDL_RenderFillRect(painter, &bg);
        SDL_SetRenderDrawColor(painter, 255, 255, 0, 255);
        text(0.5f, 0.5f, buf);
    }
    SDL_FlushRenderer(painter);
    dirty = false;
    *changed = true;
    return image;
}

/*
 * On-screen touch controls (Android), on top of descore's touch module
 * (src/android/descore/touch), as Sin and Punishment's and BattleTanx's
 * touch_controls are.
 *
 * Java (TouchControlsView) runs the JNI functions here on the UI thread: it
 * forwards touches, the screen size and whether a gamepad is connected, and
 * draws the controls from descore_touch_get_shapes(). The game thread reads
 * what they hold through pdo_touch_add(), from the shared input code
 * (src/hle/input_hle.c), so Orta and pd1 both get them, as a first
 * controller: a stick for the left thumbstick and the Xbox buttons, labelled
 * as on the pad. MENU opens the options (F1, sent by Java).
 *
 * Touch is for the controls only while the game reads its controller and
 * no options are open; otherwise touches go to the window beneath (the
 * launcher, the options), which SDL turns into mouse clicks.
 */
#include <jni.h>
#include <stdbool.h>
#include <SDL3/SDL.h>

#include "android/descore/touch/descore_touch.h"
#include "android/touch_controls.h"
#include "config.h"
#include "gfx/overlay.h"

/* Buttons, in descore's order; a held button sends its own index as its key
 * (U-TURN sends L's and R's: the game turns round on both at once). Labels
 * are Orta's actions: A fires (held: lock-on), X glides forward and B back
 * (boost, brake), Y changes form, Black is berserk, L and R turn. */
enum {
    BUTTON_A, BUTTON_B, BUTTON_X, BUTTON_Y, BUTTON_LT, BUTTON_RT, BUTTON_BLACK, BUTTON_START,
    BUTTON_UTURN,               /* L and R together */
    BUTTON_MENU,                /* a tap button: the options (Java sends F1) */
    BUTTON_COUNT
};
static const char *const labels[BUTTON_COUNT] = { "FIRE", "BRAKE", "BOOST", "MORPH", "L", "R",
                                                  "BERSERK", "START", "180", "MENU" };

/* XINPUT_GAMEPAD: bAnalogButtons[] index of A, B, X, Y, the triggers and
 * Black; wButtons bit of Start. */
static const int analog_of[BUTTON_START] = { 0, 1, 2, 3, 6, 7, 4 };
#define XB_START 0x0010

static SDL_AtomicInt held;              /* a bit per button */
static SDL_AtomicInt stick_x, stick_y;  /* the left thumbstick, Xbox units (up positive) */
static SDL_AtomicU32 last_read_ms;      /* when the game last read its controller */
static SDL_AtomicInt setting_mode = { -1 }, setting_size, setting_alpha;

static bool initialized, gamepad_connected, menu_tapped;

void pdo_touch_add(uint16_t *buttons, uint8_t analog[8], int16_t thumb[4])
{
    int h = SDL_GetAtomicInt(&held);
    int x = SDL_GetAtomicInt(&stick_x), y = SDL_GetAtomicInt(&stick_y);

    SDL_SetAtomicU32(&last_read_ms, (Uint32)SDL_GetTicks() | 1u);
    for (int b = 0; b < BUTTON_START; b++)
        if (h & (1 << b))
            analog[analog_of[b]] = 255;
    if (h & (1 << BUTTON_START))
        *buttons |= XB_START;
    if (x || y) {
        thumb[0] = (int16_t)x;
        thumb[1] = (int16_t)y;
    }
}

static void on_key(unsigned char key, int down)
{
    int old;

    if (key > BUTTON_START)
        return;
    do
        old = SDL_GetAtomicInt(&held);
    while (!SDL_CompareAndSwapAtomicInt(&held, old, down ? old | (1 << key) : old & ~(1 << key)));
}

static void on_tap(int button)
{
    if (button == BUTTON_MENU)
        menu_tapped = true;
}

/* descore's y grows downwards, the Xbox's upwards. */
static void on_stick(int index, float x, float y)
{
    (void)index;
    SDL_SetAtomicInt(&stick_x, (int)(SDL_clamp(x, -1.0f, 1.0f) * 32767.0f));
    SDL_SetAtomicInt(&stick_y, (int)(SDL_clamp(-y, -1.0f, 1.0f) * 32767.0f));
}

static void set_button(int button, float x, float y, float w, float h)
{
    unsigned char key = (unsigned char)button;
    static const unsigned char both[2] = { BUTTON_LT, BUTTON_RT };
    if (button == BUTTON_MENU)
        descore_touch_set_button(button, x, y, w, h, NULL, 0, false);
    else if (button == BUTTON_UTURN)
        descore_touch_set_button(button, x, y, w, h, both, 2, false);
    else
        descore_touch_set_button(button, x, y, w, h, &key, 1, false);
}

/* The stick bottom left with L above it; FIRE, BOOST, BRAKE, MORPH
 * where the pad has A, X, B, Y (FIRE at the bottom) bottom right with
 * R above them; MENU top left with BERSERK below it, START top right
 * with 180 below it. Sizes follow the screen height and the Touch
 * size setting. */
static void layout(int width, int height)
{
    float w = (float)width, h = (float)height;
    float scale = (float)pdo_settings_get()->touch_size / 100.0f;    /* 90-130 %, each step seen */
    float c = SDL_min(SDL_max(h * 0.12f * scale, descore_touch_min_cell_px()), h * 0.156f);
    /* At most 0.156 of the height (Touch size 130 %): above it the corners' second buttons meet
     * L and R (0.04h + 1.75c against 0.96h - 3.97c on the right). */
    float m = h * 0.04f, gap = c * 0.12f;
    float r = c * 1.25f;
    float cx = w - m - c * 1.6f, cy = h - m - c * 1.6f;     /* the face buttons' centre */
    float tw = c * 1.4f, th = c * 0.7f;                      /* the wide buttons */
    /* BERSERK and 180 a little way below MENU and START, so as not to be
     * pressed for them (or they for these). */
    float row2 = m + c * 0.6f + c * 0.45f;

    descore_touch_set_screen_size(width, height);
    descore_touch_set_stick(0, m + r * 1.15f, h - m - r * 1.15f, r, on_stick);
    descore_touch_set_stick_style(0, DESCORE_TOUCH_STICK_ANALOG);

    set_button(BUTTON_A, cx - c * 0.5f, cy + c * 0.55f, c, c);
    set_button(BUTTON_Y, cx - c * 0.5f, cy - c * 1.55f, c, c);
    set_button(BUTTON_X, cx - c * 1.6f, cy - c * 0.5f, c, c);
    set_button(BUTTON_B, cx + c * 0.6f, cy - c * 0.5f, c, c);
    set_button(BUTTON_LT, m, h - m - r * 2.3f - gap - th, tw, th);
    set_button(BUTTON_RT, w - m - tw, cy - c * 1.55f - gap - th, tw, th);
    set_button(BUTTON_MENU, m, m, tw, c * 0.6f);
    set_button(BUTTON_BLACK, m, row2, tw, th);
    set_button(BUTTON_START, w - m - tw, m, tw, c * 0.6f);
    set_button(BUTTON_UTURN, w - m - tw, row2, tw, th);
}

/* The Size step nearest a size in percent. */
static int scale_step_for(int percent)
{
    int best = DESCORE_TOUCH_SCALE_DEFAULT_STEP;
    for (int i = 0; i < DESCORE_TOUCH_SCALE_NUM_STEPS; i++)
        if (SDL_fabsf(kDescoreTouchScaleValues[i] * 100.0f - (float)percent)
                < SDL_fabsf(kDescoreTouchScaleValues[best] * 100.0f - (float)percent))
            best = i;
    return best;
}

/* The Touch controls page is open: the controls are shown as set (Auto
 * too, with a gamepad), though they take no touches. */
static SDL_AtomicInt previewing;

static bool buttons_hidden(void)
{
    if (SDL_GetAtomicInt(&previewing) && SDL_GetAtomicInt(&setting_mode) != PDO_TOUCH_OFF)
        return false;
    switch (SDL_GetAtomicInt(&setting_mode)) {
    case PDO_TOUCH_ALWAYS: return false;
    case PDO_TOUCH_OFF:    return true;
    default:               return gamepad_connected;
    }
}

static void release_all(void)
{
    for (int id = 0; id < DESCORE_TOUCH_MAX_POINTERS; id++)
        descore_touch_handle(DESCORE_TOUCH_ACTION_POINTER_UP, id, -1.0f, -1.0f);
    SDL_SetAtomicInt(&held, 0);
    SDL_SetAtomicInt(&stick_x, 0);
    SDL_SetAtomicInt(&stick_y, 0);
}

/* ── JNI (TouchControlsView) ─────────────────────────────────────────── */

/* The screen size; lays the controls out again with the current settings. */
JNIEXPORT void JNICALL Java_io_github_wootbeer_ortarecomp_TouchControlsView_nativeLayout(
    JNIEnv *env, jclass cls, jint width, jint height, jfloat density)
{
    const pdo_settings *s = pdo_settings_get();

    (void)env; (void)cls;
    if (!initialized) {
        descore_touch_init(BUTTON_COUNT, on_key, on_tap);
        initialized = true;
    }
    SDL_SetAtomicInt(&setting_mode, s->touch_buttons);
    SDL_SetAtomicInt(&setting_size, s->touch_size);
    SDL_SetAtomicInt(&setting_alpha, s->touch_opacity);
    descore_touch_set_display_density(density);
    descore_touch_set_scale_step(scale_step_for(s->touch_size));
    descore_touch_set_opacity((float)s->touch_opacity / 100.0f);
    layout(width, height);
    descore_touch_set_gamepad_connected(buttons_hidden());
    SDL_Log("touch: laid out at %dx%d (buttons %s, size %d%%, opacity %d%%)", (int)width, (int)height,
            buttons_hidden() ? "hidden" : "shown", s->touch_size, s->touch_opacity);
}

/* One pointer of a touch event (Android's action codes, which descore
 * shares). Bit 0: taken; bit 1: MENU was tapped; bit 2: redraw. */
JNIEXPORT jint JNICALL Java_io_github_wootbeer_ortarecomp_TouchControlsView_nativeTouch(
    JNIEnv *env, jclass cls, jint action, jint pointer_id, jfloat x, jfloat y)
{
    bool taken;

    (void)env; (void)cls;
    menu_tapped = false;
    if (!initialized || pointer_id < 0 || pointer_id >= DESCORE_TOUCH_MAX_POINTERS)
        return 0;
    taken = descore_touch_handle(action, pointer_id, x, y);
    return (taken ? 1 : 0) | (menu_tapped ? 2 : 0) | (taken ? 4 : 0);
}

/* Lets go of every finger, so nothing stays held. */
JNIEXPORT void JNICALL Java_io_github_wootbeer_ortarecomp_TouchControlsView_nativeReleaseAll(
    JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (initialized)
        release_all();
}

/* Whether a gamepad is connected (a handheld's built-in one too): with
 * Touch buttons on Auto the controls hide. */
JNIEXPORT void JNICALL Java_io_github_wootbeer_ortarecomp_TouchControlsView_nativeSetGamepadConnected(
    JNIEnv *env, jclass cls, jboolean connected)
{
    (void)env; (void)cls;
    if (gamepad_connected != (bool)connected)
        SDL_Log("touch: gamepad %s", connected ? "connected" : "gone");
    gamepad_connected = connected;
    if (initialized) {
        if (buttons_hidden())
            release_all();
        descore_touch_set_gamepad_connected(buttons_hidden());
    }
}

/* Shapes to draw, 7 floats each (kind, button, x, y, w, h, alpha); returns
 * how many. */
JNIEXPORT jint JNICALL Java_io_github_wootbeer_ortarecomp_TouchControlsView_nativeGetShapes(
    JNIEnv *env, jclass cls, jfloatArray out)
{
    DescoreTouchShape shapes[DESCORE_TOUCH_MAX_BUTTONS + 16];
    jsize capacity;
    jfloat *v;
    int count;

    (void)cls;
    if (!initialized)
        return 0;
    capacity = (*env)->GetArrayLength(env, out) / 7;
    count = descore_touch_get_shapes(shapes, SDL_min((int)capacity, (int)SDL_arraysize(shapes)));
    v = (*env)->GetFloatArrayElements(env, out, NULL);
    for (int i = 0; i < count; i++) {
        jfloat *o = v + i * 7;
        o[0] = (jfloat)shapes[i].kind;
        o[1] = (jfloat)shapes[i].button;
        o[2] = shapes[i].x;
        o[3] = shapes[i].y;
        o[4] = shapes[i].w;
        o[5] = shapes[i].h;
        o[6] = shapes[i].alpha;
    }
    (*env)->ReleaseFloatArrayElements(env, out, v, 0);
    return count;
}

JNIEXPORT jstring JNICALL Java_io_github_wootbeer_ortarecomp_TouchControlsView_nativeButtonLabel(
    JNIEnv *env, jclass cls, jint button)
{
    (void)cls;
    return (*env)->NewStringUTF(env, button >= 0 && button < BUTTON_COUNT ? labels[button] : "");
}

/* Bit 0: touch is for the controls now (the game read its controller in
 * the last three seconds and no options are open: between scenes, while
 * it loads, it stops reading for a moment, and the controls flickered off
 * and on at half a second). Bit 1: the settings changed, lay out again.
 * Bit 2: show them without taking touches (the Touch controls page is
 * open, so their size and opacity are seen as they change). */
JNIEXPORT jint JNICALL Java_io_github_wootbeer_ortarecomp_TouchControlsView_nativePoll(
    JNIEnv *env, jclass cls)
{
    const pdo_settings *s = pdo_settings_get();
    Uint32 read = SDL_GetAtomicU32(&last_read_ms);
    /* Signed: the time stored has its low bit set (0 is "never"), so a
     * read in this same millisecond can be 1 ms ahead; unsigned, that
     * came out as 49 days ago and the controls blinked off (the RP6). */
    Sint32 ago = (Sint32)((Uint32)SDL_GetTicks() - read);
    bool active = read && ago < 3000 && !pdo_overlay_open();
    bool changed = s->touch_buttons != SDL_GetAtomicInt(&setting_mode)
                || s->touch_size != SDL_GetAtomicInt(&setting_size)
                || s->touch_opacity != SDL_GetAtomicInt(&setting_alpha);
    bool preview = pdo_overlay_on_touch_page();

    (void)env; (void)cls;
    {   /* what turns them off and on, for the log (a few dozen at most) */
        static int was = -1, logged;
        if ((int)active != was && logged++ < 40)
            SDL_Log("touch: controls %s (game read its controller %d ms ago, options %s)", active ? "on" : "off",
                    read ? (int)ago : -1, pdo_overlay_open() ? "open" : "closed");
        was = active;
    }
    if (preview != (bool)SDL_GetAtomicInt(&previewing)) {
        SDL_SetAtomicInt(&previewing, preview);
        if (initialized)
            descore_touch_set_gamepad_connected(buttons_hidden());
        SDL_Log("touch: preview %s", preview ? "on (Touch controls page)" : "off");
    }
    return (active ? 1 : 0) | (changed ? 2 : 0) | (preview ? 4 : 0);
}

/* From the view: something about its drawing surface, for the log. */
JNIEXPORT void JNICALL Java_io_github_wootbeer_ortarecomp_TouchControlsView_nativeLog(
    JNIEnv *env, jclass cls, jstring what)
{
    static int logged;
    const char *s;

    (void)cls;
    if (logged++ >= 40)
        return;
    s = (*env)->GetStringUTFChars(env, what, NULL);
    SDL_Log("touch: %s", s ? s : "?");
    if (s)
        (*env)->ReleaseStringUTFChars(env, what, s);
}

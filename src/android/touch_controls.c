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

/* Buttons, in descore's order; a held button sends its own index as its key. */
enum {
    BUTTON_A, BUTTON_B, BUTTON_X, BUTTON_Y, BUTTON_LT, BUTTON_RT, BUTTON_START,
    BUTTON_MENU,                /* a tap button: the options (Java sends F1) */
    BUTTON_COUNT
};
static const char *const labels[BUTTON_COUNT] = { "A", "B", "X", "Y", "LT", "RT", "START", "MENU" };

/* XINPUT_GAMEPAD: bAnalogButtons[] index of A, B, X, Y and the triggers;
 * wButtons bit of Start. */
static const int analog_of[BUTTON_START] = { 0, 1, 2, 3, 6, 7 };
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

    if (key >= BUTTON_MENU)
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
    if (button == BUTTON_MENU)
        descore_touch_set_button(button, x, y, w, h, NULL, 0, false);
    else
        descore_touch_set_button(button, x, y, w, h, &key, 1, false);
}

/* The stick bottom left with LT above it; A, B, X, Y as on the pad (A at
 * the bottom) bottom right with RT above them; MENU and START top centre,
 * clear of the HUD's corners. Sizes follow the screen height and the
 * Touch size setting. */
static void layout(int width, int height)
{
    float w = (float)width, h = (float)height;
    float scale = descore_touch_scale_value(descore_touch_scale_step());
    float c = SDL_min(SDL_max(h * 0.12f * scale, descore_touch_min_cell_px()), h * 0.18f);
    float m = h * 0.04f, gap = c * 0.12f;
    float r = c * 1.25f;
    float cx = w - m - c * 1.6f, cy = h - m - c * 1.6f;     /* the face buttons' centre */

    descore_touch_set_screen_size(width, height);
    descore_touch_set_stick(0, m + r * 1.15f, h - m - r * 1.15f, r, on_stick);
    descore_touch_set_stick_style(0, DESCORE_TOUCH_STICK_ANALOG);

    set_button(BUTTON_A, cx - c * 0.5f, cy + c * 0.55f, c, c);
    set_button(BUTTON_Y, cx - c * 0.5f, cy - c * 1.55f, c, c);
    set_button(BUTTON_X, cx - c * 1.6f, cy - c * 0.5f, c, c);
    set_button(BUTTON_B, cx + c * 0.6f, cy - c * 0.5f, c, c);
    set_button(BUTTON_LT, m, h - m - r * 2.3f - gap - c * 0.7f, c * 1.4f, c * 0.7f);
    set_button(BUTTON_RT, w - m - c * 1.4f, cy - c * 1.55f - gap - c * 0.7f, c * 1.4f, c * 0.7f);
    set_button(BUTTON_MENU, w * 0.5f - c * 1.4f - gap * 0.5f, m, c * 1.4f, c * 0.6f);
    set_button(BUTTON_START, w * 0.5f + gap * 0.5f, m, c * 1.4f, c * 0.6f);
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

static bool buttons_hidden(void)
{
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
 * the last half second and no options are open). Bit 1: the settings
 * changed, lay out again. */
JNIEXPORT jint JNICALL Java_io_github_wootbeer_ortarecomp_TouchControlsView_nativePoll(
    JNIEnv *env, jclass cls)
{
    const pdo_settings *s = pdo_settings_get();
    Uint32 read = SDL_GetAtomicU32(&last_read_ms);
    bool active = read && (Uint32)SDL_GetTicks() - read < 500 && !pdo_overlay_open();
    bool changed = s->touch_buttons != SDL_GetAtomicInt(&setting_mode)
                || s->touch_size != SDL_GetAtomicInt(&setting_size)
                || s->touch_opacity != SDL_GetAtomicInt(&setting_alpha);

    (void)env; (void)cls;
    return (active ? 1 : 0) | (changed ? 2 : 0);
}

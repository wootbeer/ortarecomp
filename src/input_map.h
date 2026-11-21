#ifndef PDO_INPUT_MAP_H
#define PDO_INPUT_MAP_H

/*
 * Which key and which controller button play each Xbox button, changed in
 * the options overlay (Controls) and saved with the settings. The sticks
 * stay where they are: the left and right sticks, and WASD/IJKL on the
 * keyboard.
 */

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

enum {
    PDO_IN_A, PDO_IN_B, PDO_IN_X, PDO_IN_Y, PDO_IN_BLACK, PDO_IN_WHITE, PDO_IN_LTRIG, PDO_IN_RTRIG,
    PDO_IN_START, PDO_IN_BACK, PDO_IN_UP, PDO_IN_DOWN, PDO_IN_LEFT, PDO_IN_RIGHT, PDO_IN_LTHUMB, PDO_IN_RTHUMB,
    PDO_IN_COUNT
};

/* A controller input: a button (SDL_GamepadButton), a trigger
 * (PDO_PAD_AXIS + SDL_GamepadAxis), or none (-1). */
#define PDO_PAD_AXIS 100

typedef struct pdo_binding {
    SDL_Scancode key;       /* SDL_SCANCODE_UNKNOWN: none */
    int          pad;
    int          mouse;     /* SDL_BUTTON_LEFT..X2, 0: none */
} pdo_binding;

pdo_binding *pdo_input_bindings(void);          /* PDO_IN_COUNT of them */
const char *pdo_input_button_name(int b);       /* "A", "Start", ... */
void pdo_input_reset(void);                     /* the defaults */
/* How far a binding's controller input is pressed, 0..255 (triggers are
 * analog, buttons 0 or 255); 0 without a controller. */
int pdo_input_pad_level(SDL_Gamepad *g, int pad);
const char *pdo_input_pad_name(int pad);        /* "a", "lefttrigger", "-" */
const char *pdo_input_mouse_name(int mouse);    /* "left", ... "-" */
const char *pdo_input_pad_label(int pad);       /* short, for the menu: "A", "RB", "LT" */

/* Mouse motion while the game has the mouse (renderer.c adds it, in window
 * pixels; input_hle.c takes it once a game frame). */
void pdo_mouse_add(float dx, float dy);
void pdo_mouse_take(float *dx, float *dy);
/* Whether the game has the mouse (held in the window): mouse buttons play
 * the game only then, so the click that takes the mouse back does not. */
void pdo_mouse_set_captured(bool on);
SDL_MouseButtonFlags pdo_mouse_buttons(void);    /* none while not captured */

/* Settings file lines (config.c): key_<button>=<key name>,
 * pad_<button>=<controller input name>. */
bool pdo_input_load_line(const char *name, const char *value);
size_t pdo_input_save(char *out, size_t cap);

#endif

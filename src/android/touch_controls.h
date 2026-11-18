/* On-screen touch controls (Android): what they hold, added to the first
 * controller port's Xbox gamepad state (src/hle/input_hle.c, so Orta and
 * pd1 alike). See touch_controls.c. */
#pragma once
#include <stdint.h>

/* buttons: XINPUT_GAMEPAD.wButtons bits; analog: bAnalogButtons[8] (A, B, X,
 * Y, Black, White, L trigger, R trigger); thumb: LX, LY, RX, RY, up
 * positive. Each only raised, never lowered: a touch adds to the gamepad
 * and keyboard. Called by the game thread whenever it reads the port. */
void pdo_touch_add(uint16_t *buttons, uint8_t analog[8], int16_t thumb[4]);

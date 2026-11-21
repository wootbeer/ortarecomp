/*
 * Mouse aim on Orta's reticle (issue 51; the input is src/hle/input_hle.c).
 *
 * The reticle update's object, [player+0x3D0] (player at 0x00472030),
 * turned out to be the camera's follow of the aim: a damped spring per axis
 * in 0x0007B5D0 (pull +0x120/+0x124 = +0x110/+0x114 + [player+0x380] x the
 * error, velocity +0x130/+0x134 losing [player+0x384] of itself, angles
 * +0xC0 pitch, +0xC4 yaw). Running it with stiffness 1 and no damping while
 * the mouse aimed turned the camera with the aim, and shook it left and
 * right once it had come round (the yaw's wrap at 360). Not the reticle:
 * the hook stays as a pass-through until the reticle's own state is found
 * (PDO_WATCH_RETICLE).
 */

#include "recomp_types.h"

extern void sub_0007B5D0_gen(void);
void sub_0007B5D0(void)
{
    sub_0007B5D0_gen();
}

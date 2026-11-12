Port to Concept of Love from Orta Recomp (wootbeer/ortarecomp, branch
claude/awesome-bohr-rq90nv): shared code that Orta changed on Nov 11-12 and
Concept of Love does not have yet. Our own GPL code: copy it, renaming pdo_
to clv_. Check each against JSRF before applying where marked.

1. Windows: the log is empty when a release build is started normally
   (626358c, src/main.c open_log). A GUI program's stdout/stderr have no
   file behind them (_fileno -2), so _dup2 onto 1 and 2 leaves printf going
   nowhere, and SDL_Log finds no parent console and writes only to the
   debugger. Fix: freopen_s both streams to "NUL" when _fileno < 0, _dup2
   the log onto _fileno(stdout/stderr), SetStdHandle from those, and
   SDL_SetLogOutputFunction(log_to_stderr) writing to stderr with an
   ERROR:/WARNING: prefix. Concept of Love's open_log is still the old one.

2. Display: 1.5x resolution scale (3b6ade0). The setting is kept in halves
   (render_scale2, 2-16; the ini still says render_scale=1.5), the menu
   steps 1x, 1.5x, 2x, 3x .. 8x; in the renderer rscale is a float and every
   scaled size goes through sc(), which rounds; a copy rectangle is scaled
   at both edges.

3. Display: VSync On / Fast / Off (62b0c32). apply_vsync() next to
   apply_quality() per frame: SDL_SetGPUSwapchainParameters with VSYNC,
   MAILBOX or IMMEDIATE, falling back Off -> Fast -> On; skipped under
   PDO_BENCHMARK and Lossless Scaling. Saved as vsync=on|fast|off.

4. The log names the GPU and driver (a877c97): SDL_GetGPUDeviceProperties,
   SDL_PROP_GPU_DEVICE_NAME_STRING, DRIVER_NAME, DRIVER_VERSION.

5. Gameplay: Vibration strength (451e2dd, steps of 10 in 198f07e): rumble
   0-100 % scales both motor speeds before SDL_RumbleGamepad; the log says
   the first XInputSetState (handle, port, speeds; 9882507) and the first
   rumble (41c2cf4). Only XInputSetState's address is JSRF's own.

6. Controls: the rebinding prompt "press a key, button or click" after the
   9-column button name ran past Orta's 38-column Controls panel; now
   "press a key/button/click" (67939e6). Concept of Love's Controls layout
   differs: check its line against its own panel width.

7. Audio, the reverb send follows distance (286b548): a 3D voice's I3DL2
   send (mixbin 10) takes its distance gain, as DirectSound's per-bin
   volumes do (Orta 0x002C6E40); apply_3d calls clv_voice_set_3d_distance,
   update_gains multiplies the send by it. Check JSRF's 4134 DSOUND gives
   bin 10 the distance attenuation too before applying.

8. Audio, the mixbins' headroom (79030cd): Orta's
   DirectSound start-up gives mixbins 0-30 a headroom of 1 (-6 dB; Orta
   table at 0x002C3B2A) and the mixer now halves every bin. Find JSRF's own
   start-up table (the per-bin headroom written to APU method 0x200 + 4 *
   bin) before applying: the value may differ in XDK 4134.

9. xboxrecomp patches 0031-0033 (Linux / Steam Deck), kernel only:
   0031 reserve the whole 4 GB guest space up front (a start-up crash on
   the Deck, depending on where the system put memory), 0032 a file opened
   relative to its directory's handle (POSIX NtCreateFile ignored
   RootDirectory), 0033 directory searches end with their handle and a
   formatted cache partition empties its folder. Copy them after 0030;
   they must not overlap Concept of Love's own patches.

10. Shadows High / Low (9e16fcc, 625f840) keeps depth-only targets (shadow
    maps) at the game's size: only useful if JSRF draws shadow maps; its
    shadows are stencil volumes, so probably skip.

Orta-only, not to port: 5.1 movie soundtracks (src/audio/aix.c,
PDO_TEST_AC3), PDO_TEST_NEWEST, the Shadows option if (10) does not apply.

# Android port: plan and status

Started from Sin and Punishment Recompiled's and BattleTanx: Global Assault Recompiled's Android
builds (the same author's): their `android/` project, the activity's behaviour (Back for the
menu, the crash log offer, Share log) and later their touch controls (descore) and Lossless
Scaling. Orta is on SDL3, so SDL's own Android pieces stand in for their SDL2 additions: its
file dialog for the document picker, its `content://` reading for the disc image, its gamepads.

## Steps

1. **Builds and boots** (now): the Android CMake path (`libmain.so`, SDL static),
   xboxrecomp on Android's C library (patches 0039: no OpenSSL; 0040: no `pthread_cancel` or
   `explicit_bzero`), the crash report's stack (`_Unwind_Backtrace`), the log through logcat and
   `ortarecomp.log`, the setup's disc image picker. To try on the owner's Retroid Pocket 6
   (Snapdragon, Adreno) with its built-in controller.
2. **Touch controls** (in, to try): descore's touch module (`src/android/descore`, from
   snprecomp), the layout and the Xbox buttons in `src/android/touch_controls.c`, fed into the
   first controller in `src/hle/input_hle.c` (Orta and pd1 alike), drawn by `TouchControlsView`
   over SDL's surface as in snprecomp. A stick (left thumbstick) and Orta's actions: FIRE (A),
   BOOST (X), BRAKE (B), MORPH (Y), BERSERK (Black, under MENU), L and R (turn), 180 (L and R
   together, under START), START and MENU (the options). Options > Controls > Touch controls: when they show (Auto
   hides them while a gamepad is connected, as on the RP6), size, opacity.
3. **Performance** on the device: frame rate, the guest CPU lock on ARM, Display > Frame rate 30.
4. **Lossless Scaling** (btgarecomp's Android build runs `lib/lsfg`, which builds on Android as
   it is): the DLL picked through the file dialog and copied into the app's storage.
5. **pd1** (Pandora's Box > PANZER DRAGOON; in, to try): linked in (`PD1_GEN_DIR` is the
   desktop checkout's `gen-pd1/`). Switching programs starts the app again as the PC builds
   restart theirs: `src/guest/relaunch.c` calls `OrtaActivity.restartWith`, which starts
   `RelaunchActivity` (its own process, `:relaunch`) and ends the game's; that waits for it to
   be gone and starts `OrtaActivity` with `--boot --program NAME --launch-data FILE`
   (`getArguments`).
6. **Other GPUs** (Mali, Samsung's; snprecomp's lessons): SDL's Vulkan device asks for depth
   clamping, anisotropy, clip distances and indirect first instance, and turns down a GPU
   lacking any (older Mali drivers lack depth clamping). The last two are never asked for
   (unused); a GPU that refuses the first two gets a device without them (depth clipped
   instead, no anisotropic filtering; `PDO_GPU_BASIC=1` shows that on any GPU). A pipeline
   that fails to build is remembered, not tried at each draw (Adreno 619 failed its shaders
   at every try in snprecomp); if one with GPU vertex programs or fixed-function vertices
   fails, those vertices go to the CPU. No GPU at all: a message instead of a black screen
   with sound. Already so: DXT decoded on the CPU, depth format and MSAA checked, the crash
   handler catches a driver's abort.
   **Power check** (BattleTanx's issue 6: a game thread spinning while it waits): RP6, per-thread
   CPU (`top -H`): title screen, the busiest game thread 18% of a core, the rest 0-7%; Pandora's
   Box 18%; in play 39% (the frame's work) and the renderer 21%. No thread spins: the kernel's
   yields (NtYieldExecution, a zero KeDelayExecutionThread) return at once, but no wait in Orta
   loops on them. pd1 here: 6% on its title. The vertex/texture-hash worker pool woke all its
   workers for every job; it now wakes only as many as there are chunks for (`src/guest/vsh.c`).
7. **Release**: signing, `versionCode`, the APK on the GitHub release beside the PC downloads.

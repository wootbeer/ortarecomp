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
4. **Lossless Scaling**: tried and taken out. Built for Android (as btgarecomp's), it took the
   RP6's GPU 40-68 ms a frame at 1920x1080 (quality mode; 120 fps needs 8.3), and with a smaller
   flow scale far more (0.5: 134-228 ms, 0.25: 330-450): the Adreno driver runs its shaders badly.
   Not built on Android (CMakeLists.txt); the Advanced page has frame generation Off or our
   interpolation only (at 1x and 120 Hz that reached 76-93 fps; at 2x the steady 60 is better).
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
   **GPU driver** (in, to try): Options > Advanced > GPU driver picks a custom Vulkan driver (a
   Turnip release zip as it comes, or a .so); `GpuDriverInstaller` unpacks it into
   `gpu_drivers/NAME/` (library.txt names the library, from the zip's meta.json). At the next
   start `android_gpu.c` opens it with libadrenotools (`external/libadrenotools`, BSD-2-Clause;
   its hooks `libhook_impl.so`, `libmain_hook.so` packed unpacked: useLegacyPackaging) and SDL
   loads Vulkan from it (patches/SDL/0003). A guard file is up while it opens and from the GPU
   device until 120 frames are shown: a start that crashes falls back to the phone's driver at
   the next one, with a message. `src/android/gpu_driver/` and `GpuDriverInstaller.java` know
   nothing of the game, for snprecomp and btgarecomp (their RT64 takes the library's
   vkGetInstanceProcAddr).
   **Turnip on the RP6** (Mesa 26, Adreno 740): 4x with 4x MSAA, FXAA, bloom high and 16x
   filtering hold 60 fps once a stage's pipelines are made (Qualcomm's driver: 11-15 ms a
   frame at 2x). Turnip makes a pipeline slowly (0.3 s for a setup's own, 1-1.5 s for a
   program's own) and took 20-27 s and hundreds of MB over each of the interpreters' (ff.frag
   as it is, or the vertex-program interpreter): several at once ran it out of memory. With a
   custom driver (`fs_wait` in renderer.c) those are not made: a draw waits for its setup's
   own pipeline (left out meanwhile on a target drawn again within three frames, made in place
   on one drawn once: text, HakuX's rule), and a program without its own shader yet is drawn
   from the CPU's vertices with the setup's own shader. Three low-priority makers, two
   programs' at once. A stage's first visit plays slower for about ten seconds, with a
   "Building shaders" note in the corner; everything is kept for the next start.
   **Interpolating at 120 Hz** (Turnip, 2x): 120 most of the time, 91-102 in the busiest moments,
   no longer falling back to 60. What it took: pairing on a thread of its own; Android always in
   step with the screen (Fast/Off made frames never shown); particles (point lists) made on the
   GPU; constants in uniform blocks (two of 3 KiB: SDL binds 4 KiB); and the vertex pre-pass
   (renderer.c, vout_prepass): each program's vertices made once a game frame by a compute shader
   of its own (vsh.vert with PDO_VSH_COMPUTE), the draws reading them (vout.vert) and blending
   them with the frame before's. One compute pipeline a program instead of a graphics pipeline a
   program and render state. The scene pass was vertex work (as slow cut to one pixel and at
   1.5x). Qualcomm's own driver reaches 61-80 interpolating (its submits are slow): 60 there.
7. **Release**: the disc image picker (Game files in the launcher) draws in the launcher's
   window, its dialogs over it, instead of a window of its own (all platforms). Signing (in:
   `keystore.properties`), `versionCode`, the APK on the GitHub release beside the PC downloads.

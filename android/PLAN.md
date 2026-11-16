# Android port: plan and status

Started from Sin and Punishment Recompiled's and BattleTanx: Global Assault Recompiled's Android
builds (the same author's): their `android/` project, the activity's behaviour (Back for the
menu, the crash log offer, Share log) and later their touch controls (descore) and Lossless
Scaling. Orta is on SDL3, so SDL's own Android pieces stand in for their SDL2 additions: its
file dialog for the document picker, its `content://` reading for the disc image, its gamepads.

## Steps

1. **Builds and boots** (now): the Android CMake path (`libmain.so`, SDL static, pd1 off),
   xboxrecomp on Android's C library (patches 0039: no OpenSSL; 0040: no `pthread_cancel` or
   `explicit_bzero`), the crash report's stack (`_Unwind_Backtrace`), the log through logcat and
   `ortarecomp.log`, the setup's disc image picker. To try on the owner's Retroid Pocket 6
   (Snapdragon, Adreno) with its built-in controller.
2. **Touch controls** (in, to try): descore's touch module (`src/android/descore`, from
   snprecomp), the layout and the Xbox buttons in `src/android/touch_controls.c`, fed into the
   first controller in `src/hle/input_hle.c` (Orta and pd1 alike), drawn by `TouchControlsView`
   over SDL's surface as in snprecomp. A stick (left thumbstick), A/B/X/Y, LT/RT, START and MENU
   (the options). Options > Gameplay: Touch buttons (Auto hides them while a gamepad is
   connected, as on the RP6), Touch size, Touch opacity. Labels are the pad's; Orta's actions
   per button to put on them once known.
3. **Performance** on the device: frame rate, the guest CPU lock on ARM, Display > Frame rate 30.
4. **Lossless Scaling** (btgarecomp's Android build runs `lib/lsfg`, which builds on Android as
   it is): the DLL picked through the file dialog and copied into the app's storage.
5. **pd1** (Pandora's Box > PANZER DRAGOON): an in-process program switch
   (`docs/two-programs.md`, "Switch in-process"), since an app cannot restart itself as a
   process the way the PC builds do.
6. **Release**: signing, `versionCode`, the APK on the GitHub release beside the PC downloads.

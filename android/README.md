# Android build

The app is SDL's `SDLActivity` hosting the game, built by Android Studio from the repo-root
`CMakeLists.txt` as `libmain.so` (SDL linked in). Like the Windows and Linux releases, the APK
carries the compiled game code but nothing from the disc: the build fails if a file in it is
named like a disc image or an Xbox executable.

## One-time setup

1. **The desktop version first, with your disc** ([BUILDING.md](../BUILDING.md)): the Android
   build compiles the code `tools/recomp.py` generated there, in the desktop checkout's `gen/`.
   Regenerate there when it changes (after a new xboxrecomp patch, say); the Android build
   picks it up at its next build.

2. **A second checkout for Android**, so the desktop one keeps its own build files:

       git clone --recurse-submodules https://github.com/wootbeer/ortarecomp ortarecomp-android

3. **Android Studio, SDK Manager, SDK Tools** (tick "Show Package Details"):
   NDK **28.2.13676358** and CMake **3.31.6**. Gradle tries to install these itself when
   missing, but the SDK Manager is the reliable way.

4. **`android/local.properties`** (Android Studio creates it with `sdk.dir`; it isn't
   committed). Add the desktop checkout, with forward slashes:

       orta.desktopDir=C:/dev/ortarecomp

   (`local.properties.example` shows the whole file.) Without it, or without `gen/` there,
   the build stops and says so.

## Build and run

Open the `android/` folder of the Android checkout in Android Studio, let the Gradle sync
finish, pick the device (USB debugging on), press **Run**. The debug build compiles the game
optimised (with debug info), so the first build takes a while.

After that: `git pull` (and `git submodule update --init --recursive` if a submodule moved),
then Run.

From a terminal instead: `gradlew assembleDebug` in `android/` (the APK lands in
`app/build/outputs/apk/debug/`).

Requirements: Android 11 or newer (API 30), 64-bit ARM, Vulkan 1.1.

The game files are chosen in the game's own setup at the first start: **Disc image...** opens
the system file picker; the image's files are copied into the app's private storage (excluded
from backups and device transfers), so the image itself can be deleted afterwards.

Back (the system one, not a controller's) opens and closes the options menu (F1 on PC); the
menu's Quit closes the app. Saves and settings are in the app's storage too.

After a crash the next start offers to share the log; it can be shared any time by
long-pressing the app icon, **Share log**.

Not yet on Android: the original Panzer Dragoon from Pandora's Box (its switch restarts the
program, which an app cannot do; the Box goes back to the main menu), touch controls and
Lossless Scaling's frame generation. See [PLAN.md](PLAN.md).

## Release build

Signed with your own release key, so updates install over each other (by hand or with an
updater such as Obtainium, which takes the APK from the GitHub release). Every release must
be signed with the same key and have a higher `versionCode` (`app/build.gradle.kts`, with
`versionName` the same as the PC release's tag).

One-time: make the key with the `keytool` that comes with Android Studio, outside the repo,
and back the `.jks` file and its password up (a lost key means users must uninstall to
update):

    mkdir C:\keys
    & "C:\Program Files\Android\Android Studio\jbr\bin\keytool.exe" -genkeypair -v -keystore C:\keys\orta-release-key.jks -alias orta -keyalg RSA -keysize 4096 -validity 36500

Then copy `keystore.properties.example` to `keystore.properties` here and fill in the
passwords (gitignored). `gradlew assembleRelease` makes
`app/build/outputs/apk/release/OrtaRecomp-v<version>-android.apk`.

/*
 * Custom Vulkan drivers on Android (Adreno: Mesa's Turnip, or another
 * build), loaded with libadrenotools in place of the system's libvulkan.so.
 * Written to be shared: nothing here knows the game. A project hands the
 * returned library to whatever loads Vulkan (Orta: SDL, patches/SDL/0003;
 * RT64 projects: their loader's vkGetInstanceProcAddr), and installs
 * drivers with the Java side, GpuDriverInstaller.
 *
 * The APK must ship libadrenotools' hook libraries (libhook_impl.so,
 * libmain_hook.so) unpacked: useLegacyPackaging = true.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Opens driver_dir/lib_name (driver_dir in the app's internal storage,
 * ending in '/') as the Vulkan library. native_lib_dir: the app's
 * ApplicationInfo.nativeLibraryDir (where the hooks are); tmp_dir: a
 * writable folder (used before Android 10). A dlopen handle with
 * vkGetInstanceProcAddr, or NULL with the reason in err. */
void *gpu_driver_open(const char *driver_dir, const char *lib_name, const char *native_lib_dir,
                      const char *tmp_dir, char *err, size_t err_len);

/* A driver that crashes the app at start would do so at every start: the
 * guard is a file that exists while a driver is being tried. Begin before
 * opening it; false when the file is there already (the last try did not
 * come through): use the system's driver then. End once frames are shown. */
bool gpu_driver_guard_begin(const char *guard_path);
void gpu_driver_guard_end(const char *guard_path);

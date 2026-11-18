/* Android: a custom Vulkan driver (Turnip...) chosen in Options > Advanced >
 * GPU driver; see android_gpu.c and src/android/gpu_driver/. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* At start, before any Vulkan window: opens the chosen driver for SDL
 * (patches/SDL/0003). Falls back to the system's when it cannot, or when
 * the last start with it did not come through (its guard). */
void pdo_android_gpu_driver_load(void);
/* A custom driver was loaded (pdo_android_gpu_driver_load). */
bool pdo_android_gpu_driver_custom(void);
/* The GPU device is about to be made with it: the guard again, until... */
void pdo_android_gpu_driver_device(void);
/* ...frames are on the screen: the driver works, the guard goes. */
void pdo_android_gpu_driver_started(void);
/* Installs a picked file (content:// address): the folder's name in name,
 * or false and why in err. */
bool pdo_android_gpu_driver_install(const char *uri, char *name, size_t name_len, char *err, size_t err_len);

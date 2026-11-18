/* Custom Vulkan drivers on Android: see gpu_driver.h. */
#include "gpu_driver.h"

#include <adrenotools/driver.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void *gpu_driver_open(const char *driver_dir, const char *lib_name, const char *native_lib_dir,
                      const char *tmp_dir, char *err, size_t err_len)
{
    char path[1024];
    void *h;

    snprintf(path, sizeof path, "%s%s", driver_dir, lib_name);
    if (access(path, R_OK) != 0) {
        snprintf(err, err_len, "%s is not there", path);
        return NULL;
    }
    h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp_dir, native_lib_dir, driver_dir,
                                   lib_name, NULL, NULL);
    if (!h) {
        const char *why = dlerror();
        snprintf(err, err_len, "libadrenotools could not open it (%s)", why ? why : "no reason given");
        return NULL;
    }
    if (!dlsym(h, "vkGetInstanceProcAddr")) {
        snprintf(err, err_len, "it has no vkGetInstanceProcAddr");
        return NULL;
    }
    return h;
}

bool gpu_driver_guard_begin(const char *guard_path)
{
    FILE *f;

    if (access(guard_path, F_OK) == 0)
        return false;
    f = fopen(guard_path, "w");
    if (f)
        fclose(f);
    return true;
}

void gpu_driver_guard_end(const char *guard_path)
{
    unlink(guard_path);
}

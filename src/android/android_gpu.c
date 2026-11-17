/* Android: the GPU driver chosen in the options (android_gpu.h). The
 * driver is installed by the Java side (GpuDriverInstaller) into
 * <data>/gpu_drivers/NAME/, with library.txt naming its library, and opened
 * here with libadrenotools (gpu_driver.c) for SDL to load Vulkan from. */
#include "android/android_gpu.h"
#include "android/gpu_driver/gpu_driver.h"
#include "config.h"

#include <SDL3/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>

void PDO_SetAndroidVulkanLibrary(void *handle);     /* patches/SDL/0003 */

static bool tried;

static void guard_path(char *out, size_t n)
{
    snprintf(out, n, "%sgpu_driver_trying", pdo_data_dir());
}

/* A static String method of OrtaActivity with one String argument (or
 * none), its answer in out. */
static bool call_activity(const char *method, const char *arg, char *out, size_t n)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    jclass cls = activity ? (*env)->GetObjectClass(env, activity) : NULL;
    jmethodID mid = cls ? (*env)->GetStaticMethodID(env, cls, method, arg ? "(Ljava/lang/String;)Ljava/lang/String;"
                                                                         : "()Ljava/lang/String;") : NULL;
    jstring jarg = NULL, res = NULL;
    bool ok = false;

    out[0] = 0;
    if (mid) {
        if (arg)
            jarg = (*env)->NewStringUTF(env, arg);
        res = arg ? (jstring)(*env)->CallStaticObjectMethod(env, cls, mid, jarg)
                  : (jstring)(*env)->CallStaticObjectMethod(env, cls, mid);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        res = NULL;
    }
    if (res) {
        const char *s = (*env)->GetStringUTFChars(env, res, NULL);
        if (s) {
            SDL_strlcpy(out, s, n);
            (*env)->ReleaseStringUTFChars(env, res, s);
            ok = true;
        }
        (*env)->DeleteLocalRef(env, res);
    }
    if (jarg)
        (*env)->DeleteLocalRef(env, jarg);
    if (cls)
        (*env)->DeleteLocalRef(env, cls);
    if (activity)
        (*env)->DeleteLocalRef(env, activity);
    return ok;
}

void pdo_android_gpu_driver_load(void)
{
    pdo_settings *s = pdo_settings_get();
    char dir[PDO_PATH_MAX], lib[256], guard[PDO_PATH_MAX], tmp[PDO_PATH_MAX], native[PDO_PATH_MAX], err[512];
    size_t size = 0;
    char *text;
    void *h;

    if (!s->gpu_driver[0])
        return;
    guard_path(guard, sizeof guard);
    if (!gpu_driver_guard_begin(guard)) {
        SDL_Log("gfx: GPU driver %s: the last start with it did not come through; the system's driver now",
                s->gpu_driver);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Orta Recomp",
                                 "The GPU driver chosen in the options did not start last time, so the "
                                 "phone's own driver is used again.", NULL);
        gpu_driver_guard_end(guard);
        s->gpu_driver[0] = 0;
        pdo_settings_save();
        return;
    }
    tried = true;
    snprintf(dir, sizeof dir, "%sgpu_drivers/%s/", pdo_data_dir(), s->gpu_driver);
    snprintf(tmp, sizeof tmp, "%slibrary.txt", dir);
    text = SDL_LoadFile(tmp, &size);
    if (!text || !size) {
        SDL_Log("gfx: GPU driver %s: not installed (no %s); the system's driver", s->gpu_driver, tmp);
        SDL_free(text);
        gpu_driver_guard_end(guard);
        tried = false;
        return;
    }
    SDL_strlcpy(lib, text, sizeof lib < size + 1 ? sizeof lib : size + 1);
    SDL_free(text);
    lib[strcspn(lib, "\r\n")] = 0;
    snprintf(tmp, sizeof tmp, "%sgpu_drivers/tmp/", pdo_data_dir());
    SDL_CreateDirectory(tmp);
    if (!call_activity("nativeLibraryDir", NULL, native, sizeof native) || !native[0]) {
        SDL_Log("gfx: GPU driver %s: no native library folder; the system's driver", s->gpu_driver);
        gpu_driver_guard_end(guard);
        tried = false;
        return;
    }
    SDL_strlcat(native, "/", sizeof native);
    h = gpu_driver_open(dir, lib, native, tmp, err, sizeof err);
    if (!h) {
        SDL_Log("gfx: GPU driver %s (%s): %s; the system's driver", s->gpu_driver, lib, err);
        gpu_driver_guard_end(guard);
        tried = false;
        return;
    }
    PDO_SetAndroidVulkanLibrary(h);
    gpu_driver_guard_end(guard);        /* opened; the device and the first frames are next */
    SDL_Log("gfx: GPU driver %s (%s) loaded in place of the system's", s->gpu_driver, lib);
}

void pdo_android_gpu_driver_device(void)
{
    char guard[PDO_PATH_MAX];

    if (!tried)
        return;
    guard_path(guard, sizeof guard);
    gpu_driver_guard_begin(guard);
}

void pdo_android_gpu_driver_started(void)
{
    char guard[PDO_PATH_MAX];

    if (!tried)
        return;
    tried = false;
    guard_path(guard, sizeof guard);
    gpu_driver_guard_end(guard);
}

bool pdo_android_gpu_driver_install(const char *uri, char *name, size_t name_len, char *err, size_t err_len)
{
    char res[512];

    if (!call_activity("gpuDriverInstall", uri, res, sizeof res)) {
        SDL_strlcpy(err, "the app could not be asked", err_len);
        return false;
    }
    if (!strncmp(res, "ok:", 3)) {
        SDL_strlcpy(name, res + 3, name_len);
        return true;
    }
    SDL_strlcpy(err, !strncmp(res, "error:", 6) ? res + 6 : res, err_len);
    return false;
}

/*
 * Switching between the programs on the disc: Orta (default.xbe) and the
 * original Panzer Dragoon (pd1.xbe), which Pandora's Box launches and which
 * goes back to Orta from its title (B).
 *
 * A title boots another image with XLaunchNewImage: it fills the kernel's
 * launch data page (type, title, path, then 3 KB of its own data) and asks
 * HalReturnToFirmware to reboot (routine 2). xboxrecomp's kernel calls
 * xbox_relaunch_hook there (patch 0037). If the image is a program this
 * build has, the page is saved to a file and OrtaRecomp starts again on that
 * program with --launch-data; the new run puts the page back where the title
 * reads it (XGetLaunchInfo), as the console's reboot keeps it.
 *
 * A new process, not a switch inside this one: the kernel layer and our
 * hooks were written for one boot per process (timers, threads blocked in
 * waits, the heap, caches keyed by guest addresses), and a restart resets
 * all of it. The window closes and opens again; settings, the window's
 * place and the log carry on (the log is appended to).
 */

#include "guest/guest.h"
#include "guest/program.h"
#include "config.h"

#include "kernel.h"
#include "xbox_memory_layout.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#define PAGE_SIZE   4096u
#define PATH_OFFSET 8u          /* szLaunchPath, after LaunchDataType and TitleId */

extern ptrdiff_t g_xbox_mem_offset;
extern void (*xbox_relaunch_hook)(ULONG Routine);

static int saved_argc;
static char **saved_argv;

static uint8_t *guest(uint32_t va)
{
    return (uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
}

static uint32_t launch_page(void)
{
    uint32_t v;
    memcpy(&v, guest(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE), 4);
    return v;
}

static void launch_data_path(char *out, size_t n)
{
    snprintf(out, n, "%slaunch_data.bin", pdo_data_dir());
}

/* The program an image path names ("\Device\CdRom0;pd1.xbe", "d:\pd1.xbe"),
 * if this build has it. */
static bool program_of(const char *path, pdo_program *out)
{
    const char *base = path;

    for (const char *c = path; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ';' || *c == ':')
            base = c + 1;
    for (int p = 0; p < PDO_PROGRAM_COUNT; p++) {
        if (SDL_strcasecmp(base, pdo_program_xbe((pdo_program)p)) == 0
                && pdo_program_built_from((pdo_program)p)) {
            *out = (pdo_program)p;
            return true;
        }
    }
    return false;
}

#ifdef _WIN32
/* One argument quoted for CommandLineToArgvW / the C runtime's parser. */
static void append_quoted(wchar_t *cmd, size_t cap, const char *arg)
{
    wchar_t w[1024];
    size_t n = wcslen(cmd);
    int len = MultiByteToWideChar(CP_UTF8, 0, arg, -1, w, (int)SDL_arraysize(w));

    if (len <= 0 || n + 3 >= cap)
        return;
    if (n)
        cmd[n++] = L' ';
    cmd[n++] = L'"';
    for (const wchar_t *c = w; *c && n + 4 < cap; c++) {
        size_t slashes = 0;
        while (c[0] == L'\\') {
            slashes++;
            c++;
        }
        if (!*c || *c == L'"') {        /* backslashes before a quote or the end double */
            for (size_t i = 0; i < slashes * 2 && n + 4 < cap; i++)
                cmd[n++] = L'\\';
            if (!*c)
                break;
            cmd[n++] = L'\\';
            cmd[n++] = L'"';
        } else {
            for (size_t i = 0; i < slashes && n + 4 < cap; i++)
                cmd[n++] = L'\\';
            cmd[n++] = *c;
        }
    }
    cmd[n++] = L'"';
    cmd[n] = 0;
}
#endif

/* Start OrtaRecomp again on `program`: this run's arguments without the ones
 * that chose the program or the launcher, then --boot --program NAME
 * --launch-data FILE. Returns only if it could not. */
static void restart_on(pdo_program program, const char *data_file)
{
    const char *extra[] = { "--boot", "--program", pdo_program_name(program),
                            "--launch-data", data_file };
    const char *args[64];
    int n = 0;

    args[n++] = saved_argv[0];
    for (int i = 1; i < saved_argc && n < 50; i++) {
        const char *a = saved_argv[i];
        if (!strcmp(a, "--program") || !strcmp(a, "--launch-data") || !strcmp(a, "--import-iso")) {
            i++;                        /* and its value */
            continue;
        }
        if (!strcmp(a, "--boot") || !strcmp(a, "--no-boot") || !strcmp(a, "--setup") || !strcmp(a, "--check"))
            continue;
        args[n++] = a;
    }
    for (size_t i = 0; i < SDL_arraysize(extra); i++)
        args[n++] = extra[i];
    args[n] = NULL;

    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    {
        static wchar_t exe[MAX_PATH], cmd[8192];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;

        if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
            return;
        cmd[0] = 0;
        for (int i = 0; i < n; i++)
            append_quoted(cmd, SDL_arraysize(cmd), args[i]);
        SDL_zero(si);
        si.cb = sizeof si;
        if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            SDL_Log("launch: cannot start %s again (error %lu)", pdo_program_name(program), GetLastError());
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
#else
    /* The same process, a new image: Steam and the Deck see no exit. */
    execv("/proc/self/exe", (char *const *)args);
    SDL_Log("launch: cannot start %s again: %s", pdo_program_name(program), strerror(errno));
#endif
}

static void relaunch_hook(ULONG routine)
{
    char path[64], file[PDO_PATH_MAX];
    uint32_t page = launch_page();
    pdo_program program;
    size_t i;
    SDL_IOStream *io;

    if (routine != 2 || !page)
        return;                         /* a plain reboot or quit: exit as before */
    for (i = 0; i + 1 < sizeof path && guest(page + PATH_OFFSET)[i]; i++)
        path[i] = (char)guest(page + PATH_OFFSET)[i];
    path[i] = 0;
    if (!path[0]) {
        SDL_Log("launch: %s returns to the dashboard; exiting", pdo_program_name(pdo_program_active()));
        return;
    }
    if (!program_of(path, &program)) {
        SDL_Log("launch: %s asks for %s, which this build cannot run; exiting",
                pdo_program_name(pdo_program_active()), path);
        return;
    }
    launch_data_path(file, sizeof file);
    io = SDL_IOFromFile(file, "wb");
    if (!io || SDL_WriteIO(io, guest(page), PAGE_SIZE) != PAGE_SIZE) {
        SDL_Log("launch: cannot write %s: %s", file, SDL_GetError());
        if (io)
            SDL_CloseIO(io);
        return;
    }
    SDL_CloseIO(io);
    SDL_Log("launch: %s starts %s (%s); restarting on it",
            pdo_program_name(pdo_program_active()), pdo_program_name(program), path);
    pdo_settings_save();                /* the window's place and size */
    restart_on(program, file);
}

void pdo_relaunch_install(int argc, char **argv)
{
    saved_argc = argc;
    saved_argv = argv;
    xbox_relaunch_hook = relaunch_hook;
}

bool pdo_relaunch_load(const char *file)
{
    size_t size = 0;
    void *data = SDL_LoadFile(file, &size);
    uint32_t page;

    if (!data || size != PAGE_SIZE) {
        SDL_Log("launch: no launch data in %s", file);
        SDL_free(data);
        return false;
    }
    page = xbox_HeapAlloc(PAGE_SIZE, PAGE_SIZE);
    if (page) {
        memcpy(guest(page), data, PAGE_SIZE);
        memcpy(guest(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE), &page, 4);
        SDL_Log("launch: launch data from the program before (type %u, from %08x)",
                ((uint32_t *)data)[0], ((uint32_t *)data)[1]);
    }
    SDL_free(data);
    SDL_RemovePath(file);
    return page != 0;
}

/*
 * What each guest thread is waiting for, for the watchdog's hang report.
 *
 * A hang shows up in the host thread dump as threads parked in
 * NtWaitFor*Object*, which says nothing about which guest object each one
 * waits on, or whether whatever should signal it still exists. This records,
 * at the title's own XAPI calls:
 * - each thread's start routine and argument (XapiThreadStartup), and its
 *   end (ExitThread);
 * - which handle CreateThread returned for which routine and argument,
 *   until CloseHandle;
 * - every WaitForSingleObjectEx / WaitForMultipleObjectsEx in progress.
 * pdo_dump_guest_waits() prints the waits that have lasted, and for a wait
 * on a thread handle, whether that thread is still around and what it
 * waits on itself (a stop signal that never arrived), or not (a handle that
 * was never signalled when the thread ended).
 *
 * Each thread writes only its own slot, without locking; the report reads
 * them as they are, which is good enough for a diagnosis.
 */

#include "recomp_types.h"

#include <SDL3/SDL.h>

#include <stdint.h>
#include <string.h>

#define MAX_SLOTS   4096
#define MAX_HANDLES 8
#define MAX_CREATED 512

typedef struct wait_slot {
    SDL_ThreadID tid;
    uint32_t routine, param;            /* from XapiThreadStartup; 0 for the main thread */
    volatile int ended;                 /* through ExitThread */
    volatile int waiting;               /* 1 single, 2 multiple */
    uint32_t n, handles[MAX_HANDLES], timeout, wait_all, caller;
    Uint64 since;
} wait_slot;

static wait_slot slots[MAX_SLOTS];
static SDL_AtomicInt nslots;
static RECOMP_TLS int my_slot = -1;

static struct { uint32_t handle, routine, param; } created[MAX_CREATED];
static SDL_AtomicInt ncreated;

static wait_slot *slot(void)
{
    if (my_slot < 0) {
        int i = SDL_AddAtomicInt(&nslots, 1);
        if (i >= MAX_SLOTS)
            return NULL;
        my_slot = i;
        slots[i].tid = SDL_GetCurrentThreadID();
    }
    return &slots[my_slot];
}

/* XAPILIB XapiThreadStartup(StartRoutine, StartContext): every thread the
 * title creates begins here. */
void sub_0022D5E6_enter(void)
{
    wait_slot *s = slot();
    if (s) {
        s->routine = MEM32(g_esp + 4);
        s->param = MEM32(g_esp + 8);
    }
}

/* XAPILIB ExitThread(dwExitCode): the thread is gone from here on. */
void sub_0022D4F1_enter(void)
{
    wait_slot *s = slot();
    if (s)
        s->ended = 1;
}

/* XAPILIB CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress,
 * lpParameter, dwCreationFlags, lpThreadId): the handle it returns. */
extern void sub_0022D67E_gen(void);
void sub_0022D67E(void)
{
    uint32_t routine = MEM32(g_esp + 12), param = MEM32(g_esp + 16);
    sub_0022D67E_gen();
    if (g_eax) {
        int i = SDL_AddAtomicInt(&ncreated, 1) % MAX_CREATED;
        created[i].handle = g_eax;
        created[i].routine = routine;
        created[i].param = param;
    }
}

/* XAPILIB CloseHandle(hObject) (0x0022D6F9, NtClose underneath): the
 * value can come back for another object, so forget it as a thread. */
void sub_0022D6F9_enter(void)
{
    uint32_t h = MEM32(g_esp + 4);
    int n = SDL_GetAtomicInt(&ncreated);
    for (int i = 0; i < (n < MAX_CREATED ? n : MAX_CREATED); i++)
        if (created[i].handle == h)
            created[i].handle = 0;
}

/* XAPILIB WaitForSingleObjectEx(hHandle, dwMilliseconds, bAlertable) */
extern void sub_0022D1E2_gen(void);
void sub_0022D1E2(void)
{
    wait_slot *s = slot();
    if (s) {
        s->n = 1;
        s->handles[0] = MEM32(g_esp + 4);
        s->timeout = MEM32(g_esp + 8);
        s->wait_all = 0;
        s->caller = MEM32(g_esp);
        s->since = SDL_GetTicksNS();
        s->waiting = 1;
    }
    sub_0022D1E2_gen();
    if (s)
        s->waiting = 0;
}

/* XAPILIB WaitForMultipleObjectsEx(nCount, lpHandles, bWaitAll,
 * dwMilliseconds, bAlertable) */
extern void sub_0022D226_gen(void);
void sub_0022D226(void)
{
    wait_slot *s = slot();
    if (s) {
        uint32_t n = MEM32(g_esp + 4), h = MEM32(g_esp + 8);
        s->n = n;
        for (uint32_t i = 0; i < n && i < MAX_HANDLES; i++)
            s->handles[i] = h ? MEM32(h + 4 * i) : 0;
        s->wait_all = MEM32(g_esp + 12);
        s->timeout = MEM32(g_esp + 16);
        s->caller = MEM32(g_esp);
        s->since = SDL_GetTicksNS();
        s->waiting = 2;
    }
    sub_0022D226_gen();
    if (s)
        s->waiting = 0;
}

static int created_index(uint32_t handle)
{
    int n = SDL_GetAtomicInt(&ncreated);
    for (int i = (n < MAX_CREATED ? n : MAX_CREATED) - 1; i >= 0; i--)
        if (created[i].handle == handle)
            return i;
    return -1;
}

static void describe_wait(const wait_slot *s, Uint64 now)
{
    char hs[128] = "";
    for (uint32_t i = 0; i < s->n && i < MAX_HANDLES; i++)
        SDL_snprintf(hs + strlen(hs), sizeof hs - strlen(hs), "%s0x%08x", i ? " " : "", s->handles[i]);
    SDL_Log("  thread %llu (routine 0x%08x, argument 0x%08x): %s [%s]%s, timeout %d ms, from 0x%08x, %.1f s",
            (unsigned long long)s->tid, s->routine, s->param,
            s->waiting == 1 ? "WaitForSingleObjectEx" : "WaitForMultipleObjectsEx", hs,
            s->waiting == 2 ? (s->wait_all ? " all" : " any") : "", (int)s->timeout, s->caller,
            (double)(now - s->since) / 1e9);
}

void pdo_dump_guest_waits(void)
{
    Uint64 now = SDL_GetTicksNS();
    int n = SDL_GetAtomicInt(&nslots);

    if (n > MAX_SLOTS)
        n = MAX_SLOTS;
    SDL_Log("guest waits lasting over 2 s:");
    for (int i = 0; i < n; i++) {
        const wait_slot *s = &slots[i];
        if (!s->waiting || now - s->since < 2 * SDL_NS_PER_SECOND)
            continue;
        describe_wait(s, now);
        /* A wait on a thread's handle: is that thread still here? */
        for (uint32_t h = 0; h < s->n && h < MAX_HANDLES; h++) {
            int c = created_index(s->handles[h]);
            bool found = false;
            if (c < 0)
                continue;
            SDL_Log("    0x%08x is the thread created for routine 0x%08x, argument 0x%08x",
                    s->handles[h], created[c].routine, created[c].param);
            for (int j = 0; j < n; j++)
                if (!slots[j].ended && slots[j].param == created[c].param
                        && slots[j].routine == created[c].routine) {
                    found = true;
                    if (slots[j].waiting) {
                        SDL_Log("    it is still running, waiting itself:");
                        describe_wait(&slots[j], now);
                    } else {
                        SDL_Log("    it is still running, not in a wait");
                    }
                }
            if (!found)
                SDL_Log("    no thread with that routine and argument is running: it ended, "
                        "and its handle was not signalled");
        }
    }
}

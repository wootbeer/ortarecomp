/*
 * CRI ADX (the music and voice middleware): its lock, made a real lock.
 *
 * CRI's LOCK (0x00233BD0) and UNLOCK (0x00233C10) keep a nesting count at
 * 0x00485B20 (the same code as JSRF's 0x0013B0A0 and 0x0013B0E0, count
 * 0x0025EFA0). The outermost
 * LOCK saves the thread's priority, raises it and resumes a helper thread; the outermost UNLOCK suspends the helper and
 * restores the priority. On the Xbox's single CPU, with its strict priority
 * scheduler, that keeps CRI's other threads off the CPU, so it works as
 * mutual exclusion and the plain count is safe.
 *
 * Here guest threads run truly in parallel, so it excludes nothing: two
 * threads were seen inside at once, the non-atomic count lost a decrement
 * and never returned to 0, and from then on the priority bookkeeping was
 * wrong for good. JSRF's music never started; Orta's Sofdec movie stopped
 * reading after its first 6 MB.
 *
 * So the region between them is serialised with one host mutex (SDL mutexes
 * are recursive, as CRI's nesting needs). The game's own code still runs
 * inside, so the count, priorities and helper thread behave as on hardware.
 */

#include "recomp_types.h"
#include "hle/program_va.h"

#include <SDL3/SDL.h>

extern void sub_00233BD0_gen(void);
extern void sub_00233C10_gen(void);
extern void sub_00236BC0_gen(void);
extern void sub_00236080_gen(void);

static SDL_AtomicInt lock_ready;
static SDL_Mutex *lock;

/* src/guest/guest.c turns xboxrecomp's guest CPU lock on: one guest thread
 * runs at a time and gives the CPU up at every kernel call. A thread inside
 * CRI's region can be in such a call, waiting to get the CPU back, so a
 * thread may not wait for this mutex while it holds the CPU: the two would
 * wait for each other. Wait with the CPU given up, then take it back. */
int guest_cpu_yield_begin(void);
void guest_cpu_yield_end(int held);

static void enter_cri(void);

static SDL_Mutex *cri_lock(void)
{
    /* First use can come from two threads at once. */
    if (SDL_GetAtomicInt(&lock_ready) != 2) {
        if (SDL_CompareAndSwapAtomicInt(&lock_ready, 0, 1)) {
            lock = SDL_CreateMutex();
            SDL_SetAtomicInt(&lock_ready, 2);
        } else {
            while (SDL_GetAtomicInt(&lock_ready) != 2)
                SDL_Delay(0);
        }
    }
    return lock;
}

/* Every 10 s, how often CRI's threads ran. Its two server threads each run
 * once per vblank and share one counter (~1200 per 10 s); each wakes the
 * decoder, which runs at most once per wake-up. Music needs more than 43
 * decoder passes a second (each makes at most 1024 samples of 44.1 kHz). */
static void report_passes(void)
{
    static uint64_t next;
    static uint32_t last_server, last_worker;
    uint64_t now = SDL_GetTicks();
    uint32_t server, worker;

    if (now < next)
        return;
    server = MEM32(PROG_VA(0x00485B2Cu, 0x009F1140u));
    worker = MEM32(PROG_VA(0x00485B30u, 0x009F1144u));
    if (next)
        SDL_Log("cri: last 10 s: %u server passes, %u decoder passes",
                server - last_server, worker - last_worker);
    last_server = server;
    last_worker = worker;
    next = now + 10000;
}

static void enter_cri(void)
{
    if (!SDL_TryLockMutex(cri_lock())) {
        int held = guest_cpu_yield_begin();
        SDL_LockMutex(cri_lock());
        guest_cpu_yield_end(held);
    }
}

/* CRI LOCK */
void sub_00233BD0(void)
{
    enter_cri();
    report_passes();
    sub_00233BD0_gen();
}

/* CRI UNLOCK */
void sub_00233C10(void)
{
    sub_00233C10_gen();
    SDL_UnlockMutex(cri_lock());
}

/* CRI's server passes run under the same lock. On the Xbox, LOCK raises the
 * caller to priority 15 and resumes the spin thread (0x00233CB0, priority
 * 2): neither the vblank servers (priority 1), the decoder worker (-2) nor
 * the thread at 0x00234120 (1) can run while a LOCK is held, so no pass
 * ever runs during a locked region. Here they run in parallel with it:
 * skipping a movie, the main thread tore the movie down under LOCK while a
 * pass was still using it, and the pass called through a cleared vtable
 * (null at +0x24, from 0x0024D7C0). */

/* Server pass of one level: runs that level's registered callbacks
 * (2 vblank server, 3 main, 5 idle worker). */
void sub_00236BC0(void)
{
    enter_cri();
    sub_00236BC0_gen();
    SDL_UnlockMutex(cri_lock());
}

/* Pass of the thread at 0x00234120 (once per vblank). */
void sub_00236080(void)
{
    enter_cri();
    sub_00236080_gen();
    SDL_UnlockMutex(cri_lock());
}

/*
 * CRI's spin thread (LOCK above resumes it, UNLOCK suspends it): it counts
 * at 0x00485B28 in a loop until 0x00485B40 is set, then ends its thread.
 * The loop makes no kernel call: on the console the scheduler preempts it;
 * under the guest CPU lock (guest.c turns it on) it would hold the lock
 * forever and no other guest thread would run again. Same loop, giving the
 * lock up each time round. Nothing reads the count.
 */
#define SPIN_COUNT  PROG_VA(0x00485B28u, 0x009F113Cu)
#define SPIN_STOP   PROG_VA(0x00485B40u, 0x009F1154u)

extern void sub_00233CB0_gen(void);

void sub_00233CB0(void)
{
    while (!MEM32(SPIN_STOP)) {
        int held;
        MEM32(SPIN_COUNT) += 1;
        held = guest_cpu_yield_begin();
        SDL_Delay(1);
        guest_cpu_yield_end(held);
    }
    sub_00233CB0_gen();         /* sees the flag: ends the thread */
}

/*
 * Crash report: say where a hard crash happened.
 *
 * A crash in game code otherwise ends the process with no word in the log.
 * This prints, to stderr (the log), the exception and the host
 * address with its function and source line from the build's own debug
 * information, the guest registers, and the game-code return addresses
 * still on the guest stack: those name the guest call chain, and each one
 * can be looked up in gen/ by address.
 *
 * Released builds ship without their symbols, so a host address with none
 * is given as its offset in the executable (read with the .pdb or the
 * unstripped binary of that build) and, as a hint, the game function before
 * it (sub_XXXXXXXX, from the dispatch table; overrides in src/ are among
 * those, so our own code can sit "near" one too).
 *
 * Windows: an unhandled-exception filter, with symbols from the .pdb when
 * it is there, and F11/watchdog thread dumps. Linux: a handler for the
 * fatal signals on its own stack (a stack overflow too), with glibc's
 * backtrace; thread dumps there are gdb's (thread apply all bt).
 */

#ifdef _WIN32
/* Before the generated-code header, whose short macros would clash. */
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#else
#define _GNU_SOURCE
#include <dlfcn.h>
#ifdef __ANDROID__
#include <unwind.h>
#else
#include <execinfo.h>
#endif
#include <signal.h>
#include <unistd.h>
#endif

#include "recomp_types.h"

#include "xbox_memory_layout.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Game functions by host address, built at the first report. */
typedef struct { uint64_t host; uint32_t va; } func_at;
static func_at funcs[1 << 15];
static int nfuncs = -1;

static int by_host(const void *a, const void *b)
{
    uint64_t x = ((const func_at *)a)->host, y = ((const func_at *)b)->host;
    return x < y ? -1 : x > y;
}

static uint32_t game_function_at(uint64_t addr, uint64_t *off)
{
    int lo = 0, hi;

    if (nfuncs < 0) {
        nfuncs = 0;
        for (uint32_t va = g_xbox_code_lo; va < g_xbox_code_hi && nfuncs < (int)(sizeof funcs / sizeof *funcs); va++) {
            recomp_func_t f = recomp_lookup(va);
            if (f) {
                funcs[nfuncs].host = (uint64_t)(uintptr_t)f;
                funcs[nfuncs++].va = va;
            }
        }
        qsort(funcs, (size_t)nfuncs, sizeof *funcs, by_host);
    }
    hi = nfuncs;
    while (lo < hi) {               /* the last function starting at or before addr */
        int mid = (lo + hi) / 2;
        if (funcs[mid].host <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    /* Only between two game functions, and near the one before: past the
     * last one, or far from it, is our own code. */
    if (lo == 0 || lo == nfuncs)
        return 0;
    *off = addr - funcs[lo - 1].host;
    return *off < 0x10000 ? funcs[lo - 1].va : 0;
}

/* The guest registers, and the return addresses into game code still on the
 * guest stack: not every code-range value there is one, but the call chain
 * is among them. */
static void print_guest_state(void)
{
    uint32_t sp = g_esp;   /* not "esp": recomp_types.h defines that */
    int shown = 0;

    fprintf(stderr, "  guest eax %08X ebx %08X ecx %08X edx %08X\n"
                    "        esi %08X edi %08X esp %08X\n",
            g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, sp);
    fprintf(stderr, "  guest stack, code addresses:");
    if (sp >= 0x1000 && sp < XBOX_HEAP_TOP - 0x400) {
        for (uint32_t a = sp; a < sp + 0x400 && shown < 16; a += 4) {
            uint32_t v = MEM32(a);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, " %08X", v);
                shown++;
            }
        }
    }
    fprintf(stderr, shown ? "\n" : " (none)\n");
}

#ifdef _WIN32

static void print_host_address(const char *what, DWORD64 addr)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    IMAGEHLP_LINE64 line = { sizeof line };
    DWORD64 sym_off = 0;
    DWORD line_off = 0;

    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    fprintf(stderr, "  %s 0x%016llx", what, (unsigned long long)addr);
    if (SymFromAddr(GetCurrentProcess(), addr, &sym_off, sym)) {
        fprintf(stderr, "  %s+0x%llx", sym->Name, (unsigned long long)sym_off);
        if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &line_off, &line))
            fprintf(stderr, "  (%s:%lu)", line.FileName, line.LineNumber);
    } else {
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)addr, &mod)) {
            char *base;
            GetModuleFileNameA(mod, name, sizeof name);
            base = strrchr(name, '\\');
            fprintf(stderr, "  %s+0x%llx", base ? base + 1 : name, (unsigned long long)(addr - (DWORD64)(uintptr_t)mod));
            if (mod == GetModuleHandleA(NULL)) {
                uint64_t off = 0;
                uint32_t va = game_function_at(addr, &off);
                if (va)
                    fprintf(stderr, "  (near sub_%08X+0x%llx)", va, (unsigned long long)off);
            }
        }
    }
    fputc('\n', stderr);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;

    fprintf(stderr, "\n=== CRASH: exception 0x%08lX", er->ExceptionCode);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, " (%s host 0x%llx, guest 0x%08llx)",
                er->ExceptionInformation[0] == 1 ? "write to" :
                er->ExceptionInformation[0] == 8 ? "execute at" : "read of",
                (unsigned long long)er->ExceptionInformation[1],
                (unsigned long long)(er->ExceptionInformation[1] - (ULONG_PTR)g_xbox_mem_offset));
    fprintf(stderr, " on thread %lu\n", GetCurrentThreadId());

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
#ifdef _M_X64
    print_host_address("at", c->Rip);
    {   /* A few host frames: the C caller chain inside generated code. */
        STACKFRAME64 f = {0};
        CONTEXT copy = *c;
        f.AddrPC.Offset = c->Rip;     f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = c->Rbp;  f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = c->Rsp;  f.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 12 && StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                GetCurrentProcess(), GetCurrentThread(), &f, &copy, NULL,
                SymFunctionTableAccess64, SymGetModuleBase64, NULL); i++) {
            if (i == 0)
                continue;
            print_host_address("from", f.AddrPC.Offset);
        }
    }
#endif
    print_guest_state();
    fflush(stderr);
    if (pdo_log_file[0]) {
        char msg[PDO_PATH_MAX + 256];
        snprintf(msg, sizeof msg, "Orta Recomp crashed.\n\nThe details are in\n%s\n\n"
                 "Please attach that file when you report it.", pdo_log_file);
        MessageBoxA(NULL, msg, "Orta Recomp", MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void pdo_crash_report_install(void)
{
    SetUnhandledExceptionFilter(crash_filter);
}

/* PDO_WATCH_RETICLE (issue 51): who writes a few bytes of guest memory.
 * Their page is made read-only; each write to it faults here, is let
 * through one instruction (the page writable, the trap flag set, then
 * read-only again), and one inside the watched bytes names the code
 * doing it, once per place, with the game function it is in. Writes from
 * other threads in that one instruction's window slip by; it is for
 * finding code, not for counting. */
static uint8_t *watch_page, *watch_lo, *watch_hi;
#ifdef _MSC_VER
static __declspec(thread) int watch_rearm;    /* this thread stepped one write */
#else
static __thread int watch_rearm;
#endif
static uint64_t watch_seen[64];
static int watch_nseen;

static LONG CALLBACK watch_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    DWORD old;

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && watch_page && er->NumberParameters >= 2 &&
        er->ExceptionInformation[0] == 1) {
        uint8_t *a = (uint8_t *)er->ExceptionInformation[1];
        if (a < watch_page || a >= watch_page + 4096)
            return EXCEPTION_CONTINUE_SEARCH;
        if (a >= watch_lo && a < watch_hi) {
            uint64_t rip = ep->ContextRecord->Rip;
            int i;
            for (i = 0; i < watch_nseen && watch_seen[i] != rip; i++)
                ;
            if (i == watch_nseen && watch_nseen < 64) {
                watch_seen[watch_nseen++] = rip;
                fprintf(stderr, "watch: +0x%02X written\n", (unsigned)(a - watch_lo));
                /* the place (symbol, or the module and offset; the nearest
                 * game function at any distance: some generated ones are
                 * larger than game_function_at's 64 KB) and the game's
                 * call chain from its stack */
                print_host_address("by", rip);
                {
                    int lo = 0, hi = nfuncs;
                    while (lo < hi) {
                        int mid = (lo + hi) / 2;
                        if (funcs[mid].host <= rip)
                            lo = mid + 1;
                        else
                            hi = mid;
                    }
                    if (lo > 0)
                        fprintf(stderr, "  in or after sub_%08X (+0x%llx)\n", funcs[lo - 1].va,
                                (unsigned long long)(rip - funcs[lo - 1].host));
                }
                print_guest_state();
                fflush(stderr);
            }
        }
        VirtualProtect(watch_page, 4096, PAGE_READWRITE, &old);
        ep->ContextRecord->EFlags |= 0x100;     /* one instruction, then back here */
        watch_rearm = 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && watch_rearm) {
        watch_rearm = 0;
        VirtualProtect(watch_page, 4096, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void pdo_debug_watch_writes(void *host, size_t bytes)
{
    DWORD old;

    if (watch_page)
        return;                                 /* once a run */
    watch_lo = host;
    watch_hi = watch_lo + bytes;
    watch_page = (uint8_t *)((uintptr_t)host & ~(uintptr_t)4095);
    {   /* the function map and symbols, now rather than inside the handler */
        uint64_t off;
        game_function_at(0, &off);
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
    }
    AddVectoredExceptionHandler(1, watch_handler);
    if (!VirtualProtect(watch_page, 4096, PAGE_READONLY, &old)) {
        fprintf(stderr, "watch: could not protect the page (%lu)\n", GetLastError());
        watch_page = NULL;
        return;
    }
    fprintf(stderr, "watch: writes to %p (%zu bytes) logged\n", host, bytes);
    fflush(stderr);
}

/* F11: every thread's host call stack, for hangs. Generated functions are
 * named after their guest address, so this shows where each game thread
 * waits. Each thread is suspended only while its stack is unwound with
 * RtlVirtualUnwind (no allocation, so a thread holding the heap lock cannot
 * deadlock us); symbols are looked up after it resumes. */
#ifdef _M_X64
enum { MAX_THREADS = 64, MAX_FRAMES = 24 };

static int unwind(CONTEXT *ctx, DWORD64 *pcs)
{
    int n = 0;

    while (n < MAX_FRAMES && ctx->Rip) {
        DWORD64 base = 0, frame = 0;
        PRUNTIME_FUNCTION rf;
        PVOID handler = NULL;

        pcs[n++] = ctx->Rip;
        rf = RtlLookupFunctionEntry(ctx->Rip, &base, NULL);
        if (rf) {
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, rf, ctx, &handler, &frame, NULL);
        } else {                      /* leaf: return address on top */
            ctx->Rip = *(DWORD64 *)ctx->Rsp;
            ctx->Rsp += 8;
        }
    }
    return n;
}
#endif

void pdo_dump_threads(void)
{
#ifdef _M_X64
    static DWORD64 pcs[MAX_THREADS][MAX_FRAMES];
    static DWORD ids[MAX_THREADS];
    static int counts[MAX_THREADS];
    int nthreads = 0;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    {   /* This thread first: F11 is handled while the game's main thread
         * pumps window events (in a vblank wait or present). */
        CONTEXT ctx;
        RtlCaptureContext(&ctx);
        ids[0] = self;
        counts[nthreads++] = unwind(&ctx, pcs[0]);
    }
    for (BOOL ok = Thread32First(snap, &te); ok && nthreads < MAX_THREADS; ok = Thread32Next(snap, &te)) {
        HANDLE th;
        CONTEXT ctx;
        int n = 0;

        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
            continue;
        th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                        te.th32ThreadID);
        if (!th)
            continue;
        if (SuspendThread(th) != (DWORD)-1) {
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &ctx))
                n = unwind(&ctx, pcs[nthreads]);
            ResumeThread(th);
        }
        CloseHandle(th);
        ids[nthreads] = te.th32ThreadID;
        counts[nthreads++] = n;
    }
    CloseHandle(snap);

    fprintf(stderr, "\n=== THREADS: %d\n", nthreads);
    for (int t = 0; t < nthreads; t++) {
        fprintf(stderr, "thread %lu%s\n", ids[t], t ? "" : " (this one: took the dump)");
        for (int i = 0; i < counts[t]; i++)
            print_host_address(i ? "from" : "at", pcs[t][i]);
    }
    fprintf(stderr, "=== end of threads\n");
    fflush(stderr);
#endif
}

#else

static void print_host_frame(void *pc)
{
    Dl_info info;

    fprintf(stderr, "  at %p", pc);
    if (dladdr(pc, &info) && info.dli_fname) {
        const char *base = strrchr(info.dli_fname, '/');
        uint64_t off = 0;
        uint32_t va;
        fprintf(stderr, "  %s+0x%llx", base ? base + 1 : info.dli_fname,
                (unsigned long long)((uintptr_t)pc - (uintptr_t)info.dli_fbase));
        if (info.dli_sname)
            fprintf(stderr, "  %s", info.dli_sname);
        else if ((va = game_function_at((uint64_t)(uintptr_t)pc, &off)) != 0)
            fprintf(stderr, "  (near sub_%08X+0x%llx)", va, (unsigned long long)off);
    }
    fputc('\n', stderr);
}

#ifdef __ANDROID__
/* Android's C library has backtrace() only from Android 13 (API 33); the
 * unwinder every Android build carries does the same. */
typedef struct unwind_trace { void **pcs; int n, max; } unwind_trace;

static _Unwind_Reason_Code unwind_one(struct _Unwind_Context *ctx, void *arg)
{
    unwind_trace *t = arg;
    uintptr_t pc = _Unwind_GetIP(ctx);

    if (pc && t->n < t->max)
        t->pcs[t->n++] = (void *)pc;
    return t->n < t->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}

static int backtrace(void **pcs, int max)
{
    unwind_trace t = { pcs, 0, max };
    _Unwind_Backtrace(unwind_one, &t);
    return t.n;
}
#endif

static void crash_signal(int sig, siginfo_t *si, void *uc)
{
    void *pcs[24];
    int n;

    (void)uc;
    fprintf(stderr, "\n=== CRASH: signal %d (%s), address %p (guest 0x%08llx) on thread %ld\n", sig,
            strsignal(sig), si->si_addr,
            (unsigned long long)((uintptr_t)si->si_addr - (uintptr_t)g_xbox_mem_offset), (long)gettid());
    n = backtrace(pcs, 24);
    for (int i = 1; i < n; i++)     /* 0 is this handler */
        print_host_frame(pcs[i]);
    print_guest_state();
    if (pdo_log_file[0])
        fprintf(stderr, "Orta Recomp crashed. Please attach this log (%s) when you report it.\n", pdo_log_file);
    fflush(stderr);
    signal(sig, SIG_DFL);           /* and end as the signal would have */
    raise(sig);
}

void pdo_crash_report_install(void)
{
    static char alt[64 * 1024];     /* this thread (the main one): a stack overflow still gets its report */
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
    struct sigaction sa;
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };

    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
        sigaction(sigs[i], &sa, NULL);
    backtrace(&(void *){ NULL }, 1);    /* loads libgcc now, not in the handler */
}

void pdo_dump_threads(void)
{
    fprintf(stderr, "thread dump: Windows only; on Linux use gdb (thread apply all bt)\n");
}

#endif

#ifndef _WIN32
void pdo_debug_watch_writes(void *host, size_t bytes)
{
    (void)host;
    (void)bytes;
    fprintf(stderr, "watch: Windows only\n");
}
#endif

/* nn::os threading and synchronisation on top of Win32.
 *
 * Guest synchronisation objects (MutexType, EventType, ...) are opaque here:
 * each one is identified by its guest address and backed by a host object
 * kept in a side table, so the guest layout never has to be known. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <malloc.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
/* L1 round 3, TEST ONLY: guest sync calls during the SSBU_L1_TRACE window (display.c) */
int display_l1_trace(void); double display_l1_ms(void); const char *guest_thread_name_of(unsigned long tid);
#define L1O(what, key) do { if (display_l1_trace()) { const char *n_ = guest_thread_name_of(GetCurrentThreadId()); printf("l1-trace: %+8.2f %-26s lr %09llx %s %llx%c", display_l1_ms(), n_ ? n_ : "?", (unsigned long long)c->x[30], what, (unsigned long long)(key), 10); } } while (0)

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define TICK_HZ 19200000ULL

typedef struct Obj {
    uint64_t key;
    struct Obj *next;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    int signaled, autoclear;                 /* events */
    int count, max;                          /* semaphores */
    uint64_t *buf, cap, len, head;           /* message queues */
    HANDLE thread;                           /* threads */
    uint64_t fn, arg, stack, stack_size, name;
    int priority, finished;
    void *fiber;                             /* fibers */
    uint64_t tp;                             /* fibers: thread block (tpidr) of the host thread that runs the fiber now */
    int ideal_core, core_set;                /* threads: nn::os ideal core and affinity mask as the game set them */
    uint64_t core_mask;
    uint64_t target, user;                   /* multi-wait holders */
    struct Obj *holders[64];                 /* multi-wait */
    int holder_count;
    Ctx *fctx;                               /* fibers: the guest context of the fiber (R1 save / restore) */
    volatile DWORD run_tid;                  /* fibers: host thread running it now, 0 = switched away (R1) */
    void *fls;                               /* fibers: the fighter runtime's per-fiber block (FLS slot g_r1_fls_idx) when it last switched away (R1) */
} Obj;

#define BUCKETS 65536
static Obj *g_table[BUCKETS];
extern int g_r1_keep_fibers;                           /* R1 (r1_state.c): set only with SSBU_R1 */
DWORD g_r1_fls_idx = 0xFFFFFFFF;                       /* R1: FLS slot of fighter_host.c's per-fiber block (from the map) */
static SRWLOCK g_table_lock = SRWLOCK_INIT;

/* Round 17 (P3 section 16): every lock / unlock / event call of every guest thread came through this look-up, which
 * took ONE process-wide lock exclusively. Records are never removed and a record's key and next pointer never change
 * once it is in the table, so a look-up can walk the bucket without the lock; only a miss takes the lock (and looks
 * again under it). SSBU_OBJ_LOCKFREE=0: the lock at every look-up, as before. */
#define OBJ_LOCKFREE_DEFAULT 0
static int g_obj_lf = -1;
static Obj *obj(uint64_t key) {
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (BUCKETS - 1);
    if (g_obj_lf < 0) { const char *e = getenv("SSBU_OBJ_LOCKFREE"); g_obj_lf = e && e[0] ? atoi(e) != 0 : OBJ_LOCKFREE_DEFAULT; }
    if (g_obj_lf) {
        Obj *f = *(Obj *volatile *)&g_table[h];
        while (f && f->key != key) f = f->next;
        if (f) return f;
    }
    AcquireSRWLockExclusive(&g_table_lock);
    Obj *o = g_table[h];
    while (o && o->key != key) o = o->next;
    if (!o) {
        o = calloc(1, sizeof *o);
        o->key = key;
        InitializeCriticalSection(&o->cs);
        InitializeConditionVariable(&o->cv);
        o->next = g_table[h];
        InterlockedExchangePointer((void *volatile *)&g_table[h], o);      /* published complete */
    }
    ReleaseSRWLockExclusive(&g_table_lock);
    return o;
}
/* Forget an object so a new one created at the same address starts clean. */
static void obj_reset(uint64_t key) {
    Obj *o = obj(key);
    EnterCriticalSection(&o->cs);
    o->signaled = o->autoclear = o->count = o->max = 0;
    o->len = o->head = 0;
    o->holder_count = 0;
    LeaveCriticalSection(&o->cs);
}

static DWORD ns_to_ms(int64_t ns) { return ns <= 0 ? 0 : (DWORD)((ns + 999999) / 1000000); }

/* nn::os::SleepThread takes nanoseconds and the console's timer is good to a few microseconds. Sleep() counts whole
 * milliseconds on a 1 ms (or 15.6 ms) grid, so the wait runs on a high-resolution waitable timer (100 ns units,
 * Windows 10 1803 and later); Sleep() is only the fallback. A zero or negative span yields, as on the console. */
static void sleep_ns(int64_t ns) {
    static __declspec(thread) HANDLE timer;
    static __declspec(thread) int no_timer;
    LARGE_INTEGER due;
    if (ns <= 0) { SwitchToThread(); return; }
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!timer) no_timer = 1;
    }
    due.QuadPart = -((ns + 99) / 100);
    /* Session 8 (track V), SSBU_SLEEP_LOG=1: a line for every sleep that lasted 2 ms longer than asked (first 200). */
    static int lg = -1; static LONG nl;
    if (lg < 0) lg = getenv("SSBU_SLEEP_LOG") && atoi(getenv("SSBU_SLEEP_LOG"));
    LARGE_INTEGER q0, q1, fq;
    if (lg) QueryPerformanceCounter(&q0);
    if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) Sleep(ns_to_ms(ns));
    else WaitForSingleObject(timer, INFINITE);
    if (lg) {
        extern long long nvn_front_frame_next(void);
        QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&fq);
        double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart, asked = (double)ns / 1e6;
        {   /* per thread and frame: how many sleeps, asked and slept in all; a line when a frame after 1900 had over 8 ms of them */
            static __declspec(thread) long long fr0, cnt, zero; static __declspec(thread) double a_sum, s_sum, a_max; static LONG nf;
            long long fr = nvn_front_frame_next();
            if (fr != fr0) {
                if (fr0 > 1900 && s_sum > 8.0 && InterlockedIncrement(&nf) <= 300) printf("sleep_log: frame %lld, thread %lu: %lld sleeps in the frame, asked %.3f ms in all (longest asked %.3f), slept %.3f ms\n", fr0, GetCurrentThreadId(), cnt, a_sum, a_max, s_sum);
                fr0 = fr; cnt = 0; a_sum = s_sum = a_max = 0;
            }
            cnt++; a_sum += asked; s_sum += ms; if (asked > a_max) a_max = asked;
        }
        if (ms > asked + 2.0 && InterlockedIncrement(&nl) <= 600) { printf("sleep_log: frame %lld, thread %lu: asked %.3f ms, slept %.3f ms (%s)\n", nvn_front_frame_next(), GetCurrentThreadId(), asked, ms, timer ? "high-resolution timer" : "Sleep"); fflush(stdout); }
    }
}

/* nn::os thread priorities run from 0 (highest) to 31 (lowest), 16 is the default; the application's main thread
 * starts at the priority of its NPDM (44 - 28 = 16 in nn::os terms). Windows has seven levels per process class:
 * the order is kept, the spacing is not. */
#define OS_DEFAULT_PRIORITY 16
#define OS_DEFAULT_CORE 0                    /* main.npdm: default core id 0 */
/* Console scheduling model, OPT-IN: SSBU_OS_CONSOLE_CORES=1 (bounded round 2026-10-09). What the game asks of nn::os in a
 * match (os_thread_report, the list at the end of every run): every thread is created on ONE core of the console's
 * three (mask 1, 2 or 4) except four with mask 7, at the priorities 1, 8, 9, 15, 16, 23 and 31, and the console runs on
 * each core only the highest-priority ready thread of that core. With the switch on:
 *   - three host processors stand for the console's cores 0..2 and every guest thread's affinity is its nn::os core mask
 *     mapped onto them (set at StartThread and at every SetThreadCoreMask), so two threads the console keeps on one core
 *     never run at the same time here either;
 *   - six priority levels instead of five (<= 4, 5..8, 9..15, 16, 17..27, 28..31), Windows' dynamic priority boost off:
 *     the order of the seven values the game uses is kept except 9 against 15;
 *   - GetCurrentCoreNumber / GetCurrentProcessorNumber answer the core the thread is really on.
 * NOT the console's scheduler all the same: Windows time-slices equal priorities, other processes of the PC take the
 * three processors too, and host-side threads (graphics driver, audio device, movie decoding) are not part of it.
 * Off by default: three pinned processors on a busy PC cost frame rate (measured in the round: see R6_fake_audit.md). */
static int g_cc = -1;
static DWORD_PTR g_cc_core[3];
static int console_cores(void) {
    if (g_cc < 0) {
        int n = 0, on = 0;
        if (getenv("SSBU_OS_CONSOLE_CORES")) {
            DWORD_PTR proc = 0, sys = 0;
            GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
            for (int b = (int)(sizeof(DWORD_PTR) * 8) - 1; b >= 0 && n < 3; b--)      /* from the top, every second processor: not two hardware threads of one core */
                if (proc >> b & 1) { g_cc_core[n++] = (DWORD_PTR)1 << b; b--; }
            on = n == 3;
            printf("nn::os: console core model %s (SSBU_OS_CONSOLE_CORES): host processor masks %llx %llx %llx for cores 0 1 2\n", on ? "ON" : "NOT possible (fewer than 3 usable processors)",
                   (unsigned long long)g_cc_core[0], (unsigned long long)g_cc_core[1], (unsigned long long)g_cc_core[2]);
        }
        g_cc = on;
    }
    return g_cc;
}
static void apply_sched(HANDLE h, uint64_t core_mask) {
    DWORD_PTR m = 0;
    if (!console_cores() || !h) return;
    for (int i = 0; i < 3; i++) if (core_mask >> i & 1) m |= g_cc_core[i];
    SetThreadAffinityMask(h, m ? m : g_cc_core[0]);
    SetThreadPriorityBoost(h, TRUE);
}
static int host_priority(int prio) {
    if (console_cores()) {
        if (prio <= 4) return THREAD_PRIORITY_TIME_CRITICAL;
        if (prio <= 8) return THREAD_PRIORITY_HIGHEST;
    }
    if (prio <= 4) return THREAD_PRIORITY_HIGHEST;
    if (prio <= 15) return THREAD_PRIORITY_ABOVE_NORMAL;
    if (prio == 16) return THREAD_PRIORITY_NORMAL;
    if (prio <= 27) return THREAD_PRIORITY_BELOW_NORMAL;
    return THREAD_PRIORITY_LOWEST;
}

/* ---------------------------------------------------------------- threads */
static __declspec(thread) uint64_t t_thread;          /* guest ThreadType* of the running thread */
static __declspec(thread) uint64_t t_tls[64];
static uint64_t g_tls_dtor[64];                       /* guest destructor of each slot, 0 = none */
static volatile LONG g_tls_used[64];
static void tls_run_destructors(Ctx *c);
static __declspec(thread) uint64_t t_fiber;           /* guest FiberType* currently running, 0 if none */
static __declspec(thread) void *t_thread_fiber;       /* host fiber of the thread itself */
/* A fiber can be resumed by another host thread than the one it left. The compiler computes the address of a thread
 * variable once per function and keeps it across calls, so after a SwitchToFiber or a guest call (which can switch)
 * the old thread's variables would be read and written: fiber_main then switched to the OLD thread's own fiber, whose
 * stack continued on the wrong thread until its thread function returned there - a TaskWorker "finished" and the game
 * waited for it forever (Linux build, frozen after 15 minutes of play, 2026-10-14). These accessors are not inlined
 * and the empty asm keeps the compiler from treating them as pure, so every use reads the running thread's own copy. */
static __attribute__((noinline)) uint64_t fiber_cur(void) { __asm__ volatile("" ::: "memory"); return t_fiber; }
static __attribute__((noinline)) void fiber_set_cur(uint64_t f) { __asm__ volatile("" ::: "memory"); t_fiber = f; }
static __attribute__((noinline)) void *thread_fiber(void) {
    __asm__ volatile("" ::: "memory");
    if (!t_thread_fiber) t_thread_fiber = ConvertThreadToFiber(NULL);
    return t_thread_fiber;
}
static uint8_t g_main_thread[0x400];
uint64_t os_main_thread_type(void) { return (uint64_t)(uintptr_t)g_main_thread; }   /* boot.c: stored at thread block +0x1f8, as [sdk 0x3349dc] does */
uint64_t g_main_stack_base, g_main_stack_size;        /* set by boot.c */
static volatile LONG g_thread_count;

static Ctx *new_ctx(uint64_t stack_top) {
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    memset(c, 0, sizeof *c);
    c->x[31] = stack_top & ~15ull;
    c->tpidr = (uint64_t)VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return c;
}

/* Every guest thread's context, so a report can say where each one is. */
static struct { Obj *o; Ctx *c; } g_threads[256];
static volatile LONG g_threads_n;

void dump_guest_threads(FILE *out) {
    fprintf(out, "\nguest threads (last function entered, link register):\n");
    for (int i = 0; i < g_threads_n && i < 256; i++) {
        Obj *o = g_threads[i].o;
        Ctx *c = g_threads[i].c;
        fprintf(out, "  %-20s %s  f_%llx  lr=%llx  %s%s (from %llx)\n", o->name ? (const char *)(uintptr_t)o->name : "?",
                o->finished ? "finished" : "running ", (unsigned long long)c->fn, (unsigned long long)c->x[30],
                c->in_import ? "in " : "", c->in_import ? c->in_import : "guest code", (unsigned long long)c->import_from);
    }
}

/* hitch.c (SSBU_HITCH): guest thread i -> context, name, host handle; 0 past the end. */
int guest_thread_info(int i, Ctx **c, const char **name, HANDLE *h) {
    if (i < 0 || i >= g_threads_n || i >= 256 || !g_threads[i].o || !g_threads[i].c) return 0;
    *c = g_threads[i].c; *name = g_threads[i].o->name ? (const char *)(uintptr_t)g_threads[i].o->name : NULL; *h = g_threads[i].o->finished ? NULL : g_threads[i].o->thread;
    return 1;
}
void hitch_cur_ctx(Ctx *c);
/* Name of the guest thread running on host thread `tid`, or NULL (nvn_gl.c: CPU time per thread, P3). */
const char *guest_thread_name_of(DWORD tid) {
    for (int i = 0; i < g_threads_n && i < 256; i++) {
        Obj *o = g_threads[i].o;
        if (o && o->thread && GetThreadId(o->thread) == tid) return o->name ? (const char *)(uintptr_t)o->name : "?";
    }
    return NULL;
}

static DWORD WINAPI thread_main(LPVOID arg) {
    Obj *o = arg;
    t_thread = o->key;
    { void display_perf_thread(void); display_perf_thread(); }      /* track V session 14: the game's threads on the performance cores of a hybrid CPU (display.c; SSBU_PCORES=0 off) */
    Ctx *c = new_ctx(o->stack + o->stack_size);
    wr64(c->tpidr + 0x1f8, o->key);                   /* Horizon thread-local region: the ThreadType pointer lives at +0x1f8 */
    LONG slot = InterlockedIncrement(&g_threads_n) - 1;
    if (slot < 256) { g_threads[slot].o = o; g_threads[slot].c = c; }
    c->x[0] = o->arg;
    trace_printf("--- thread %llx started (entry %llx, name %s)\n", (unsigned long long)o->key, (unsigned long long)o->fn,
                 o->name ? (const char *)(uintptr_t)o->name : "?");
    call_guest(c, o->fn);
    tls_run_destructors(c);
    trace_printf("--- thread %llx finished\n", (unsigned long long)o->key);
    EnterCriticalSection(&o->cs);
    o->finished = 1;
    WakeAllConditionVariable(&o->cv);
    LeaveCriticalSection(&o->cs);
    return 0;
}

static void os_CreateThread(Ctx *c) {                 /* (ThreadType*, fn, arg, stack, stackSize, priority[, core]) */
    Obj *o = obj(A0);
    o->fn = A1; o->arg = A2; o->stack = A3; o->stack_size = A4; o->priority = (int)c->x[5];
    o->finished = 0; o->thread = NULL; o->name = 0;
    o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; o->core_set = 1;
    RET(0);
}
static void os_CreateThreadOnCore(Ctx *c) {           /* (ThreadType*, fn, arg, stack, stackSize, priority, idealCore) */
    uint64_t t = A0;
    int core = (int)c->x[6];
    os_CreateThread(c);
    if (core >= 0 && core < 64) { Obj *o = obj(t); o->ideal_core = core; o->core_mask = 1ull << core; }
}
/* Evidence for the audit rows "nn::os objects" / "thread scheduling" (bounded round 2026-10-09): every thread the game
 * starts, with the nn::os arguments it was given, and every later change of priority or core mask. Printed at the end
 * of a run by os_thread_report (hostrt.c report_and_exit). */
static char g_oslog[24000];
static volatile LONG g_oslog_n, g_oslog_lock;
static void oslog(const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    while (InterlockedCompareExchange(&g_oslog_lock, 1, 0)) YieldProcessor();
    if (g_oslog_n + n < (LONG)sizeof g_oslog - 1) { memcpy(g_oslog + g_oslog_n, line, (size_t)n); g_oslog_n += n; }
    g_oslog_lock = 0;
}
/* Track R1 (r1_state.c, SSBU_R1 only): every emulated object's waitable state (event flag, semaphore count, queue
 * length), to compare a save point with a restore point. Reads without the objects' locks (diagnostic). */
void os_r1_objs(void (*cb)(uint64_t key, int signaled, int count, uint64_t len, uint64_t thread, uint64_t stack, uint64_t stack_size)) {
    for (int h = 0; h < BUCKETS; h++)
        for (Obj *o = g_table[h]; o; o = o->next)
            cb(o->key, o->signaled, o->count, o->len, (uint64_t)(uintptr_t)o->thread | (uint64_t)(uintptr_t)o->fiber, o->thread ? o->stack : 0, o->thread ? o->stack_size : 0);
}
/* R1: every fiber that has run: its guest context and stack */
void os_r1_fibers(void (*cb)(uint64_t key, Ctx *c, uint64_t stack, uint64_t stack_size, void *fiber)) {
    for (int h = 0; h < BUCKETS; h++)
        for (Obj *o = g_table[h]; o; o = o->next)
            if (o->fiber) cb(o->key, o->fctx, o->stack, o->stack_size, o->fiber);
}
void os_thread_report(FILE *out) {
    if (!g_oslog_n) return;
    fprintf(out, "nn::os threads of this run (priority 0 highest .. 31 lowest; cores 0..2 exist on the console):\n%.*s", (int)g_oslog_n, g_oslog);
}
static void os_StartThread(Ctx *c) {
    Obj *o = obj(A0);
    oslog("  start  %-28s entry %llx priority %2d ideal core %d mask %llx stack 0x%llx (called from %llx)\n", o->name ? (const char *)(uintptr_t)o->name : "?",
          (unsigned long long)o->fn, o->priority, o->ideal_core, (unsigned long long)o->core_mask, (unsigned long long)o->stack_size, (unsigned long long)c->x[30]);
    InterlockedIncrement(&g_thread_count);
    o->thread = CreateThread(NULL, (SIZE_T)256 << 20, thread_main, o, STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, NULL);
    if (o->thread) { SetThreadPriority(o->thread, host_priority(o->priority)); apply_sched(o->thread, o->core_mask); ResumeThread(o->thread); }
}
/* boot.c, on the main guest thread before the game starts: main.npdm gives it core 0 and priority 44 - 28 = 16 */
void os_apply_main_sched(void) {
    if (!console_cores()) return;
    apply_sched(GetCurrentThread(), 1ull << OS_DEFAULT_CORE);
    SetThreadPriority(GetCurrentThread(), host_priority(OS_DEFAULT_PRIORITY));
}
static void os_WaitThread(Ctx *c) { Obj *o = obj(A0); if (o->thread) WaitForSingleObject(o->thread, INFINITE); }
static void os_DestroyThread(Ctx *c) { Obj *o = obj(A0); if (o->thread) { CloseHandle(o->thread); o->thread = NULL; } }
static void os_SetThreadName(Ctx *c) {               /* the guest string may be a temporary: keep a copy */
    char *copy = _strdup((const char *)(uintptr_t)A1);
    obj(A0)->name = (uint64_t)(uintptr_t)copy;
}
static void os_GetCurrentThread(Ctx *c) { RET(t_thread ? t_thread : (uint64_t)g_main_thread); }
static Obj *thread_obj(uint64_t key);
static void os_GetThreadPriority(Ctx *c) { RET((int64_t)thread_obj(A0 == (uint64_t)(uintptr_t)g_main_thread ? 0 : A0)->priority); }
static void os_ChangeThreadPriority(Ctx *c) {
    Obj *o = obj(A0);
    int old = o->priority;
    oslog("  change priority of %s: %d -> %d (called from %llx)\n", A0 == (uint64_t)(uintptr_t)g_main_thread ? "the main thread" : o->name ? (const char *)(uintptr_t)o->name : "?", old, (int)A1, (unsigned long long)c->x[30]);
    o->priority = (int)A1;
    if (o->thread) SetThreadPriority(o->thread, host_priority(o->priority));
    else if (A0 == (uint64_t)(uintptr_t)g_main_thread || A0 == t_thread) SetThreadPriority(GetCurrentThread(), host_priority(o->priority));
    RET((int64_t)old);
}
static void os_GetThreadId(Ctx *c) { Obj *o = obj(A0); RET(o->thread ? GetThreadId(o->thread) : GetCurrentThreadId()); }
static void os_YieldThread(Ctx *c) { (void)c; SwitchToThread(); }
/* SSBU_MOVIE_LOG: the movie player's pre-roll loop [game 0x71036f27b4] sleeps 10 ms up to 125 times until its video
 * renderer holds more than 1 frame (+0xb0) and its audio renderer has filled more than 8 / 16 buffers (+0x110). */
static void movie_preroll_diag(Ctx *c) {
    static int on = -1, n;
    if (on < 0) on = getenv("SSBU_MOVIE_LOG") != NULL;
    if (!on || c->import_from != 0x71036f2814ULL || (n++ % 25)) return;
    uint64_t p = c->x[21], vr = rd64(p + 0x58) ? rd64(rd64(p + 0x58)) : 0, ar = rd64(p + 0x68) ? rd64(rd64(p + 0x68)) : 0;
    printf("movie: pre-roll sleep %d (loop count %u of %u): video frames queued %lld", n, (unsigned)c->x[24], (unsigned)c->x[25], vr ? (long long)rd64(vr + 0xb0) : -1LL);
    if (ar) printf("; audio: filled %lld, accumulate %u, pool %d buffers of %d bytes (flush at %d), free %lld, pending outputs %lld, +0x40 %d", (long long)rd64(ar + 0x110), rd8(ar + 0x50), (int)rd32(ar + 0x44), (int)rd32(ar + 0x48), (int)rd32(ar + 0x4c),
        (long long)((rd64(ar + 0x78) - rd64(ar + 0x70)) / 0x40), (long long)((rd64(ar + 0x60) - rd64(ar + 0x58)) / 0x18), (int)rd32(ar + 0x40));
    printf("%c", 10);
}
static void os_SleepThread(Ctx *c) { movie_preroll_diag(c); sleep_ns((int64_t)A0); }
static void os_GetCurrentStackInfo(Ctx *c) {
    Obj *o = t_thread ? obj(t_thread) : NULL;
    if (A0) wr64(A0, o ? o->stack : g_main_stack_base);
    if (A1) wr64(A1, o ? o->stack_size : g_main_stack_size);
}
/* Core numbers and affinity masks. The console runs the application on cores 0..2 (GetThreadAvailableCoreMask = 7);
 * a thread is created on one core (the NPDM default core 0, or the one given to CreateThread) and stays there until
 * SetThreadCoreMask widens its mask. The values are kept per thread and returned as set; the Windows scheduler is
 * not told to pin anything (a PC has more than three cores and the game's threads would pile up on host CPU 0). */
static Obj *thread_obj(uint64_t key) {
    Obj *o = obj(key ? key : (uint64_t)(uintptr_t)g_main_thread);
    if (!o->core_set) { o->core_set = 1; o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; if (!o->priority) o->priority = OS_DEFAULT_PRIORITY; }
    return o;
}
static void os_SetThreadCoreMask(Ctx *c) {            /* (ThreadType*, int idealCore, Bit64 affinityMask) */
    Obj *o = thread_obj(A0);
    int ideal = (int)A1;
    uint64_t mask = A2;
    oslog("  core mask of %s: ideal %d mask %llx (was ideal %d mask %llx; %s; called from %llx)\n", A0 == (uint64_t)(uintptr_t)g_main_thread || !A0 ? "the main thread" : o->name ? (const char *)(uintptr_t)o->name : "?",
          ideal, (unsigned long long)mask, o->ideal_core, (unsigned long long)o->core_mask, o->thread ? "running" : "not started", (unsigned long long)c->x[30]);
    int is_self = t_thread ? A0 == t_thread : (!A0 || A0 == (uint64_t)(uintptr_t)g_main_thread);      /* the calling thread names itself */
    HANDLE h = o->thread ? o->thread : is_self ? GetCurrentThread() : NULL;                             /* NULL: not started yet (applied at StartThread) */
    if (ideal == -2) { o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; apply_sched(h, o->core_mask); return; }   /* IdealCoreUseDefaultValue: mask is ignored */
    if (ideal == -3) { o->core_mask = mask; apply_sched(h, o->core_mask); return; }                           /* IdealCoreNoUpdate */
    o->core_mask = mask;
    if (ideal == -1) { if (!(mask >> (o->ideal_core & 63) & 1)) o->ideal_core = mask ? (int)__builtin_ctzll(mask) : OS_DEFAULT_CORE; }   /* IdealCoreDontCare */
    else o->ideal_core = ideal;
    apply_sched(h, o->core_mask);
}
static void os_GetThreadCoreMask(Ctx *c) {            /* (int *outIdealCore, Bit64 *outMask, const ThreadType*) */
    Obj *o = thread_obj(A2);
    if (A0) wr32(A0, (uint32_t)o->ideal_core);
    if (A1) wr64(A1, o->core_mask);
}
static void os_GetCurrentCoreNumber(Ctx *c) {
    if (console_cores()) {                                /* the core the thread is on now, as the console answers */
        DWORD_PTR here = (DWORD_PTR)1 << GetCurrentProcessorNumber();
        for (int i = 0; i < 3; i++) if (g_cc_core[i] == here) { RET(i); return; }
    }
    RET((int64_t)thread_obj(t_thread)->ideal_core);
}
static void os_AvailableCoreMask(Ctx *c) { RET(7); }
static void nothing(Ctx *c) { (void)c; }
static void ret0(Ctx *c) { RET(0); }

/* thread-local storage slots */
static volatile LONG g_tls_next;
/* (TlsSlot *out, void (*destructor)(uintptr_t value)) -> Result. The destructor of a slot runs when a thread ends
 * with a non-zero value in it; FreeTlsSlot makes the slot available again. */
static void os_AllocateTlsSlot(Ctx *c) {
    (void)g_tls_next;
    for (int i = 0; i < 64; i++)
        if (InterlockedCompareExchange(&g_tls_used[i], 1, 0) == 0) { g_tls_dtor[i] = A1; wr32(A0, (uint32_t)i); RET(0); return; }
    guest_trap("nn::os::AllocateTlsSlot: all 64 slots are in use (the console has 32)", c->x[30] - 4);
}
static void os_FreeTlsSlot(Ctx *c) { uint32_t i = (uint32_t)A0 & 63; g_tls_dtor[i] = 0; InterlockedExchange(&g_tls_used[i], 0); }
static void tls_run_destructors(Ctx *c) {
    for (int i = 0; i < 64; i++) {
        uint64_t v = t_tls[i], fn = g_tls_dtor[i];
        t_tls[i] = 0;
        if (v && fn && g_tls_used[i]) { c->x[0] = v; call_guest(c, fn); }
    }
}
static void os_GetTlsValue(Ctx *c) { RET(t_tls[A0 & 63]); }
static void os_SetTlsValue(Ctx *c) { t_tls[A0 & 63] = A1; }

/* ---------------------------------------------------------------- mutexes */
static void mtx_init(Ctx *c) { (void)obj(A0); }
/* Round 17, MEASUREMENT (SSBU_MUTEXSTAT=1; nothing runs without it): calls of the libc++ / nn::os mutex lock import, the
 * time in the table look-up and in the lock itself, and how many calls found the mutex taken, for the thread that
 * queues the presents (the game's main thread) and for all other threads; a line every 200,000 calls of the main thread. */
unsigned long nvn_front_present_tid(void);                 /* nvn_gl.c */
static int g_mstat = -1;
static volatile LONG64 g_ms_calls[2], g_ms_look[2], g_ms_enter[2], g_ms_cont[2], g_ms_cont_tk[2];
static void mtx_lock_stat(Ctx *c) {
    LARGE_INTEGER t0, t1, t2;
    int k = GetCurrentThreadId() == nvn_front_present_tid() ? 0 : 1, cont;
    QueryPerformanceCounter(&t0);
    Obj *o = obj(A0);
    QueryPerformanceCounter(&t1);
    cont = !TryEnterCriticalSection(&o->cs);
    if (cont) EnterCriticalSection(&o->cs);
    QueryPerformanceCounter(&t2);
    InterlockedAdd64(&g_ms_look[k], t1.QuadPart - t0.QuadPart); InterlockedAdd64(&g_ms_enter[k], t2.QuadPart - t1.QuadPart);
    if (cont) { InterlockedIncrement64(&g_ms_cont[k]); InterlockedAdd64(&g_ms_cont_tk[k], t2.QuadPart - t1.QuadPart); }
    LONG64 n = InterlockedIncrement64(&g_ms_calls[k]);
    if (k == 0 && n % 200000 == 0) {
        static LONG64 last_t, last[2][5];
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double ns = 1e9 / (double)fq.QuadPart, secs = last_t ? (double)(t2.QuadPart - last_t) / (double)fq.QuadPart : 0;
        for (int i = 0; i < 2; i++) {
            LONG64 v[5] = {g_ms_calls[i], g_ms_look[i], g_ms_enter[i], g_ms_cont[i], g_ms_cont_tk[i]}, d[5];
            for (int q = 0; q < 5; q++) { d[q] = v[q] - last[i][q]; last[i][q] = v[q]; }
            if (secs > 0 && d[0] > 0)
                printf("os_mutexstat: %s: %lld lock calls in %.1f s (%.0f per 1/60 s), table look-up %.0f ns per call, the lock itself %.0f ns per call (%.3f ms per 1/60 s both together), found taken %lld times (%.2f %%), waited %.3f ms per 1/60 s for those, %.1f us each | look-up %s\n",
                       i ? "other threads" : "main thread", (long long)d[0], secs, (double)d[0] / secs / 60.0, (double)d[1] * ns / (double)d[0], (double)d[2] * ns / (double)d[0], (double)(d[1] + d[2]) * ns / 1e6 / secs / 60.0,
                       (long long)d[3], 100.0 * (double)d[3] / (double)d[0], (double)d[4] * ns / 1e6 / secs / 60.0, d[3] ? (double)d[4] * ns / 1e3 / (double)d[3] : 0.0, g_obj_lf ? "without the table lock" : "under the table lock");
        }
        last_t = t2.QuadPart;
        fflush(stdout);
    }
}
static void mtx_lock(Ctx *c) {
    if (g_mstat < 0) g_mstat = getenv("SSBU_MUTEXSTAT") != NULL;
    if (g_mstat) { mtx_lock_stat(c); return; }
    EnterCriticalSection(&obj(A0)->cs);
}
static void mtx_unlock(Ctx *c) { LeaveCriticalSection(&obj(A0)->cs); }
static void mtx_trylock(Ctx *c) { RET(TryEnterCriticalSection(&obj(A0)->cs) ? 1 : 0); }
/* hitch.c (SSBU_HITCH): host thread id that owns the guest mutex `key` now, 0 = nobody. Diagnostics only. */
DWORD os_mutex_owner(uint64_t key) { return key ? (DWORD)(uintptr_t)obj(key)->cs.OwningThread : 0; }
static void pmtx_lock(Ctx *c) { EnterCriticalSection(&obj(A0)->cs); RET(0); }
static void pmtx_unlock(Ctx *c) { LeaveCriticalSection(&obj(A0)->cs); RET(0); }
static void pmtx_trylock(Ctx *c) { RET(TryEnterCriticalSection(&obj(A0)->cs) ? 0 : 16); }   /* EBUSY */

/* ---------------------------------------------------------------- events (Event, LightEvent, SystemEvent) */
/* nn::os::EventClearMode: 0 = ManualClear, 1 = AutoClear */
static void ev_init(Ctx *c) { obj_reset(A0); Obj *o = obj(A0); o->signaled = (int)(A1 & 1); o->autoclear = A2 == 1; }
static void ev_signal(Ctx *c) {
    L1O("event SIGNAL", A0);
    Obj *o = obj(A0);
    EnterCriticalSection(&o->cs);
    o->signaled = 1;
    WakeAllConditionVariable(&o->cv);
    LeaveCriticalSection(&o->cs);
}
/* Host-side access to guest events (used for the display's vsync event). */
void os_init_event(uint64_t key, int autoclear) { obj_reset(key); Obj *o = obj(key); o->signaled = 0; o->autoclear = autoclear; }
void os_signal_event(uint64_t key) {
    Obj *o = obj(key);
    EnterCriticalSection(&o->cs);
    o->signaled = 1;
    WakeAllConditionVariable(&o->cv);
    LeaveCriticalSection(&o->cs);
}
static void ev_clear(Ctx *c) { Obj *o = obj(A0); EnterCriticalSection(&o->cs); o->signaled = 0; LeaveCriticalSection(&o->cs); }
static int ev_wait_ms(Obj *o, DWORD ms) {
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (!o->signaled) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->signaled; break; }
    }
    if (ok && o->autoclear) o->signaled = 0;
    LeaveCriticalSection(&o->cs);
    return ok;
}
static void ev_wait(Ctx *c) { L1O("event wait >", A0); ev_wait_ms(obj(A0), INFINITE); L1O("event wait <", A0); }
static void ev_trywait(Ctx *c) { RET(ev_wait_ms(obj(A0), 0)); }
static void ev_timedwait(Ctx *c) { uint64_t k_ = A0; L1O("event timedwait >", k_); RET(ev_wait_ms(obj(A0), ns_to_ms((int64_t)A1))); L1O("event timedwait <", k_); }

/* ---------------------------------------------------------------- condition variables */
static void cv_wait(Ctx *c) { uint64_t k_ = A0; L1O("cv wait >", k_); SleepConditionVariableCS(&obj(A0)->cv, &obj(A1)->cs, INFINITE); L1O("cv wait <", k_); }
static void cv_timedwait(Ctx *c) {                    /* returns ConditionVariableStatus: 0 timeout, 1 signalled */
    RET(SleepConditionVariableCS(&obj(A0)->cv, &obj(A1)->cs, ns_to_ms((int64_t)A2)) ? 1 : 0);
}
static void cv_signal(Ctx *c) { L1O("cv SIGNAL", A0); WakeConditionVariable(&obj(A0)->cv); }
static void cv_broadcast(Ctx *c) { L1O("cv BROADCAST", A0); WakeAllConditionVariable(&obj(A0)->cv); }
/* std::condition_variable: the argument is a unique_lock<mutex> whose first word is the mutex pointer */
static void std_cv_wait(Ctx *c) { uint64_t k_ = A0; L1O("std cv wait >", k_); SleepConditionVariableCS(&obj(A0)->cv, &obj(rd64(A1))->cs, INFINITE); L1O("std cv wait <", k_); }
static void std_cv_timedwait(Ctx *c) {                /* deadline: system_clock nanoseconds since 1970 */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t now = (int64_t)(((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) * 100);
    SleepConditionVariableCS(&obj(A0)->cv, &obj(rd64(A1))->cs, ns_to_ms((int64_t)A2 - now));
}

/* ---------------------------------------------------------------- semaphores */
static void sem_init(Ctx *c) { obj_reset(A0); Obj *o = obj(A0); o->count = (int)A1; o->max = (int)A2; }
static void sem_acquire(Ctx *c) {
    L1O("semaphore acquire", A0);
    Obj *o = obj(A0);
    EnterCriticalSection(&o->cs);
    while (o->count <= 0) SleepConditionVariableCS(&o->cv, &o->cs, INFINITE);
    o->count--;
    LeaveCriticalSection(&o->cs);
}
static void sem_release(Ctx *c) {
    L1O("semaphore RELEASE", A0);
    Obj *o = obj(A0);
    EnterCriticalSection(&o->cs);
    if (o->count < o->max) o->count++;               /* nn::os semaphores saturate at their maximum */
    WakeConditionVariable(&o->cv);
    LeaveCriticalSection(&o->cs);
}

/* ---------------------------------------------------------------- message queues (of 64-bit values) */
static void mq_init(Ctx *c) {                         /* (queue, buffer, count): host-side storage is used */
    obj_reset(A0);
    Obj *o = obj(A0);
    free(o->buf);
    o->cap = A2 ? A2 : 1;
    o->buf = calloc((size_t)o->cap, 8);
}
static int mq_put(Obj *o, uint64_t v, int front, DWORD ms) {
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (o->len == o->cap) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->len < o->cap; break; }
    }
    if (ok) {
        if (front) { o->head = (o->head + o->cap - 1) % o->cap; o->buf[o->head] = v; }
        else o->buf[(o->head + o->len) % o->cap] = v;
        o->len++;
        WakeAllConditionVariable(&o->cv);
    }
    LeaveCriticalSection(&o->cs);
    return ok;
}
static int mq_get(Obj *o, uint64_t out, DWORD ms) {
    int ok = 1;
    EnterCriticalSection(&o->cs);
    while (o->len == 0) {
        if (!SleepConditionVariableCS(&o->cv, &o->cs, ms)) { ok = o->len > 0; break; }
    }
    if (ok) {
        wr64(out, o->buf[o->head]);
        o->head = (o->head + 1) % o->cap;
        o->len--;
        WakeAllConditionVariable(&o->cv);
    }
    LeaveCriticalSection(&o->cs);
    return ok;
}
static void mq_send(Ctx *c) { L1O("queue SEND", A0); mq_put(obj(A0), A1, 0, INFINITE); }
static void mq_trysend(Ctx *c) { RET(mq_put(obj(A0), A1, 0, 0)); }
static void mq_jam(Ctx *c) { mq_put(obj(A0), A1, 1, INFINITE); }
static void mq_tryjam(Ctx *c) { RET(mq_put(obj(A0), A1, 1, 0)); }
static void mq_receive(Ctx *c) { uint64_t k_ = A1; L1O("queue receive >", k_); mq_get(obj(A1), A0, INFINITE); L1O("queue receive <", k_); }
static void mq_tryreceive(Ctx *c) { RET(mq_get(obj(A1), A0, 0)); }
static void mq_timedreceive(Ctx *c) { RET(mq_get(obj(A1), A0, ns_to_ms((int64_t)A2))); }

/* ---------------------------------------------------------------- multi-wait (polling implementation) */
static void mw_init(Ctx *c) { obj_reset(A0); }
static void mw_holder_init(Ctx *c) { Obj *h = obj(A0); h->target = A1; h->user = 0; }
static void mw_link(Ctx *c) { Obj *m = obj(A0); if (m->holder_count < 64) m->holders[m->holder_count++] = obj(A1); }
static void mw_unlink(Ctx *c) {
    Obj *h = obj(A0);
    AcquireSRWLockExclusive(&g_table_lock);
    for (int b = 0; b < BUCKETS; b++)
        for (Obj *m = g_table[b]; m; m = m->next)
            for (int i = 0; i < m->holder_count; i++)
                if (m->holders[i] == h) { m->holders[i] = m->holders[--m->holder_count]; i--; }
    ReleaseSRWLockExclusive(&g_table_lock);
}
static void mw_set_user(Ctx *c) { obj(A0)->user = A1; }
static void mw_get_user(Ctx *c) { RET(obj(A0)->user); }
static uint64_t mw_poll(Obj *m) {
    for (int i = 0; i < m->holder_count; i++) {
        Obj *t = obj(m->holders[i]->target);
        if (t->signaled || t->finished) return m->holders[i]->key;
    }
    return 0;
}
static void mw_waitany(Ctx *c) { Obj *m = obj(A0); uint64_t r; while (!(r = mw_poll(m))) Sleep(1); RET(r); }
static void mw_trywaitany(Ctx *c) { RET(mw_poll(obj(A0))); }

/* ---------------------------------------------------------------- fibers */
static void CALLBACK fiber_main(void *arg) {
    Obj *o = arg;
    Ctx *c = new_ctx(o->stack + o->stack_size);
    o->fctx = c;
    VirtualFree((void *)(uintptr_t)c->tpidr, 0, MEM_RELEASE);   /* a fiber has no thread block of its own: it uses the one of the thread running it */
    for (;;) {
        c->tpidr = o->tp;
        c->x[0] = o->arg;
        hitch_cur_ctx(c);
        call_guest(c, o->fn);                         /* returns the fiber to switch to next, or null */
        uint64_t next = c->x[0];
        if (next) obj(next)->tp = c->tpidr;
        fiber_set_cur(next);
        if (g_r1_keep_fibers) { o->run_tid = 0; if (g_r1_fls_idx != 0xFFFFFFFF) o->fls = FlsGetValue(g_r1_fls_idx); if (next) obj(next)->run_tid = GetCurrentThreadId(); }
        SwitchToFiber(next ? obj(next)->fiber : thread_fiber());
    }
}
static void fib_init(Ctx *c) {                        /* (FiberType*, fn, arg, stack, stackSize, flags) */
    Obj *o = obj(A0);
    o->fn = A1; o->arg = A2; o->stack = A3; o->stack_size = A4;
    o->fiber = CreateFiberEx(1 << 20, (SIZE_T)256 << 20, 0, fiber_main, o);
    RET(0);
}
static void fib_switch(Ctx *c) {
    void *own = thread_fiber();
    uint64_t next = A0, self = fiber_cur();
    DWORD tid = GetCurrentThreadId();
    if (next) obj(next)->tp = c->tpidr;               /* the thread block (errno, ...) belongs to the host thread, not to the fiber */
    fiber_set_cur(next);
    if (g_r1_keep_fibers) { if (self) { obj(self)->run_tid = 0; if (g_r1_fls_idx != 0xFFFFFFFF) obj(self)->fls = FlsGetValue(g_r1_fls_idx); } if (next) obj(next)->run_tid = GetCurrentThreadId(); }
    SwitchToFiber(next ? obj(next)->fiber : own);
    if (!self && GetCurrentThreadId() != tid) {        /* a thread's own context must come back on its own thread (see fiber_cur) */
        static volatile LONG said;
        if (!InterlockedExchange(&said, 1)) { printf("nn::os: BUG: a thread's own context resumed on another host thread (%lu, left on %lu)\n", (unsigned long)GetCurrentThreadId(), (unsigned long)tid); fflush(stdout); }
    }
    if (self) c->tpidr = obj(self)->tp;               /* resumed, possibly by another thread */
    hitch_cur_ctx(c);
}
/* R1: a fiber object's host-side fields (fiber, context, entry, argument, stack, size): read at the save, written back at the restore */
/* R1: look-up without the table lock (R1 holds threads that may be inside obj(); records are never removed) */
static Obj *obj_peek(uint64_t key) {
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (BUCKETS - 1);
    Obj *f = *(Obj *volatile *)&g_table[h];
    while (f && f->key != key) f = f->next;
    return f;
}
DWORD os_r1_fiber_tid(uint64_t key) { Obj *o = obj_peek(key); return o ? o->run_tid : 0; }
void *os_r1_fiber_fls(uint64_t key) { Obj *o = obj_peek(key); return o ? o->fls : NULL; }
void os_r1_fiber_get(uint64_t key, uint64_t v[6]) { Obj *o = obj_peek(key); if (!o) return; v[0] = (uint64_t)(uintptr_t)o->fiber; v[1] = (uint64_t)(uintptr_t)o->fctx; v[2] = o->fn; v[3] = o->arg; v[4] = o->stack; v[5] = o->stack_size; }
void os_r1_fiber_put(uint64_t key, const uint64_t v[6]) { Obj *o = obj_peek(key); if (!o) return; o->fiber = (void *)(uintptr_t)v[0]; o->fctx = (Ctx *)(uintptr_t)v[1]; o->fn = v[2]; o->arg = v[3]; o->stack = v[4]; o->stack_size = v[5]; }
/* R1 restore: give a guest fiber back the host fiber it had at the save (kept alive by g_r1_keep_fibers), or a fresh one */
void os_r1_fiber_set(uint64_t key, void *fiber, Ctx *fctx, int fresh) {
    Obj *o = obj_peek(key); if (!o) return;
    if (fresh) { o->fiber = CreateFiberEx(1 << 20, (SIZE_T)256 << 20, 0, fiber_main, o); o->fctx = NULL; }
    else { o->fiber = fiber; o->fctx = fctx; }
}
static void fib_current(Ctx *c) { RET(fiber_cur()); }
int g_r1_keep_fibers;                                  /* R1 (r1_state.c, SSBU_R1 only): a finalized fiber's host fiber is kept, so a
                                                        * restore that brings the guest fiber back finds it (resumed, it starts over) */
static void fib_finalize(Ctx *c) { Obj *o = obj(A0); if (o->fiber && !g_r1_keep_fibers) { DeleteFiber(o->fiber); o->fiber = NULL; } }

/* ---------------------------------------------------------------- misc */
static void os_ConvertToTick(Ctx *c) { RET((uint64_t)((double)(int64_t)A0 * (double)TICK_HZ / 1e9)); }
static void os_ConvertToTimeSpan(Ctx *c) { RET((uint64_t)((double)(int64_t)A0 * 1e9 / (double)TICK_HZ)); }
/* The console fills the buffer from a generator seeded with kernel entropy: unpredictable bytes. Same here, from the
 * system generator (RtlGenRandom). */
BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);
/* Test switch SSBU_FIXED_SEED=<n> (track V, session 4): the bytes come from a splitmix64 stream seeded with n instead, so
 * that a scripted match meets the same random stage / CPU fighter in every run. Each call is logged (first 16). */
int host_fixed_random(void *buffer, uint64_t size, const char *who, uint64_t lr) {
    static int mode = -1;
    static volatile LONG lock, calls;
    static uint64_t state;
    if (mode < 0) { const char *e = getenv("SSBU_FIXED_SEED"); if (e && *e) { state = strtoull(e, NULL, 0) * 0x9E3779B97F4A7C15ull + 1; mode = 1; } else mode = 0; }
    if (!mode) return 0;
    while (InterlockedExchange(&lock, 1)) YieldProcessor();
    for (uint64_t i = 0; i < size; i += 8) {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
        memcpy((uint8_t *)buffer + i, &z, size - i < 8 ? (size_t)(size - i) : 8);
    }
    LONG n = ++calls;
    InterlockedExchange(&lock, 0);
    if (n <= 16) printf("fixed seed: %s call %ld, %llu bytes, caller %llx\n", who, n, (unsigned long long)size, (unsigned long long)lr);
    return 1;
}
static void os_GenerateRandomBytes(Ctx *c) {
    uint8_t *p = (uint8_t *)(uintptr_t)A0;
    if (host_fixed_random(p, A1, "nn::os::GenerateRandomBytes", c->x[30] - 4)) return;
    for (uint64_t done = 0; done < A1;) {
        ULONG n = A1 - done > 0x10000000 ? 0x10000000 : (ULONG)(A1 - done);
        if (!SystemFunction036(p + done, n)) guest_trap("nn::os::GenerateRandomBytes: the system random generator failed", c->x[30] - 4);
        done += n;
    }
}

#define OS(n) "_ZN2nn2os" n
static const ImportDef thread_imports[] = {
    {OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"), os_CreateThread},
    {OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii"), os_CreateThreadOnCore},
    {OS("25GetCurrentProcessorNumberEv"), os_GetCurrentCoreNumber}, {OS("20GetCurrentCoreNumberEv"), os_GetCurrentCoreNumber},
    {OS("11StartThreadEPNS0_10ThreadTypeE"), os_StartThread}, {OS("10WaitThreadEPNS0_10ThreadTypeE"), os_WaitThread},
    {OS("13DestroyThreadEPNS0_10ThreadTypeE"), os_DestroyThread}, {OS("13SetThreadNameEPNS0_10ThreadTypeEPKc"), os_SetThreadName},
    {OS("17SetThreadCoreMaskEPNS0_10ThreadTypeEim"), os_SetThreadCoreMask}, {OS("17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE"), os_GetThreadCoreMask},
    {OS("26GetThreadAvailableCoreMaskEv"), os_AvailableCoreMask},
    {OS("16GetCurrentThreadEv"), os_GetCurrentThread}, {OS("17GetThreadPriorityEPKNS0_10ThreadTypeE"), os_GetThreadPriority},
    {OS("24GetThreadCurrentPriorityEPKNS0_10ThreadTypeE"), os_GetThreadPriority},
    {OS("20ChangeThreadPriorityEPNS0_10ThreadTypeEi"), os_ChangeThreadPriority}, {OS("11GetThreadIdEPKNS0_10ThreadTypeE"), os_GetThreadId},
    {OS("11YieldThreadEv"), os_YieldThread}, {OS("11SleepThreadENS_8TimeSpanE"), os_SleepThread},
    {OS("19GetCurrentStackInfoEPmS1_"), os_GetCurrentStackInfo},
    {OS("15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE"), os_AllocateTlsSlot}, {OS("18SdkAllocateTlsSlotEPNS0_7TlsSlotEPFvmE"), os_AllocateTlsSlot},
    {OS("11FreeTlsSlotENS0_7TlsSlotE"), os_FreeTlsSlot}, {OS("11GetTlsValueENS0_7TlsSlotE"), os_GetTlsValue}, {OS("11SetTlsValueENS0_7TlsSlotEm"), os_SetTlsValue},
    {OS("15InitializeMutexEPNS0_9MutexTypeEbi"), mtx_init}, {OS("13FinalizeMutexEPNS0_9MutexTypeE"), nothing},
    {OS("9LockMutexEPNS0_9MutexTypeE"), mtx_lock}, {OS("11UnlockMutexEPNS0_9MutexTypeE"), mtx_unlock}, {OS("12TryLockMutexEPNS0_9MutexTypeE"), mtx_trylock},
    {"pthread_mutex_init", ret0}, {"pthread_mutex_destroy", ret0}, {"pthread_mutex_lock", pmtx_lock}, {"pthread_mutex_unlock", pmtx_unlock},
    {"pthread_mutex_trylock", pmtx_trylock},
    {"_ZNSt3__15mutex4lockEv", mtx_lock}, {"_ZNSt3__15mutex6unlockEv", mtx_unlock}, {"_ZNSt3__15mutex8try_lockEv", mtx_trylock},
    {"_ZNSt3__115recursive_mutex4lockEv", mtx_lock}, {"_ZNSt3__115recursive_mutex6unlockEv", mtx_unlock},
    {"_ZNSt3__115recursive_mutex8try_lockEv", mtx_trylock},
    {OS("15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE"), ev_init}, {OS("13FinalizeEventEPNS0_9EventTypeE"), nothing},
    {OS("11SignalEventEPNS0_9EventTypeE"), ev_signal}, {OS("10ClearEventEPNS0_9EventTypeE"), ev_clear}, {OS("9WaitEventEPNS0_9EventTypeE"), ev_wait},
    {OS("14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE"), ev_timedwait},
    {OS("20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE"), ev_init}, {OS("18FinalizeLightEventEPNS0_14LightEventTypeE"), nothing},
    {OS("16SignalLightEventEPNS0_14LightEventTypeE"), ev_signal}, {OS("15ClearLightEventEPNS0_14LightEventTypeE"), ev_clear},
    {OS("14WaitLightEventEPNS0_14LightEventTypeE"), ev_wait},
    {OS("18DestroySystemEventEPNS0_15SystemEventTypeE"), nothing}, {OS("16ClearSystemEventEPNS0_15SystemEventTypeE"), ev_clear},
    {OS("18TryWaitSystemEventEPNS0_15SystemEventTypeE"), ev_trywait}, {OS("15WaitSystemEventEPNS0_15SystemEventTypeE"), ev_wait},
    {OS("20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE"), ev_timedwait},
    {OS("27InitializeConditionVariableEPNS0_21ConditionVariableTypeE"), mtx_init}, {OS("25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE"), nothing},
    {OS("21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE"), cv_wait},
    {OS("26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE"), cv_timedwait},
    {OS("23SignalConditionVariableEPNS0_21ConditionVariableTypeE"), cv_signal}, {OS("26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE"), cv_broadcast},
    {"_ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE", std_cv_wait},
    {"_ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_5ratioILl1ELl1000000000EEEEEEE", std_cv_timedwait},
    {"_ZNSt3__118condition_variable10notify_oneEv", cv_signal}, {"_ZNSt3__118condition_variable10notify_allEv", cv_broadcast},
    {"pthread_cond_broadcast", cv_broadcast}, {"pthread_cond_destroy", ret0},
    {OS("19InitializeSemaphoreEPNS0_13SemaphoreTypeEii"), sem_init}, {OS("17FinalizeSemaphoreEPNS0_13SemaphoreTypeE"), nothing},
    {OS("16AcquireSemaphoreEPNS0_13SemaphoreTypeE"), sem_acquire}, {OS("16ReleaseSemaphoreEPNS0_13SemaphoreTypeE"), sem_release},
    {OS("22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm"), mq_init}, {OS("20FinalizeMessageQueueEPNS0_16MessageQueueTypeE"), nothing},
    {OS("16SendMessageQueueEPNS0_16MessageQueueTypeEm"), mq_send}, {OS("19TrySendMessageQueueEPNS0_16MessageQueueTypeEm"), mq_trysend},
    {OS("15JamMessageQueueEPNS0_16MessageQueueTypeEm"), mq_jam}, {OS("18TryJamMessageQueueEPNS0_16MessageQueueTypeEm"), mq_tryjam},
    {OS("19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"), mq_receive}, {OS("22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"), mq_tryreceive},
    {OS("24TimedReceiveMessageQueueEPmPNS0_16MessageQueueTypeENS_8TimeSpanE"), mq_timedreceive},
    {OS("19InitializeMultiWaitEPNS0_13MultiWaitTypeE"), mw_init}, {OS("17FinalizeMultiWaitEPNS0_13MultiWaitTypeE"), nothing},
    {OS("25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_10ThreadTypeE"), mw_holder_init},
    {OS("25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE"), mw_holder_init},
    {OS("23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE"), nothing},
    {OS("19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE"), mw_link},
    {OS("21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE"), mw_unlink},
    {OS("26SetMultiWaitHolderUserDataEPNS0_19MultiWaitHolderTypeEm"), mw_set_user},
    {OS("26GetMultiWaitHolderUserDataEPKNS0_19MultiWaitHolderTypeE"), mw_get_user},
    {OS("7WaitAnyEPNS0_13MultiWaitTypeE"), mw_waitany}, {OS("10TryWaitAnyEPNS0_13MultiWaitTypeE"), mw_trywaitany},
    {OS("15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi"), fib_init}, {OS("13FinalizeFiberEPNS0_9FiberTypeE"), fib_finalize},
    {OS("13SwitchToFiberEPNS0_9FiberTypeE"), fib_switch}, {OS("15GetCurrentFiberEv"), fib_current},
    {OS("13ConvertToTickENS_8TimeSpanE"), os_ConvertToTick}, {OS("17ConvertToTimeSpanENS0_4TickE"), os_ConvertToTimeSpan},
    {OS("19GenerateRandomBytesEPvm"), os_GenerateRandomBytes},
};

void register_thread_imports(void) {
    register_imports(thread_imports, (int)(sizeof thread_imports / sizeof thread_imports[0]));
}
int guest_thread_count(void) { return (int)g_thread_count; }

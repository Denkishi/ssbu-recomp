/* Small SDK services: nn::oe notifications, nn::prepo, nn::aoc, nn::err,
 * the C-linkage nnos* wrappers, setjmp/longjmp, and "offline console" answers for the
 * network-facing services (friends, account extras, nifm, socket, ldn, ec accessor, album lists ...).
 *
 * Where a behaviour or layout is stated here it was read from one of:
 *   [sdk]   the real SDK module (binaries\13.0.4\sdk.elf), disassembled by symbol name;
 *   [game]  the game's own code at the quoted address;
 *   [doc]   public documentation (switchbrew / open-ead nnheaders), not checked against code.
 * Everything else only returns "success", "nothing" or "not available" and touches no
 * guest structure.
 *
 * Registration never replaces a handler another file already installed, with one
 * exception (nn::err::ErrorResultVariant, see below). */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <setjmp.h>
#include <windows.h>
#include <wincrypt.h>
#include "runtime.h"
#include "hostrt.h"

#pragma comment(lib, "advapi32.lib")

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define STR(n) ((const char *)(uintptr_t)c->x[n])

/* Some non-zero nn::Result. The SDK's real "not available" codes differ per service and are
 * not established; the game only tests these for failure. Same value as nnsdk.c. */
#define RESULT_UNAVAILABLE 0x3e87aull

void os_init_event(uint64_t key, int autoclear);          /* os_threads.c */
void os_signal_event(uint64_t key);

static void keep(Ctx *c) { (void)c; }                     /* void function, or returns `this` */
static void ret0(Ctx *c) { RET(0); }                      /* Result success / false / 0 / null */
static void ret1(Ctx *c) { RET(1); }
static void pctl_permitted(Ctx *c) { trace_printf("nn::pctl: permission check (showError=%d) -> permitted (no parental controls)\n", (int)(c->x[0] & 1)); RET(1); }
static void unavailable(Ctx *c) { RET(RESULT_UNAVAILABLE); }
/* nn::account network-service-account functions on a machine without a linked account: see nnfs.c ([sdk] 0x4b3a8,
 * account results 124-0200..0269 mean "no usable network service account"). */
static void acc_nsa_unavailable(Ctx *c) { RET(0x1907cull); }

static void fatal(Ctx *c, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("\n%s (guest caller 0x%llx)\n", msg, (unsigned long long)c->x[30] - 4);
    trace_printf("%s (guest caller 0x%llx)\n", msg, (unsigned long long)c->x[30] - 4);
    report_and_exit(3);
}
static const char *safe_str(uint64_t p) { return p > 0x10000 ? (const char *)(uintptr_t)p : "(null)"; }

/* ---------------------------------------------------------------- C-linkage nn::os wrappers
 * [sdk] every nnosXxx is a tail jump to the C++ function with the same arguments, so each one
 * forwards to whatever handler is registered for the C++ name (looked up on first use). */
static ImportFn forward_target(Ctx *c, const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) fatal(c, "nnmisc: no handler for %s, needed by a C wrapper", name);
    return f;
}
#define FWD(fn, target) static void fn(Ctx *c) { static ImportFn f; if (!f) f = forward_target(c, target); f(c); }
#define OS(n) "_ZN2nn2os" n
FWD(nnos_GetCurrentThread, OS("16GetCurrentThreadEv"))
FWD(nnos_CreateThread, OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"))
FWD(nnos_StartThread, OS("11StartThreadEPNS0_10ThreadTypeE"))
FWD(nnos_WaitThread, OS("10WaitThreadEPNS0_10ThreadTypeE"))
FWD(nnos_DestroyThread, OS("13DestroyThreadEPNS0_10ThreadTypeE"))
FWD(nnos_GetThreadPriority, OS("17GetThreadPriorityEPKNS0_10ThreadTypeE"))
FWD(nnos_GetThreadCoreMask, OS("17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE"))
FWD(nnos_SetThreadCoreMask, OS("17SetThreadCoreMaskEPNS0_10ThreadTypeEim"))
FWD(nnos_SetThreadName, OS("13SetThreadNameEPNS0_10ThreadTypeEPKc"))      /* the host keeps a copy either way */
FWD(nnos_SleepThread, OS("11SleepThreadENS_8TimeSpanE"))
FWD(nnos_InitializeMutex, OS("15InitializeMutexEPNS0_9MutexTypeEbi"))
FWD(nnos_FinalizeMutex, OS("13FinalizeMutexEPNS0_9MutexTypeE"))
FWD(nnos_LockMutex, OS("9LockMutexEPNS0_9MutexTypeE"))
FWD(nnos_UnlockMutex, OS("11UnlockMutexEPNS0_9MutexTypeE"))
FWD(nnos_InitializeEvent, OS("15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE"))
FWD(nnos_FinalizeEvent, OS("13FinalizeEventEPNS0_9EventTypeE"))
FWD(nnos_SignalEvent, OS("11SignalEventEPNS0_9EventTypeE"))
FWD(nnos_WaitEvent, OS("9WaitEventEPNS0_9EventTypeE"))
FWD(nnos_GetSystemTick, OS("13GetSystemTickEv"))
FWD(nnos_GetSystemTickFrequency, OS("22GetSystemTickFrequencyEv"))
FWD(nnos_ConvertTickToNanoSeconds, OS("17ConvertToTimeSpanENS0_4TickE"))

/* ---------------------------------------------------------------- setjmp / longjmp
 * The game has exactly one setjmp call (0x71038faf40) and one longjmp call (0x71038fae7c):
 * Lua's luaD_rawrunprotected and luaD_throw. [game] the call site is
 *
 *     bl   setjmp              ; x0 = &lj.b   (lj on the stack, jmp_buf 0x138 bytes)
 *     cbnz w0, after
 *     mov  x0, x19
 *     mov  x1, x21
 *     blr  x20                 ; the protected call
 *   after:                     ; reads lj.status from memory, not from setjmp's value
 *
 * Lifted code runs each guest function as a host function on the host stack, so the host
 * handler of setjmp cannot "return a second time" once it has returned. Instead the handler
 * performs the protected call itself (the three instructions after the cbnz) while its own
 * host frame is still alive, and then returns non-zero so the lifted code continues at
 * `after`. A guest longjmp unwinds the host stack back into that handler with the host's
 * longjmp, restores the guest callee-saved registers and stack pointer saved at setjmp time,
 * and the handler returns non-zero as well. Both ways reach `after`, exactly as on the console.
 *
 * Limitation: this is only valid for a setjmp whose call site has exactly this shape. The
 * handler checks the four instruction words and stops with a diagnostic for any other caller.
 * The host record lives in the handler's frame; the guest jmp_buf holds a magic and a pointer
 * to it, so nesting, guest threads and fibers need no extra bookkeeping. */
#define JMP_MAGIC 0x4a4d5052454330ull
typedef struct {
    jmp_buf host;
    uint64_t x[13];                                       /* x19..x30, sp */
    uint64_t d[8];                                        /* d8..d15 */
    const char *in_import;
    uint64_t import_from;
} JmpRec;

static void c_setjmp(Ctx *c) {
    Ctx *volatile cc = c;
    volatile uint64_t buf = A0;
    uint64_t site = c->x[30];                             /* the instruction after the bl */
    if (rd32(site) != 0x35000080u || rd32(site + 4) != 0xaa1303e0u || rd32(site + 8) != 0xaa1503e1u ||
        rd32(site + 12) != 0xd63f0280u)
        fatal(c, "setjmp: call site is not the `cbnz w0; mov x0,x19; mov x1,x21; blr x20` pattern this port can run");
    JmpRec r;
    for (int i = 0; i < 12; i++) r.x[i] = c->x[19 + i];
    r.x[12] = c->x[31];
    for (int i = 0; i < 8; i++) r.d[i] = c->v[8 + i].d[0];
    r.in_import = c->in_import;
    r.import_from = c->import_from;
    wr64(buf, JMP_MAGIC);
    wr64(buf + 8, (uint64_t)(uintptr_t)&r);
    if (setjmp(r.host) == 0) {
        c->x[0] = c->x[19];
        c->x[1] = c->x[21];
        c->x[30] = site + 16;
        c->in_import = NULL;                              /* guest code runs from here */
        call_indirect(c, c->x[20]);
    } else {                                              /* a guest longjmp landed here */
        Ctx *g = cc;
        for (int i = 0; i < 12; i++) g->x[19 + i] = r.x[i];
        g->x[31] = r.x[12];
        for (int i = 0; i < 8; i++) { g->v[8 + i].d[0] = r.d[i]; g->v[8 + i].d[1] = 0; }
    }
    cc->in_import = r.in_import;
    cc->import_from = r.import_from;
    wr64(buf, 0);                                         /* the record dies with this frame */
    cc->x[0] = 1;
}

static void c_longjmp(Ctx *c) {
    if (!A0 || rd64(A0) != JMP_MAGIC)
        fatal(c, "longjmp: jmp_buf %llx was not filled by a setjmp that is still active", (unsigned long long)A0);
    JmpRec *r = (JmpRec *)(uintptr_t)rd64(A0 + 8);
    longjmp(r->host, 1);
}

/* ---------------------------------------------------------------- swprintf
 * The guest wchar_t is 32 bits. The format is narrowed, formatted by the registered snprintf
 * (same variadic argument positions: x3.. / v0..), and the result widened again.
 * Limitation: %ls / %lc arguments (wide strings) are not supported; the game's formats are
 * numeric ([game] L"%0*d" at 0x710446f514). */
static void c_swprintf(Ctx *c) {
    static ImportFn f;
    if (!f) f = forward_target(c, "snprintf");
    uint64_t out = A0, n = A1, wfmt = A2;
    char fmt[256], tmp[1024];
    size_t k = 0;
    for (; k < sizeof fmt - 1; k++) {
        uint32_t ch = rd32(wfmt + k * 4);
        if (!ch) break;
        if (ch > 0x7e) fatal(c, "swprintf: non-ASCII format character U+%04x is not supported", ch);
        fmt[k] = (char)ch;
    }
    fmt[k] = 0;
    for (size_t i = 0; i + 1 < k; i++)
        if (fmt[i] == '%' && fmt[i + 1] != '%') {
            size_t j = i + 1;
            while (fmt[j] && strchr("-+ #0123456789.*hljztL", fmt[j])) j++;
            if ((fmt[j] == 's' || fmt[j] == 'c') && j > 0 && fmt[j - 1] == 'l')
                fatal(c, "swprintf: wide string arguments (\"%s\") are not supported", fmt);
        } else if (fmt[i] == '%') i++;
    c->x[0] = (uint64_t)(uintptr_t)tmp;
    c->x[1] = sizeof tmp;
    c->x[2] = (uint64_t)(uintptr_t)fmt;
    f(c);
    int len = (int)c->x[0];
    if (len < 0 || (uint64_t)len >= n || (size_t)len >= sizeof tmp) {     /* does not fit: C says negative */
        if (n) wr32(out, 0);
        RET((int64_t)-1);
        return;
    }
    for (int i = 0; i <= len; i++) wr32(out + (uint64_t)i * 4, (uint8_t)tmp[i]);
    RET((int64_t)len);
}

/* ---------------------------------------------------------------- abort helpers (never return on the console) */
static void diag_AbortImpl(Ctx *c) {                      /* (condition, function, file, line) */
    printf("\nnn::diag abort: %s  in %s  (%s:%d)\n", safe_str(A0), safe_str(A1), safe_str(A2), (int)A3);
    trace_printf("nn::diag abort: %s  in %s  (%s:%d)\n", safe_str(A0), safe_str(A1), safe_str(A2), (int)A3);
    guest_trap("guest called nn::diag::detail::AbortImpl", c->x[30] - 4);
}
static void diag_AbortFormatImpl(Ctx *c) {                /* (condition, function, file, line, format, ...) */
    printf("\nnn::diag abort: %s  in %s  (%s:%d)  format \"%s\"\n", safe_str(A0), safe_str(A1), safe_str(A2), (int)A3, safe_str(A4));
    trace_printf("nn::diag abort: %s  in %s  (%s:%d)  format \"%s\"\n", safe_str(A0), safe_str(A1), safe_str(A2), (int)A3, safe_str(A4));
    guest_trap("guest called nndetailDiagAbortFormatImpl", c->x[30] - 4);
}
static void diag_UnexpectedDefault(Ctx *c) {              /* (function, file, line) */
    printf("\nnn unexpected default: %s  (%s:%d)\n", safe_str(A0), safe_str(A1), (int)A2);
    trace_printf("nn unexpected default: %s  (%s:%d)\n", safe_str(A0), safe_str(A1), (int)A2);
    guest_trap("guest called nn::detail::UnexpectedDefaultImpl", c->x[30] - 4);
}
static void c_assert_fail(Ctx *c) {                       /* (expression, file, line, function) */
    printf("\nassertion failed: %s  (%s:%d, %s)\n", safe_str(A0), safe_str(A1), (int)A2, safe_str(A3));
    trace_printf("assertion failed: %s  (%s:%d, %s)\n", safe_str(A0), safe_str(A1), (int)A2, safe_str(A3));
    guest_trap("guest called __assert_fail", c->x[30] - 4);
}

/* ---------------------------------------------------------------- nn::oe */
static LARGE_INTEGER g_start;
/* Notification messages. On the console the system posts a message when the application's focus state changes
 * (HOME menu, an overlay), when the console is docked or undocked, and when it asks the application to exit; the
 * application reads them with TryPopNotificationMessage and may wait on the event GetNotificationMessageEvent
 * returns, which is signalled while messages are pending.
 * Here: the focus state follows the game window. When the window stops being the foreground window the state
 * becomes OutOfFocus (2) and MessageFocusStateChanged (15) is posted; back in the foreground: InFocus (1) and the
 * message again. [sdk] 0x32c3d0: SetFocusHandlingMode passes "notify focus changes" = true for the modes 1
 * (NoSuspend) and 2 (SuspendHomeSleepNotify) only, so the message is posted only in those modes.
 * The operation mode never changes on a PC (always the TV mode), so MessageOperationModeChanged (30) and
 * MessagePerformanceModeChanged (31) never occur; the enable flags are kept as the game sets them.
 * (close4: the state now follows the applet manager's table, see oe_track_focus: not in focus = 2 only while the window is
 * visible; minimised = 3 for a NoSuspend application and no change at all in the suspending modes.)
 * Differences from the console, stated: (1) in the suspending modes (0, 2, 3) the console also FREEZES the application
 * while it is in the background (mode 3: also while obscured); this port does not freeze the game's threads: a minimised
 * game keeps running (design of the freeze: plan\phases\R6_fake_audit.md, fourth round). (2) A window
 * that has never been in the foreground (the automated tests keep theirs off screen) is treated as in focus.
 * (3) Closing the window is the exit request: see oe_exit_request below.
 * SSBU_FOCUS_NOTIFY=0 turns the focus tracking off. */
#define OE_MSG_FOCUS_STATE_CHANGED 15u
static uint8_t g_oe_event[0x40];
static uint32_t g_oe_queue[16];
static int g_oe_count, g_oe_event_ready, g_oe_focus_state = 1, g_oe_focus_mode = 0, g_oe_was_foreground;
static int g_oe_opmode_notify, g_oe_perfmode_notify, g_oe_resume_notify, g_oe_focus_pending, g_oe_focus_recheck;
static HWND g_oe_window;
static CRITICAL_SECTION g_oe_lock;
static void oe_event_setup(void) {
    if (!g_oe_event_ready) { os_init_event((uint64_t)(uintptr_t)g_oe_event, 0); g_oe_event_ready = 1; }
}
static void oe_push(uint32_t message) {
    EnterCriticalSection(&g_oe_lock);
    oe_event_setup();
    if (g_oe_count < 16) g_oe_queue[g_oe_count++] = message;
    os_signal_event((uint64_t)(uintptr_t)g_oe_event);
    LeaveCriticalSection(&g_oe_lock);
}
/* Suspension (close4). The applet manager suspends the application's process while it is not runnable (suyu am/applet.cpp
 * UpdateSuspensionStateLocked, am/lifecycle_manager.cpp IsRunnable): in the background unless the handling mode is NoSuspend,
 * obscured only in AlwaysSuspend; never while an exit request is pending. A message "Resume" (16) is queued at that moment when
 * the application enabled it (RequestResumeNotification). Here a watcher thread applies the rule to the window every 20 ms and
 * sets hostrt.c's g_guest_frozen: every guest thread then stops at its next SDK call or return (hostrt.c host_import).
 * SSBU_FREEZE_TEST=<start s>,<seconds> minimises and restores the window by itself (test; the log line counts the polls of the
 * game's main loop during the freeze). */
extern volatile LONG g_guest_frozen;
static volatile LONG g_oe_poll_count;
static int g_oe_exit_requested;                           /* defined with the exit request code below */
#define OE_MSG_RESUME 16u
static BOOL CALLBACK oe_find_window(HWND w, LPARAM out) {
    DWORD pid = 0;
    if (GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { *(HWND *)out = w; return FALSE; }
    return TRUE;
}
static DWORD WINAPI oe_watch(LPVOID unused) {
    const char *test = getenv("SSBU_FREEZE_TEST");
    double t_start = 0, t_len = 0;
    int test_phase = 0;
    ULONGLONG t0 = GetTickCount64(), frozen_at = 0;
    LONG polls_at = 0;
    if (test) sscanf(test, "%lf,%lf", &t_start, &t_len);
    for (;;) {
        Sleep(20);
        double t = (double)(GetTickCount64() - t0) / 1000.0;
        if (test && test_phase == 0 && t >= t_start) {
            HWND w = NULL;
            EnumWindows(oe_find_window, (LPARAM)&w);
            if (w) { g_oe_window = w; g_oe_was_foreground = 1; ShowWindow(w, SW_SHOWMINNOACTIVE); test_phase = 1; printf("nn::oe: freeze test: window minimised (handling mode %d)\n", g_oe_focus_mode); fflush(stdout); }
        } else if (test_phase == 1 && t >= t_start + t_len) {
            ShowWindow(g_oe_window, SW_SHOWNOACTIVATE);
            test_phase = 2;
        }
        DWORD pid = 0;
        HWND fg = GetForegroundWindow();
        int ours = fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
        if (ours) { g_oe_was_foreground = 1; g_oe_window = fg; }
        if (!g_oe_was_foreground) continue;
        int minimised = g_oe_window && IsIconic(g_oe_window);
        int runnable = ours || g_oe_exit_requested || (minimised ? g_oe_focus_mode == 1 : g_oe_focus_mode != 3);
        if (!runnable && !g_guest_frozen) {
            if (g_oe_resume_notify) oe_push(OE_MSG_RESUME);
            frozen_at = GetTickCount64(); polls_at = g_oe_poll_count;
            InterlockedExchange(&g_guest_frozen, 1);
            trace_printf("     nn::oe: application suspended (%s, handling mode %d)\n", minimised ? "background" : "obscured", g_oe_focus_mode);
        } else if (runnable && g_guest_frozen) {
            LONG polls = g_oe_poll_count - polls_at;
            InterlockedExchange(&g_guest_frozen, 0);
            printf("nn::oe: application resumed after %llu ms of suspension; main-loop polls during it: %ld\n", (unsigned long long)(GetTickCount64() - frozen_at), (long)polls);
            fflush(stdout);
            trace_printf("     nn::oe: application resumed after %llu ms\n", (unsigned long long)(GetTickCount64() - frozen_at));
        }
        if (test_phase == 2 && !g_guest_frozen) { g_oe_was_foreground = 0; test_phase = 3; }      /* the test window was never the foreground window: back to "in focus" */
    }
    return 0;
}
static void oe_track_focus(void) {
    static int enabled = -1;
    static ULONGLONG last;
    static volatile LONG watcher;
    ULONGLONG now = GetTickCount64();
    if (enabled < 0) { const char *e = getenv("SSBU_FOCUS_NOTIFY"); enabled = !(e && e[0] == '0'); }
    if (enabled && InterlockedCompareExchange(&watcher, 1, 0) == 0) CloseHandle(CreateThread(NULL, 0, oe_watch, NULL, 0, NULL));
    if (!enabled || (now - last < 30 && !g_oe_focus_recheck)) return;
    last = now;
    g_oe_focus_recheck = 0;
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    int ours = fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
    if (ours) { g_oe_was_foreground = 1; g_oe_window = fg; }
    if (!g_oe_was_foreground) return;
    /* close4: the applet manager's rule (suyu am/lifecycle_manager.cpp UpdateRequestedFocusState, GetFocusStateWhile*), with
     * the window as the activity state: foreground = ForegroundVisible; visible but not the foreground window = ForegroundObscured
     * (something else has the input, as under an overlay or a library applet); minimised = Background (as behind the HOME menu).
     * The handling mode is what [sdk 0x32c3d0, table 0xa7ff70] sends for the oe mode: 0 = (notify 0, suspend 1), 1 = (notify 1,
     * suspend 0) NoSuspend, 2 = (notify 1, suspend 1), 3 = (notify 0, suspend 1) + SetOutOfFocusSuspendingEnabled(1) AlwaysSuspend. */
    int minimised = g_oe_window && IsIconic(g_oe_window);
    int state = ours ? 1                                                    /* InFocus */
              : g_oe_focus_mode == 3 ? 1                                    /* AlwaysSuspend: the application never learns it */
              : !minimised ? 2                                              /* obscured: NotInFocus in the other three modes */
              : g_oe_focus_mode == 1 ? 3 : 1;                               /* background: Background (3) for a NoSuspend application, else unchanged */
    if (state != g_oe_focus_state) {
        g_oe_focus_state = state;
        g_oe_focus_pending = 1;                                             /* m_has_focus_state_changed */
        trace_printf("     nn::oe: focus state -> %d (%s; handling mode %d)\n", state, state == 1 ? "in focus" : state == 2 ? "not in focus" : "background", g_oe_focus_mode);
    }
    if (g_oe_focus_pending && (g_oe_focus_mode == 1 || g_oe_focus_mode == 2)) { g_oe_focus_pending = 0; oe_push(OE_MSG_FOCUS_STATE_CHANGED); }   /* delivered only while notification is enabled */
}
/* Exit request. [sdk 0x32cc20 / 0x32cca0] Enter / LeaveExitRequestHandlingSection keep a nesting count under a mutex and call the
 * applet manager's ISelfController LockExit when the count goes 0 -> 1 and UnlockExit when it goes 1 -> 0. The applet manager is a
 * system module (not in the dump); its rule, from the reference implementation (suyu am/service/self_controller.cpp LockExit /
 * UnlockExit, application_accessor.cpp RequestExit):
 *   exit requested while the exit is locked   -> the application is NOT ended; it gets MessageExitRequest (4) and keeps running
 *   exit requested while it is not locked     -> the process is terminated at once
 *   LockExit after an exit request            -> terminated at once
 *   UnlockExit after an exit request          -> terminated
 * Here the exit request is the user closing the game window (display.c WM_CLOSE calls oe_exit_request). */
#define OE_MSG_EXIT_REQUEST 4u
static int g_oe_exit_nest, g_oe_exit_locked, g_oe_exit_requested;
static void oe_terminate(const char *why) {
    printf("\nnn::oe: %s: the application ends\n", why);
    trace_printf("     nn::oe: %s: the application ends\n", why);
    report_and_exit(0);
}
int oe_exit_request(void) {                              /* display.c, window thread. Returns 1 when the game keeps running until it leaves its section */
    int locked;
    EnterCriticalSection(&g_oe_lock);
    locked = g_oe_exit_locked;
    if (locked) g_oe_exit_requested = 1;
    LeaveCriticalSection(&g_oe_lock);
    if (!locked) return 0;
    trace_printf("     nn::oe: exit requested inside an exit-request handling section: MessageExitRequest posted\n");
    oe_push(OE_MSG_EXIT_REQUEST);
    return 1;
}
static void oe_EnterExitRequestHandlingSection(Ctx *c) {
    int terminate = 0;
    (void)c;
    EnterCriticalSection(&g_oe_lock);
    if (g_oe_exit_nest++ == 0) { if (g_oe_exit_requested) terminate = 1; else g_oe_exit_locked = 1; }
    LeaveCriticalSection(&g_oe_lock);
    if (terminate) oe_terminate("exit-request handling section entered after an exit request");
}
static void oe_LeaveExitRequestHandlingSection(Ctx *c) {
    int terminate = 0;
    (void)c;
    EnterCriticalSection(&g_oe_lock);
    if (--g_oe_exit_nest == 0) { g_oe_exit_locked = 0; terminate = g_oe_exit_requested; }
    LeaveCriticalSection(&g_oe_lock);
    if (terminate) oe_terminate("exit-request handling section left with an exit request pending");
}
static void oe_TryPopNotificationMessage(Ctx *c) {       /* (uint32_t *out) -> bool */
    uint64_t out = A0;
    int got = 0;
    InterlockedIncrement(&g_oe_poll_count);
    oe_track_focus();
    EnterCriticalSection(&g_oe_lock);
    if (g_oe_count > 0) {
        if (out) wr32(out, g_oe_queue[0]);
        memmove(g_oe_queue, g_oe_queue + 1, (size_t)(--g_oe_count) * sizeof g_oe_queue[0]);
        got = 1;
        if (g_oe_count == 0 && g_oe_event_ready) os_init_event((uint64_t)(uintptr_t)g_oe_event, 0);   /* nothing pending: event cleared */
    }
    LeaveCriticalSection(&g_oe_lock);
    RET(got);
}
static void oe_GetNotificationMessageEvent(Ctx *c) {     /* -> nn::os::SystemEvent *, signalled while a message is pending */
    EnterCriticalSection(&g_oe_lock);
    oe_event_setup();
    LeaveCriticalSection(&g_oe_lock);
    RET(g_oe_event);
}
static void oe_GetCurrentFocusState(Ctx *c) { oe_track_focus(); RET(g_oe_focus_state); }
static void oe_SetFocusHandlingMode(Ctx *c) { g_oe_focus_mode = (int)(uint32_t)A0; g_oe_focus_recheck = 1; oe_track_focus(); }               /* 0 SuspendHomeSleep, 1 NoSuspend, 2 SuspendHomeSleepNotify, 3 AlwaysSuspend */
static void oe_SetOperationModeChangedNotificationEnabled(Ctx *c) { g_oe_opmode_notify = (int)(A0 & 1); }
/* While the game is inside such a section the console does not dim the screen or go to sleep by itself; the same is
 * asked of Windows (display and system kept awake) for as long as at least one section is open. */
static volatile LONG g_oe_no_sleep;
static void oe_BeginAutoSleepDisabledSection(Ctx *c) { (void)c; if (InterlockedIncrement(&g_oe_no_sleep) == 1) SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED); }
static void oe_EndAutoSleepDisabledSection(Ctx *c) { (void)c; if (InterlockedDecrement(&g_oe_no_sleep) == 0) SetThreadExecutionState(ES_CONTINUOUS); }
static void oe_GetProgramTotalActiveTime(Ctx *c) {                    /* -> TimeSpan (nanoseconds) */
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    RET((int64_t)((double)(now.QuadPart - g_start.QuadPart) * 1e9 / (double)freq.QuadPart));
}
static void oe_RequestToRelaunchApplication(Ctx *c) {
    printf("\nthe game asked to be relaunched (nn::oe::RequestToRelaunchApplication): stopping.\n");
    trace_printf("nn::oe::RequestToRelaunchApplication\n");
    (void)c;
    report_and_exit(0);
}

/* ---------------------------------------------------------------- nn::settings
 * [doc] nn::settings::Language values and their codes (switchbrew "Settings services"); the
 * order agrees with what nnsdk.c already uses (4 = "it"). LanguageCode is 8 chars, returned in x0. */
static const char *const g_language_codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru",
                                               "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
static uint64_t language_code(uint32_t lang) {
    uint64_t v = 0;
    if (lang < sizeof g_language_codes / sizeof g_language_codes[0]) memcpy(&v, g_language_codes[lang], strlen(g_language_codes[lang]));
    return v;
}
static void settings_LanguageCode_Make(Ctx *c) { RET(language_code((uint32_t)A0)); }
static void settings_LanguageEqualsCode(Ctx *c) {         /* operator==(const Language&, const LanguageCode&) */
    char a[9] = {0}, b[9] = {0};
    uint64_t v = language_code(rd32(A0));
    memcpy(a, &v, 8);
    memcpy(b, (void *)(uintptr_t)A1, 8);
    RET(v != 0 && strcmp(a, b) == 0);
}
static void settings_CodeEqualsLanguage(Ctx *c) {         /* operator==(const LanguageCode&, const Language&): the same comparison, arguments swapped */
    uint64_t t = c->x[0];
    c->x[0] = c->x[1];
    c->x[1] = t;
    settings_LanguageEqualsCode(c);
}
/* nn::oe::GetDesiredLanguage: the language the application should use = the system language when the application
 * supports it, otherwise the system's choice among the supported ones. The system language here is the Windows
 * display language of the user. Languages this game ships (its message files): ja, en-US, en-GB, fr, fr-CA, de, it,
 * es, es-419, zh-Hans, zh-Hant, ko, nl, ru; anything else gives en-US (NOT VERIFIED: the console decides from the
 * supported-language mask of the application's control data, which was not read here).
 * SSBU_LANGUAGE=<code> overrides (for example SSBU_LANGUAGE=en-US). */
static uint64_t desired_language(void) {
    static uint64_t cached;
    static const char *const supported[] = {"ja", "en-US", "en-GB", "fr", "fr-CA", "de", "it", "es", "es-419", "zh-Hans", "zh-Hant", "ko", "nl", "ru"};
    if (cached) return cached;
    char name[LOCALE_NAME_MAX_LENGTH * 3] = "", pick[16] = "en-US";
    const char *env = getenv("SSBU_LANGUAGE");
    wchar_t wname[LOCALE_NAME_MAX_LENGTH] = L"";
    ULONG count = 0, size = 0;
    if (env && env[0]) snprintf(name, sizeof name, "%s", env);
    else {
        /* the display language of the user interface, not the regional format */
        wchar_t list[512];
        size = 512;
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, list, &size) && count > 0) wcsncpy(wname, list, LOCALE_NAME_MAX_LENGTH - 1);
        else GetUserDefaultLocaleName(wname, LOCALE_NAME_MAX_LENGTH);
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof name, NULL, NULL);
    }
    int exact = 0;
    for (size_t i = 0; i < sizeof supported / sizeof supported[0]; i++) if (!_stricmp(name, supported[i])) { strcpy(pick, supported[i]); exact = 1; }
    if (!exact) {
        if (!_strnicmp(name, "zh", 2)) strcpy(pick, strstr(name, "TW") || strstr(name, "HK") || strstr(name, "MO") || strstr(name, "Hant") ? "zh-Hant" : "zh-Hans");
        else if (!_strnicmp(name, "en", 2)) strcpy(pick, !_stricmp(name, "en-US") || !_stricmp(name, "en-CA") ? "en-US" : "en-GB");
        else if (!_strnicmp(name, "fr", 2)) strcpy(pick, !_stricmp(name, "fr-CA") ? "fr-CA" : "fr");
        else if (!_strnicmp(name, "es", 2)) strcpy(pick, !_stricmp(name, "es-ES") || !_stricmp(name, "es") ? "es" : "es-419");
        else {
            char two[3] = {name[0], name[1], 0};
            for (size_t i = 0; i < sizeof supported / sizeof supported[0]; i++) if (!_stricmp(two, supported[i])) strcpy(pick, supported[i]);
        }
    }
    uint64_t v = 0;
    memcpy(&v, pick, strlen(pick));
    printf("nn::oe::GetDesiredLanguage: Windows display language \"%s\" -> \"%s\"\n", name, pick);
    cached = v;
    return v;
}
static void oe_GetDesiredLanguage(Ctx *c) { RET(desired_language()); }   /* LanguageCode (8 chars) by value in x0 */

/* ---------------------------------------------------------------- nn::err
 * [sdk] ErrorResultVariant { uint32_t state @0 (0 none, 1 Result, 2 ErrorCode); payload @4:
 * Result (4 bytes) or ErrorCode (8 bytes, unaligned) } -- 12 bytes. These replace the two
 * entries in nnsdk.c, which wrote the Result at +0 and cleared 16 bytes in the constructor. */
static void err_Variant_ctor(Ctx *c) { wr32(A0, 0); }
static void err_Variant_fromResult(Ctx *c) { wr32(A0, 1); wr32(A0 + 4, rd32(A1)); }       /* C1(const Result&), operator= */
static void err_Variant_fromCode(Ctx *c) { wr32(A0, 2); wr64(A0 + 4, rd64(A1)); }          /* C1(const ErrorCode&), operator= */
static void err_Variant_GetState(Ctx *c) { RET(rd32(A0)); }
static void err_Variant_toResult(Ctx *c) { RET(rd32(A0 + 4)); }
static void err_Variant_toCode(Ctx *c) { RET(rd64(A0 + 4)); }
/* The console's error viewer shows "Error Code: 2MMM-DDDD" (2000 + module, description) in a dialog and returns when
 * the user closes it. Here it is a Windows message box on the calling thread, which blocks the same way. Scripted
 * runs (SSBU_SEQ / SSBU_PLAY / SSBU_PRESS / SSBU_SCRIPT) and SSBU_ERROR_DIALOG=0 only log, so tests never hang. */
static void err_dialog(unsigned category, unsigned number) {
    const char *off = getenv("SSBU_ERROR_DIALOG");
    char text[160];
    if ((off && off[0] == '0') || getenv("SSBU_SEQ") || getenv("SSBU_PLAY") || getenv("SSBU_PRESS") || getenv("SSBU_SCRIPT")) return;
    snprintf(text, sizeof text, "Error Code: %04u-%04u\n\nAn error has occurred.", category, number);
    MessageBoxA(NULL, text, "Error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
}
static void err_ShowErrorResult(Ctx *c) {                 /* (Result): the console shows the error applet */
    uint32_t r = (uint32_t)A0;
    err_dialog(2000 + (r & 0x1ff), (r >> 9) & 0x1fff);
    printf("nn::err::ShowError: result 0x%x (module %u, description %u), caller 0x%llx\n", r, r & 0x1ff, (r >> 9) & 0x1fff,
           (unsigned long long)c->x[30] - 4);
    trace_printf("     nn::err::ShowError result 0x%x (module %u, description %u)\n", r, r & 0x1ff, (r >> 9) & 0x1fff);
}
static void err_ShowErrorCode(Ctx *c) {                   /* (ErrorCode { uint32_t category, number }) in x0 */
    printf("nn::err::ShowError: error code %04u-%04u, caller 0x%llx\n", (unsigned)(uint32_t)A0, (unsigned)(A0 >> 32),
           (unsigned long long)c->x[30] - 4);
    trace_printf("     nn::err::ShowError error code %04u-%04u\n", (unsigned)(uint32_t)A0, (unsigned)(A0 >> 32));
    err_dialog((unsigned)(uint32_t)A0, (unsigned)(A0 >> 32));
}

/* ---------------------------------------------------------------- nn::prepo (play reports): accepted and discarded
 * [sdk] PlayReport(): byte @0 = 0, three 8-byte fields @0x20..0x37 = 0. */
static void prepo_PlayReport_ctor(Ctx *c) { wr8(A0, 0); wr64(A0 + 0x20, 0); wr64(A0 + 0x28, 0); wr64(A0 + 0x30, 0); }

/* ---------------------------------------------------------------- nn::aoc: no add-on content installed */
static void aoc_ListAddOnContent(Ctx *c) { RET(0); }      /* (int *out, int offset, int count) -> number written */

/* ---------------------------------------------------------------- nn::util */
static HCRYPTPROV crypt_provider(void) {
    static HCRYPTPROV prov;
    static volatile LONG lock;
    while (InterlockedExchange(&lock, 1)) SwitchToThread();
    if (!prov && !CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) prov = 0;
    InterlockedExchange(&lock, 0);
    return prov;
}
int host_fixed_random(void *buffer, uint64_t size, const char *who, uint64_t lr);      /* os_threads.c: SSBU_FIXED_SEED */
static void random_bytes(void *p, size_t n) {
    if (host_fixed_random(p, n, "nn::crypto / uuid random bytes", 0)) return;
    HCRYPTPROV prov = crypt_provider();
    if (!prov || !CryptGenRandom(prov, (DWORD)n, p)) for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)rand();
}
/* [sdk] GenerateUuid: 16 random bytes made an RFC 4122 version-4 UUID (byte 6 = 0x4X, byte 8 =
 * 10xxxxxx), returned by value in x0/x1 (16-byte struct; [game] 0x71016fa08c stores x0 and x1). */
static void util_GenerateUuid(Ctx *c) {
    uint8_t b[16];
    random_bytes(b, sizeof b);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    memcpy(&c->x[0], b, 8);
    memcpy(&c->x[1], b + 8, 8);
}
/* [sdk] BinaryFileHeader checks (layout in nnsdk.c): signature = first 8 bytes; alignment shift u8 @0xe;
 * byte-order mark i16 @0xc, reversed when it reads 0xFFFE. BinaryBlockHeader: next-block offset u32 @4. */
static void util_BinaryFileHeader_IsSignatureValid(Ctx *c) { RET(rd64(A0) == A1); }
static void util_BinaryFileHeader_IsAlignmentValid(Ctx *c) { RET((A0 & ~(~0ull << (rd8(A0 + 0xe) & 63))) == 0); }
static void util_BinaryFileHeader_IsEndianReverse(Ctx *c) { RET(rd16(A0 + 0xc) == 0xfffe); }
static void util_BinaryBlockHeader_GetNextBlock(Ctx *c) { uint32_t off = rd32(A0 + 4); RET(off ? A0 + off : 0); }

/* ---------------------------------------------------------------- nn::crypto (Windows CryptoAPI) */
static void crypto_RandomBytes(Ctx *c) { random_bytes((void *)(uintptr_t)A0, (size_t)A1); }
static void crypto_Sha1(Ctx *c) {                         /* (void *out, size_t outSize, const void *data, size_t size) */
    HCRYPTHASH h = 0;
    uint8_t digest[20] = {0};
    DWORD len = sizeof digest;
    if (crypt_provider() && CryptCreateHash(crypt_provider(), CALG_SHA1, 0, 0, &h)) {
        CryptHashData(h, (const BYTE *)(uintptr_t)A2, (DWORD)A3, 0);
        CryptGetHashParam(h, HP_HASHVAL, digest, &len, 0);
        CryptDestroyHash(h);
    }
    memcpy((void *)(uintptr_t)A0, digest, A1 < sizeof digest ? (size_t)A1 : sizeof digest);
}
/* Md5Impl: the guest object is not touched; the running hash is kept on the host, keyed by its address. */
static struct { uint64_t key; HCRYPTHASH h; } g_md5[64];
static CRITICAL_SECTION g_md5_lock;
static HCRYPTHASH *md5_slot(uint64_t key, int create) {
    HCRYPTHASH *found = NULL;
    EnterCriticalSection(&g_md5_lock);
    for (int i = 0; i < 64 && !found; i++) if (g_md5[i].key == key) found = &g_md5[i].h;
    for (int i = 0; i < 64 && !found && create; i++) if (!g_md5[i].key) { g_md5[i].key = key; g_md5[i].h = 0; found = &g_md5[i].h; }
    LeaveCriticalSection(&g_md5_lock);
    return found;
}
static void md5_drop(uint64_t key) {
    EnterCriticalSection(&g_md5_lock);
    for (int i = 0; i < 64; i++) if (g_md5[i].key == key) { if (g_md5[i].h) CryptDestroyHash(g_md5[i].h); g_md5[i].key = 0; g_md5[i].h = 0; }
    LeaveCriticalSection(&g_md5_lock);
}
static void crypto_Md5_Initialize(Ctx *c) {
    md5_drop(A0);
    HCRYPTHASH *h = md5_slot(A0, 1);
    if (!h) fatal(c, "nn::crypto Md5Impl: more than 64 hashes in progress");
    if (!crypt_provider() || !CryptCreateHash(crypt_provider(), CALG_MD5, 0, 0, h)) *h = 0;
}
static void crypto_Md5_Update(Ctx *c) { HCRYPTHASH *h = md5_slot(A0, 0); if (h && *h) CryptHashData(*h, (const BYTE *)(uintptr_t)A1, (DWORD)A2, 0); }
static void crypto_Md5_GetHash(Ctx *c) {                  /* (this, void *out, size_t outSize) */
    HCRYPTHASH *h = md5_slot(A0, 0);
    uint8_t digest[16] = {0};
    DWORD len = sizeof digest;
    if (h && *h) CryptGetHashParam(*h, HP_HASHVAL, digest, &len, 0);
    memcpy((void *)(uintptr_t)A1, digest, A2 < sizeof digest ? (size_t)A2 : sizeof digest);
}
static void crypto_Md5_dtor(Ctx *c) { md5_drop(A0); }

/* ---------------------------------------------------------------- nn::fs::GetSaveDataSize
 * (int64_t *outSize, int64_t *outJournalSize, const Uid&) -> Result: the sizes of the user's save data,
 * which the console creates from the application control property (NACP) and which stay at those values
 * unless the game extends the save. Values read from the control.nacp of the user's own 13.0.4 update
 * dump with extract-tools\nacp_extract.py: UserAccountSaveDataSize 0x6000000 (96 MiB),
 * UserAccountSaveDataJournalSize 0xa00000 (10 MiB). Overridable with SSBU_SAVE_SIZE /
 * SSBU_SAVE_JOURNAL_SIZE (bytes). [game] 0x71017c15e4 uses journalSize - 0x80000. */
#define NACP_USER_SAVE_SIZE 0x6000000ll
#define NACP_USER_SAVE_JOURNAL_SIZE 0xa00000ll
static int64_t env_size(const char *name, int64_t fallback) {
    const char *v = getenv(name);
    return v && v[0] ? strtoll(v, NULL, 0) : fallback;
}
static void fs_GetSaveDataSize(Ctx *c) {
    if (A0) wr64(A0, (uint64_t)env_size("SSBU_SAVE_SIZE", NACP_USER_SAVE_SIZE));
    if (A1) wr64(A1, (uint64_t)env_size("SSBU_SAVE_JOURNAL_SIZE", NACP_USER_SAVE_JOURNAL_SIZE));
    RET(0);
}

/* nn::mii: see nnmii.c (Database on the SDK's own default-Mii code); nn::nfp, nn::web, nn::swkbd, nn::ec shop pages:
 * nnapplet.c; nn::album screenshots and movie maker: nnalbum.c. */

/* ---------------------------------------------------------------- nn::friends: no friends, no network
 * [sdk] NotificationQueue::GetSystemEvent() returns this+0x38 (the SystemEvent inside the queue
 * object) once the queue is initialised. Events are keyed by address in os_threads.c; this one is
 * never signalled. */
static void friends_NotificationQueue_GetSystemEvent(Ctx *c) {
    static uint64_t known[16];
    static volatile LONG n;
    uint64_t ev = A0 + 0x38;
    int seen = 0;
    for (int i = 0; i < n && i < 16; i++) if (known[i] == ev) seen = 1;
    if (!seen) {                                          /* first time for this queue: make it a clean manual-clear event */
        LONG slot = InterlockedIncrement(&n) - 1;
        if (slot < 16) known[slot] = ev;
        os_init_event(ev, 0);
    }
    RET(ev);
}
/* An asynchronous request that has already finished (with a failure): its event is signalled. */
static void async_GetSystemEvent(Ctx *c) { os_init_event(A1, 0); os_signal_event(A1); RET(0); }   /* (this, SystemEvent *out) */
static void out_count0_at0(Ctx *c) { if (A0) wr32(A0, 0); RET(0); }               /* (int *outCount, ...) -> Result */

/* ---------------------------------------------------------------- nn::account extras: the one local user, no online account */
static void acc_ShowUserSelector(Ctx *c) { wr64(A0, 1); wr64(A0 + 8, 0); RET(0); }   /* (Uid *out, settings): uid as in nnfs.c */

/* nn::ldn, nn::nifm leftovers, nn::nsd, nn::ssl, sockets and the nn::ec accessor: nnnet_c4a.inc (included by nnnet.c). */

/* nn::album (Initialize / Finalize, the file lists, IsMovieMakerRunning): nnalbum.c */

/* ---------------------------------------------------------------- nn::ngc ProfanityFilter: nothing is masked */
static void ngc_MaskProfanityWordsInText(Ctx *c) { if (A1) wr32(A1, 0); RET(0); } /* (this, int *outCount, char16_t *text, uint32_t patterns) */

/* ---------------------------------------------------------------- byte order helpers of nn::socket */
static void socket_swap16(Ctx *c) { RET((uint16_t)(((A0 & 0xff) << 8) | ((A0 >> 8) & 0xff))); }
static void socket_swap32(Ctx *c) { RET(bswap32((uint32_t)A0)); }

/* ---------------------------------------------------------------- registration */
#define OE(n) "_ZN2nn2oe" n
#define PR(n) "_ZN2nn5prepo10PlayReport" n
#define FR(n) "_ZN2nn7friends" n
#define AC(n) "_ZN2nn7account" n
#define SO(n) "_ZN2nn6socket" n
#define LDN(n) "_ZN2nn3ldn" n
#define ERRV(n) "_ZN2nn3err18ErrorResultVariant" n

/* Replace whatever is registered: exact SDK behaviour for one small class (see nn::err above). */
static const ImportDef replace_imports[] = {
    {"_ZN2nn2oe25TryPopNotificationMessageEPj", oe_TryPopNotificationMessage}, {"_ZN2nn2oe20GetCurrentFocusStateEv", oe_GetCurrentFocusState},
    {"_ZN2nn2oe27GetNotificationMessageEventEv", oe_GetNotificationMessageEvent},
    {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", oe_SetFocusHandlingMode},
    {"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", oe_SetOperationModeChangedNotificationEnabled},
    {"_ZN2nn2oe18GetDesiredLanguageEv", oe_GetDesiredLanguage},
    {"_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE", settings_CodeEqualsLanguage},
    {"_ZN2nn2oe29BeginAutoSleepDisabledSectionEv", oe_BeginAutoSleepDisabledSection}, {"_ZN2nn2oe27EndAutoSleepDisabledSectionEv", oe_EndAutoSleepDisabledSection},
    {ERRV("C1Ev"), err_Variant_ctor}, {ERRV("aSERKNS_6ResultE"), err_Variant_fromResult}, {ERRV("C1ERKNS_6ResultE"), err_Variant_fromResult},
    {ERRV("aSERKNS0_9ErrorCodeE"), err_Variant_fromCode}, {ERRV("C1ERKNS0_9ErrorCodeE"), err_Variant_fromCode},
    {"_ZNK2nn3err18ErrorResultVariant8GetStateEv", err_Variant_GetState},
    {"_ZNK2nn3err18ErrorResultVariantcvNS_6ResultEEv", err_Variant_toResult},
    {"_ZNK2nn3err18ErrorResultVariantcvNS0_9ErrorCodeEEv", err_Variant_toCode},
};

/* Only installed where no other file provides the function. */
static const ImportDef fill_imports[] = {
    /* C wrappers */
    {"nnosGetCurrentThread", nnos_GetCurrentThread}, {"nnosCreateThread", nnos_CreateThread}, {"nnosStartThread", nnos_StartThread},
    {"nnosWaitThread", nnos_WaitThread}, {"nnosDestroyThread", nnos_DestroyThread}, {"nnosGetThreadPriority", nnos_GetThreadPriority},
    {"nnosGetThreadCoreMask", nnos_GetThreadCoreMask}, {"nnosSetThreadCoreMask", nnos_SetThreadCoreMask},
    {"nnosSetThreadNamePointer", nnos_SetThreadName}, {"nnosSleepThread", nnos_SleepThread},
    {"nnosInitializeMutex", nnos_InitializeMutex}, {"nnosFinalizeMutex", nnos_FinalizeMutex}, {"nnosLockMutex", nnos_LockMutex},
    {"nnosUnlockMutex", nnos_UnlockMutex}, {"nnosInitializeEvent", nnos_InitializeEvent}, {"nnosFinalizeEvent", nnos_FinalizeEvent},
    {"nnosSignalEvent", nnos_SignalEvent}, {"nnosWaitEvent", nnos_WaitEvent}, {"nnosGetSystemTick", nnos_GetSystemTick},
    {"nnosGetSystemTickFrequency", nnos_GetSystemTickFrequency}, {"nnosConvertTickToNanoSeconds", nnos_ConvertTickToNanoSeconds},
    {"setjmp", c_setjmp}, {"longjmp", c_longjmp}, {"swprintf", c_swprintf},
    {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i", diag_AbortImpl}, {"nndetailDiagAbortFormatImpl", diag_AbortFormatImpl},
    {"_ZN2nn6detail21UnexpectedDefaultImplEPKcS2_i", diag_UnexpectedDefault}, {"__assert_fail", c_assert_fail},
    /* nn::oe */
    {OE("25TryPopNotificationMessageEPj"), oe_TryPopNotificationMessage}, {OE("20GetCurrentFocusStateEv"), oe_GetCurrentFocusState},
    {OE("25GetProgramTotalActiveTimeEv"), oe_GetProgramTotalActiveTime}, {OE("28RequestToRelaunchApplicationEv"), oe_RequestToRelaunchApplication},
    {OE("17FinishStartupLogoEv"), keep}, {OE("42SetOperationModeChangedNotificationEnabledEb"), keep},
    {OE("31EnterExitRequestHandlingSectionEv"), oe_EnterExitRequestHandlingSection}, {OE("31LeaveExitRequestHandlingSectionEv"), oe_LeaveExitRequestHandlingSection},
    {OE("29BeginAutoSleepDisabledSectionEv"), keep}, {OE("27EndAutoSleepDisabledSectionEv"), keep},
    {OE("21SwitchLcdBacklightOffEv"), keep}, {OE("20SwitchLcdBacklightOnEv"), keep},
    {OE("27SetTvPowerStateMatchingModeENS0_24TvPowerStateMatchingModeE"), keep}, {OE("16EnableScreenShotEv"), keep},
    {OE("17DisableScreenShotEv"), keep}, {OE("15SetCpuBoostModeENS0_12CpuBoostModeE"), keep},
    /* nn::settings, nn::err */
    {"_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE", settings_LanguageCode_Make},
    {"_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", settings_LanguageEqualsCode},
    {"_ZN2nn3err9ShowErrorENS_6ResultE", err_ShowErrorResult}, {"_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", err_ShowErrorCode},
    /* nn::prepo */
    {"_ZN2nn5prepo10InitializeEv", ret0}, {"_ZN2nn5prepo28RequestImmediateTransmissionEv", ret0},
    {PR("C1Ev"), prepo_PlayReport_ctor}, {PR("10SetEventIdEPKc"), ret0}, {PR("9SetBufferEPvm"), keep},
    {PR("3AddEPKcRKNS0_10Any64BitIdE"), ret0}, {PR("3AddEPKcl"), ret0}, {PR("3AddEPKcd"), ret0}, {PR("3AddEPKcS3_"), ret0},
    {PR("4SaveERKNS_7account3UidE"), ret0},
    /* nn::aoc, nn::pctl, nn::pdm */
    {"_ZN2nn3aoc17CountAddOnContentEv", ret0}, {"_ZN2nn3aoc16ListAddOnContentEPiii", aoc_ListAddOnContent},
    /* close5: both return a BOOL, not a Result: [sdk 0x33a620] `cset w0, eq` on the service Result (slot +0x28), [sdk 0x33aef0] the
     * byte the service writes (slot +0x148). A console without parental controls: true (suyu pctl/parental_control_service.cpp
     * CheckFreeCommunicationPermissionImpl / ConfirmStereoVisionPermissionImpl: no PIN -> true). The old ret0 answered "refused". */
    {"_ZN2nn4pctl32CheckFreeCommunicationPermissionEb", pctl_permitted}, {"_ZN2nn4pctl27CheckStereoVisionPermissionEb", pctl_permitted},
    {"_ZN2nn3pdm30QueryApplicationPlayStatisticsEPNS0_25ApplicationPlayStatisticsEPKNS_13ApplicationIdEi", ret0},
    /* nn::util, nn::crypto */
    {"_ZN2nn4util12GenerateUuidEv", util_GenerateUuid},
    {"_ZNK2nn4util16BinaryFileHeader16IsSignatureValidEl", util_BinaryFileHeader_IsSignatureValid},
    {"_ZNK2nn4util16BinaryFileHeader16IsAlignmentValidEv", util_BinaryFileHeader_IsAlignmentValid},
    {"_ZNK2nn4util16BinaryFileHeader15IsEndianReverseEv", util_BinaryFileHeader_IsEndianReverse},
    {"_ZNK2nn4util17BinaryBlockHeader12GetNextBlockEv", util_BinaryBlockHeader_GetNextBlock},
    {"_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", crypto_RandomBytes},
    {"_ZN2nn6crypto16GenerateSha1HashEPvmPKvm", crypto_Sha1},
    {"_ZN2nn6crypto6detail7Md5Impl10InitializeEv", crypto_Md5_Initialize}, {"_ZN2nn6crypto6detail7Md5Impl6UpdateEPKvm", crypto_Md5_Update},
    {"_ZN2nn6crypto6detail7Md5Impl7GetHashEPvm", crypto_Md5_GetHash}, {"_ZN2nn6crypto6detail7Md5ImplD1Ev", crypto_Md5_dtor},
    /* nn::fs */
    {"_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE", fs_GetSaveDataSize},
    /* nn::friends */
    {FR("17NotificationQueue14GetSystemEventEv"), friends_NotificationQueue_GetSystemEvent},
    {FR("17NotificationQueue3PopEPNS0_16NotificationInfoE"), unavailable}, {FR("17NotificationQueueD1Ev"), keep},
    {FR("12AsyncContextD1Ev"), keep}, {FR("12AsyncContext14GetSystemEventEPNS_2os11SystemEventE"), async_GetSystemEvent},
    {"_ZNK2nn7friends12AsyncContext9GetResultEv", unavailable},
    {"_ZNK2nn7friends6Friend7IsValidEv", ret0}, {"_ZNK2nn7friends7Profile7IsValidEv", ret0},
    {FR("18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii"), out_count0_at0},
    {FR("14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi"), unavailable},
    {FR("16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi"), unavailable},
    {FR("18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_"), unavailable},
    {FR("14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_"), unavailable},
    {FR("29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb"), unavailable},
    /* nn::account */
    {AC("16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE"), acc_ShowUserSelector},
    {AC("44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE"), acc_nsa_unavailable},
    {AC("12AsyncContextaSEOS1_"), keep}, {AC("12AsyncContextC1EOS1_"), keep}, {AC("12AsyncContext6CancelEv"), ret0},
    {AC("12AsyncContext14GetSystemEventEPNS_2os11SystemEventE"), async_GetSystemEvent},
    {AC("37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE"), acc_nsa_unavailable},
    {AC("26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE"), acc_nsa_unavailable},
    {AC("29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvm"), acc_nsa_unavailable},
    /* nn::nifm, nn::nsd, nn::ssl */
    /* sockets */
    {SO("9InetHtonsEt"), socket_swap16}, {SO("9InetNtohsEt"), socket_swap16}, {SO("9InetHtonlEj"), socket_swap32}, {SO("9InetNtohlEj"), socket_swap32},
    {"nnsocketInetHtons", socket_swap16}, {"nnsocketInetNtohs", socket_swap16},
    /* nn::ldn */
    /* nn::ec, nn::album, nn::ngc */
    {"_ZN2nn3ngc15ProfanityFilterC1Ev", keep}, {"_ZN2nn3ngc15ProfanityFilterD1Ev", keep}, {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ret0},
    {"_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", keep}, {"_ZN2nn3ngc15ProfanityFilter15SkipAtSignCheckENS1_8SkipModeE", keep},
    {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", ngc_MaskProfanityWordsInText},
};

/* ---------------------------------------------------------------- self-test of setjmp/longjmp
 * SSBU_MISC_SELFTEST=1: run the game's own luaD_rawrunprotected (0x71038faf00) on a blank
 * lua_State with host callbacks that return normally, throw through the game's luaD_throw
 * (0x71038fae10), and throw from inside a nested protected call; then exit. */
#define LUAD_RAWRUNPROTECTED GA(0x71038faf00ull)
#define LUAD_THROW GA(0x71038fae10ull)
static int g_selftest_mode;
static void selftest_callback(Ctx *c) {                   /* Pfunc: (lua_State *L, void *ud) */
    uint64_t L = A0;
    if (g_selftest_mode == 0) return;
    if (g_selftest_mode == 2) {                           /* nested protected call that throws 5, then throw 7 here */
        uint64_t sp = c->x[31], lr = c->x[30];
        g_selftest_mode = 1;
        c->x[0] = L; c->x[1] = hostfn_address("nnmisc selftest callback", selftest_callback); c->x[2] = 0;
        call_guest(c, LUAD_RAWRUNPROTECTED);
        printf("  nested protected call returned status %d (expected 5), sp %s\n", (int)c->x[0], c->x[31] == sp ? "restored" : "WRONG");
        c->x[30] = lr;
        c->x[0] = L; c->x[1] = 7;
        call_guest(c, LUAD_THROW);
    } else { c->x[0] = L; c->x[1] = 5; call_guest(c, LUAD_THROW); }
    printf("  ERROR: luaD_throw returned\n");
}
static void misc_selftest(void) {
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    uint8_t *stack = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *L = calloc(1, 0x400);
    static const int expect[3] = {0, 5, 7};
    int bad = 0;
    for (int mode = 0; mode < 3; mode++) {
        memset(c, 0, sizeof *c);
        c->x[31] = (uint64_t)(uintptr_t)(stack + (1 << 20) - 0x100);
        for (int i = 19; i < 30; i++) c->x[i] = 0x1111111111111111ull * (uint64_t)(i - 18);
        for (int i = 8; i < 16; i++) c->v[i].d[0] = 0xd0d0d0d000000000ull + (uint64_t)i;
        wr64((uint64_t)(uintptr_t)L + 0x58, 0);
        wr16((uint64_t)(uintptr_t)L + 0xc6, 0x1234);
        uint64_t sp = c->x[31];
        g_selftest_mode = mode;
        c->x[0] = (uint64_t)(uintptr_t)L; c->x[1] = hostfn_address("nnmisc selftest callback", selftest_callback); c->x[2] = 0;
        call_guest(c, LUAD_RAWRUNPROTECTED);
        int ok = (int)c->x[0] == expect[mode] && c->x[31] == sp && rd64((uint64_t)(uintptr_t)L + 0x58) == 0 &&
                 rd16((uint64_t)(uintptr_t)L + 0xc6) == 0x1234;
        for (int i = 19; i < 30; i++) if (c->x[i] != 0x1111111111111111ull * (uint64_t)(i - 18)) ok = 0;
        for (int i = 8; i < 16; i++) if (c->v[i].d[0] != 0xd0d0d0d000000000ull + (uint64_t)i) ok = 0;
        printf("setjmp/longjmp self-test %d (%s): status %d, expected %d -> %s\n", mode,
               mode == 0 ? "normal return" : mode == 1 ? "throw" : "throw after a nested throw", (int)c->x[0], expect[mode], ok ? "ok" : "FAILED");
        bad += !ok;
    }
    fflush(stdout);
    ExitProcess((UINT)bad);
}

/* SSBU_DUMP_UNREG=<file>: write the names of all imports that still have no handler. */
static void dump_unregistered(void) {
    const char *path = getenv("SSBU_DUMP_UNREG");
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && dyn_syms[i].name[0] && !import_lookup(dyn_syms[i].name)) fprintf(f, "%s\n", dyn_syms[i].name);
    fclose(f);
}

/* close5 (lead) self-test: the two parental-control checks return BOOL true on a console without parental controls
 * ([sdk 0x33a620], [0x33aef0]); the image's text is read-only, its data writable (hostrt.c runtime_init). */
int close5_lead_selftest(Ctx *c) {
    int bad = 0, n = 0;
    static const char *names[2] = {"_ZN2nn4pctl32CheckFreeCommunicationPermissionEb", "_ZN2nn4pctl27CheckStereoVisionPermissionEb"};
    for (int i = 0; i < 2; i++)
        for (int show = 0; show < 2; show++) {
            ImportFn f = import_lookup(names[i]);
            c->x[0] = (uint64_t)show | 0xdead00000000ull;
            if (f) f(c);
            n++; if (!f || (c->x[0] & 0xff) != 1) { bad++; printf("  close5 lead: %s(%d) is not true\n", names[i], show); }
        }
    if (!getenv("SSBU_NO_PROTECT")) {
        n += 3;
        if (!IsBadWritePtr((void *)(uintptr_t)0x7100000000ull, 1)) { bad++; printf("  close5 lead: the first text page is writable\n"); }
        if (!IsBadWritePtr((void *)(uintptr_t)((GUEST_BASE + g_main.data_start) - 1), 1)) { bad++; printf("  close5 lead: the last read-only data byte is writable\n"); }
        if (IsBadWritePtr((void *)(uintptr_t)(GUEST_BASE + g_main.data_start), 1) || IsBadReadPtr((void *)(uintptr_t)0x7100000000ull, 16)) { bad++; printf("  close5 lead: data segment not writable or text not readable\n"); }
    }
    printf("  close5 lead (pctl bool answers, image protection): %d cases, %d failed\n", n, bad);
    return bad;
}

void register_misc_imports(void) {
    QueryPerformanceCounter(&g_start);
    InitializeCriticalSection(&g_oe_lock);
    InitializeCriticalSection(&g_md5_lock);
    register_imports(replace_imports, (int)(sizeof replace_imports / sizeof replace_imports[0]));
    int n = (int)(sizeof fill_imports / sizeof fill_imports[0]), added = 0;
    for (int i = 0; i < n; i++) {
        if (!import_data(fill_imports[i].name)) { printf("nnmisc: \"%s\" is not an import of the game (typo?)\n", fill_imports[i].name); continue; }
        if (!import_lookup(fill_imports[i].name)) { register_imports(&fill_imports[i], 1); added++; }
    }
    for (int i = 0; i < (int)(sizeof replace_imports / sizeof replace_imports[0]); i++)
        if (!import_data(replace_imports[i].name)) printf("nnmisc: \"%s\" is not an import of the game (typo?)\n", replace_imports[i].name);
    trace_printf("nnmisc: %d of %d functions installed (the rest are provided elsewhere or not imported)\n", added, n);
    dump_unregistered();
    (void)ret1;
    if (getenv("SSBU_MISC_SELFTEST")) misc_selftest();
}

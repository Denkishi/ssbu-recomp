/* Host display: a real window, 60 Hz pacing (120 Hz in battles with frame_rate = 120, hfr.c) and the guest's vsync event.
 * The window's contents are drawn by nvn_gl.c, whose render thread owns the OpenGL context. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define WIDTH 1280
#define HEIGHT 720
#define FRAME_NS 16666667LL

void os_init_event(uint64_t key, int autoclear);      /* os_threads.c */
void os_signal_event(uint64_t key);
int hfr_rate(void);                                   /* hfr.c: 60, or 120 during a battle with frame_rate = 120: refreshes per second of the clock below */

static HWND g_hwnd;
static volatile LONG g_ready;
static volatile LONG64 g_frames;
void nvn_gl_start(void *hwnd);                        /* nvn_gl.c */
void nvn_gl_report(FILE *out);
static uint64_t g_vsync_event;
static LARGE_INTEGER g_freq;

/* P3 round 3: ONE refresh clock. The game counts the signals of the vsync event on a thread of its own ("VSync Count
 * Thread", 0x7103810a40: window object + 0x138) and, at every present (0x710386fc30), advances its frame counter
 * (0x710593a440) by the number of refreshes counted since the previous present. So the refresh count IS the game's
 * time. Before this, the event came from a Sleep() loop and presentation was held by a second, independent 60 Hz
 * clock that restarted its phase after every late frame: between two presents the game counted 0, 1 or 2 refreshes,
 * and "0" counts as one step while "2" counts as two, so the match clock gained (10.77 s per 600 frames).
 * Now: refresh n happens at t0 + n/60 s; at each one the vsync event is signalled and then the frames waiting to be
 * shown are released. A frame queued between refresh n and n+1 is shown at n+1, and no sooner than `interval`
 * refreshes after the frame before it (nvnWindowSetPresentInterval); the game is held in its wait for the window
 * texture (nvnSyncWait on the sync of nvnWindowAcquireTexture), which is AFTER it has read the refresh count, as on
 * the console. SSBU_OLD_PACING=1: the two clocks as before. SSBU_CLOCKSTAT=1: one line every 600 presented frames. */
static int g_old_pacing = -1, g_clockstat;
static volatile LONG64 g_vsync_n;            /* refreshes passed (published after the event was signalled) */
static volatile LONG64 g_vsync_signals, g_vsync_late;
static SRWLOCK g_vsync_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_vsync_cv = CONDITION_VARIABLE_INIT;
static LONG64 g_latch;                       /* refresh at which the last queued frame is shown */
static volatile LONG g_latch_pending;        /* a frame was queued and nobody has waited for its refresh yet */
static int old_pacing(void) {
    if (g_old_pacing < 0) { const char *e = getenv("SSBU_OLD_PACING"); g_old_pacing = e && atoi(e) != 0; g_clockstat = getenv("SSBU_CLOCKSTAT") && atoi(getenv("SSBU_CLOCKSTAT")) != 0; }
    return g_old_pacing;
}
static void vsync_test_key(void);

/* TEST ONLY, SSBU_SPEED=<n> (debug_mode.c, tests\DEBUG_MODE.md): 2, 4 (any number > 0) = the refresh clock runs n times
 * faster; 0 = uncapped. Not set or 1 = nothing changes. In this mode there is no free-running vsync thread: the refresh is
 * given when the frame the game queued is due, ONE refresh per queued frame (interval 1), the event is signalled and the
 * wait goes on until the game's VSync Count Thread has counted it (window object + 0x138), so the game steps one frame per
 * presented frame whatever the host speed; the wall-clock gap between refreshes is 1/60/n s (none when uncapped). */
static double g_speed = 1.0;
static int g_lockstep;
static volatile uint64_t g_wnd_count_addr;           /* guest address of the window object's refresh count, once known */
static void speed_init(void) {
    const char *e = getenv("SSBU_SPEED");
    if (!e || !e[0]) return;
    g_speed = atof(e);
    if (g_speed < 0) g_speed = 0;
    g_lockstep = g_speed != 1.0;
    if (g_lockstep) printf("display: SSBU_SPEED=%s: %s, one refresh per presented frame\n", e, g_speed > 0 ? "paced at n x 60 Hz" : "uncapped");
}

int oe_exit_request(void);                            /* nnmisc.c */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {                             /* the exit request: nn::oe decides (nnmisc.c oe_exit_request) whether the game ends now or after its exit-request handling section */
        printf("\nwindow closed by the user after %lld frames\n", (long long)g_frames);
        if (!oe_exit_request()) report_and_exit(0);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcA(h, m, w, l);
}

static char g_title[96];

static DWORD WINAPI window_thread(LPVOID arg) {
    (void)arg;
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ssbu_native_window";
    wc.style = CS_OWNDC;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    /* Track O: window mode and size from config\graphics.ini (gfx_options.c). borderless / fullscreen: a frameless window
     * over the primary monitor (fullscreen: also topmost). */
    const GfxOptions *go = gfx_options();
    DWORD style = go->window_mode ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    int cw = go->width, ch = go->height, px = CW_USEDEFAULT, py = CW_USEDEFAULT;
    if (go->window_mode) {
        MONITORINFO mi;
        POINT origin = {0, 0};
        memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi)) { cw = mi.rcMonitor.right - mi.rcMonitor.left; ch = mi.rcMonitor.bottom - mi.rcMonitor.top; px = mi.rcMonitor.left; py = mi.rcMonitor.top; }
    }
    RECT r = {0, 0, cw, ch};
    AdjustWindowRect(&r, style, FALSE);
    printf("display: window %s, client %dx%d (graphics.ini)\n", go->window_mode == 2 ? "fullscreen (borderless, topmost)" : go->window_mode ? "borderless" : "windowed", cw, ch);
    /* Only the copies a person plays (ssbu_native.exe, ssbu_user.exe, or SSBU_SHOW=1) get a window on screen;
     * test copies started under any other name keep theirs off-screen and out of the taskbar, so automated
     * runs do not cover the desktop. SSBU_TITLE names the window. */
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe, *show = getenv("SSBU_SHOW");
    int visible = show ? atoi(show) != 0 : (!_stricmp(base, "ssbu_native.exe") || !_stricmp(base, "ssbu_user.exe"));
    snprintf(g_title, sizeof g_title, "%s", getenv("SSBU_TITLE") ? getenv("SSBU_TITLE") : "SSBU native (study build)");
    if (visible)
        g_hwnd = CreateWindowExA(go->window_mode == 2 ? WS_EX_TOPMOST : 0, wc.lpszClassName, g_title, style | WS_VISIBLE, px, py, r.right - r.left,
                                 r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    else {
        g_hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, g_title, style, -32000, -32000,
                                 r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }
    InterlockedExchange(&g_ready, 1);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}

/* Signals the guest's vsync event 60 times per second (hfr_rate(): 120 during a battle in the 120 Hz mode). */
static DWORD WINAPI vsync_thread(LPVOID arg) {
    (void)arg;
    LONGLONG hz = hfr_rate();
    LARGE_INTEGER next;
    QueryPerformanceCounter(&next);
    if (!old_pacing()) {
        LARGE_INTEGER t0 = next, now;
        LONG64 n = 0, n0 = 0;                  /* refresh n0 happened at t0; the clock's rate changes there (hfr.c) */
        HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        for (;;) {
            LONGLONG want = hfr_rate();
            if (want != hz) { t0.QuadPart += (n - n0) * g_freq.QuadPart / hz; n0 = n; hz = want; }      /* re-anchor at the last refresh */
            LONGLONG target = t0.QuadPart + (n - n0 + 1) * g_freq.QuadPart / hz;       /* no accumulated rounding */
            QueryPerformanceCounter(&now);
            LONGLONG left = target - now.QuadPart;
            if (left > 0) {
                LARGE_INTEGER due;
                due.QuadPart = -((left - g_freq.QuadPart / 2000) * 10000000 / g_freq.QuadPart);
                if (due.QuadPart < 0) { if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100); else Sleep((DWORD)(left * 1000 / g_freq.QuadPart)); }
                for (QueryPerformanceCounter(&now); now.QuadPart < target; QueryPerformanceCounter(&now)) YieldProcessor();
            }
            LONG64 passed = n0 + (now.QuadPart - t0.QuadPart) * hz / g_freq.QuadPart;   /* >= n + 1; more when this thread was kept off the CPU */
            if (passed < n + 1) passed = n + 1;
            if (passed > n + 1) InterlockedExchangeAdd64(&g_vsync_late, passed - (n + 1));
            n = passed;
            if (g_vsync_event) { os_signal_event(g_vsync_event); InterlockedIncrement64(&g_vsync_signals); }
            AcquireSRWLockExclusive(&g_vsync_lock);
            InterlockedExchange64(&g_vsync_n, n);
            ReleaseSRWLockExclusive(&g_vsync_lock);
            WakeAllConditionVariable(&g_vsync_cv);
            vsync_test_key();
        }
    }
    for (;;) {
        next.QuadPart += g_freq.QuadPart / hfr_rate();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG ms = (next.QuadPart - now.QuadPart) * 1000 / g_freq.QuadPart;
        if (ms > 0) Sleep((DWORD)ms); else next = now;
        if (g_vsync_event) { os_signal_event(g_vsync_event); InterlockedIncrement64(&g_vsync_signals); }
        vsync_test_key();
    }
}
static void vsync_test_key(void) {
    {
        /* Test aid: SSBU_AUTOKEY=<letter>,<first second>,<every n seconds> presses a key now and then, so a run without a person can get
         * past "press any button". Only while this window is the foreground window, so the key never reaches another program. */
        static int key = -1, first, every, tick, down;
        if (key == -1) {
            const char *e = getenv("SSBU_AUTOKEY");
            key = 0;
            if (e && e[0]) { key = e[0]; first = 30; every = 5; sscanf(e + 1, ",%d,%d", &first, &every); if (every < 1) every = 1; }
        }
        tick++;
        if (key && down && tick - down >= 8) { keybd_event((BYTE)key, 0, KEYEVENTF_KEYUP, 0); down = 0; }
        else if (key && !down && tick >= first * 60 && (tick - first * 60) % (every * 60) == 0 && ((tick % 600 == 0 ? (void)printf("display: test key wanted, window foreground: %d%c", GetForegroundWindow() == g_hwnd, 10) : (void)0), 1) && (GetForegroundWindow() == g_hwnd || (SetForegroundWindow(g_hwnd), GetForegroundWindow() == g_hwnd))) { keybd_event((BYTE)key, 0, 0, 0); down = tick; printf("display: test key %c pressed at frame tick %d%c", key, tick, 10); }
    }
}

void hitch_init(void); void hitch_pacing(int on);      /* hitch.c: SSBU_HITCH */
void display_init(void) {
    QueryPerformanceFrequency(&g_freq);
    /* P3 round 3: the console's waits with a time-out (nn::os::TimedWaitEvent, TimedReceiveMessageQueue, timed condition
     * variables: os_threads.c, SleepConditionVariableCS with whole milliseconds) end at the time asked for. At the
     * default Windows timer period they ended at the next 15.6 ms tick, so a thread that polls with a 1 ms time-out ran
     * 64 times per second: the movie player's two feed threads delivered one sample per tick and its pre-roll loop
     * [game 0x71036f27b4, 125 x 10 ms, holding the player's mutex] ran out every time (1.3 s, main thread blocked).
     * SSBU_TIMER_PERIOD=<ms> (default 1; 0 = leave the system default). */
    { const char *e = getenv("SSBU_TIMER_PERIOD"); int ms = e ? atoi(e) : 1; if (ms > 0) { MMRESULT r = timeBeginPeriod((UINT)ms); printf("display: system timer period %d ms requested (%s)%c", ms, r == TIMERR_NOERROR ? "ok" : "refused", 10); } }
    hitch_init();
    CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    while (!g_ready) Sleep(1);
    nvn_gl_start(g_hwnd);
    speed_init();
    if (!g_lockstep) CreateThread(NULL, 0, vsync_thread, NULL, 0, NULL);
    { void nvn_vk_selftest_hook(void); nvn_vk_selftest_hook(); }      /* nvn_vk.c: SSBU_RENDERER=vk SSBU_VK_SELFTEST=<frames>, then exit (plan\phases\V1_vulkan.md, M1) */
}

/* Called after every presented frame (nvn_gl.c has already put it in the window): holds the caller to 60 Hz. */
static volatile LONG g_interval = 1;         /* nvnWindowSetPresentInterval: display refreshes per presented frame (0 = do not wait) */
void display_set_interval(int n) { InterlockedExchange(&g_interval, n < 0 ? 0 : n > 5 ? 5 : n); }
void nvn_pf_pace(LONG64 ticks);            /* nvn_gl.c: timing scopes */
/* SSBU_CLOCKSTAT=1: per 600 presented frames, the real time, the vsync signals sent, the refreshes the game counted
 * (its frame counter at 0x710593a440: 600 = one step per presented frame) and the refreshes this thread slept through. */
static LONG64 g_cs_steps, g_cs_hist[4];      /* game steps (1 + refreshes skipped) and the histogram of "refreshes counted since the last present" 0, 1, 2, 3+ */
/* nvn_gl.c, at nvnWindowAcquireTexture: the game's window object is the index pointer - 0x30; + 0x138 = refreshes its
 * VSync Count Thread has counted, + 0x140 = the count at its previous present. Right after this call the game computes
 * steps = max(count - last, 1) (0x710386fc30) and then stores last = count. */
void display_clock_sample(uint64_t index_ptr) {
    { void hfr_picture(void); hfr_picture(); }      /* hfr.c: once per picture, before the game's next count of updates */
    if (g_lockstep && index_ptr) g_wnd_count_addr = index_ptr - 0x30 + 0x138;
    if (!g_clockstat || !index_ptr) return;
    uint64_t count = rd64(index_ptr - 0x30 + 0x138), last = rd64(index_ptr - 0x30 + 0x140);
    LONG64 d = (LONG64)(count - last);
    g_cs_steps += d > 1 ? d : 1;
    g_cs_hist[d <= 0 ? 0 : d >= 3 ? 3 : d]++;
}
static void clock_stat(LONG64 n, LARGE_INTEGER now) {
    static LARGE_INTEGER t_last; static LONG64 sig_last, late_last, steps_last, h_last[4];
    if (!g_clockstat || n % 600) return;
    LONG64 sig = g_vsync_signals, late = g_vsync_late;
    if (t_last.QuadPart) printf("clock: frames %lld-%lld real %.3f s, vsync signals %lld (%.2f Hz), game steps %lld = %.2f s of match time, refreshes counted between presents 0:%lld 1:%lld 2:%lld 3+:%lld, slept through by the vsync thread %lld%c", (long long)(n - 600), (long long)n,
        (double)(now.QuadPart - t_last.QuadPart) / (double)g_freq.QuadPart, (long long)(sig - sig_last), (double)(sig - sig_last) * (double)g_freq.QuadPart / (double)(now.QuadPart - t_last.QuadPart), (long long)(g_cs_steps - steps_last), (double)(g_cs_steps - steps_last) / (double)hfr_rate(),
        (long long)(g_cs_hist[0] - h_last[0]), (long long)(g_cs_hist[1] - h_last[1]), (long long)(g_cs_hist[2] - h_last[2]), (long long)(g_cs_hist[3] - h_last[3]), (long long)(late - late_last), 10);
    t_last = now; sig_last = sig; late_last = late; steps_last = g_cs_steps; memcpy(h_last, g_cs_hist, sizeof h_last);
}
/* New pacing, part 1 (nvnQueuePresentTexture, after the frame is in the window): decides the refresh that shows it. */
static void frame_queued(void) {
    static LARGE_INTEGER second_start;
    static LONG64 second_frames;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!second_start.QuadPart) second_start = now;
    LONG64 n = InterlockedIncrement64(&g_frames);
    second_frames++;
    LONG64 at = g_vsync_n + 1, iv = g_interval;
    if (!iv) at = g_vsync_n;                                       /* interval 0: shown at once, nobody waits */
    else if (at < g_latch + iv) at = g_latch + iv;
    g_latch = at;
    InterlockedExchange(&g_latch_pending, iv != 0);
    clock_stat(n, now);
    {   /* TEST ONLY, SSBU_FRAMELOG=<k>: a line "frame-mark: <n>" every k presented frames, to place log events in game frames */
        static int every = -1;
        if (every < 0) { const char *e = getenv("SSBU_FRAMELOG"); every = e ? atoi(e) : 0; }
        if (every > 0 && n % every == 0) printf("frame-mark: %lld%c", (long long)n, 10);
        { void debug_mode_frame(long long frame); debug_mode_frame((long long)n); }      /* debug_mode.c: returns at once unless SSBU_DEBUG_END / SSBU_DEBUG_MATCH is set */
    }
    if ((now.QuadPart - second_start.QuadPart) >= g_freq.QuadPart) {
        char title[160];
        if (g_lockstep) printf("speed: frame %lld, %lld frames presented in the last %.2f s (%.1f per second)%c", (long long)n, (long long)second_frames,
                               (double)(now.QuadPart - second_start.QuadPart) / (double)g_freq.QuadPart, (double)second_frames * (double)g_freq.QuadPart / (double)(now.QuadPart - second_start.QuadPart), 10);
        if (gfx_options()->overlay && second_frames) {       /* track O: frame_time_overlay = on */
            double ms = (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames;
            printf("frame-time: %.2f ms per frame over the last second (%lld frames)\n", ms, (long long)second_frames);
            snprintf(title, sizeof title, "%s - frame %lld - %lld fps - %.2f ms per frame", g_title, (long long)n, (long long)second_frames, ms);
        } else
        snprintf(title, sizeof title, "%s - frame %lld - %lld fps", g_title, (long long)n, (long long)second_frames);
        SetWindowTextA(g_hwnd, title);
        second_frames = 0;
        second_start = now;
    }
}
/* New pacing, part 2: holds the caller until the refresh that shows the last queued frame. Returns 1 if it waited. */
int display_wait_refresh(void) {
    if (old_pacing() || !InterlockedExchange(&g_latch_pending, 0)) return 0;
    if (g_vsync_n >= g_latch) return 0;
    if (g_lockstep) {                                              /* SSBU_SPEED: see the comment at g_speed */
        static LONG64 due;                                         /* wall-clock time of the next refresh (QPC ticks) */
        LARGE_INTEGER a, now;
        QueryPerformanceCounter(&a);
        while (g_vsync_n < g_latch) {
            if (g_speed > 0) {
                LONGLONG step = (LONGLONG)((double)g_freq.QuadPart / ((double)hfr_rate() * g_speed));
                if (!due) due = a.QuadPart;
                due += step;
                QueryPerformanceCounter(&now);
                if (now.QuadPart - due > 4 * step) due = now.QuadPart;      /* far behind (a load): do not run to catch up */
                while (now.QuadPart < due) {
                    LONGLONG left = due - now.QuadPart;
                    if (left * 1000 > 2 * g_freq.QuadPart) Sleep(1); else YieldProcessor();
                    QueryPerformanceCounter(&now);
                }
            }
            uint64_t ca = g_wnd_count_addr, c0 = ca ? rd64(ca) : 0;
            AcquireSRWLockExclusive(&g_vsync_lock);
            g_vsync_n++;
            ReleaseSRWLockExclusive(&g_vsync_lock);
            if (g_vsync_event) { os_signal_event(g_vsync_event); InterlockedIncrement64(&g_vsync_signals); }
            if (ca) {                                              /* wait until the game's VSync Count Thread has counted it */
                LARGE_INTEGER t0, t1;
                QueryPerformanceCounter(&t0);
                while (rd64(ca) == c0) {
                    QueryPerformanceCounter(&t1);
                    if ((t1.QuadPart - t0.QuadPart) * 1000 > 100 * g_freq.QuadPart) { printf("display: speed mode: the game did not count a refresh within 100 ms%c", 10); break; }
                    Sleep(0);
                }
            }
        }
        return 1;
    }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    hitch_pacing(1);
    AcquireSRWLockExclusive(&g_vsync_lock);
    while (g_vsync_n < g_latch) SleepConditionVariableSRW(&g_vsync_cv, &g_vsync_lock, 100, 0);
    ReleaseSRWLockExclusive(&g_vsync_lock);
    hitch_pacing(0);
    QueryPerformanceCounter(&b);
    nvn_pf_pace(b.QuadPart - a.QuadPart);
    return 1;
}
/* Before a present: a frame queued earlier that nobody waited for (a caller that does not wait on the window sync). */
void display_before_present(void) {
    static int said;
    if (old_pacing() || !g_latch_pending) return;
    if (!said) { said = 1; printf("display: a present without a wait on the window texture's sync: held at the next present instead%c", 10); }
    display_wait_refresh();
}
void display_pace(void) {
    static LARGE_INTEGER next, second_start;
    static LONG64 second_frames;
    LARGE_INTEGER now;
    if (!old_pacing()) { frame_queued(); return; }
    QueryPerformanceCounter(&now);
    if (!next.QuadPart) { next = now; second_start = now; }
    LONG64 n = InterlockedIncrement64(&g_frames);
    second_frames++;
    clock_stat(n, now);
    next.QuadPart += g_freq.QuadPart * g_interval / hfr_rate();
    /* P3: wait until `next` exactly. Sleep(ms) cut the wait to whole milliseconds and, at the default 15.6 ms timer period,
     * could overshoot by a frame; a frame that came late was then followed by frames shown early (up to 100 ms of debt).
     * Now: a high-resolution waitable timer up to 0.5 ms before the deadline, then a spin; a late frame may be made up for
     * by at most one refresh interval, as a display that missed a refresh shows the next frame at the next one. */
    LONGLONG left = next.QuadPart - now.QuadPart, frame_ticks = g_freq.QuadPart * (g_interval ? g_interval : 1) / hfr_rate();
    if (!g_interval) next = now;
    else if (left > 0) {
        static HANDLE timer;
        LARGE_INTEGER after, due;
        hitch_pacing(1);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        due.QuadPart = -((left - g_freq.QuadPart / 2000) * 10000000 / g_freq.QuadPart);          /* relative, 100 ns units */
        if (due.QuadPart < 0) { if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100); else Sleep((DWORD)(left * 1000 / g_freq.QuadPart)); }
        for (QueryPerformanceCounter(&after); after.QuadPart < next.QuadPart; QueryPerformanceCounter(&after)) YieldProcessor();
        hitch_pacing(0);
        nvn_pf_pace(after.QuadPart - now.QuadPart);
    } else if (-left > frame_ticks) next.QuadPart = now.QuadPart - frame_ticks;
    if ((now.QuadPart - second_start.QuadPart) >= g_freq.QuadPart) {
        char title[160];
        if (gfx_options()->overlay && second_frames) printf("frame-time: %.2f ms per frame over the last second (%lld frames)\n", (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames, (long long)second_frames);
        if (gfx_options()->overlay && second_frames) snprintf(title, sizeof title, "%s - frame %lld - %lld fps - %.2f ms per frame", g_title, (long long)n, (long long)second_frames, (double)(now.QuadPart - second_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart / (double)second_frames);
        else
        snprintf(title, sizeof title, "%s - frame %lld - %lld fps", g_title, (long long)n, (long long)second_frames);
        SetWindowTextA(g_hwnd, title);
        second_frames = 0;
        second_start = now;
    }
}

void display_report(FILE *out) {
    fprintf(out, "\ndisplay: %lld frames presented\n", (long long)g_frames);
    fprintf(out, "display: %s; %lld vsync signals, %lld refreshes slept through by the vsync thread\n", old_pacing() ? "old pacing (SSBU_OLD_PACING=1: two clocks)" : "one refresh clock", (long long)g_vsync_signals, (long long)g_vsync_late);
    { void hfr_report(FILE *out); hfr_report(out); }
    nvn_gl_report(out);
}

/* ---- nn::vi: one display, one layer; the native window is an opaque token for NVN */
static uint8_t g_display_token[16], g_layer_token[16], g_native_window_token[16];
static void vi_OpenDefaultDisplay(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_display_token); RET(0); }
static void vi_CreateLayer(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_layer_token); RET(0); }
static void vi_GetNativeWindow(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_native_window_token); RET(0); }
static void vi_GetDisplayVsyncEvent(Ctx *c) {         /* (SystemEventType *out, Display *) */
    os_init_event(A0, 1);
    g_vsync_event = A0;
    RET(0);
}
static void nothing(Ctx *c) { (void)c; }

static const ImportDef vi_imports[] = {
    {"_ZN2nn2vi10InitializeEv", nothing}, {"_ZN2nn2vi8FinalizeEv", nothing},
    {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_OpenDefaultDisplay}, {"_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", nothing},
    {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_CreateLayer}, {"_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", nothing},
    {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_GetNativeWindow},
    {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_GetDisplayVsyncEvent},
};

void register_display_imports(void) {
    register_imports(vi_imports, (int)(sizeof vi_imports / sizeof vi_imports[0]));
}

/* Exercise the real focus watcher, notification queue and nn::os events.
 * Only foreground-window queries are simulated; no desktop focus is changed. */
#include <windows.h>
#include <assert.h>
#define ret0 os_test_ret0
#include "../runtime/os_threads.c"
#undef ret0

static volatile LONG test_foreground, test_minimised, test_offscreen, test_noactivate;
static volatile LONG test_visible = 1;
static HWND test_GetForegroundWindow(void) {
    return InterlockedCompareExchange(&test_foreground, 0, 0) ? (HWND)(uintptr_t)1 : NULL;
}
static DWORD test_GetWindowThreadProcessId(HWND w, LPDWORD pid) {
    if (pid) *pid = w ? GetCurrentProcessId() : 0;
    return w ? 1 : 0;
}
static BOOL test_IsIconic(HWND w) {
    (void)w; return InterlockedCompareExchange(&test_minimised, 0, 0) != 0;
}
static BOOL test_IsWindowVisible(HWND w) {
    (void)w; return InterlockedCompareExchange(&test_visible, 0, 0) != 0;
}
static LONG_PTR test_GetWindowLongPtrA(HWND w, int index) {
    (void)w; assert(index == GWL_EXSTYLE);
    return InterlockedCompareExchange(&test_noactivate, 0, 0) ? WS_EX_NOACTIVATE : 0;
}
static HMONITOR test_MonitorFromWindow(HWND w, DWORD flags) {
    (void)w; assert(flags == MONITOR_DEFAULTTONULL);
    return InterlockedCompareExchange(&test_offscreen, 0, 0) ? NULL : (HMONITOR)(uintptr_t)1;
}
#define IsWindowVisible test_IsWindowVisible
#define GetWindowLongPtrA test_GetWindowLongPtrA
#define MonitorFromWindow test_MonitorFromWindow
#define GetForegroundWindow test_GetForegroundWindow
#define GetWindowThreadProcessId test_GetWindowThreadProcessId
#define IsIconic test_IsIconic
#include "../runtime/nnmisc.c"
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
#undef IsIconic
#undef IsWindowVisible
#undef GetWindowLongPtrA
#undef MonitorFromWindow

static uint64_t notification_event;
static void mode(int value) {
    Ctx c = {0}; c.x[0] = value; oe_SetFocusHandlingMode(&c);
}
static void drain(void) {
    uint32_t message;
    Ctx c = {0}; c.x[0] = (uint64_t)(uintptr_t)&message;
    for (;;) { oe_TryPopNotificationMessage(&c); if (!c.x[0]) break; c.x[0] = (uint64_t)(uintptr_t)&message; }
    assert(!ev_wait_ms(obj(notification_event), 0));
}
static int await_notification(int state, DWORD timeout) {
    /* No oe import/poll until the host event wakes us, just as the paused game does. */
    if (!ev_wait_ms(obj(notification_event), timeout)) return 0;
    EnterCriticalSection(&g_oe_lock);
    int ok = g_oe_focus_state == state && g_oe_count == 1 && g_oe_queue[0] == OE_MSG_FOCUS_STATE_CHANGED;
    LeaveCriticalSection(&g_oe_lock);
    assert(ok);
    assert(ev_wait_ms(obj(notification_event), 0)); /* manual-clear until queue drained */
    drain(); return 1;
}
static void change_focus(int foreground, int minimised) {
    InterlockedExchange(&test_minimised, minimised);
    InterlockedExchange(&test_foreground, foreground);
}
static void require_transition(int foreground, int minimised, int state) {
    change_focus(foreground, minimised);
    assert(await_notification(state, 2000));
}
static void wait_frozen(int expected) {
    ULONGLONG deadline = GetTickCount64() + 2000;
    while (!!InterlockedCompareExchange(&g_guest_frozen, 0, 0) != expected && GetTickCount64() < deadline) Sleep(5);
    assert(!!InterlockedCompareExchange(&g_guest_frozen, 0, 0) == expected);
}
static int ignored_startup(const char *name) {
    /* The watcher alone sees a transient foreground window, then a background one.
     * Also query from the guest during the pulse: both producers must use the same rule. */
    Ctx c = {0};
    change_focus(1, test_minimised); Sleep(100);
    oe_GetCurrentFocusState(&c);
    change_focus(0, 0); Sleep(100);
    EnterCriticalSection(&g_oe_lock);
    int ok = !g_oe_was_foreground && !g_guest_frozen && g_oe_focus_state == 1 && !g_oe_count;
    LeaveCriticalSection(&g_oe_lock);
    if (!ok) fprintf(stderr, "FAIL %s startup incorrectly armed focus tracking\n", name);
    return ok;
}
int main(int argc, char **argv) {
    int disabled = argc > 1 && !strcmp(argv[1], "--disabled");
    _putenv("SSBU_FREEZE_TEST=");
    _putenv(disabled ? "SSBU_FOCUS_NOTIFY=0" : "SSBU_FOCUS_NOTIFY=1");
    InitializeCriticalSection(&g_oe_lock);
    Ctx c = {0};
    /* Asking for the event must also start the watcher; no other oe call here. */
    oe_GetNotificationMessageEvent(&c); notification_event = c.x[0];
    if (argc > 1 && !strcmp(argv[1], "--guest-bootstrap")) mode(2);
    if (disabled) {
        Sleep(100); mode(2); drain(); change_focus(0, 1); Sleep(100);
        assert(!ev_wait_ms(obj(notification_event), 0) && !g_guest_frozen && g_oe_focus_state == 1);
        puts("PASS focus tracking disabled: no focus notification or suspension"); return 0;
    }
    change_focus(0, 0);
    InterlockedExchange(&test_offscreen, 1);
    if (!ignored_startup("off-screen")) return 1;
    InterlockedExchange(&test_offscreen, 0);
    InterlockedExchange(&test_noactivate, 1);
    if (!ignored_startup("non-activating")) return 1;
    InterlockedExchange(&test_noactivate, 0);
    InterlockedExchange(&test_visible, 0);
    if (!ignored_startup("hidden")) return 1;
    InterlockedExchange(&test_visible, 1);
    InterlockedExchange(&test_minimised, 1);
    if (!ignored_startup("minimized")) return 1;
    puts("PASS transient off-screen, non-activating, hidden and minimized startup never arms focus tracking");
    change_focus(1, 0);
    ULONGLONG deadline = GetTickCount64() + 2000;
    int seen = 0;
    while (!seen && GetTickCount64() < deadline) {
        EnterCriticalSection(&g_oe_lock); seen = g_oe_was_foreground; LeaveCriticalSection(&g_oe_lock); Sleep(5);
    }
    if (!seen) { puts("FAIL event-only subscriber never starts focus observation"); return 1; }
    mode(2); drain();
    for (int i = 0; i < 12; i++) {
        /* Guest observes focus loss, drains the queue, then blocks awaiting resume. */
        change_focus(0, 0); Sleep(50); oe_GetCurrentFocusState(&c); assert(c.x[0] == 2); drain();
        change_focus(1, 0);
        if (!await_notification(1, 2000)) { puts("FAIL returning focus did not wake the blocked guest"); return 1; }
    }
    puts("PASS 12 focus returns wake a blocked guest without guest polling");
    Sleep(100); assert(!ev_wait_ms(obj(notification_event), 0));
    puts("PASS steady focus does not repost notifications; draining clears the event");
    mode(1); drain();
    require_transition(0, 0, 2); require_transition(1, 0, 1);
    require_transition(0, 1, 3); wait_frozen(0); require_transition(1, 0, 1);
    puts("PASS NoSuspend: obscured and minimised notifications, no suspension");
    mode(2); drain(); change_focus(0, 1); wait_frozen(1);
    change_focus(1, 0); wait_frozen(0); drain();
    puts("PASS SuspendHomeSleepNotify: minimise suspends and returning resumes");
    mode(3); drain(); change_focus(0, 0); wait_frozen(1);
    assert(!ev_wait_ms(obj(notification_event), 0)); change_focus(1, 0); wait_frozen(0);
    mode(0); drain(); change_focus(0, 0); Sleep(100);
    assert(!ev_wait_ms(obj(notification_event), 0)); change_focus(1, 0); Sleep(100);
    change_focus(0, 1); wait_frozen(1); assert(!ev_wait_ms(obj(notification_event), 0));
    change_focus(1, 0); wait_frozen(0);
    mode(2); drain(); Sleep(100); assert(!ev_wait_ms(obj(notification_event), 0));
    puts("PASS modes without focus notifications stay quiet and can return to notify mode");
    return 0;
}

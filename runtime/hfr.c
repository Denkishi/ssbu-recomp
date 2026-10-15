/* The 120 Hz mode: graphics.ini frame_rate = 120 (env SSBU_FRAME_RATE=120 wins).
 *
 * The engine already draws between its steps: in a global slow motion (the final hit of a match, a fighter's "whole"
 * slow) it runs the battle with a divisor N: logic (status scripts, hit boxes, every frame counter) on one update in N,
 * and the N - 1 updates between drawn by the engine itself at fractional motion frames. This mode uses exactly that:
 * during a battle the refresh clock runs at 120 Hz (display.c) and the divisor is doubled, so logic still runs 60 times
 * per second and every second presented frame is the engine's own picture between two steps. Nothing is blended or
 * guessed, and a slow motion of the game (N = 3) keeps its real length (N = 6 at 120 Hz).
 *
 * Main 13.0.4 [SEEN in the binary unless marked]:
 *   0x710135ca00  the battle's per-update step (vtable 0x71050603f0, slot 6). It keeps the slow state at 0x71052c4184
 *                 (u32 cycle counter, u8 N of the cycle at +4, u8 N asked for at +5, u8 update within the cycle at +6,
 *                 f32 1 / N at +0xc) and then updates every battle object (0x710137b3f0) with a per-update record
 *                 built from it (0x71003ac660: logic on update 0 of a cycle, the others through the motion module's
 *                 sub-frame slots).
 *   0x71014a2cd0  the largest N of the slow request table *(0x71052c2610) + 0x20.. (enabled, N byte pairs; at least 1).
 *                 The step calls it when the table's dirty flag (+0x40) is set, stores the clamped result as the N
 *                 asked for, and takes it as the cycle's N at the next cycle boundary (at once when N is 1).
 * The hook on 0x71014a2cd0 doubles its result in a battle; the hook on the step marks the battle as running and sets the
 * dirty flag whenever the N asked for is 1 (the battle's set-up writes 1 there), so the doubled N is taken even when no
 * request changes. Every other value (the cycle,
 * the rate, the per-update records) is the game's own work. Not done: writing N after the step returns (the objects have
 * been updated inside it by then: a cycle-end update ran with N = 1 and the next one reset each motion to a saved frame
 * that was never written: frozen fighters, measured 2026-10-15).
 * The step runs only in a battle: the refresh clock is 120 Hz while it has run in the last quarter second, else 60 Hz, so
 * the menus keep their console speed.
 *
 * At frame_rate = 60 nothing here runs: no hooks, a 60 Hz clock. */
#include <windows.h>
#include <stdio.h>
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

#define FN_SLOW_STEP 0x710135ca00ULL
#define FN_SLOW_MAX 0x71014a2cd0ULL
#define SLOW_TABLE_PTR 0x71052c2610ULL                 /* -> the slow request table; + 0x40 = its dirty flag */
#define SLOW 0x71052c4184ULL                           /* the slow state: + 4 N of the cycle, + 6 update within the cycle */
#define RNG_ROOT 0x71052c25b0ULL                       /* -> -> -> the 9 script random generators of app::sv_math::rand, 20 bytes each (fighter_sdk\fighter_host.c) */
int hook_install(uint64_t addr, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);      /* fighter_sdk\hook_host.c (in nnro.c) */

static int g_setting;                                  /* 60 or 120 once hfr_init has run */
static ImportFn g_orig_step, g_orig_max;
static volatile LONG64 g_step_qpc;                     /* QueryPerformanceCounter at the last battle step */
static volatile LONG g_doubled_now;                    /* the current cycle runs with the doubled divisor */
static LONG64 g_qpf, g_steps, g_battles, g_doubled, g_added;

int hfr_setting(void) { return g_setting == 120 ? 120 : 60; }
static int battle_running(LONG64 now) { return g_step_qpc && now - g_step_qpc < g_qpf / 4; }
/* Refreshes per second of the clock now: 120 in a battle with frame_rate = 120, else 60. Any thread. */
int hfr_rate(void) {
    if (g_setting != 120) return 60;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return battle_running(now.QuadPart) && g_doubled_now ? 120 : 60;      /* 120 only once the cycles are doubled: before, every update is a console update */
}

static void slow_step_hook(Ctx *c, void *user) {
    (void)user;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!battle_running(now.QuadPart)) g_battles++;
    if (rd8(SLOW + 5) < 2) {                           /* N asked for is 1: never a doubled value, so the game set it (a battle's set-up does, after
                                                        * its first updates: measured 2026-10-15); have the step ask the request maximum again */
        uint64_t t = rd64(SLOW_TABLE_PTR);
        if (t) wr8(t + 0x40, 1);
    }
    InterlockedExchange64(&g_step_qpc, now.QuadPart);
    g_steps++;
    /* The updates this mode adds are the odd ones of a cycle of 2N (the even ones are the console's updates of a cycle of
     * N). Effects draw from the script random generators on every update, so the added ones would move the generators
     * that the game logic also draws from, and the fight would take another course than at 60 Hz. An added update gets
     * the generators back as they were before it: the logic sees the same random numbers as at 60 Hz. */
    unsigned n = rd8(SLOW + 4), next = rd8(SLOW + 6) + 1u;
    uint64_t rb = rd64(RNG_ROOT);
    if (rb) rb = rd64(rb);
    if (rb) rb = rd64(rb);
    int added = n > 1 && next < n && (next & 1) && rb;
    uint8_t keep[180];
    if (added) memcpy(keep, (void *)(uintptr_t)rb, sizeof keep);
    g_orig_step(c);
    InterlockedExchange(&g_doubled_now, rd8(SLOW + 4) >= 2);
    if (added) { memcpy((void *)(uintptr_t)rb, keep, sizeof keep); g_added++; }
}
static void slow_max_hook(Ctx *c, void *user) {
    (void)user;
    g_orig_max(c);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!battle_running(now.QuadPart)) return;
    unsigned n = (unsigned)(c->x[0] & 0xff);
    if (!n) n = 1;
    c->x[0] = n * 2 > 100 ? 100 : n * 2;              /* the step clamps to 1..100 as well */
    g_doubled++;
}

/* boot.c, after the static constructors, before nnMain. */
void hfr_init(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    g_setting = 60;
    if (gfx_options()->frame_rate != 120) return;
    int a = hook_install(FN_SLOW_STEP, slow_step_hook, NULL, &g_orig_step), b = a ? hook_install(FN_SLOW_MAX, slow_max_hook, NULL, &g_orig_max) : 0;
    if (!a || !b) { printf("hfr: frame_rate = 120 asked for, but a hook failed (step %d, request maximum %d): 60 is used\n", a, b); return; }
    g_setting = 120;
    printf("hfr: frame_rate = 120: battles at 120 Hz (the engine's slow-motion divisor doubled, logic 60 steps per second), menus at 60 Hz\n");
}

void hfr_report(FILE *out) {
    if (g_setting == 120) fprintf(out, "hfr: %lld battles, %lld battle updates (%lld of them added between two of the console's), slow divisor doubled %lld times\n", (long long)g_battles, (long long)g_steps, (long long)g_added, (long long)g_doubled);
}

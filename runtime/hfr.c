/* The 120 Hz mode: graphics.ini frame_rate = 120 (env SSBU_FRAME_RATE=120 wins).
 *
 * The engine already draws between its steps: in a global slow motion (training mode's 1/2 speed, the final hit of a
 * match) it runs the battle with a divisor N, logic (status scripts, hit boxes, every frame counter) on one update in N,
 * and the N - 1 updates between are drawn by the engine itself at fractional motion frames. This mode uses exactly that:
 * during a battle the refresh clock runs at 120 Hz (display.c) and the divisor is doubled, so logic still runs 60 times
 * per second and every second presented frame is the engine's own picture between two steps. Nothing is blended or
 * guessed, the game logic is the same, and a slow motion of the game (N = 3) keeps its real length (N = 6 at 120 Hz).
 *
 * The slow state, [SEEN in main.elf 13.0.4] the battle's per-update step 0x710135ca00 (vtable 0x71050603f0, slot 6):
 *   0x71052c4184 u32  cycle counter: +1 when a cycle of N updates begins
 *   0x71052c4188 u8   N of the current cycle; 0x71052c4189 u8 N asked for (the largest slow request, 1..100)
 *   0x71052c418a u8   update within the cycle (0 = the logic update)
 *   0x71052c4190 f32  1 / N; also stored to 0x710523c138
 * At the start of a cycle the step copies the asked N into the current one. The hook below doubles it there (and the rate
 * 1 / N), so a cycle lasts 2N updates. 0x71003ac660 (and its copies) builds each battle object's per-update record from
 * the current N: logic on update 0, the in-between updates through the motion module's sub-frame path [SEEN; the
 * sub-frame reading of those paths is INFERRED from training mode, where it is visible].
 * The step runs only in a battle: the refresh clock is 120 Hz while it has run in the last quarter second, else 60 Hz, so
 * the menus keep their console speed.
 *
 * At frame_rate = 60 nothing here runs: no hook, a 60 Hz clock. */
#include <windows.h>
#include <stdio.h>
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

#define FN_SLOW_STEP 0x710135ca00ULL
#define SLOW 0x71052c4184ULL                   /* the slow state above */
#define SLOW_RATE_COPY 0x710523c138ULL
int hook_install(uint64_t addr, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);      /* fighter_sdk\hook_host.c (in nnro.c) */

static int g_setting;                          /* 60 or 120 once hfr_init has run */
static ImportFn g_orig_step;
static volatile LONG64 g_step_qpc;             /* QueryPerformanceCounter at the last battle step */
static LONG64 g_qpf, g_cycles, g_steps;

int hfr_setting(void) { return g_setting == 120 ? 120 : 60; }
/* Refreshes per second of the clock now: 120 in a battle with frame_rate = 120, else 60. Any thread. */
int hfr_rate(void) {
    if (g_setting != 120 || !g_step_qpc) return 60;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart - g_step_qpc < g_qpf / 4 ? 120 : 60;
}

static void slow_step_hook(Ctx *c, void *user) {
    (void)user;
    uint32_t cycle = rd32(SLOW);
    g_orig_step(c);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    InterlockedExchange64(&g_step_qpc, now.QuadPart);
    g_steps++;
    if (rd32(SLOW) != cycle) {                 /* a cycle began: N = the asked divisor; make it 2N */
        unsigned n = rd8(SLOW + 4) ? rd8(SLOW + 4) : 1, m = n * 2 > 200 ? 200 : n * 2;
        float r = 1.0f / (float)m;
        uint32_t bits;
        memcpy(&bits, &r, 4);
        wr8(SLOW + 4, (uint8_t)m);
        wr32(SLOW + 0xc, bits);
        wr32(SLOW_RATE_COPY, bits);
        g_cycles++;
    }
}

/* boot.c, after the static constructors, before nnMain. */
void hfr_init(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    g_setting = 60;
    if (gfx_options()->frame_rate != 120) return;
    int id = hook_install(FN_SLOW_STEP, slow_step_hook, NULL, &g_orig_step);
    if (!id) { printf("hfr: frame_rate = 120 asked for, but the hook on the battle's slow step %llx failed: 60 is used\n", (unsigned long long)FN_SLOW_STEP); return; }
    g_setting = 120;
    printf("hfr: frame_rate = 120: battles at 120 Hz (the engine's slow-motion divisor doubled, logic 60 steps per second), menus at 60 Hz\n");
}

void hfr_report(FILE *out) {
    if (g_setting == 120) fprintf(out, "hfr: %lld battle updates, %lld slow cycles doubled\n", (long long)g_steps, (long long)g_cycles);
}

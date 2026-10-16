/* The 120 Hz mode: graphics.ini frame_rate = 120 (env SSBU_FRAME_RATE=120 wins). At frame_rate = 60 nothing here runs:
 * no hook is installed and the refresh clock stays at 60 Hz.
 *
 * The engine already draws between its steps: in a global slow motion (the final hit of a match, a fighter's "whole"
 * slow) it runs the battle with a divisor N: logic (status scripts, hit boxes, every frame counter) on one update in N,
 * and the N - 1 updates between drawn by the engine itself at fractional motion frames. This mode uses exactly that:
 * during a battle the refresh clock runs at 120 Hz (display.c) and the divisor the battle step asks for is doubled, so
 * logic still runs 60 times per second and every second presented frame is the engine's own picture between two steps.
 * Nothing is blended or guessed, and a slow motion of the game (N = 3) keeps its real length (N = 6 at 120 Hz).
 *
 * Rules kept here (review of PR #9):
 *   - the game's memory is changed only at fixed points of the game's own work: around the battle step, in the request
 *     maximum when the battle step calls it, after the frame end, and in the results of the two readers of the slow
 *     request (below). Every decision is taken from
 *     the game's memory and from counts of updates and frame ends, NEVER from the host's clock: the host's clock only
 *     decides when a refresh is signalled (display.c), as at 60 Hz. So a run with SSBU_DETERMINISTIC (no catch-up
 *     updates) is to be the same whatever the host's speed and load: tests\hfr_det.py is the check.
 *   - the update rate is replaced only where it is the game's own 60; any other rate the game sets is left alone, and
 *     the mode then stands aside (no doubling, 60 Hz clock) until the game is back at 60.
 *   - the divisor is doubled only for the battle step's own call of the request maximum (its return address and the
 *     thread inside the step are checked); the game's other readers of the slow request see the console's values.
 *   - the match timer component is used only after its type and values were checked (TIMER below).
 *   - every write to the game's memory is marked for the simulation state save / restore (r1_wrote, r1_state.c).
 *
 * Main 13.0.4 [SEEN in the binary unless marked]:
 *   0x710135ca00  the battle's per-update step (vtable 0x71050603f0 slot 6, and the same slot of about 50 other vtables
 *                 [INFERRED: the battle's sequence classes]). It keeps the slow state at 0x71052c4184 (u32 cycle counter, u8 N of
 *                 the cycle at +4, u8 N asked for at +5, u8 update within the cycle at +6, f32 1 / N at +0xc) and then
 *                 updates every battle object (0x710137b3f0) with a per-update record built from it (0x71003ac660:
 *                 logic on update 0 of a cycle, the others through the motion module's sub-frame slots). When the
 *                 byte at 0x71052b82e8 is set the cycle is not advanced (0x710135ca30 / 0x710135cc10); the objects are
 *                 still updated, told so. MEASURED 2026-10-16 (SSBU_HFR_TRACE, pause in a 120 Hz fight): the game's
 *                 pause is this: the step goes on running every update, the cycle and the match timer stand still.
 *   0x71014a2cd0  the largest N of the slow request table *(0x71052c2610) + 0x20.. (enabled, N byte pairs; at least 1).
 *                 The step calls it (BL at 0x710135cadc) when the table's dirty flag (+0x40) is set, stores the clamped
 *                 result (1..100) as the N asked for and clears the flag; the asked N becomes the cycle's N at the next
 *                 cycle boundary (at once when the cycle's N is 1). Its two other callers are the readers of the slow
 *                 request: app::lua_bind::BattleObjectSlow__rate_request_impl (0x7101fca5c0, BL at 0x7101fca5d8) and
 *                 the Lua binding "rate_request" (0x7101fca6d0, registered at 0x710213802c and 0x7102139240): both
 *                 return 1 / max when the table is dirty, else 1 / (the N asked for).
 * The hook on 0x71014a2cd0 doubles its result only when it returns to 0x710135cae0 on the thread that is inside the
 * step hook. The step hook sets the dirty flag when the N asked for cannot be ours (a doubled N is always even: the
 * battle's set-up writes 1 there), so the doubled N is taken even when no request changes; with the mode standing aside
 * it sets it when the asked N may be ours, so the console's N comes back. The two readers would see the doubled asked N
 * while the table is clean: their result is put back to 1 / (asked N / 2) then (the console's value). Every other value
 * (the cycle, the per-update records) is the game's own work. Not done: writing N after the step returns (the objects
 * have been updated inside it by then: frozen fighters, measured 2026-10-15). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

#define FN_SLOW_STEP 0x710135ca00ULL
#define FN_SLOW_MAX 0x71014a2cd0ULL
#define STEP_MAX_RET 0x710135cae0ULL                   /* return address of the step's call of FN_SLOW_MAX (BL at 0x710135cadc) */
#define FN_RATE_REQ 0x7101fca5c0ULL                    /* app::lua_bind::BattleObjectSlow__rate_request_impl: s0 = 1 / N */
#define FN_RATE_REQ_LUA 0x7101fca6d0ULL                /* Lua "rate_request": pushes 1 / N (f32 at top, type 3 at top + 8; top = L + 0x10), returns 1 */
#define SLOW_TABLE_PTR 0x71052c2610ULL                 /* -> the slow request table; + 0x40 = its dirty flag */
#define SLOW 0x71052c4184ULL                           /* the slow state (see above) */
#define SLOW_STOPPED 0x71052b82e8ULL                   /* u32: not 0 = the step does not advance the cycle (the game's pause: measured 2026-10-16) */
/* The update rate, 0x710523c004 (app::sv_information::get_delta_frame returns it; about 45 readers compute 60 / rate
 * frames per update: the match's sequencing, the battle's set-up and entrance, ...). Those run on every update, outside
 * the slow divisor: at 120 updates per second they would run twice as fast. Writers [SEEN]: 60 at nine places, 120 at
 * 0x710155b680, 180 at 0x710155b870, a value from a table at 0x71004f5820, 0x71014b5770, 0x71014b5bf4 (the other speeds:
 * replays, training). While the cycles are doubled the game's own 60 is replaced by 119 (60 / 119 = 0.504 frames per
 * update, 0.84 % fast); any other value is left alone and the mode stands aside. Not 120: the code that tests
 * rate / 60 >= 2 (the engine's own 2x replay speed: Brave::get_special_lw_param_frame, shiokarazu get_slow_rate,
 * 0x71006b92c0) would take that branch. 119 is not a value the game writes, so 119 there is always ours; it is put back
 * to 60 when the cycles stop being doubled and when the battle ends.
 * The main loop [SEEN 0x7103749b38..0x7103749be0] counts updates per frame from the same value: acc (0x710593a454) -= rate,
 * then one update for every 60 that brings acc above 0: rate / 60 updates per frame. After every frame end (once per
 * iteration of the main loop, before its next count) acc gets 119 - 60 while the rate is ours, so the count stays one
 * update per frame. The updates for refreshes the game missed (its catch-up, 0x710593a44c) are added by the game apart
 * from this count and still are (SSBU_DETERMINISTIC clears them, fighter_sdk\fighter_host.c). */
#define UPDATE_RATE 0x710523c004ULL
#define UPDATE_ACC 0x710593a454ULL
#define HFR_RATE 119
#define FN_FRAME_END 0x710386fc30ULL                   /* the graphics frame end (acquires the window texture); called only from 0x7103747b78, once per main loop iteration */
#define CATCH_UP 0x710593a44cULL                       /* the game's count of catch-up updates in this frame (only logged) */
/* TIMER. The match timer: the battle rule component, vtable 0x710506fbf0, keeps the remaining frames at +0x3d4 (slot 0x128 =
 * 0x710157c950 returns it; sv_information::get_remaining_time_as_frame reads it there). Its init, slot 0x120 = 0x710157c8f0
 * (the only vtable entry that points there), stores +0x3d4 = 0 and, when the rule's time (*(this + 0x10) + 0x48) is not
 * 0 and +0x3d3 is 0, +0x3d2 = 1 and +0x3d4 = the rule's time. MEASURED (SSBU_HFR_LOG, 2026-10-15): the remaining frames
 * drop by 1 inside every battle step, the game's catch-up updates included; the count does not read the update rate.
 * An added update gives its frame back. The component is taken from its init only when the object has this vtable and
 * the init took the timed branch; before every write: the vtable again (read without a fault: the object may be gone),
 * +0x3d2 = 1, the value before the step in 1 .. the rule's time and the value after it exactly one less. It is replaced
 * at the next init (kept over the end of a battle: if the step does not run during a pause, the battle looks over to the
 * code below, and the timer must still be found after it; a freed object fails the vtable read or no longer counts). */
#define RULE_VTABLE 0x710506fbf0ULL
#define FN_TIMER_INIT 0x710157c8f0ULL
#define RNG_ROOT 0x71052c25b0ULL                      /* -> -> -> the 9 script random generators of app::sv_math::rand, 20 bytes each (fighter_sdk\fighter_host.c) */
#define RNG_BYTES 180
#define LIVE_FRAMES 3                                  /* frame ends without a battle step after which the battle is over (one step per frame in a battle) */
int hook_install(uint64_t addr, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);      /* fighter_sdk\hook_host.c (in nnro.c) */
void r1_wrote(uint64_t a, uint64_t n);                 /* r1_state.c: host writes to the game's memory are marked (nothing without SSBU_R1 marks) */

static int g_setting;                                  /* 60 or 120 once hfr_init has run */
static ImportFn g_orig_step, g_orig_max, g_orig_timer_init, g_orig_frame_end, g_orig_rate_req, g_orig_rate_req_lua;
static volatile DWORD g_step_tid;                      /* the thread inside the step hook (0: none) */
static int g_step_double;                              /* the step now running may have its divisor doubled */
static volatile LONG g_live;                           /* a battle step ran in the last LIVE_FRAMES frame ends */
static volatile LONG g_doubled_now;                    /* the current cycle runs with the doubled divisor */
static int g_since_step;                               /* frame ends since the last battle step */
static uint64_t g_timer_comp;                          /* the rule component, from its init (checked), 0 = none */
static int32_t g_timer_full;                           /* the rule's time at that init */
static FILE *g_trace;                                  /* SSBU_HFR_TRACE (test only) */
static LONG64 g_steps, g_battles, g_doubled, g_max_other, g_added, g_timer, g_timer_bad, g_comp_bad, g_reasks, g_rate_set, g_rate_back, g_rate_other, g_acc_fix, g_req_fix;

static void put8(uint64_t a, uint8_t v) { r1_wrote(a, 1); wr8(a, v); }
static void put32(uint64_t a, uint32_t v) { r1_wrote(a, 4); wr32(a, v); }
static int read_ok(uint64_t a, void *buf, size_t n) {           /* a read that cannot fault (an object that may have been freed) */
    SIZE_T got = 0;
    return a >= 0x10000 && ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)a, buf, n, &got) && got == n;
}
static uint64_t rng_base(void) {
    uint64_t p = rd64(RNG_ROOT);
    if (p) p = rd64(p);
    if (p) p = rd64(p);
    return p;
}
static uint32_t fnv32(const uint8_t *p, size_t n) { uint32_t h = 2166136261u; while (n--) { h ^= *p++; h *= 16777619u; } return h; }

int hfr_setting(void) { return g_setting == 120 ? 120 : 60; }
/* Refreshes per second of the clock now: 120 while a battle runs with doubled cycles in the 120 Hz mode, else 60. Any
 * thread. Both flags are set by the game's thread from the game's state and counted frame ends (no clock). */
int hfr_rate(void) { return g_setting == 120 && g_live && g_doubled_now ? 120 : 60; }
/* TEST ONLY (SSBU_SHOT_BATTLE lines): the update within the slow cycle as the render thread sees it, -1 at 60 Hz. */
int hfr_shot_phase(void) { return g_setting == 120 ? (int)rd8(SLOW + 6) : -1; }
/* TEST ONLY (nnhid.c SSBU_SEQ without SSBU_DETERMINISTIC): 1 when the next update is one this mode adds, so that the input
 * sequence, which counts the console's updates, does not move for it. From the game's slow state, not from time. While
 * the cycle stands still (pause) every update counts (else a pause on an odd phase would hold the sequence for good). */
int hfr_seq_skip(void) { return hfr_rate() == 120 && !rd32(SLOW_STOPPED) && ((rd8(SLOW + 6) + 1u) & 1u); }

static void battle_over(void) {
    InterlockedExchange(&g_live, 0);
    InterlockedExchange(&g_doubled_now, 0);
    if (rd32(UPDATE_RATE) == HFR_RATE) { put32(UPDATE_RATE, 60); g_rate_back++; }      /* ours: the game's 60 back */
    if (g_trace) fprintf(g_trace, "battle over after %lld updates\n", (long long)g_steps);
}

static void slow_step_hook(Ctx *c, void *user) {
    (void)user;
    uint32_t rate = rd32(UPDATE_RATE);
    int mode = rate == 60 || rate == HFR_RATE;          /* the game's own rate (or ours in its place): double; any other: stand aside */
    unsigned asked = rd8(SLOW + 5);
    uint64_t t = rd64(SLOW_TABLE_PTR);
    if (t && !rd8(t + 0x40) && (mode ? !asked || (asked & 1) : asked >= 2 && !(asked & 1))) { put8(t + 0x40, 1); g_reasks++; }      /* the step asks the maximum again */
    if (rate != 60 && rate != HFR_RATE) g_rate_other++;
    g_since_step = 0;
    if (!g_live) { g_battles++; InterlockedExchange(&g_live, 1); if (g_trace) fprintf(g_trace, "battle %lld begins at update %lld\n", (long long)g_battles, (long long)g_steps); }
    g_steps++;
    /* The updates this mode adds are the odd ones of a cycle of 2N (the even ones are the console's updates of a cycle of
     * N). Effects draw from the script random generators on every update, so the added ones would move the generators
     * that the game logic also draws from. An added update gets the generators back as they were before it: the logic
     * sees the same random numbers as at 60 Hz. Whether the update was an added one is read from the slow state AFTER
     * the step (the cycle may not advance, e.g. while the battle is stopped), so the state before it is kept every time. */
    uint32_t cyc0 = rd32(SLOW);
    unsigned ph0 = rd8(SLOW + 6);
    uint64_t rb = rng_base();
    uint8_t keep[RNG_BYTES];
    if (rb) memcpy(keep, (void *)(uintptr_t)rb, sizeof keep);
    uint64_t tc = g_timer_comp, vt = 0;
    int32_t before = -1;
    if (tc) {
        if (read_ok(tc, &vt, 8) && vt == RULE_VTABLE) before = (int32_t)rd32(tc + 0x3d4);
        else { g_timer_comp = tc = 0; g_comp_bad++; }      /* the object is gone or is something else now */
    }
    g_step_double = mode;
    g_step_tid = GetCurrentThreadId();
    g_orig_step(c);
    g_step_tid = 0;
    unsigned n = rd8(SLOW + 4), ph = rd8(SLOW + 6);
    int advanced = rd32(SLOW) != cyc0 || ph != ph0;
    int doubled = mode && n >= 2 && !(n & 1);          /* in the mode every N the step takes is a doubled one (even); the set-up's 1 is not */
    int added = doubled && advanced && (ph & 1);
    InterlockedExchange(&g_doubled_now, doubled);
    uint32_t r2 = rd32(UPDATE_RATE);                   /* the game may have set it inside the step (the battle's set-up writes 60) */
    if (doubled && r2 == 60) { put32(UPDATE_RATE, HFR_RATE); g_rate_set++; }
    else if (!doubled && r2 == HFR_RATE) { put32(UPDATE_RATE, 60); g_rate_back++; }
    int32_t after = tc ? (int32_t)rd32(tc + 0x3d4) : -1;
    if (added) {
        if (rb) { r1_wrote(rb, sizeof keep); memcpy((void *)(uintptr_t)rb, keep, sizeof keep); }
        g_added++;
        if (tc && before > 0) {
            if (before <= g_timer_full && after == before - 1 && rd8(tc + 0x3d2) == 1) { put32(tc + 0x3d4, (uint32_t)before); g_timer++; after = before; }
            else if (after != before) g_timer_bad++;   /* changed, but not by the one frame of an update: left as it is */
        }
    }
    if (g_trace)       /* game state only (no times, no host counters but the update number): equal between runs that are equal */
        fprintf(g_trace, "u %lld c %u n %u a %u p %u add %d rate %u t %d rng %08x\n", (long long)g_steps, rd32(SLOW), n, rd8(SLOW + 5), ph, added, rd32(UPDATE_RATE), after,
                rb ? fnv32((const uint8_t *)(uintptr_t)rb, RNG_BYTES) : 0u);
    {   /* TEST ONLY, SSBU_HFR_LOG=1: the slow state and the match timer for 40 updates (from the 600th) */
        static int log = -1, shown;
        if (log < 0) log = getenv("SSBU_HFR_LOG") && atoi(getenv("SSBU_HFR_LOG")) != 0;
        if (log && tc && shown < 40 && g_steps > 600) { shown++; printf("hfr-log: update %lld N %u phase %u added %d rate %u: timer %d -> %d (catch-up %u)\n", (long long)g_steps, n, ph, added, rd32(UPDATE_RATE), before, after, rd32(CATCH_UP)); }
    }
}
static void slow_max_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t ret = c->x[30];
    g_orig_max(c);
    if (ret != STEP_MAX_RET || g_step_tid != GetCurrentThreadId() || !g_step_double) { g_max_other++; return; }      /* a reader of the request, or the mode stands aside */
    unsigned n = (unsigned)(c->x[0] & 0xff);
    if (!n) n = 1;                                     /* the step takes 0 as 1 */
    c->x[0] = n * 2 > 100 ? 100 : n * 2;              /* the step clamps to 1..100 as well */
    g_doubled++;
}
/* The readers of the slow request: with the table clean they return 1 / (asked N), which is doubled while the mode runs
 * (the rate is ours only then); the console's 1 / N is given instead, computed as the game does (1.0f / N). */
static int req_doubled(int clean, unsigned *n) { unsigned a = rd8(SLOW + 5); *n = a >> 1; return clean && rd32(UPDATE_RATE) == HFR_RATE && a >= 2 && !(a & 1); }
static void rate_req_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t t = rd64(SLOW_TABLE_PTR);
    int clean = t && !rd8(t + 0x40);
    unsigned n;
    g_orig_rate_req(c);
    if (req_doubled(clean, &n)) { c->v[0].f[0] = 1.0f / (float)n; g_req_fix++; }
}
static void rate_req_lua_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t L = c->x[0], t = rd64(SLOW_TABLE_PTR);
    int clean = t && !rd8(t + 0x40);
    unsigned n;
    g_orig_rate_req_lua(c);
    if ((uint32_t)c->x[0] != 1 || !L || !req_doubled(clean, &n)) return;
    uint64_t v = rd64(L + 0x10) - 0x10;               /* the value it pushed */
    if (rd32(v + 8) != 3) return;
    float f = 1.0f / (float)n;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    put32(v, bits);
    g_req_fix++;
}

static void timer_init_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t comp = c->x[0];
    g_orig_timer_init(c);
    uint64_t rule = comp ? rd64(comp + 0x10) : 0;
    int32_t full = rule ? (int32_t)rd32(rule + 0x48) : 0;
    if (comp && rd64(comp) == RULE_VTABLE && full > 0 && !rd8(comp + 0x3d3) && rd8(comp + 0x3d2) == 1 && (int32_t)rd32(comp + 0x3d4) == full) { g_timer_comp = comp; g_timer_full = full; }
    else { g_timer_comp = 0; if (comp && full > 0) g_comp_bad++; }      /* not timed, or not the layout above: the timer is left alone */
}

/* Once per main loop iteration, after the frame end, before the loop's next count of updates. */
static void frame_end_hook(Ctx *c, void *user) {
    (void)user;
    g_orig_frame_end(c);
    if (g_live && ++g_since_step > LIVE_FRAMES) battle_over();
    if (rd32(UPDATE_RATE) == HFR_RATE) { put32(UPDATE_ACC, rd32(UPDATE_ACC) + (HFR_RATE - 60)); g_acc_fix++; }      /* one update per frame while the rate is ours (see UPDATE_RATE) */
}

static void trace_close(void) { if (g_trace) { fclose(g_trace); g_trace = NULL; } }

/* boot.c, after the static constructors, before nnMain. */
void hfr_init(void) {
    g_setting = 60;
    if (gfx_options()->frame_rate != 120) return;
    int a = hook_install(FN_SLOW_STEP, slow_step_hook, NULL, &g_orig_step), b = a ? hook_install(FN_SLOW_MAX, slow_max_hook, NULL, &g_orig_max) : 0,
        f = b ? hook_install(FN_FRAME_END, frame_end_hook, NULL, &g_orig_frame_end) : 0;
    if (!a || !b || !f) { printf("hfr: frame_rate = 120 asked for, but a hook failed (step %d, request maximum %d, frame end %d): 60 is used\n", a, b, f); return; }
    int t = hook_install(FN_TIMER_INIT, timer_init_hook, NULL, &g_orig_timer_init), r = hook_install(FN_RATE_REQ, rate_req_hook, NULL, &g_orig_rate_req),
        l = hook_install(FN_RATE_REQ_LUA, rate_req_lua_hook, NULL, &g_orig_rate_req_lua);
    const char *tr = getenv("SSBU_HFR_TRACE");
    if (tr && tr[0] && (g_trace = fopen(tr, "w")) != NULL) atexit(trace_close);
    g_setting = 120;
    printf("hfr: frame_rate = 120: battles at 120 Hz (the battle step's slow divisor doubled, logic 60 steps per second), menus at 60 Hz; match timer %s, slow request readers %s / %s%s\n",
           t ? "kept" : "NOT kept (hook failed)", r ? "kept" : "NOT kept", l ? "kept" : "NOT kept", g_trace ? "; SSBU_HFR_TRACE on" : "");
}

void hfr_report(FILE *out) {
    if (g_setting != 120) return;
    fprintf(out, "hfr: %lld battles, %lld battle updates (%lld of them added between two of the console's); slow divisor doubled %lld times for the battle step, %lld calls of the request maximum left alone; asked again %lld times\n",
            (long long)g_battles, (long long)g_steps, (long long)g_added, (long long)g_doubled, (long long)g_max_other, (long long)g_reasks);
    fprintf(out, "hfr: update rate set to %d %lld times, given back %lld times, another rate of the game seen at %lld updates (the mode stood aside); one-update-per-frame corrections %lld; slow request readers given the console's value %lld times\n",
            HFR_RATE, (long long)g_rate_set, (long long)g_rate_back, (long long)g_rate_other, (long long)g_acc_fix, (long long)g_req_fix);
    fprintf(out, "hfr: match timer frames given back %lld; timer changes left alone %lld; rule components not used (type / values) %lld\n", (long long)g_timer, (long long)g_timer_bad, (long long)g_comp_bad);
}

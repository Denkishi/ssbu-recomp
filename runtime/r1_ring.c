/* Track R2 (TEST ONLY, default OFF): the SNAPSHOT RING for rollback, on top of R1's marked-page save (r1_state.c).
 * Included by r1_state.c (it uses that file's statics: the mark maps, the flat shadow of the first full save, the
 * thread holds, the fiber save, the parallel page copy). No game content in this file. Plan: plan\phases\R2_snapshot_ring.md.
 *
 * Design ("pointer shadow + undo log"):
 *  - every guest page the ring tracks has a global id: window 1 (the game's 62 GB region) pages, then window 2
 *    (image + modules) pages, then the "extras" (main's guest stack, TLS blocks, fighter DLL data, catch table, malloc
 *    arena: the ranges r1 copies whole);
 *  - g_rg_idx[id] says where the page's value AT THE NEWEST CAPTURE is: 0 = the flat shadow of the first full save
 *    (never written since), 1 = decommitted by the game, >= 2 = a 4 KB buffer of the page pool;
 *  - capture(f): every page marked since the previous capture (and every extras page that differs) gets a NEW pool
 *    buffer with its current bytes (one copy, done by main + helper threads); the slot of f keeps the list
 *    (id, previous idx) = an undo log: the buffers of the values at the previous capture are not copied, only moved
 *    into the log. Plus per slot: main's registers, the other threads' wait places and registers, the fibers' host
 *    state, the command buffers' recordings in progress (nvn_gl.c's save area, through the linker map);
 *  - load(g): the undo logs of the slots newer than g are applied newest first, pointers only (no copy); every page
 *    they name, every page marked since the newest capture and every extras page is then copied ONCE from its value at
 *    g into memory; the slots newer than g are dropped (g becomes the newest), older ones stay loadable;
 *  - the oldest slot is dropped when the ring is full (SSBU_RB_DEPTH, default 7 frames back = 8 states).
 * Public API (a future rb_rollback.c, at the safe point run-ahead uses, the kick after the submission):
 *    int r1_ring_capture(long long frame, Ctx *c);  1 = captured; 0 = not now (a thread not at a wait in time)
 *    int r1_ring_load(long long frame, Ctx *c);     1 = loaded (c = main's registers of that frame); 0 = not in the ring; -1 = refused for now (pub1: a thread was running)
 *    void r1_ring_release(void);                    every slot dropped
 *    long long r1_ring_oldest(void), r1_ring_newest(void);   (-1 = empty)
 *    double r1_ring_last_ms(int load);              the last capture / load in ms
 * Test driver (SSBU_RB_SIM=<N>[,<K>[,<warm>]]): a ROLLBACK SIMULATION in one process: every pass of the fight is
 * captured; every N real passes the state of k passes back is loaded (k = 1..K rotating) and those passes run again
 * with player 1's recorded pads, not presented (run-ahead's quiet phase); tests\rb_sim_cmp.py checks the real timeline
 * against a run without it and every re-run frame against the frame it replays. SSBU_RB_SIM_SKIP=<mask>: run-ahead's
 * kick-skip mask in re-run passes except the last one (experiment). SSBU_RB_PARK_MS (default 30): hold limit. */

#define RG_MAXSLOT 16
extern volatile int g_rgp_seg;                            /* (R2 round 3, profile 3: which part of a pass main is in) */
#define RG_FLAT 0u
#define RG_DECOM 1u
#define RG_POOL_MAX (1u << 20)                 /* 4 GB of 4 KB buffers (reserved, committed as used) */
#define RG_ENT_RES (64ull << 20)               /* per slot: room for 8 M (id, old) pairs */
#define RG_FP_RES (256ull << 20)               /* per slot: fibers' host stacks */
#define RG_CB_RES (64ull << 20)                /* per slot: command-buffer recordings */
#define RG_CBS_SIZE 144                        /* nvn_gl.c CbSave: CmdBuf *cb; CmdBuf copy (120: data, len, cap, ...); data; cap */
typedef struct {
    long long frame;
    uint32_t *ent; size_t nent, entcom;
    Ctx ctx;
    Th th[256]; int nth, ninfo;
    Fib *fib; int nfib; uint8_t *fpool; size_t fpool_used, fpool_com;
    int ncbs; uint8_t *cbs; uint8_t *cbd; size_t cbd_used, cbd_com;
    size_t pages, xpages;
} RgSlot;
/* (hs13) g_rg_nomem: 1 = the ring's base could not be made for lack of memory (nothing kept: tried again at the next match),
 * 2 = the ring broke on the way (its own allocations or a rebase failed: off for the rest of the process). rb_rollback.c then
 * plays the match without rollback (each frame waits for the other player's pad: delay-based, never a guess). */
static int g_rg_nomem;
static int rbg_on(void);
static RgSlot *g_rg; static int g_rg_cap, g_rg_head, g_rg_count, g_rg_ready, g_rg_depth = 7;
static uint32_t *g_rg_idx; static uint8_t *g_rg_touch; static uint32_t *g_rg_tl; static size_t g_rg_ntl, g_rg_tlcap;
static size_t g_rg_n1, g_rg_n2, g_rg_nx;               /* page counts: window 1, window 2, extras */
static uint8_t **g_rg_xa, **g_rg_xb;                   /* extras page e: its address, its flat (first save) bytes */
static uint8_t *g_rg_pool; static uint32_t g_rg_bump, g_rg_pcom, *g_rg_free, g_rg_nfree;
static uint8_t *g_rg_cbs; static int *g_rg_ncbs; static int g_rg_cb_ok = -1;
static double g_rg_park_ms = 30;
static int g_rg_keep, g_rg_kept, g_rg_keep_on = -1; static long long g_rg_kept_n, g_rg_kept_same;   /* capture -> load at the same safe point: threads stay held */
static long long g_rg_caps, g_rg_cap_fail, g_rg_loads, g_rg_overflow;
static double g_rg_last[2];
static int g_rgs_on; static long long g_rgv_target; static void rgv_before(int si, long long target); static void rgv_after(void);   /* (simulation / verify, below) */
/* costs: capture (all, hold, sweep + queue, copy, fibers / threads / cb), load per k */
#define RG_NST 65536
static double *g_rgc[5]; static size_t *g_rgc_pg, *g_rgc_xp; static int g_rgc_n;
#define RG_NL 4096
static double g_rgl[RG_MAXSLOT][RG_NL][3]; static size_t g_rgl_pg[RG_MAXSLOT][RG_NL]; static int g_rgl_n[RG_MAXSLOT];

static uint8_t *rg_vres(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_READWRITE); }
static int rg_vcom(uint8_t *base, size_t *com, size_t need, size_t step) {   /* commit [0, need) of a reservation */
    if (need <= *com) return 1;
    size_t to = (need + step - 1) / step * step;
    if (!VirtualAlloc(base + *com, to - *com, MEM_COMMIT, PAGE_READWRITE)) return 0;
    *com = to; return 1;
}
/* (hs14) THE POOL RUNNING OUT IN A MATCH. Before: a capture that found no pool buffer (or no log room) went on without the
 * rest of its pages (their marks already cleared by the sweep), then dropped every older state: a later rollback could find
 * no state, or load a newest state with stale pages (= a desync). Now (SSBU_RB_POOLFIX=0: the old way):
 *  - g_rg_need = the oldest frame a rollback may still need (rb_rollback.c: the newest frame with both players' pads known;
 *    -1 = not known): the newest slot at or before it and every newer one are never dropped;
 *  - out of buffers in a capture: first the slots older than that one are dropped (their buffers go back to the pool);
 *  - still none: the capture is GIVEN UP cleanly: the pages it took go back to their previous values, every page it swept
 *    is marked again, nothing else changes (as a capture skipped for a thread not at its wait); g_rg_short tells
 *    rb_rollback.c, which then runs no frame on a guess (each real pass waits for the other player's pad) until a capture
 *    succeeds again: no rollback is ever needed for a state the ring could not keep;
 *  - the ring full with its oldest slot the only one at or before g_rg_need: the capture is skipped instead of dropping it.
 * Test (forces it): SSBU_RB_NET_TEST_POOL=<from frame>,<frames>[,<mode>]: captures of those frames get at most 64 buffers;
 * mode 1 (default): then only buffers given back to the pool, 2: none at all. */
static long long g_rg_need = -1; static int g_rg_poolfix = -1, g_rg_short; static long long g_rg_short_n, g_rg_freed_slots, g_rg_skip_full;
static long long g_rg_tp_f = -2, g_rg_tp_n; static int g_rg_tp_mode, g_rg_tp_active, g_rg_tp_gets; static long long g_rg_tp_refused;
static uint32_t rg_pool_get(void) {
    if (g_rg_tp_active && ++g_rg_tp_gets > 64) {          /* (test) */
        if (g_rg_tp_mode != 2 && g_rg_nfree) return g_rg_free[--g_rg_nfree];
        g_rg_tp_refused++; return 0;
    }
    if (g_rg_nfree) return g_rg_free[--g_rg_nfree];
    if (g_rg_bump >= RG_POOL_MAX) return 0;
    if (g_rg_bump >= g_rg_pcom) {
        if (!VirtualAlloc(g_rg_pool + (size_t)g_rg_pcom * PG, 256 * PG, MEM_COMMIT, PAGE_READWRITE)) return 0;
        g_rg_pcom += 256;
    }
    return 2 + g_rg_bump++;
}
static void rg_pool_put(uint32_t i) { if (i >= 2) g_rg_free[g_rg_nfree++] = i; }
static uint8_t *rg_pool_ptr(uint32_t i) { return g_rg_pool + (size_t)(i - 2) * PG; }
static uint8_t *rg_addr(uint32_t id) {
    if (id < g_rg_n1) return g_win1 + (size_t)id * PG;
    if (id < g_rg_n1 + g_rg_n2) return (uint8_t *)(uintptr_t)WIN2 + (size_t)(id - g_rg_n1) * PG;
    return g_rg_xa[id - g_rg_n1 - g_rg_n2];
}
/* the bytes of page `id` as recorded at the newest capture (NULL = zero page, (uint8_t *)1 = decommitted) */
static uint8_t *rg_src(uint32_t id) {
    uint32_t v = g_rg_idx[id];
    if (v >= 2) return rg_pool_ptr(v);
    if (v == RG_DECOM) return (uint8_t *)1;
    if (id < g_rg_n1) return g_sh1c[id] ? g_sh1 + (size_t)id * PG : NULL;
    if (id < g_rg_n1 + g_rg_n2) { size_t j = id - g_rg_n1; return g_sh2c[j] ? g_sh2 + j * PG : NULL; }
    return g_rg_xb[id - g_rg_n1 - g_rg_n2];
}
static int rg_push(RgSlot *s, uint32_t id, uint32_t old) {
    if (!rg_vcom((uint8_t *)s->ent, &s->entcom, (s->nent + 1) * 8, 1u << 20)) return 0;
    s->ent[2 * s->nent] = id; s->ent[2 * s->nent + 1] = old; s->nent++;
    return 1;
}
static void rg_touch(uint32_t id) {
    if (g_rg_touch[id]) return;
    if (g_rg_ntl >= g_rg_tlcap) { g_rg_overflow++; return; }
    g_rg_touch[id] = 1; g_rg_tl[g_rg_ntl++] = id;
}
/* every page marked since the last sweep, in both windows (the same block lists as mark_sweep); marks cleared */
static void rg_sweep(void (*fn)(uint32_t id, int w, size_t j)) {
    if (g_full_sweep < 0) g_full_sweep = getenv("SSBU_R1_FULL_SWEEP") != NULL;
    if (!g_blk1 && !g_full_sweep && g_win1) blk1_init();
    if (g_blk1 && g_blk1_dirty) blk1_rebuild();
    for (int w = 0; w < 2; w++) {
        uint8_t *map = w ? *pm2 : *pm1; size_t np = (size_t)((w ? WN2 : g_wn1) >> 12);
        uint8_t *mem = w ? (uint8_t *)(uintptr_t)WIN2 : g_win1;
        uint64_t *q = (uint64_t *)map;
        uint32_t *bl = !g_blk1 ? NULL : w ? g_blk2_list : g_blk1_list;
        size_t nq = !bl ? np / 8 : (w ? g_blk2_n : g_blk1_n) * 8;
        for (size_t qi = 0; qi < nq; qi++) {
            size_t i = !bl ? qi : (size_t)bl[qi >> 3] * 8 + (qi & 7);
            if (!q[i]) continue;
            for (size_t j = i * 8; j < i * 8 + 8; j++) {
                if (!map[j]) continue;
                map[j] = 0;
                if (g_audio_free && is_async(mem + j * PG)) continue;
                if (w ? g_ns2[j] : g_ns1[j]) continue;          /* not simulation */
                fn((uint32_t)(w ? g_rg_n1 + j : j), w, j);
            }
        }
    }
}
static RgSlot *g_rg_cur; static int g_rg_fail_now; static int g_rgs_left;
#include "rb_learned.c"                              /* R4: the learned-set save (the ring on the normal, unmarked game build) */
/* (R2 round 3) SSBU_RB_SIM_PAGES=1: the captured pages counted per 1 MB block and pass kind (0 real, 1 re-run with the skips, 2 the
 * last re-run pass): which memory a frame writes, and which of it only the presentation writes (absent from the skipped passes). */
static int g_rgpg_on = -1; static int g_rgpg_kind; static long long g_rgpg_caps[3];
#define RGPG_N 16384
static struct { uint64_t blk; long long n[3]; } g_rgpg[RGPG_N];
static void rgpg_note(uint8_t *a) {
    uint64_t b = (uint64_t)(uintptr_t)a >> 20; unsigned h = (unsigned)((b * 0x9e3779b97f4a7c15ull) >> 50) % RGPG_N;
    for (int i = 0; i < 64; i++) { unsigned k = (h + (unsigned)i) % RGPG_N; if (g_rgpg[k].blk == b) { g_rgpg[k].n[g_rgpg_kind]++; return; } if (!g_rgpg[k].blk) { g_rgpg[k].blk = b; g_rgpg[k].n[g_rgpg_kind]++; return; } }
}
static uint32_t rg_pool_get_cap(void); static void rg_remark(uint32_t id);
static void rg_cap_page(uint32_t id, int w, size_t j) {
    if (g_rg_fail_now) { if (g_rg_poolfix) rg_remark(id); return; }
    uint32_t old = g_rg_idx[id];
    if (!w && g_dc[j]) {                                        /* decommitted by the game: its value is "decommitted" */
        if (old == RG_DECOM) return;
        if (!rg_push(g_rg_cur, id, old)) { g_rg_fail_now = 1; if (g_rg_poolfix) rg_remark(id); return; }
        g_rg_idx[id] = RG_DECOM; g_rg_cur->pages++;
        return;
    }
    uint32_t nb = rg_pool_get_cap();
    if (!nb || !rg_push(g_rg_cur, id, old)) { if (nb) rg_pool_put(nb); g_rg_fail_now = 1; if (g_rg_poolfix) rg_remark(id); return; }
    cp_add(rg_pool_ptr(nb), rg_addr(id));
    g_rg_idx[id] = nb; g_rg_cur->pages++;
    if (g_rgpg_on > 0) rgpg_note(rg_addr(id));
}
static void rg_load_page(uint32_t id, int w, size_t j) { (void)w; (void)j; rg_touch(id); }
static void rg_remark(uint32_t id) {                     /* (hs14) a window page marked as written again (a capture given up) */
    if (id < g_rg_n1) (*pm1)[id] = 1; else if (id < g_rg_n1 + g_rg_n2) (*pm2)[id - g_rg_n1] = 1;
}
static void rg_slot_drop(RgSlot *s, int log_too) {     /* log_too: the values in its undo log are no longer reachable */
    if (log_too) for (size_t i = 0; i < s->nent; i++) rg_pool_put(s->ent[2 * i + 1]);
    s->nent = 0; s->frame = -1;
}
static RgSlot *rg_slot(int i);
/* (hs14) the oldest slots no rollback can need (a newer slot at or before g_rg_need exists) dropped; their buffers back to the pool */
static int rg_free_unneeded(void) {
    int n = 0;
    if (g_rg_need < 0) return 0;
    while (g_rg_count >= 2 && rg_slot(1)->frame <= g_rg_need) { rg_slot_drop(rg_slot(0), 1); g_rg_head = (g_rg_head + 1) % g_rg_cap; g_rg_count--; n++; }
    g_rg_freed_slots += n;
    return n;
}
static uint32_t rg_pool_get_cap(void) {                  /* a buffer for the capture now running */
    uint32_t nb = rg_pool_get();
    if (!nb && g_rg_poolfix && rg_free_unneeded()) nb = rg_pool_get();
    return nb;
}
static void rg_cb_find(void) {
    if (g_rg_cb_ok >= 0) return;
    uint64_t a = map_sym("g_cbs"), n = map_sym("g_ncbs"), all = map_sym("g_all_cb");
    size_t nvn_gl_r1_cb_size(void);                      /* (R3 pub1) the entry size from nvn_gl.c itself: the compiler of the package exe put g_ncbs between the two arrays */
    g_rg_cb_ok = a && n && all && (all - a == 512ull * RG_CBS_SIZE || nvn_gl_r1_cb_size() == RG_CBS_SIZE);
    if (g_rg_cb_ok) { g_rg_cbs = (uint8_t *)(uintptr_t)a; g_rg_ncbs = (int *)(uintptr_t)n; }
    LOG("r1: RING: command-buffer save area of nvn_gl.c %s (g_cbs %llx, g_ncbs %llx, g_all_cb %llx)\n", g_rg_cb_ok ? "found: saved per slot" : "NOT found (layout differs): the newest capture's is put back at every load",
        (unsigned long long)a, (unsigned long long)n, (unsigned long long)all);
}
static int rg_cb_capture(RgSlot *s) {
    s->ncbs = 0; s->cbd_used = 0;
    if (!g_rg_cb_ok) return 1;
    int n = *g_rg_ncbs; if (n < 0 || n > 512) return 0;
    memcpy(s->cbs, g_rg_cbs, (size_t)n * RG_CBS_SIZE);
    for (int i = 0; i < n; i++) {
        uint8_t *e = g_rg_cbs + (size_t)i * RG_CBS_SIZE;
        size_t len = *(size_t *)(e + 16); uint8_t *d = *(uint8_t **)(e + 128);
        if (!*(void **)e || !len || !d) continue;
        if (!rg_vcom(s->cbd, &s->cbd_com, s->cbd_used + len, 1u << 20)) return 0;
        memcpy(s->cbd + s->cbd_used, d, len); s->cbd_used += len;
    }
    s->ncbs = n;
    return 1;
}
static void rg_cb_load(RgSlot *s) {
    if (g_rg_cb_ok) {
        size_t off = 0;
        for (int i = 0; i < s->ncbs; i++) {
            uint8_t *e = g_rg_cbs + (size_t)i * RG_CBS_SIZE, *se = s->cbs + (size_t)i * RG_CBS_SIZE;
            uint8_t *d = *(uint8_t **)(e + 128); size_t cap = *(size_t *)(e + 136);
            memcpy(e, se, 128);                                 /* cb + the record's copy; the save area's own buffer stays */
            size_t len = *(size_t *)(se + 16);
            if (*(void **)se && len && *(uint8_t **)(se + 128)) {
                if (cap < len) { cap = len + 65536; d = realloc(d, cap); }
                memcpy(d, s->cbd + off, len); off += len;
            }
            *(uint8_t **)(e + 128) = d; *(size_t *)(e + 136) = cap;
        }
        *g_rg_ncbs = s->ncbs;
    }
    nvn_r1_cb_restore();
}
static void rg_meta_capture(RgSlot *s, Ctx *c) {       /* threads still held: their places, the fibers, the command buffers */
    s->ctx = *c;
    s->nth = g_nth; s->ninfo = g_ninfo;
    memcpy(s->th, g_th, (size_t)g_nth * sizeof *g_th);
    for (int i = 0; i < s->nth; i++) { s->th[i].held = 0; s->th[i].cok = 0; }
    s->nfib = g_nfib; memcpy(s->fib, g_fib, (size_t)g_nfib * sizeof *g_fib);
    s->fpool_used = 0;
    if (g_fpool_used && rg_vcom(s->fpool, &s->fpool_com, g_fpool_used, 1u << 20)) { memcpy(s->fpool, g_fpool, g_fpool_used); s->fpool_used = g_fpool_used; }
    else if (g_fpool_used) for (int i = 0; i < s->nfib; i++) s->fib[i].susp = 0;
    if (!rg_cb_capture(s)) s->ncbs = 0;
}
static void rg_free_all(void) {                         /* (hs13) a ring that could not be made: what it took is given back */
    if (g_rg_idx) VirtualFree(g_rg_idx, 0, MEM_RELEASE);
    if (g_rg_touch) VirtualFree(g_rg_touch, 0, MEM_RELEASE);
    if (g_rg_tl) VirtualFree(g_rg_tl, 0, MEM_RELEASE);
    if (g_rg_pool) VirtualFree(g_rg_pool, 0, MEM_RELEASE);
    if (g_rg_free) VirtualFree(g_rg_free, 0, MEM_RELEASE);
    free(g_rg_xa); free(g_rg_xb);
    for (int i = 0; g_rg && i < g_rg_cap; i++) {
        RgSlot *s = &g_rg[i];
        if (s->ent) VirtualFree(s->ent, 0, MEM_RELEASE);
        if (s->fpool) VirtualFree(s->fpool, 0, MEM_RELEASE);
        if (s->cbd) VirtualFree(s->cbd, 0, MEM_RELEASE);
        free(s->fib); free(s->cbs);
    }
    free(g_rg);
    g_rg_idx = g_rg_tl = g_rg_free = NULL; g_rg_touch = g_rg_pool = NULL; g_rg_xa = g_rg_xb = NULL; g_rg = NULL;
    g_rg_pcom = 0; g_rg_bump = 0; g_rg_nfree = 0;
}
static int rg_init(void) {
    g_rg_n1 = g_win1 ? (size_t)(g_wn1 >> 12) : 0; g_rg_n2 = (size_t)(WN2 >> 12);
    g_rg_nx = 0; for (int i = 0; i < g_nx; i++) g_rg_nx += g_x[i].n / PG;
    size_t tot = g_rg_n1 + g_rg_n2 + g_rg_nx;
    { const char *e = getenv("SSBU_RB_DEPTH"); if (e) g_rg_depth = atoi(e); if (g_rg_depth < 1) g_rg_depth = 1; if (g_rg_depth > RG_MAXSLOT - 1) g_rg_depth = RG_MAXSLOT - 1; }
    g_rg_cap = g_rg_depth + 1;
    g_rg_idx = VirtualAlloc(NULL, tot * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_rg_touch = VirtualAlloc(NULL, tot, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_rg_tlcap = 8u << 20; g_rg_tl = VirtualAlloc(NULL, g_rg_tlcap * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_rg_pool = rg_vres((size_t)RG_POOL_MAX * PG);
    g_rg_free = VirtualAlloc(NULL, (size_t)RG_POOL_MAX * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_rg_xa = malloc((g_rg_nx + 1) * sizeof *g_rg_xa); g_rg_xb = malloc((g_rg_nx + 1) * sizeof *g_rg_xb);
    g_rg = calloc((size_t)g_rg_cap, sizeof *g_rg);
    if (rb_test_nomem() == 3 || !g_rg_idx || !g_rg_touch || !g_rg_tl || !g_rg_pool || !g_rg_free || !g_rg_xa || !g_rg_xb || !g_rg) { LOG("r1: RING: allocation FAILED%s\n", rb_test_nomem() == 3 ? " (TEST: SSBU_RB_NET_TEST_NOMEM=3)" : ""); rg_free_all(); return 0; }
    size_t e = 0;
    for (int i = 0; i < g_nx; i++) for (size_t o = 0; o < g_x[i].n; o += PG) { g_rg_xa[e] = g_x[i].a + o; g_rg_xb[e] = g_x[i].b + o; e++; }
    for (int i = 0; i < g_rg_cap; i++) {
        RgSlot *s = &g_rg[i]; s->frame = -1;
        s->ent = (uint32_t *)rg_vres(RG_ENT_RES); s->fpool = rg_vres(RG_FP_RES); s->cbd = rg_vres(RG_CB_RES);
        s->fib = malloc(1024 * sizeof *s->fib); s->cbs = malloc(512 * RG_CBS_SIZE);
        if (!s->ent || !s->fpool || !s->cbd || !s->fib || !s->cbs) { LOG("r1: RING: slot allocation FAILED\n"); rg_free_all(); return 0; }
    }
    for (int k = 0; k < 5; k++) if (!g_rgc[k]) g_rgc[k] = calloc(RG_NST, sizeof(double));
    if (!g_rgc_pg) g_rgc_pg = calloc(RG_NST, sizeof(size_t));
    if (!g_rgc_xp) g_rgc_xp = calloc(RG_NST, sizeof(size_t));
    for (int k = 0; k < 5; k++) if (!g_rgc[k]) { LOG("r1: RING: statistics allocation FAILED\n"); rg_free_all(); return 0; }
    if (!g_rgc_pg || !g_rgc_xp) { LOG("r1: RING: statistics allocation FAILED\n"); rg_free_all(); return 0; }
    if (getenv("SSBU_RB_PARK_MS")) g_rg_park_ms = atof(getenv("SSBU_RB_PARK_MS"));
    rg_cb_find(); cp_init();
    LOG("r1: RING on: depth %d frames (%d states), pages: window 1 %zu, window 2 %zu, extras %zu (%.1f MB); pool up to %.0f GB; hold limit %.1f ms\n",
        g_rg_depth, g_rg_cap, g_rg_n1, g_rg_n2, g_rg_nx, g_rg_nx * PG / 1048576.0, (double)RG_POOL_MAX * PG / 1073741824.0, g_rg_park_ms);
    return 1;
}
static RgSlot *rg_slot(int i) { return &g_rg[(g_rg_head + i) % g_rg_cap]; }     /* i = 0 oldest */
void r1_ring_let_go(void) { if (g_rg_kept) { unpark(); g_rg_kept = 0; } }      /* after a capture made with g_rg_keep and no load */
/* (R3 hs11) the threads held for a load that follows at once, WITHOUT a capture of the state that is about to be thrown away
 * (the load finds what was written since the newest capture by its own sweep); r1_ring_load / r1_ring_let_go lets them go */
int r1_ring_hold(void) {
    if (!g_rg_ready || g_rg_kept) return g_rg_kept;
    if (!ra_hold_quiet(g_rg_park_ms)) return 0;
    g_rg_kept = 1; return 1;
}
long long r1_ring_oldest(void) { return g_rg_count ? rg_slot(0)->frame : -1; }
long long r1_ring_newest(void) { return g_rg_count ? rg_slot(g_rg_count - 1)->frame : -1; }
double r1_ring_last_ms(int load) { return g_rg_last[load ? 1 : 0]; }
void r1_ring_release(void) {                              /* every slot dropped: the newest values stay (g_rg_idx), the logs' old values go back to the pool */
    for (int i = 0; i < g_rg_count; i++) rg_slot_drop(rg_slot(i), 1);
    g_rg_count = 0; g_rg_head = 0;
}
/* (R3 hs16, test) the metadata parts of captures / loads, ms (printed with SSBU_RB_HOLD_PROF) */
static double g_mp[9], g_mp_b[2]; static long long g_mp_n[2];
static void rg_meta_prof_report(void) {
    if (g_hp_on <= 0) return;
    double a = g_mp_n[0] ? (double)g_mp_n[0] : 1, b = g_mp_n[1] ? (double)g_mp_n[1] : 1;
    LOG("r1: META PROF captures %lld: fibers %.4f (fiber pool %.0f KB), command buffers %.4f (%.0f KB), threads %.4f, let go %.4f ms; loads %lld: thread registers %.4f, fibers %.4f, catch table %.4f, command buffers %.4f, let go %.4f ms\n",
        g_mp_n[0], g_mp[0] / a, g_mp_b[0] / a / 1024, g_mp[1] / a, g_mp_b[1] / a / 1024, g_mp[2] / a, g_mp[3] / a, g_mp_n[1], g_mp[4] / b, g_mp[5] / b, g_mp[6] / b, g_mp[7] / b, g_mp[8] / b);
}
int r1_ring_capture(long long frame, Ctx *c) {
    R1_CTR("capture (frame)", frame, 0);
    double t0 = now_ms(); int seg_was = g_rgp_seg; g_rgp_seg = 4;
    if (!g_rg_ready) {                                    /* the first time: r1's full save (shadow filled, marks on: one hitch), the ring's base */
        g_rgp_seg = seg_was;
        if (g_rg_nomem) return 0;                         /* (hs13) out of memory: not tried again in this match */
        g_save_failed = 0; g_ra_wd_pause = 1; g_rs_drop = 1;
        save(c); release(); g_rs_drop = 0;
        g_ra_wd_pause = 0;
        if (g_save_failed == 2) { g_rg_nomem = 1; LOG("r1: RING: the base state was NOT made (out of memory: free commit %.0f MB, needed %.0f MB + 1024 MB left for the game): no rollback ring\n", g_save_avail_mb, g_save_need_mb); return 0; }
        if (g_save_failed) return 0;
        g_ra_ready = 1; g_cycle = 1;
        if (g_mark <= 0) { g_ra_off = 1; LOG("r1: RING: no marks (the game DLL with marks is not loaded): OFF\n"); return 0; }
        if (!rg_init()) { g_rg_nomem = 2; if (!rbg_on()) g_ra_off = 1; LOG("r1: RING: its own allocations FAILED (out of memory): no rollback ring for the rest of this session\n"); return 0; }
        g_rg_ready = 1;
        RgSlot *s = &g_rg[0]; g_rg_head = 0; g_rg_count = 1;
        s->frame = frame; s->nent = 0;
        rg_meta_capture(s, &g_ctx_save);
        LOG("r1: RING: base state (frame %lld): full save in %.1f ms\n", frame, now_ms() - t0);
        return 1;
    }
    if (g_rg_poolfix < 0) { const char *e = getenv("SSBU_RB_POOLFIX"); g_rg_poolfix = !(e && e[0] == '0'); }
    if (g_rg_tp_f == -2) { const char *e = getenv("SSBU_RB_NET_TEST_POOL"); g_rg_tp_f = -1; g_rg_tp_mode = 1; if (e && *e) { sscanf(e, "%lld,%lld,%d", &g_rg_tp_f, &g_rg_tp_n, &g_rg_tp_mode); LOG("r1: RING TEST: captures of frames %lld .. %lld get at most 64 pool buffers (mode %d)\n", g_rg_tp_f, g_rg_tp_f + g_rg_tp_n - 1, g_rg_tp_mode); } }
    g_rg_tp_active = g_rg_tp_f >= 0 && frame >= g_rg_tp_f && frame < g_rg_tp_f + g_rg_tp_n; g_rg_tp_gets = 0;
    if (g_rg_poolfix && g_rg_count == g_rg_cap && g_rg_need >= 0 && g_rg_cap >= 2 && rg_slot(1)->frame > g_rg_need) {      /* (hs14) full, and its oldest state is still needed */
        g_rgp_seg = seg_was; g_rg_skip_full++;
        if (g_rg_skip_full <= 10) LOG("r1: RING: capture at frame %lld skipped: the ring is full and its oldest state (frame %lld) is the newest one at or before frame %lld, which a rollback may still need\n", frame, rg_slot(0)->frame, g_rg_need);
        return 0;
    }
    if (!ra_hold_quiet(g_rg_park_ms)) { g_rgp_seg = seg_was; g_rg_cap_fail++; if (g_rg_cap_fail <= 20 || g_rg_cap_fail % 100 == 0) LOG("r1: RING: capture %lld (frame %lld) skipped: thread %s not at a wait within %.1f ms\n", g_rg_cap_fail, frame, g_ra_busy ? g_ra_busy : "?", g_rg_park_ms); return 0; }
    double t1 = now_ms();
    if (g_rg_count == g_rg_cap) { rg_slot_drop(rg_slot(0), 1); g_rg_head = (g_rg_head + 1) % g_rg_cap; g_rg_count--; }
    RgSlot *s = rg_slot(g_rg_count);
    s->frame = frame; s->nent = 0; s->pages = 0; s->xpages = 0;
    g_rg_cur = s; g_rg_fail_now = 0;
    static uint8_t *xdiff; static size_t xdn; if (xdn < g_rg_nx + 1) { free(xdiff); xdn = g_rg_nx + 1; xdiff = calloc(xdn, 1); }   /* (hs12: the extras can change at a rebase) */
    for (size_t e = 0; e < g_rg_nx; e++) cp_add(g_rg_xa[e], rg_src((uint32_t)(g_rg_n1 + g_rg_n2 + e)));      /* (round 3) extras compared by main + the copy threads */
    g_cp_cmp = xdiff; cp_run(); g_cp_cmp = NULL;
    if (g_rgpg_on < 0) g_rgpg_on = getenv("SSBU_RB_SIM_PAGES") != NULL;
    g_rgpg_kind = g_ra_phase != 2 ? 0 : g_rgs_left > 1 ? 1 : 2; g_rgpg_caps[g_rgpg_kind]++;
    if (g_ls_active) ls_capture(rg_cap_page); else rg_sweep(rg_cap_page);      /* R4: the learned set (rb_learned.c) instead of the marks */
    for (size_t e = 0; e < g_rg_nx && !g_rg_fail_now; e++) {             /* extras: the pages that differ from their newest value */
        uint32_t id = (uint32_t)(g_rg_n1 + g_rg_n2 + e);
        if (!xdiff[e]) continue;
        uint32_t nb = rg_pool_get_cap();
        if (!nb || !rg_push(s, id, g_rg_idx[id])) { if (nb) rg_pool_put(nb); g_rg_fail_now = 1; break; }
        cp_add(rg_pool_ptr(nb), g_rg_xa[e]); g_rg_idx[id] = nb; s->xpages++;
    }
    double t2 = now_ms();
    cp_run();
    if (g_rg_fail_now && g_rg_poolfix) {                    /* (hs14) given up: this capture's pages back to their previous values, marked again */
        size_t undone = s->nent;
        for (size_t n = s->nent; n-- > 0;) { uint32_t id = s->ent[2 * n], old = s->ent[2 * n + 1]; rg_pool_put(g_rg_idx[id]); g_rg_idx[id] = old; rg_remark(id); }
        s->nent = 0; s->frame = -1; s->pages = s->xpages = 0;
        if (g_rg_keep) g_rg_kept = 1; else unpark();
        g_rg_short = 1; g_rg_short_n++; g_rgp_seg = seg_was;
        if (g_rg_short_n <= 20 || g_rg_short_n % 100 == 0) LOG("r1: RING: capture at frame %lld GIVEN UP: out of pool memory (pool %u buffers made, %u free; %lld old states freed so far; test refusals %lld): %zu pages put back, nothing lost; states kept %d (frames %lld .. %lld), needed from frame %lld\n",
            frame, g_rg_bump, g_rg_nfree, g_rg_freed_slots, g_rg_tp_refused, undone, g_rg_count, r1_ring_oldest(), r1_ring_newest(), g_rg_need);
        return 0;
    }
    g_rg_short = 0;
    double t3 = now_ms();
    ls_capture_end();                                     /* R4 (SSBU_LS_DECAY) */
    fibers_save(); double t3a = now_ms();
    nvn_r1_cb_save(); double t3b = now_ms();
    rg_meta_capture(s, c);
    double t4 = now_ms();
    if (g_rg_keep) g_rg_kept = 1;                          /* a load follows at once (r1_ring_load / r1_ring_let_go lets them go) */
    else unpark();                                         /* every thread, the audio ones too (a real pass follows) */
    double t5 = now_ms();
    g_mp[0] += t3a - t3; g_mp[1] += t3b - t3a; g_mp[2] += t4 - t3b; g_mp[3] += t5 - t4; g_mp_n[0]++; g_mp_b[0] += (double)g_fpool_used; g_mp_b[1] += (double)s->cbd_used;
    g_rg_count++; g_rg_caps++;
    if (g_rg_fail_now) {                                   /* out of pool / log room: the newest values are right, the older states are not reachable */
        LOG("r1: RING: capture at frame %lld ran out of room (pool %u buffers): older states dropped\n", frame, g_rg_bump);
        while (g_rg_count > 1) { rg_slot_drop(rg_slot(0), 1); g_rg_head = (g_rg_head + 1) % g_rg_cap; g_rg_count--; }
    }
    g_rg_last[0] = t5 - t0; g_rgp_seg = seg_was;
    if (g_rgc_n < RG_NST) { int i = g_rgc_n++; g_rgc[0][i] = t5 - t0; g_rgc[1][i] = t1 - t0; g_rgc[2][i] = t2 - t1; g_rgc[3][i] = t3 - t2; g_rgc[4][i] = (t4 - t3) + (t5 - t4); g_rgc_pg[i] = s->pages; g_rgc_xp[i] = s->xpages; }
    return 1;
}
/* (R3 hs12) a NEW BASE for the next match of the same process (two online matches in one session). Between two matches
 * the game loaded and freed a lot through host code that marks nothing (file loads, module loads, the loader threads), so
 * the ring's newest values (g_rg_idx, the flat shadow) are stale: every slot is dropped and, with every guest thread held,
 * the base is made again the way the first full save made it, in place: every committed read-write page of both windows
 * is copied into the shadow (its value = the shadow again, pool buffers freed), the shadow of a page not committed now is
 * decommitted (= a zero page, as for a page the first save never saw), the marks are cleared, the extras (pages outside
 * the windows: main's guest stack, TLS blocks, fighter DLL data, catch table, malloc arena) are listed again from scratch
 * (their count may change), host stacks made since are left out, modules loaded since get the mark maps. Then slot 0 = this
 * frame (threads, fibers, command buffers as a capture). 1 = done; 0 = not now (a thread not at a wait: next pass). */
static void rg_noop(uint32_t id, int w, size_t j) { (void)id; (void)w; (void)j; }
int r1_ring_rebase(long long frame, Ctx *c) {
    if (g_rg_nomem == 2) return 0;                        /* (hs13) the ring broke earlier: no rollback */
    if (!g_rg_ready) return r1_ring_capture(frame, c);
    double t0 = now_ms();
    r1_ring_let_go();
    r1_ring_release();
    if (!ra_hold_quiet(200)) { LOG("r1: RING rebase (frame %lld): no quiet moment within 200 ms: next pass\n", frame); return 0; }
    double t1 = now_ms();
    hs_exclude_all();
    int nmod = 0;
    {   HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        MODULEENTRY32 me; me.dwSize = sizeof me;
        for (BOOL ok = sn != INVALID_HANDLE_VALUE && Module32First(sn, &me); ok; ok = Module32Next(sn, &me)) {
            uint8_t **a1 = (uint8_t **)GetProcAddress(me.hModule, "r1_m1"), **a2 = (uint8_t **)GetProcAddress(me.hModule, "r1_m2");
            uint64_t *l1 = (uint64_t *)GetProcAddress(me.hModule, "r1_lo1"), *n1 = (uint64_t *)GetProcAddress(me.hModule, "r1_n1"), *n2 = (uint64_t *)GetProcAddress(me.hModule, "r1_n2");
            if (!a1 || !a2 || !l1 || !n1 || !n2) continue;
            if (*a1 != *pm1) { *a1 = *pm1; *a2 = *pm2; *l1 = (uint64_t)(uintptr_t)g_win1; MemoryBarrier(); *n2 = WN2; *n1 = g_wn1; nmod++; LOG("r1: RING rebase: mark maps set in %s (loaded after the first save)\n", me.szModule); }
        }
        if (sn != INVALID_HANDLE_VALUE) CloseHandle(sn); }
    rg_sweep(rg_noop);                                    /* marks since the last capture: the copy below takes everything */
    g_rg_ntl = 0;
    collect(c);
    size_t nwin = g_rg_n1 + g_rg_n2, copied = 0, dropped = 0, freed = 0;
    uint8_t *in = calloc(nwin, 1);
    static Extra nx[8192]; int nnx = 0;
    for (int i = 0; i < g_nr; i++) {
        Rng *r = &g_r[i];
        for (size_t p = 0; p < r->n / PG; p++) {
            uint8_t *a = r->a + p * PG, *cm; size_t pi;
            uint8_t *sh = in_shadow(a, &cm, &pi);
            if (sh) {
                int w2 = cm == g_sh2c;
                if (w2 ? g_ns2[pi] : g_ns1[pi]) continue;                    /* not simulation (a host stack) */
                in[w2 ? g_rg_n1 + pi : pi] = 1;
                if (!sh_commit(sh, cm, pi)) { free(in); g_rg_nomem = 2; unpark(); LOG("r1: RING rebase: commit of the shadow FAILED (out of memory): no rollback ring for the rest of this session\n"); return 0; }
                if (!ls_lazy()) memcpy(sh, a, PG);              /* (SSBU_LS_LAZY: the first write copies it, ls_open) */
                copied++;
                if (!w2) g_dc[pi] = 0;
            } else if (nnx < 8192) {
                if (nnx && nx[nnx - 1].a + nx[nnx - 1].n == a) nx[nnx - 1].n += PG;
                else { nx[nnx].a = a; nx[nnx].n = PG; nx[nnx].b = NULL; nnx++; }
            }
        }
    }
    for (size_t id = 0; id < nwin; id++) {
        if (g_rg_idx[id] != RG_FLAT) { if (g_rg_idx[id] >= 2) freed++; rg_pool_put(g_rg_idx[id]); g_rg_idx[id] = RG_FLAT; }
        if (in[id]) continue;
        int w2 = id >= g_rg_n1; size_t pi = w2 ? id - g_rg_n1 : id; uint8_t *cm = w2 ? g_sh2c : g_sh1c;
        if (cm[pi]) { VirtualFree((w2 ? g_sh2 : g_sh1) + pi * PG, PG, MEM_DECOMMIT); cm[pi] = 0; dropped++; }
    }
    free(in);
    {   /* the extras again */
        size_t npg = 0; for (int i = 0; i < nnx; i++) npg += nx[i].n / PG;
        size_t tot = nwin + npg;
        for (size_t e = 0; e < g_rg_nx; e++) rg_pool_put(g_rg_idx[nwin + e]);
        uint32_t *nidx = VirtualAlloc(NULL, tot * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        uint8_t *ntouch = VirtualAlloc(NULL, tot, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        uint8_t **nxa = malloc((npg + 1) * sizeof *nxa), **nxb = malloc((npg + 1) * sizeof *nxb);
        if (!nidx || !ntouch || !nxa || !nxb) { LOG("r1: RING rebase: allocation FAILED (out of memory): no rollback ring for the rest of this session\n"); g_rg_nomem = 2; unpark(); return 0; }
        memcpy(nidx, g_rg_idx, nwin * 4);
        VirtualFree(g_rg_idx, 0, MEM_RELEASE); VirtualFree(g_rg_touch, 0, MEM_RELEASE); free(g_rg_xa); free(g_rg_xb);
        for (int i = 0; i < g_nx; i++) free(g_x[i].b);
        g_rg_idx = nidx; g_rg_touch = ntouch; g_rg_xa = nxa; g_rg_xb = nxb;
        g_nx = nnx; g_xbytes = 0; size_t e = 0;
        for (int i = 0; i < nnx; i++) {
            g_x[i] = nx[i]; g_x[i].b = malloc(g_x[i].n);
            if (!g_x[i].b) { g_nx = i; g_rg_nx = e; g_rg_nomem = 2; unpark(); LOG("r1: RING rebase: allocation of an extra FAILED (out of memory): no rollback ring for the rest of this session\n"); return 0; }
            memcpy(g_x[i].b, g_x[i].a, g_x[i].n); g_xbytes += g_x[i].n;
            for (size_t o = 0; o < g_x[i].n; o += PG) { g_rg_xa[e] = g_x[i].a + o; g_rg_xb[e] = g_x[i].b + o; e++; }
        }
        g_rg_nx = npg;
    }
    double t2 = now_ms();
    RgSlot *s = &g_rg[0]; g_rg_head = 0; g_rg_count = 1;
    s->frame = frame; s->nent = 0; s->pages = 0; s->xpages = 0;
    fibers_save();
    nvn_r1_cb_save();
    rg_meta_capture(s, c);
    ls_rebase();                                          /* R4: the windows read-only again, the learned set emptied */
    unpark();
    g_rg_caps++;
    LOG("r1: RING rebase: base state (frame %lld) made again in %.1f ms (hold %.1f, copy %.1f): %zu window pages %s the shadow (%.1f MB), %zu pool buffers freed, %zu shadow pages of pages not committed now dropped; extras %d ranges %.1f MB (%zu pages); %d modules got the mark maps\n",
        frame, now_ms() - t0, t1 - t0, t2 - t1, copied, ls_lazy() ? "pending (lazy: copied at their first write) in" : "copied into", copied * (double)PG / 1048576.0, freed, dropped, g_nx, g_xbytes / 1048576.0, g_rg_nx, nmod);
    return 1;
}
/* (R3 hs13: tl4 / nmr / le3a / lg2a / lh4, c00000ff in ntdll's unwinder at a lua2cpp L2CExceptionShutdown throw, about 1 loop
 * run in 5 at the same frame) nnunwind.c's catch table g_catch maps a guest frame (CFA) to a host jmp_buf ON A HOST STACK.
 * The table is state (an extra, rolled back with the rest), but only SOME host stacks go back with it: those of the fibers
 * that were switched away at the capture and are copied back by fibers_restore_all. Every other host stack (the threads,
 * the fibers running now, a fiber that got a new host fiber since) keeps its present frames, while the table got the
 * entries of the capture: a coroutine whose entry frame was registered on its new host fiber found, after a load, the older
 * entry of the same guest frame pointing into its previous host fiber, and longjmp'ed across stacks (proved by the
 * nnunwind.c check: "CROSS-STACK longjmp", lh4a / lh4b). Now, after a load: the restored table keeps only the entries whose
 * jmp_buf lies on a host stack that was copied back; the present entries of every other stack are put back.
 * SSBU_R1_CATCHFIX=0: the old way (test). */
typedef struct { uint64_t cfa; uint64_t jb; } RgCatch;
static int g_rgc_fix = -1; static long long g_rgc_kept, g_rgc_dropped, g_rgc_loads_changed;
static RgCatch g_rgc_now[8192];
static void rgc_before(void) {                          /* threads held, before the load copies: the table as it is now */
    if (g_rgc_fix < 0) { const char *e = getenv("SSBU_R1_CATCHFIX"); g_rgc_fix = !(e && e[0] == '0'); }
    if (!g_rgc_fix || !g_catch_addr) return;
    memcpy(g_rgc_now, (void *)(uintptr_t)g_catch_addr, sizeof g_rgc_now);
}
void nnunwind_catch_reinsert(uint64_t cfa, uint64_t jb); void nnunwind_catch_free(unsigned i);   /* nnunwind.c (hs15: its hash and probing) */
/* (hs15) after a load: the host fiber stacks still in use (some guest fiber's host fiber, a thread's current fiber, a stack a
 * thread's stack pointer is on); a catch record on any other host fiber stack ever made is dead (the coroutine there was made
 * again on a new host fiber, or the fiber exists only in states thrown away): dropped instead of kept for ever. These dead
 * records piled up at the same CFA (h15e0: 25 of one frame) and, with the old hash, overwrote live ones (pp3h). */
static uint64_t g_rgc_live[2048]; static int g_rgc_nlive; static long long g_rgc_dead;
static void rgc_live_cb(uint64_t key, Ctx *c, uint64_t stack, uint64_t size, void *fiber) {
    (void)key; (void)c; (void)stack; (void)size;
    if (fiber && g_rgc_nlive < 2048) g_rgc_live[g_rgc_nlive++] = *(uint64_t *)((uint8_t *)fiber + 0x20);
}
static int rgc_dead(uint64_t jb) {                     /* jb on a host fiber stack that nothing uses now */
    size_t i; for (i = 0; i < g_nbf; i++) if (jb >= g_bf_lo[i] && jb < g_bf_hi[i]) break;
    if (i == g_nbf) return 0;                           /* not a fiber stack made by os_threads.c (a thread's own stack): kept */
    uint64_t lo = g_bf_lo[i], hi = g_bf_hi[i];
    for (int k = 0; k < g_rgc_nlive; k++) if (g_rgc_live[k] == lo) return 0;
    for (int k = 0; k < g_nlive; k++) if (g_live_rsp[k] >= lo && g_live_rsp[k] < hi) return 0;     /* (live_rsps of fibers_restore_all: a thread is on it) */
    return 1;
}
static void rgc_after(void) {                           /* after the copies and fibers_restore_all (g_rest_lo / g_rest_hi: the stacks copied back) */
    if (!g_rgc_fix || !g_catch_addr) return;
    RgCatch *t = (RgCatch *)(uintptr_t)g_catch_addr; long long dropped = 0, kept = 0;
    static int deadfix = -1; if (deadfix < 0) { const char *e = getenv("SSBU_R1_CATCHDEAD"); deadfix = !(e && e[0] == '0'); }
    if (deadfix) { g_rgc_nlive = 0; os_r1_fibers(rgc_live_cb); AcquireSRWLockShared(&g_bf_lock); }
    static int tr = -1; if (tr < 0) tr = getenv("SSBU_UNWIND_TRACE") != NULL;
    if (tr) { void nnunwind_trace_note(const char *what, uint64_t cfa, uint64_t jb);
              for (int i = 0; i < 8192; i++) if (t[i].cfa > 1) nnunwind_trace_note(rest_stack(t[i].jb) ? "load: restored record kept (its stack copied back)" : "load: restored record dropped", t[i].cfa, t[i].jb);
              for (int k = 0; k < 8192; k++) if (g_rgc_now[k].cfa > 1) nnunwind_trace_note(rest_stack(g_rgc_now[k].jb) ? "load: present record not kept (its stack copied back)" : deadfix && rgc_dead(g_rgc_now[k].jb) ? "load: present record dead" : "load: present record kept", g_rgc_now[k].cfa, g_rgc_now[k].jb); }
    for (int i = 0; i < 8192; i++) if (t[i].cfa > 1 && !rest_stack(t[i].jb)) { nnunwind_catch_free((unsigned)i); dropped++; }
    for (int k = 0; k < 8192; k++) {                    /* nnunwind.c catch_put, without its lock (every guest thread is held) */
        if (g_rgc_now[k].cfa <= 1 || !g_rgc_now[k].jb || rest_stack(g_rgc_now[k].jb)) continue;
        if (deadfix && rgc_dead(g_rgc_now[k].jb)) { g_rgc_dead++; continue; }
        nnunwind_catch_reinsert(g_rgc_now[k].cfa, g_rgc_now[k].jb); kept++;     /* (the records are per host stack: nnunwind.c catch_here) */
    }
    if (deadfix) ReleaseSRWLockShared(&g_bf_lock);
    g_rgc_dropped += dropped; g_rgc_kept += kept; if (dropped || kept) g_rgc_loads_changed++;
}
static long long g_rg_refused;                        /* (R3 pub1) loads refused: a thread still running after the hold */
static long long g_rg_ldpg;                           /* (R3 net3) pages to load, all loads (the written ones: g_cp_wrote) */
int r1_ring_load(long long frame, Ctx *c) {
    int si = -1;
    for (int i = 0; i < g_rg_count; i++) if (rg_slot(i)->frame == frame) si = i;
    if (si < 0) return 0;
    double t0 = now_ms(); g_rgp_seg = 5;
    RgSlot *g = rg_slot(si);
    int k = g_rg_count - 1 - si;
    static Th cur[256]; int ncur = g_rg_kept ? g_nth : 0;  /* threads still held from the capture just made (g_rg_keep) */
    if (ncur) memcpy(cur, g_th, (size_t)ncur * sizeof *cur);
    g_rg_kept = 0;
    g_ninfo = g->ninfo;
    stop_born_threads();
    if (g_nborn) LOG("r1: RING: %d guest threads created after frame %lld, held for good:%s\n", g_nborn, frame, g_born);
    g_nth = g->nth; memcpy(g_th, g->th, (size_t)g->nth * sizeof *g_th);
    if (ncur) g_rg_kept_n++;
    for (int j = 0; j < ncur; j++) {                      /* held at the same wait as in capture g: stays held; otherwise let go (the loop below waits for it) */
        if (!cur[j].held) continue;
        int keep = 0;
        for (int i = 0; i < g_nth; i++) if (g_th[i].h == cur[j].h) { if (g_th[i].parked && cur[j].from == g_th[i].from && cur[j].imp == g_th[i].imp) { g_th[i].held = 1; g_th[i].rsp = cur[j].rsp; g_th[i].rip = cur[j].rip; g_th[i].cok = cur[j].cok; keep = 1; } break; }
        if (keep) g_rg_kept_same++; else ResumeThread(cur[j].h);
    }
    for (double tq = now_ms();;) {                        /* each thread at the wait it was held at in that capture, no job running */
        ra_hold_restore();
        if (ra_fibers_quiet() || now_ms() - tq > 200) { if (g_ra_busy_fibers) LOG("r1: RING load: %d fibers still busy after 200 ms\n", g_ra_busy_fibers); break; }
        for (int i = 0; i < g_nth; i++) if (g_th[i].held) { ResumeThread(g_th[i].h); g_th[i].held = 0; }
        SwitchToThread();
    }
    /* (R3 pub1: fx2j / fx3h, c0000005 in the sound thread's memcpy right after "nus3::audio update not back at its wait after 500 ms
     * ... now guest code") a thread that is still RUNNING guest code (not blocked at a wait) after the hold's 500 ms was suspended in
     * the middle of its code, and the load then changed the memory under its registers. Now such a load is REFUSED: every thread
     * goes on, nothing was changed yet, the wrong guess stays pending (rb_rollback.c) and the next tick loads again (one frame
     * further back). SSBU_R1_RUNFIX=0: the old way (loaded anyway). SSBU_RB_NET_TEST_REFUSE=<n> (TEST ONLY): every n-th load is
     * refused as if a thread were running (the timeline must not change). */
    {   static int runfix = -1, tn = -1; static long long tc;
        if (runfix < 0) { const char *e = getenv("SSBU_R1_RUNFIX"); runfix = !(e && e[0] == '0'); e = getenv("SSBU_RB_NET_TEST_REFUSE"); tn = e ? atoi(e) : 0; }
        int forced = tn > 0 && ++tc % tn == 0;
        if ((runfix && g_th_running) || forced) {
            unpark();
            Ctx *tc2; const char *nm2; HANDLE h2;                  /* the threads made after that frame (stop_born_threads) go on too */
            for (int i = g_ninfo; guest_thread_info(i, &tc2, &nm2, &h2); i++) if (h2 && GetThreadId(h2) != GetCurrentThreadId()) ResumeThread(h2);
            g_rg_refused++;
            if (g_rg_refused <= 20 || g_rg_refused % 100 == 0) LOG("r1: RING load of frame %lld (k %d) REFUSED (%lld so far): %s; every thread goes on, the rollback is tried again next tick\n",
                frame, k, g_rg_refused, forced ? "test switch SSBU_RB_NET_TEST_REFUSE" : "a thread still running guest code after 500 ms");
            g_rgp_seg = 1;
            return -1;
        }
    }
    double t1 = now_ms();
    R1_CTR("load (state of frame)", frame, 0);
    rgc_before();
    g_rg_ntl = 0;
    if (g_rgs_on > 0) rgv_before(si, r1_ring_newest());
    for (int i = g_rg_count - 1; i > si; i--) {           /* undo logs newest first: pointers only */
        RgSlot *s = rg_slot(i);
        for (size_t n = 0; n < s->nent; n++) {
            uint32_t id = s->ent[2 * n], old = s->ent[2 * n + 1];
            rg_pool_put(g_rg_idx[id]); g_rg_idx[id] = old; rg_touch(id);
        }
        rg_slot_drop(s, 0);
    }
    g_rg_count = si + 1;
    if (g_ls_active) ls_each(rg_load_page); else rg_sweep(rg_load_page);      /* written since the newest capture (R4: the learned set) */
    for (size_t e = 0; e < g_rg_nx; e++) rg_touch((uint32_t)(g_rg_n1 + g_rg_n2 + e));
    size_t pages = g_rg_ntl; g_rg_ldpg += (long long)pages;
    for (size_t n = 0; n < g_rg_ntl; n++) {               /* each page once, from its value at `frame` */
        uint32_t id = g_rg_tl[n]; g_rg_touch[id] = 0;
        uint8_t *a = rg_addr(id), *src = rg_src(id);
        if (src == (uint8_t *)1) continue;                 /* decommitted then: left as it is */
        if (id < g_rg_n1 && g_dc[id]) { if (!src) continue; VirtualAlloc(a, PG, MEM_COMMIT, PAGE_READWRITE); g_dc[id] = 0; }
        if (!src) memset(a, 0, PG); else cp_add(a, src);
    }
    {   /* (R3 net3) SSBU_RB_LOADCMP=1 (option, default off: measured no faster, na0 / na1): a page is written only if it differs from the value loaded */
        static int lc = -1; if (lc < 0) { const char *e = getenv("SSBU_RB_LOADCMP"); lc = e && e[0] == '1'; }
        g_cp_cond = lc; cp_run(); g_cp_cond = 0; }
    double t2 = now_ms();
    g_nfib = g->nfib; memcpy(g_fib, g->fib, (size_t)g->nfib * sizeof *g_fib);
    if (g->fpool_used) memcpy(g_fpool, g->fpool, g->fpool_used);
    g_fpool_used = g->fpool_used;
    restore_thread_regs(); double t2a = now_ms();
    fibers_restore_all(); double t2b = now_ms();
    rgc_after(); double t2c = now_ms();
    rg_cb_load(g);
    double t3 = now_ms();
    unpark();
    g_mp[4] += t2a - t2; g_mp[5] += t2b - t2a; g_mp[6] += t2c - t2b; g_mp[7] += t3 - t2c; g_mp[8] += now_ms() - t3; g_mp_n[1]++;
    *c = g->ctx;
    g_rg_loads++;
    if (g_th_mismatch || g_fib_diff) LOG("r1: RING load of frame %lld (k %d): %d threads not at their wait, %d fibers not restored\n", frame, k, g_th_mismatch, g_fib_diff);
    g_rg_last[1] = now_ms() - t0; g_rgp_seg = 1;
    if (k < RG_MAXSLOT && g_rgl_n[k] < RG_NL) { int i = g_rgl_n[k]++; g_rgl[k][i][0] = now_ms() - t0; g_rgl[k][i][1] = t1 - t0; g_rgl[k][i][2] = t2 - t1; g_rgl_pg[k][i] = pages; (void)t3; }
    return 1;
}

/* ---------------------------------------------------------------- the rollback simulation (SSBU_RB_SIM) */
static DWORD g_rgp_main; static int g_rgs_prof; extern volatile int g_rgp_seg; static void rgs_samp_start(void);
static int g_rgs_lastlite;                              /* R3 (SSBU_RB_NET_LASTLITE=1): the last re-run pass skips like the others (the real pass after it runs everything) */
static int g_rgs_on = -1, g_rgs_n = 10, g_rgs_k = 7, g_rgs_warm = 30, g_rgs_skip, g_rgs_left, g_rgs_ki;
static long long g_rgs_next, g_rgs_real_since, g_rgs_done, g_rgs_nocap, g_rgs_padmiss, g_rgs_ksum[RG_MAXSLOT];
static PadRec g_rgs_pad[256]; static long long g_rgs_padf[256]; static int g_rgs_polls_cur; static long long g_rgs_multi;
static double g_rgs_pass_t0, g_rgs_pass[2]; static long long g_rgs_npass[2];
static double g_rgs_pl[2], g_rgs_plv[2][RG_NST / 4]; static long long g_rgs_npl[2]; static int g_rgs_nplv[2];      /* (round 3) re-run passes: 0 = skipped ones (not the last), 1 = the last one */
static double g_rgs_pass_v[2][RG_NST / 4]; static int g_rgs_pass_nv[2];
static int rbg_on(void); static void rbg_step(Ctx *c); static void rbg_pad(int id, uint64_t *b, int *lx, int *ly, int *rx, int *ry); static void rbg_report(void); static void rbg_fight_off(void);   /* R3: rb_rollback.c */
static int rgs_wanted(void) {
    if (g_rgs_on < 0 && rbg_on()) { g_rgs_on = 1; g_rgs_n = 1 << 30; return 1; }      /* R3 online rollback (SSBU_RB_NET): the ring driven by the input queues */
    if (g_rgs_on < 0) { const char *e = getenv("SSBU_RB_SIM"); g_rgs_on = e && e[0] && e[0] != '0'; if (g_rgs_on) sscanf(e, "%d,%d,%d", &g_rgs_n, &g_rgs_k, &g_rgs_warm); if (g_rgs_k < 1) g_rgs_k = 1; if (g_rgs_k > RG_MAXSLOT - 1) g_rgs_k = RG_MAXSLOT - 1; if (g_rgs_n < 1) g_rgs_n = 1; }
    return g_rgs_on;
}
static void rgs_report(void);
/* (R3 hs16) the main thread's CPU cycles per pass (QueryThreadCycleTime): a measure of the code's own speed that other
 * processes on the PC disturb much less than wall time (A/B of builds on a busy PC) */
static ULONG64 g_rgs_cyc0; static double g_rgs_cyc[2];
static ULONG64 rgs_cyc_now(void) { ULONG64 v = 0; QueryThreadCycleTime(GetCurrentThread(), &v); return v; }
/* at the safe point (H2) of every pass while the fight runs */
static void rgs_step(Ctx *c) {
    double t = now_ms(); ULONG64 cy = rgs_cyc_now();
    g_rgp_main = GetCurrentThreadId(); g_rgp_seg = 1; if (g_rgs_prof == 3 && g_rg_ready) rgs_samp_start();
    if (g_rgs_pass_t0 > 0 && g_rg_ready) {                /* the pass that just ended: real (0) or re-run (1), without the capture / load */
        int kd = g_ra_phase == 2; double d = t - g_rgs_pass_t0; if (g_rgs_cyc0) g_rgs_cyc[kd] += (double)(cy - g_rgs_cyc0);
        if (kd) { int l = g_rgs_left <= 1; g_rgs_pl[l] += d; g_rgs_npl[l]++; if (g_rgs_nplv[l] < RG_NST / 4) g_rgs_plv[l][g_rgs_nplv[l]++] = d; }
        g_rgs_pass[kd] += d; g_rgs_npass[kd]++; if (g_rgs_pass_nv[kd] < RG_NST / 4) g_rgs_pass_v[kd][g_rgs_pass_nv[kd]++] = d;
    }
    if (g_rgs_polls_cur > 1) g_rgs_multi++;
    g_rgs_polls_cur = 0;
    if (g_ra_off || !ra_fight_on()) { ls_fight_off(); if (g_ra_phase) g_ra_after_quiet = 1; g_ra_phase = 0; g_rgs_left = 0; g_rgs_pass_t0 = 0; if (rbg_on()) rbg_fight_off(); return; }   /* (R3: the report once at the end of each match; hs12: the session reset for the next match) */
    if (rbg_on()) { rbg_step(c); g_rgs_pass_t0 = now_ms(); g_rgs_cyc0 = rgs_cyc_now(); g_ra_t_mark = g_rgs_pass_t0; return; }      /* R3: online rollback */
    long long i = g_rgs_next;                             /* the pass that ended here */
    if (g_rg_keep_on < 0) { const char *e = getenv("SSBU_RB_KEEP"); g_rg_keep_on = !e || atoi(e) != 0; }
    {   /* will a load follow this capture? then the threads stay held from the capture to the load (SSBU_RB_KEEP=0: not) */
        int ph = g_ra_phase == 2 && g_rgs_left - 1 <= 0 ? 0 : g_ra_phase;
        g_rg_keep = g_rg_keep_on && g_rg_ready && ph == 0 && g_rg_caps + 1 >= g_rgs_warm && g_rgs_real_since + (ph == 0) >= g_rgs_n;
    }
    int ok = r1_ring_capture(i, c);
    g_rg_keep = 0;
    if (g_rgv_target >= 0 && i == g_rgv_target) { if (ok) rgv_after(); else g_rgv_target = -1; }
    if (!g_rg_ready) { g_rgs_pass_t0 = 0; return; }
    if (!ok) g_rgs_nocap++;
    if (g_ra_phase == 2 && --g_rgs_left <= 0) { g_ra_phase = 0; g_ra_after_quiet = 1; g_rgs_left = 0; }
    g_rgs_next = i + 1;
    if (g_ra_phase == 0) g_rgs_real_since++;
    if (g_ra_phase == 0 && ok && g_rg_caps >= g_rgs_warm && g_rgs_real_since >= g_rgs_n) {
        int k = 1 + g_rgs_ki % g_rgs_k;
        long long target = i - k, best = -1;
        for (int s = 0; s < g_rg_count; s++) { long long f = rg_slot(s)->frame; if (f <= target && f > best) best = f; }
        if (best >= 0 && r1_ring_load(best, c) > 0) {
            g_rgs_ki++; g_rgs_real_since = 0; g_rgs_done++;
            g_rgs_left = (int)(i - best); g_rgs_next = best + 1; g_ra_phase = 2;
            if (g_rgs_left < RG_MAXSLOT) g_rgs_ksum[g_rgs_left]++;
            if (g_rgs_done <= 30 || g_rgs_done % 50 == 0) LOG("r1: RB SIM: rollback %lld at pass %lld (fight frame %lld) to pass %lld (k %d) in %.3f ms\n", g_rgs_done, i, g_ff, best, g_rgs_left, g_rg_last[1]);
        }
    }
    r1_ring_let_go();                                     /* (no load after a kept capture) */
    g_rgs_pass_t0 = now_ms(); g_ra_t_mark = g_rgs_pass_t0;
}
/* ---- logic-only re-run (R2_logic_only_study.md section 6): presentation-side callees of the main loop skipped in re-run passes
 * but the last (SSBU_RB_SIM_LITE=<mask>: 1 camera, 2 UI, 4 lighting, 8 render layers, 16 recorder, 32 dynamic resolution,
 * 64 effect emitter tick); only when called from the main loop FUN 0x7103747270. SSBU_RB_SIM_PROF=1: the same hooks time their
 * calls (real and re-run passes) without skipping anything. */
typedef struct { uint64_t a; int grp; uint64_t ret; ImportFn orig; long long n[2], sk; double ms[2]; } RgsLite;
static RgsLite g_rgl_h[] = {
    {0x710361d040ull, 1, 0}, {0x71036186d0ull, 1, 0}, {0x7103864700ull, 1, 0}, {0x7103619080ull, 1, 0}, {0x7103618b80ull, 1, 0},
    {0x7103632850ull, 2, 0}, {0x710361e7e0ull, 2, 0}, {0x71035c13d0ull, 2, 0},
    {0x7103575fa0ull, 4, 0}, {0x7103575c60ull, 4, 0}, {0x71035763c0ull, 4, 0}, {0x7103593c40ull, 4, 0},
    {0x71037857f0ull, 8, 0},
    {0x71036f2c00ull, 16, 0}, {0x71036f2d40ull, 16, 0}, {0x71036e6e00ull, 16, 0}, {0x71036f7410ull, 16, 1},
    {0x7103618ef0ull, 32, 0},
    {0x710356d7a0ull, 64, 0},
};
#define RGL_N ((int)(sizeof g_rgl_h / sizeof g_rgl_h[0]))
static int g_rgs_lite; static uint64_t g_rgl_lo, g_rgl_hi;
static void rgs_lite_hook(Ctx *c, void *user) {
    RgsLite *h = user; uint64_t lr = c->x[30];
    int inloop = lr >= g_rgl_lo && lr < g_rgl_hi, kd = g_ra_phase == 2;
    if (inloop && (g_rgs_lite & h->grp) && g_rgs_on > 0 && g_ra_phase == 2 && (g_rgs_left > 1 || g_rgs_lastlite)) { h->sk++; c->x[0] = h->ret; return; }
    if (!g_rgs_prof || !inloop) { h->orig(c); return; }
    double t = now_ms(); h->orig(c); h->ms[kd] += now_ms() - t; h->n[kd]++;
}
/* ---- SSBU_RB_SIM_PROF=2 (R2 round 3): where the logic step's time goes. The game's task tree walker FUN 0x7103724a80
 * (main_loop calls it at 0x710374c624 per sim step; it calls node->vt[0x28], the children through vt[0x28] of the child
 * container with a callback that re-enters the walker, then node->vt[0x30]) is timed per node class (vtable), exclusive of
 * its children, on the main thread; the job-counter waits by return address; the job runner FUN 0x7103548240 per job
 * (worker CPU time, keyed by the job object's vtable). Real and re-run passes apart. */
#define RGP_N 4096
typedef struct { uint64_t key; long long n[2]; double incl[2], excl[2]; int dmin; } RgpE;
static RgpE g_rgp[RGP_N], g_rgpj[RGP_N], g_rgpw[64]; static SRWLOCK g_rgpj_lock = SRWLOCK_INIT;
static ImportFn g_rgp_orig_tree, g_rgp_orig_run, g_rgp_orig_x[8]; static int g_rgp_depth; static double g_rgp_child[256];
static RgpE *rgp_slot(RgpE *t, int cap, uint64_t key) {
    unsigned h = (unsigned)((key * 0x9e3779b97f4a7c15ull) >> 52) % (unsigned)cap;
    for (int i = 0; i < cap; i++) { RgpE *e = &t[(h + (unsigned)i) % (unsigned)cap]; if (e->key == key) return e; if (!e->key) { e->key = key; e->dmin = 999; return e; } }
    return NULL;
}
static void rgp_tree(Ctx *c, void *user) {
    (void)user;
    if (!g_rg_ready || GetCurrentThreadId() != g_rgp_main || g_rgp_depth >= 255) { g_rgp_orig_tree(c); return; }
    uint64_t node = c->x[0], vt = node ? rd64(node) : 0, lr = c->x[30]; int kd = g_ra_phase == 2, d = g_rgp_depth;
    g_rgp_child[d + 1] = 0; g_rgp_depth++;
    double t0 = now_ms(); g_rgp_orig_tree(c); double dt = now_ms() - t0;
    g_rgp_depth--;
    RgpE *e = rgp_slot(g_rgp, RGP_N, vt ? vt : 1);
    if (e) { e->n[kd]++; e->incl[kd] += dt; e->excl[kd] += dt - g_rgp_child[d + 1]; if (d < e->dmin) e->dmin = d; }
    g_rgp_child[d] += dt;
    if (d == 0) { RgpE *w = rgp_slot(g_rgpw, 64, lr | (1ull << 62)); if (w) { w->n[kd]++; w->incl[kd] += dt; } }
}
static void rgp_run(Ctx *c, void *user) {          /* the job runner (task workers): x0 = the job's node */
    (void)user;
    if (!g_rg_ready) { g_rgp_orig_run(c); return; }
    uint64_t node = c->x[0], vt = node ? rd64(node - 8) : 0; int kd = g_ra_phase == 2;
    double t0 = now_ms(); g_rgp_orig_run(c); double dt = now_ms() - t0;
    AcquireSRWLockExclusive(&g_rgpj_lock);
    RgpE *e = rgp_slot(g_rgpj, RGP_N, vt ? vt : 1); if (e) { e->n[kd]++; e->incl[kd] += dt; }
    ReleaseSRWLockExclusive(&g_rgpj_lock);
}
/* a few single functions of the logic step, timed when called on main: poll, dispatch, the EffectManager update */
#define RGP_NX 4
static uint64_t g_rgp_xa[RGP_NX];                        /* (the running build's addresses, set by rgp_init) */
static void rgp_x(Ctx *c, void *user) {
    int i = (int)(intptr_t)user; int kd = g_ra_phase == 2;
    if (!g_rg_ready || GetCurrentThreadId() != g_rgp_main) { g_rgp_orig_x[i](c); return; }
    double t0 = now_ms(); g_rgp_orig_x[i](c); double dt = now_ms() - t0;
    RgpE *w = rgp_slot(g_rgpw, 64, g_rgp_xa[i] | (2ull << 62)); if (w) { w->n[kd]++; w->incl[kd] += dt; }
}
static void rgp_wait_note(uint64_t lr, double dt) {      /* from ra_kick: a job-counter wait on main */
    if (g_rgs_prof < 2 || !g_rg_ready || GetCurrentThreadId() != g_rgp_main) return;
    RgpE *w = rgp_slot(g_rgpw, 64, lr | (3ull << 62)); if (w) { w->n[g_ra_phase == 2]++; w->incl[g_ra_phase == 2] += dt; }
}
static void rgp_init(void) {
    g_rgp_xa[0] = GA(0x71036676e0ull); g_rgp_xa[1] = GA(0x710375ea70ull); g_rgp_xa[2] = GA(0x710355b540ull); g_rgp_xa[3] = GA(0x7103569c70ull);
    int a = hook_install(GA(0x7103724a80ull), rgp_tree, NULL, &g_rgp_orig_tree), b = hook_install(GA(0x7103548240ull), rgp_run, NULL, &g_rgp_orig_run), x = 0;
    for (int i = 0; i < RGP_NX; i++) x += hook_install(g_rgp_xa[i], rgp_x, (void *)(intptr_t)i, &g_rgp_orig_x[i]);
    LOG("r1: RB SIM profile 2: task tree walker %s, job runner %s, %d of %d single functions\n", a ? "hooked" : "FAILED", b ? "hooked" : "FAILED", x, RGP_NX);
}
static int rgp_cmp(const void *a, const void *b) { const RgpE *x = a, *y = b; double p = x->excl[0] + x->incl[0] * 1e-9, q = y->excl[0] + y->incl[0] * 1e-9; return p < q ? 1 : p > q ? -1 : 0; }
static int rgp_cmpi(const void *a, const void *b) { const RgpE *x = a, *y = b; return x->incl[0] < y->incl[0] ? 1 : x->incl[0] > y->incl[0] ? -1 : 0; }
/* ---- SSBU_RB_SIM_PROF=3: a SAMPLING profile of the main thread during the fight (one helper thread suspends main every
 * ~100 us, walks its host stack with the x64 unwind data and maps every frame to the lifted guest function it is in,
 * guest_function_of). Per segment of the pass (1 = safe point -> poll, 2 = poll -> frame end = the logic step, 3 = frame
 * end -> safe point) and pass kind (real / re-run): self and inclusive sample counts per guest function. Test only. */
uint64_t guest_function_of(uint64_t rip);
volatile int g_rgp_seg;                                     /* set by r1_poll / r1_frame_end / rgs_step */
#define RGS_SN 65536
typedef struct { uint64_t fn; long long self[2][6], incl[2][6]; } RgsS;
static RgsS g_rgss[RGS_SN]; static long long g_rgss_tot[2][6], g_rgss_host[2][6]; static HANDLE g_rgss_main; static volatile int g_rgss_run;
static RgsS *rgss_slot(uint64_t fn) {
    unsigned h = (unsigned)((fn * 0x9e3779b97f4a7c15ull) >> 48) % RGS_SN;
    for (int i = 0; i < 64; i++) { RgsS *e = &g_rgss[(h + (unsigned)i) % RGS_SN]; if (e->fn == fn) return e; if (!e->fn) { e->fn = fn; return e; } }
    return NULL;
}
static DWORD WINAPI rgss_thread(void *p) {
    (void)p; uint64_t fr[64]; int nf;
    double next = now_ms();
    for (;;) {
        while (now_ms() < next) YieldProcessor();
        next += 0.1;
        int seg = g_rgp_seg, kd = g_ra_phase == 2;
        if (kd && g_rgs_left <= 1 && seg < 4) continue;              /* (the last re-run pass runs everything: not counted) */
        if (!g_rg_ready || !g_rgss_main || seg < 1 || seg > 5) { if (now_ms() > next + 5) next = now_ms(); continue; }
        if (SuspendThread(g_rgss_main) == (DWORD)-1) continue;
        CONTEXT x; memset(&x, 0, sizeof x); x.ContextFlags = CONTEXT_FULL; nf = 0;
        if (GetThreadContext(g_rgss_main, &x)) {
            for (int d = 0; d < 64 && nf < 64; d++) {
                DWORD64 base = 0; PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(x.Rip, &base, NULL);
                uint64_t g = guest_function_of(x.Rip);
                if (g && (!nf || fr[nf - 1] != g)) fr[nf++] = g; else if (!g && d == 0) fr[nf++] = ((uint64_t)x.Rip >> 4 << 4) | (1ull << 63);      /* 1 = host code (imports, hooks); capture / load: the host address */
                if (!f) break;
                void *hd; DWORD64 est; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, x.Rip, f, &x, &hd, &est, NULL);
                if (!x.Rip) break;
            }
        }
        ResumeThread(g_rgss_main);
        if (!nf) continue;
        g_rgss_tot[kd][seg]++;
        RgsS *e = rgss_slot(fr[0]); if (e) e->self[kd][seg]++;
        if (fr[0] == 1 || (fr[0] >> 63)) g_rgss_host[kd][seg]++;
        for (int i = 0; i < nf; i++) { int dup = 0; for (int j = 0; j < i; j++) if (fr[j] == fr[i]) dup = 1; if (dup) continue; RgsS *q = rgss_slot(fr[i]); if (q) q->incl[kd][seg]++; }
    }
    return 0;
}
static void rgs_samp_start(void) {
    if (g_rgss_run || g_rgs_prof != 3) return;
    g_rgss_run = 1;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_rgss_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(CreateThread(NULL, 0, rgss_thread, NULL, 0, NULL));
    LOG("r1: RB PROF 3: sampling the main thread every 0.1 ms during the fight\n");
}
static int g_rgss_k[3];
static int rgss_cmp(const void *a, const void *b) {
    const RgsS *x = a, *y = b; int k = g_rgss_k[0];
    int sg = g_rgss_k[2]; long long p = g_rgss_k[1] ? x->incl[k][sg] : x->self[k][sg], q = g_rgss_k[1] ? y->incl[k][sg] : y->self[k][sg];
    return p < q ? 1 : p > q ? -1 : 0;
}
static RgsS g_rgss_c[RGS_SN];
static void rgs_samp_report(void) {
    if (!g_rgss_run) return;
    long long np = g_rgs_npass[0] ? g_rgs_npass[0] : 1, nr = g_rgs_npl[0] ? g_rgs_npl[0] : 1;      /* re-run = the skipped passes only */
    LOG("r1: RB PROF 3 samples (0.1 ms each) real / re-run: start->poll %lld / %lld, logic %lld / %lld, frame end->safe point %lld / %lld; host code in the logic %lld / %lld; passes %lld / %lld\n",
        g_rgss_tot[0][1], g_rgss_tot[1][1], g_rgss_tot[0][2], g_rgss_tot[1][2], g_rgss_tot[0][3], g_rgss_tot[1][3], g_rgss_host[0][2], g_rgss_host[1][2], np, nr);
    LOG("r1: RB PROF 3 samples in captures %lld / %lld, loads %lld / %lld (host code %lld / %lld)\n", g_rgss_tot[0][4], g_rgss_tot[1][4], g_rgss_tot[0][5], g_rgss_tot[1][5], g_rgss_host[0][4] + g_rgss_host[0][5], g_rgss_host[1][4] + g_rgss_host[1][5]);
    memcpy(g_rgss_c, g_rgss, sizeof g_rgss);
    static const char *sgn[6] = {"", "start->poll", "logic", "frame end->safe point", "capture", "load"};
    for (int sg = 1; sg <= 5; sg++) for (int kd = 0; kd < 2; kd++) for (int inc = 1; inc >= 0; inc--) {
        if (sg >= 4 && kd == 1) continue;                    /* captures / loads: one list (real + re-run passes together) */
        if (sg != 2 && kd == 0 && sg < 4) continue;          /* start->poll / tail: the re-run passes only */
        g_rgss_k[0] = kd; g_rgss_k[1] = inc; g_rgss_k[2] = sg;
        if (sg >= 4) for (int i = 0; i < RGS_SN; i++) { g_rgss_c[i].self[0][sg] += g_rgss[i].self[1][sg]; g_rgss_c[i].incl[0][sg] += g_rgss[i].incl[1][sg]; }
        qsort(g_rgss_c, RGS_SN, sizeof *g_rgss_c, rgss_cmp);
        long long tot = sg >= 4 ? g_rgss_tot[0][sg] + g_rgss_tot[1][sg] : g_rgss_tot[kd][sg]; long long div = sg >= 4 ? (g_rg_caps + g_rg_loads ? (sg == 4 ? g_rg_caps : g_rg_loads) : 1) : (kd ? nr : np);
        for (int i = 0; i < (sg == 2 ? 80 : 40); i++) {
            RgsS *e = &g_rgss_c[i]; long long v = inc ? e->incl[kd][sg] : e->self[kd][sg]; if (!v) break;
            LOG("r1: RB PROF 3 %s %s %s %llx: %.4f ms per %s (%lld of %lld samples); self %.4f\n", kd ? "re-run" : "real", sgn[sg], inc ? "incl" : "self", (unsigned long long)e->fn,
                v * 0.1 / div, sg == 4 ? "capture" : sg == 5 ? "load" : "pass", v, tot, e->self[kd][sg] * 0.1 / div);
        }
        memcpy(g_rgss_c, g_rgss, sizeof g_rgss);
    }
}
static RgpE g_rgp_s[RGP_N];
static void rgp_report(void) {
    if (g_rgs_prof < 2) return;
    rgs_samp_report();
    if (g_rgs_prof != 2) return;
    long long np = g_rgs_npass[0] ? g_rgs_npass[0] : 1, nr = g_rgs_npass[1] ? g_rgs_npass[1] : 1;
    LOG("r1: RB PROF per pass (real %lld, re-run %lld): ms per pass real / re-run\n", g_rgs_npass[0], g_rgs_npass[1]);
    memcpy(g_rgp_s, g_rgpw, sizeof g_rgpw); qsort(g_rgp_s, 64, sizeof *g_rgp_s, rgp_cmpi);
    for (int i = 0; i < 64; i++) if (g_rgp_s[i].key) { uint64_t k = g_rgp_s[i].key; int ty = (int)(k >> 62); k &= ~(3ull << 62);
        LOG("r1: RB PROF %s %llx: calls %lld / %lld, %.4f / %.4f ms per pass\n", ty == 1 ? "tree walk from" : ty == 2 ? "function" : "wait at", (unsigned long long)k, g_rgp_s[i].n[0], g_rgp_s[i].n[1], g_rgp_s[i].incl[0] / np, g_rgp_s[i].incl[1] / nr); }
    memcpy(g_rgp_s, g_rgp, sizeof g_rgp); qsort(g_rgp_s, RGP_N, sizeof *g_rgp_s, rgp_cmp);
    for (int i = 0; i < RGP_N && i < 80; i++) if (g_rgp_s[i].key) LOG("r1: RB PROF node vt %llx depth %d: calls/pass %.2f / %.2f, exclusive %.4f / %.4f ms per pass, inclusive %.4f / %.4f\n",
        (unsigned long long)g_rgp_s[i].key, g_rgp_s[i].dmin, g_rgp_s[i].n[0] / (double)np, g_rgp_s[i].n[1] / (double)nr, g_rgp_s[i].excl[0] / np, g_rgp_s[i].excl[1] / nr, g_rgp_s[i].incl[0] / np, g_rgp_s[i].incl[1] / nr);
    AcquireSRWLockExclusive(&g_rgpj_lock); memcpy(g_rgp_s, g_rgpj, sizeof g_rgpj); ReleaseSRWLockExclusive(&g_rgpj_lock); qsort(g_rgp_s, RGP_N, sizeof *g_rgp_s, rgp_cmpi);
    for (int i = 0; i < RGP_N && i < 60; i++) if (g_rgp_s[i].key) LOG("r1: RB PROF job vt %llx: jobs/pass %.2f / %.2f, worker ms per pass %.4f / %.4f\n",
        (unsigned long long)g_rgp_s[i].key, g_rgp_s[i].n[0] / (double)np, g_rgp_s[i].n[1] / (double)nr, g_rgp_s[i].incl[0] / np, g_rgp_s[i].incl[1] / nr);
}
static void rgs_lite_init(void) {
    const char *e = getenv("SSBU_RB_SIM_LITE"); g_rgs_lite = e ? atoi(e) : 0; g_rgs_prof = getenv("SSBU_RB_SIM_PROF") ? atoi(getenv("SSBU_RB_SIM_PROF")) : 0;
    if (g_rgs_prof == 2) rgp_init();
    if (!g_rgs_lite && !g_rgs_prof) return;
    g_rgl_lo = GA(0x7103747270ull); g_rgl_hi = GA(0x710374e000ull);
    int ok = 0;
    for (int i = 0; i < RGL_N; i++) if ((g_rgs_prof || (g_rgs_lite & g_rgl_h[i].grp)) && hook_install(GA(g_rgl_h[i].a), rgs_lite_hook, &g_rgl_h[i], &g_rgl_h[i].orig)) ok++;
    LOG("r1: RB SIM logic-only re-run: skip mask %d, profile %d: %d hooks installed\n", g_rgs_lite, g_rgs_prof, ok);
}
static void rgs_lite_report(void) {
    if (!g_rgs_lite && !g_rgs_prof) return;
    for (int i = 0; i < RGL_N; i++) if (g_rgl_h[i].orig) LOG("r1: RB SIM lite %llx (group %d): skipped %lld; timed real %lld calls %.4f ms/call, re-run %lld calls %.4f ms/call\n", (unsigned long long)g_rgl_h[i].a, g_rgl_h[i].grp, g_rgl_h[i].sk,
        g_rgl_h[i].n[0], g_rgl_h[i].n[0] ? g_rgl_h[i].ms[0] / g_rgl_h[i].n[0] : 0, g_rgl_h[i].n[1], g_rgl_h[i].n[1] ? g_rgl_h[i].ms[1] / g_rgl_h[i].n[1] : 0);
}
/* ---- SSBU_RB_SIM_VERIFY=1: every rollback, the whole tracked state is compared, not only the fighters' hash: at the load, a hash of the
 * NEWEST value of every page the newer slots changed; when the re-run is back at that pass, the same pages (and every page the re-run
 * wrote that the original window did not: against its value at the loaded state). Window pages and extras counted apart. */
static int g_rgv_on = -1; static uint8_t *g_rgv_in; static uint32_t *g_rgv_id; static uint64_t *g_rgv_h; static size_t g_rgv_n;
#define RGV_MAX (8u << 20)
static long long g_rgv_target = -1, g_rgv_from, g_rgv_runs, g_rgv_bad_runs, g_rgv_pages, g_rgv_diff[3];
static uint64_t rgv_hash(const uint8_t *p) {
    if (!p) return 0x9e3779b97f4a7c15ull;
    if (p == (uint8_t *)1) return 1;
    const uint64_t *q = (const uint64_t *)p; uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < PG / 8; i++) h = (h ^ q[i]) * 1099511628211ull + (h >> 29);
    return h;
}
static void rgvd_keep(size_t n, const uint8_t *src); static void rgvd_show(size_t n, uint32_t id, const uint8_t *now);
static uint8_t *rg_src_v(uint32_t id, uint32_t v) { uint32_t s = g_rg_idx[id]; g_rg_idx[id] = v; uint8_t *r = rg_src(id); g_rg_idx[id] = s; return r; }
static void rgv_before(int si, long long target) {        /* in r1_ring_load, before the undo logs are applied */
    if (g_rgv_on < 0) g_rgv_on = getenv("SSBU_RB_SIM_VERIFY") ? (atoi(getenv("SSBU_RB_SIM_VERIFY")) > 1 ? 2 : 1) : 0;
    if (!g_rgv_on) return;
    size_t tot = g_rg_n1 + g_rg_n2 + g_rg_nx;
    if (!g_rgv_in) { g_rgv_in = calloc(tot, 1); g_rgv_id = malloc((size_t)RGV_MAX * 4); g_rgv_h = malloc((size_t)RGV_MAX * 8); }
    g_rgv_n = 0;
    for (int i = g_rg_count - 1; i > si; i--) {
        RgSlot *s = rg_slot(i);
        for (size_t n = 0; n < s->nent; n++) {
            uint32_t id = s->ent[2 * n]; if (g_rgv_in[id] || g_rgv_n >= RGV_MAX / 2) continue;
            g_rgv_in[id] = 1; g_rgv_id[g_rgv_n] = id; g_rgv_h[g_rgv_n] = rgv_hash(rg_src(id)); rgvd_keep(g_rgv_n, rg_src(id)); g_rgv_n++;
        }
    }
    g_rgv_target = target; g_rgv_from = rg_slot(si)->frame;
}
/* (R2 round 3) SSBU_RB_SIM_VERIFY=2: for the first 12 rollbacks the original bytes of the compared pages are kept and every
 * differing page is printed: its address (where()), the number of differing 8-byte words, the first 4 (offset, original, re-run). */
#define RGVD_MAX 8192
static uint8_t *g_rgvd; static long long g_rgvd_lines;
static void rgvd_keep(size_t n, const uint8_t *src) {
    if (g_rgv_on < 2 || g_rgv_runs >= 12 || n >= RGVD_MAX) return;
    if (!g_rgvd) g_rgvd = VirtualAlloc(NULL, (size_t)RGVD_MAX * PG, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_rgvd) return;
    if (!src) memset(g_rgvd + n * PG, 0, PG); else if (src == (uint8_t *)1) memset(g_rgvd + n * PG, 0xdd, PG); else memcpy(g_rgvd + n * PG, src, PG);
}
static void rgvd_show(size_t n, uint32_t id, const uint8_t *now) {
    if (g_rgv_on < 2 || g_rgv_runs >= 12 || n >= RGVD_MAX || !g_rgvd || g_rgvd_lines > 6000) return;
    static uint8_t z[PG]; if (!now) now = z; if (now == (uint8_t *)1) return;
    const uint64_t *a = (const uint64_t *)(g_rgvd + n * PG), *b = (const uint64_t *)now; int nd = 0; char t[400]; int o = 0; t[0] = 0;
    for (int i = 0; i < PG / 8; i++) if (a[i] != b[i]) { nd++; if (nd <= 4 && o < 360) o += snprintf(t + o, sizeof t - o, " +%03x: %llx -> %llx", i * 8, (unsigned long long)a[i], (unsigned long long)b[i]); }
    g_rgvd_lines++;
    LOG("r1: RB VERIFY page %s (%p) rollback %lld: %d words differ;%s\n", where(rg_addr(id)), (void *)rg_addr(id), g_rgv_runs, nd, t);
}
static int rgv_kind(uint32_t id) { return id < g_rg_n1 ? 0 : id < g_rg_n1 + g_rg_n2 ? 1 : 2; }
/* differing pages by 64 KB block (where() name of the block), over every rollback: which memory is not reproduced */
#define RGV_NB 4096
static struct { uint64_t blk; long long n, runs; long long last; } g_rgv_b[RGV_NB]; static int g_rgv_nb;
static void rgv_note(uint32_t id) {
    uint64_t blk = (uint64_t)(uintptr_t)rg_addr(id) >> 16;
    for (int i = 0; i < g_rgv_nb; i++) if (g_rgv_b[i].blk == blk) { g_rgv_b[i].n++; if (g_rgv_b[i].last != g_rgv_runs) { g_rgv_b[i].runs++; g_rgv_b[i].last = g_rgv_runs; } return; }
    if (g_rgv_nb < RGV_NB) { g_rgv_b[g_rgv_nb].blk = blk; g_rgv_b[g_rgv_nb].n = 1; g_rgv_b[g_rgv_nb].runs = 1; g_rgv_b[g_rgv_nb].last = g_rgv_runs; g_rgv_nb++; }
}
static int rgv_cmpb(const void *a, const void *b) { long long x = ((const long long *)a)[1], y = ((const long long *)b)[1]; return x < y ? 1 : x > y ? -1 : 0; }
static void rgv_blocks_report(void) {
    if (!g_rgv_nb) return;
    qsort(g_rgv_b, (size_t)g_rgv_nb, sizeof g_rgv_b[0], rgv_cmpb);
    char b[3000]; int o = 0;
    for (int i = 0; i < g_rgv_nb && i < 40 && o < 2900; i++) o += snprintf(b + o, sizeof b - o, " [%s: %lld pages, %lld rollbacks]", where((uint8_t *)(uintptr_t)(g_rgv_b[i].blk << 16)), g_rgv_b[i].n, g_rgv_b[i].runs);
    b[o] = 0;
    LOG("r1: RB VERIFY differing 64 KB blocks (%d, the 40 largest):%s\n", g_rgv_nb, b);
}
static void rgv_after(void) {                               /* right after the capture of the re-run pass that reached the target */
    long long d[3] = {0, 0, 0}, pages = (long long)g_rgv_n; char first[400]; int o = 0; first[0] = 0;
    for (size_t n = 0; n < g_rgv_n; n++) {
        uint32_t id = g_rgv_id[n];
        if (rgv_hash(rg_src(id)) != g_rgv_h[n]) { int k = rgv_kind(id); d[k]++; rgv_note(id); rgvd_show(n, id, rg_src(id)); if (o < 330 && d[k] <= 3) o += snprintf(first + o, sizeof first - o, " %s", where(rg_addr(id))); }
    }
    size_t extra = 0;
    for (int i = 0; i < g_rg_count; i++) {                  /* pages the re-run wrote that the original window did not */
        RgSlot *s = rg_slot(i); if (s->frame <= g_rgv_from || s->frame > g_rgv_target) continue;
        for (size_t n = 0; n < s->nent; n++) {
            uint32_t id = s->ent[2 * n]; if (g_rgv_in[id]) continue;
            if (g_rgv_n + extra >= RGV_MAX) continue;
            g_rgv_in[id] = 2; g_rgv_id[g_rgv_n + extra++] = id;
            pages++;
            if (rgv_hash(rg_src(id)) != rgv_hash(rg_src_v(id, s->ent[2 * n + 1]))) { int k = rgv_kind(id); d[k]++; rgv_note(id); if (o < 330 && d[k] <= 3) o += snprintf(first + o, sizeof first - o, " %s(re-run only)", where(rg_addr(id))); }
        }
    }
    for (size_t n = 0; n < g_rgv_n + extra; n++) g_rgv_in[g_rgv_id[n]] = 0;
    g_rgv_runs++; g_rgv_pages += pages; for (int k = 0; k < 3; k++) g_rgv_diff[k] += d[k];
    if (d[0] || d[1]) g_rgv_bad_runs++;
    if (d[0] || d[1] || d[2] || g_rgv_runs <= 5) LOG("r1: RB VERIFY rollback to %lld, back at %lld: %lld pages compared; differ: window 1 %lld, window 2 %lld, extras %lld;%s\n", g_rgv_from, g_rgv_target, pages, d[0], d[1], d[2], first);
    g_rgv_target = -1;
}
static int rgs_skip_now(void) { return g_rgs_on > 0 && g_ra_phase == 2 && (g_rgs_left > 1 || g_rgs_lastlite) ? g_rgs_skip : 0; }
static int rgs_poll(void) { g_rgs_polls_cur++; return g_ra_phase == 2; }    /* a re-run pass: the input sequence waits */
static void rgs_pad(uint64_t *b, int *lx, int *ly, int *rx, int *ry) {
    if (rbg_on()) { rbg_pad(0, b, lx, ly, rx, ry); return; }      /* R3 */
    long long p = g_rgs_next; int x = (int)(p & 255);
    if (g_ra_phase == 2) {
        if (g_rgs_padf[x] == p) { PadRec r = g_rgs_pad[x]; *b = r.b; *lx = r.lx; *ly = r.ly; *rx = r.rx; *ry = r.ry; } else g_rgs_padmiss++;
    } else { PadRec r = {*b, *lx, *ly, *rx, *ry}; g_rgs_pad[x] = r; g_rgs_padf[x] = p; }
}
static int cmp_d(const void *a, const void *b);
static double rg_pct(const double *v, int n, int p) {
    if (n <= 0) return 0;
    double *w = malloc((size_t)n * sizeof *w); memcpy(w, v, (size_t)n * sizeof *w); qsort(w, (size_t)n, sizeof *w, cmp_d);
    double r = w[(size_t)n * p / 100 < (size_t)n ? (size_t)n * p / 100 : (size_t)n - 1]; free(w); return r;
}
static double rg_mean(const double *v, int n) { double s = 0; for (int i = 0; i < n; i++) s += v[i]; return n ? s / n : 0; }
static void rgs_report(void) {
    ls_report();                                          /* R4 */
    if (!g_rg_ready) { if (rbg_on()) rbg_report(); return; }      /* (hs14: a match without the ring has its report at the exit too) */
    rbg_report();
    LOG("r1: RING memory (hs14, fix %s): captures given up for lack of pool memory %lld (pages put back and marked again), old states freed early %lld, captures skipped to keep the oldest needed state %lld; test refusals %lld\n",
        g_rg_poolfix ? "on" : "OFF", g_rg_short_n, g_rg_freed_slots, g_rg_skip_full, g_rg_tp_refused);
    int n = g_rgc_n; double pg = 0, xp = 0; for (int i = 0; i < n; i++) { pg += (double)g_rgc_pg[i]; xp += (double)g_rgc_xp[i]; }
    LOG("r1: RING report: %lld captures (%lld skipped: a thread not at a wait), %lld loads, %u pool buffers made (%.1f MB), %u free; log overflows %lld\n",
        g_rg_caps, g_rg_cap_fail, g_rg_loads, g_rg_bump, g_rg_bump * (double)PG / 1048576.0, g_rg_nfree, g_rg_overflow);
    LOG("r1: RING loads right after a capture with the threads kept held: %lld (threads kept at the same wait: %lld); fibers %d (last); stale fiber-block notes refused %lld\n", g_rg_kept_n, g_rg_kept_same, g_nfib, g_r1_fls_stale);
    LOG("r1: RING capture: median %.3f mean %.3f p99 %.3f ms; parts (means): hold %.3f, sweep + undo log %.3f, page copy (parallel) %.3f, fibers + threads + command buffers + let go %.3f; pages %.0f + extras %.0f mean (%.1f MB)\n",
        rg_pct(g_rgc[0], n, 50), rg_mean(g_rgc[0], n), rg_pct(g_rgc[0], n, 99), rg_mean(g_rgc[1], n), rg_mean(g_rgc[2], n), rg_mean(g_rgc[3], n), rg_mean(g_rgc[4], n), n ? pg / n : 0, n ? xp / n : 0, n ? (pg + xp) / n * PG / 1048576.0 : 0);
    {   long long w = 0; for (int q = 0; q < 9; q++) w += g_cp_wrote[q];
        LOG("r1: RING loads (net3, SSBU_RB_LOADCMP): pages to load %lld, of them written (they differed) %lld (%.1f%%)\n", g_rg_ldpg, w, g_rg_ldpg ? 100.0 * (double)w / (double)g_rg_ldpg : 0.0); }
    for (int k = 0; k < RG_MAXSLOT; k++) {
        int m = g_rgl_n[k]; if (!m) continue;
        static double v[RG_NL], h[RG_NL], cpy[RG_NL]; double p = 0;
        for (int i = 0; i < m; i++) { v[i] = g_rgl[k][i][0]; h[i] = g_rgl[k][i][1]; cpy[i] = g_rgl[k][i][2]; p += (double)g_rgl_pg[k][i]; }
        LOG("r1: RING load k %d (%d): median %.3f mean %.3f p99 %.3f ms (hold %.3f, undo + page copy %.3f, rest %.3f); pages to load %.0f mean (%.1f MB)\n",
            k, m, rg_pct(v, m, 50), rg_mean(v, m), rg_pct(v, m, 99), rg_mean(h, m), rg_mean(cpy, m), rg_mean(v, m) - rg_mean(h, m) - rg_mean(cpy, m), p / m, p / m * PG / 1048576.0);
    }
    if (g_rgs_on > 0) {
        if (g_rgv_on > 0) rgv_blocks_report();
        if (g_rgv_on > 0) LOG("r1: RB VERIFY: %lld rollbacks checked, %lld with window pages differing; pages compared %lld; differ: window 1 %lld, window 2 %lld, extras %lld\n", g_rgv_runs, g_rgv_bad_runs, g_rgv_pages, g_rgv_diff[0], g_rgv_diff[1], g_rgv_diff[2]);
        rgs_lite_report();
        if (g_rgpg_on > 0) {
            static struct { uint64_t blk; long long n[3]; } c[RGPG_N]; memcpy(c, g_rgpg, sizeof c);
            for (int i = 0; i < RGPG_N; i++) for (int j = i + 1; j < RGPG_N; j++) if (c[j].n[0] > c[i].n[0]) { uint64_t b = c[i].blk; long long n0 = c[i].n[0], n1 = c[i].n[1], n2 = c[i].n[2]; c[i] = c[j]; c[j].blk = b; c[j].n[0] = n0; c[j].n[1] = n1; c[j].n[2] = n2; }
            double t0 = 0, t1 = 0; for (int i = 0; i < RGPG_N; i++) { t0 += c[i].n[0]; t1 += c[i].n[1]; }
            LOG("r1: RB PAGES captures real %lld / re-run skipped %lld / last re-run %lld; pages per capture real %.0f, re-run skipped %.0f\n", g_rgpg_caps[0], g_rgpg_caps[1], g_rgpg_caps[2], t0 / (g_rgpg_caps[0] ? g_rgpg_caps[0] : 1), t1 / (g_rgpg_caps[1] ? g_rgpg_caps[1] : 1));
            for (int i = 0; i < 60 && c[i].n[0]; i++) LOG("r1: RB PAGES block %s (1 MB): per capture real %.1f, re-run skipped %.1f, last re-run %.1f\n", where((uint8_t *)(uintptr_t)(c[i].blk << 20)),
                c[i].n[0] / (double)(g_rgpg_caps[0] ? g_rgpg_caps[0] : 1), c[i].n[1] / (double)(g_rgpg_caps[1] ? g_rgpg_caps[1] : 1), c[i].n[2] / (double)(g_rgpg_caps[2] ? g_rgpg_caps[2] : 1));
        }
        rgp_report();
        LOG("r1: RB SIM: %lld rollbacks (k: 1 %lld, 2 %lld, 3 %lld, 4 %lld, 5 %lld, 6 %lld, 7 %lld), captures failed %lld, re-run pads missing %lld, passes with more than one poll %lld\n",
            g_rgs_done, g_rgs_ksum[1], g_rgs_ksum[2], g_rgs_ksum[3], g_rgs_ksum[4], g_rgs_ksum[5], g_rgs_ksum[6], g_rgs_ksum[7], g_rgs_nocap, g_rgs_padmiss, g_rgs_multi);
        LOG("r1: RB SIM passes (safe point to safe point, without the capture / load): real %lld mean %.3f median %.3f ms; re-run %lld mean %.3f median %.3f ms (kick skip mask in re-run passes but the last: %d)\n",
            g_rgs_npass[0], g_rgs_npass[0] ? g_rgs_pass[0] / g_rgs_npass[0] : 0, rg_pct(g_rgs_pass_v[0], g_rgs_pass_nv[0], 50), g_rgs_npass[1], g_rgs_npass[1] ? g_rgs_pass[1] / g_rgs_npass[1] : 0, rg_pct(g_rgs_pass_v[1], g_rgs_pass_nv[1], 50), g_rgs_skip);
        LOG("r1: RB SIM main-thread CPU per pass (safe point to safe point, without the capture / load; QueryThreadCycleTime): real %.3f, re-run %.3f million cycles\n",
            g_rgs_npass[0] ? g_rgs_cyc[0] / g_rgs_npass[0] / 1e6 : 0, g_rgs_npass[1] ? g_rgs_cyc[1] / g_rgs_npass[1] / 1e6 : 0);
        LOG("r1: RB SIM re-run passes but the last (the replayed frames a rollback pays for): %lld mean %.3f median %.3f ms; last re-run passes %lld mean %.3f median %.3f ms\n", g_rgs_npl[0], g_rgs_npl[0] ? g_rgs_pl[0] / g_rgs_npl[0] : 0, rg_pct(g_rgs_plv[0], g_rgs_nplv[0], 50), g_rgs_npl[1], g_rgs_npl[1] ? g_rgs_pl[1] / g_rgs_npl[1] : 0, rg_pct(g_rgs_plv[1], g_rgs_nplv[1], 50));
        for (int k = 0; k < 3; k += 2) if (g_rapn[k]) LOG("r1: RB SIM %s passes (%lld), means ms: pass start -> poll %.3f, poll -> frame end %.3f, frame end call %.3f, frame end -> safe point %.3f\n",
            k == 0 ? "real" : "re-run", g_rapn[k], g_rap[k][0] / g_rapn[k], g_rap[k][1] / g_rapn[k], g_rap[k][2] / g_rapn[k], g_rap[k][3] / g_rapn[k]);
    }
}

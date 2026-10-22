/* Track R4 (plan/phases/R4_state_map.md sections 6-9): THE LEARNED-SET SAVE. The snapshot ring (r1_ring.c) on the NORMAL game
 * build (ssbu_game_eh_o1.dll + the normal script modules + ssbu_native.exe): no second, "marked" compile of the game DLL and of
 * the modules is needed for rollback. Included by r1_ring.c; it uses the statics of r1_state.c / r1_ring.c (windows, shadow,
 * not-simulation maps, the ring's page ids). No game content in this file: only addresses of the game's allocator entry points
 * and of three allocation sites (13.0.4 addresses, through GA(); hostrt.c maps them to 13.0.5).
 *
 * When: whenever the ring is wanted (SSBU_RB_NET online rollback / its loop test, SSBU_RB_SIM) and NO game DLL with store marks
 * is loaded (ssbu_game_r1.dll of older installs, the study's ssbu_game_p15r1.dll, SSBU_R1_GAMEDLL): with such a DLL the marks
 * are used as before (the fallback while the marked build exists). SSBU_LS=0: never (the ring then needs the marked DLL);
 * SSBU_LS=1: also when the ring is not wanted (SSBU_R1 tests).
 *   - at the ring's base (r1's full save, every guest thread held) every committed read-write page of window 1 (the game's
 *     region) and window 2 (image + modules) that is simulation (not a host stack, not audio output) is made READ-ONLY;
 *   - the first write of such a page faults into ls_veh: the page becomes writable for good and joins the LEARNED SET (a
 *     list of page ids). Before it joins, its bytes are still those of the base (nothing wrote it), which the shadow holds:
 *     every older state the ring has finds the right value there;
 *   - a capture takes EVERY page of the learned set (the copy the marked ring makes of the pages marked since the last
 *     capture); a load copies every learned page back (the pages written since the newest capture are among them);
 *   - host code that writes guest memory through the KERNEL (file reads: nnfs.c read_at, nnalbum.c streams) cannot fault: it
 *     calls r1_wrote first, which opens the pages (writable, learned); a decommit makes its pages learned (the ring records
 *     "decommitted"); a commit during the fight makes its new pages read-only (their first write faults). User-mode writers
 *     outside the game code (CRT memcpy, libopus, the exe's helpers, nnldn.c receives) fault like the game code and are served;
 *   - at the end of the fight every page still read-only is made writable again (menus, loads: no protection); the next
 *     match's base (r1_ring_rebase) protects everything again with an empty learned set.
 *   SSBU_LS_LAZY=1 (default off until measured on Windows): the base copies NOTHING into the shadow. A page protected
 *   at the base is "pending" (g_ls_pend); its first write (the fault, r1_wrote, a decommit) copies it into the shadow before
 *   it opens: until then its bytes ARE the base's. A fight writes ~2% of the 1.9 GB it protects (measured: 34 MB learned),
 *   so the base's two full copies (the save and the shadow fill, most of its ~0.7 s hitch) go; the faults copy 4 KB each.
 *   SSBU_LS_CMP (default ON since R3 net3; =0 off): a capture compares each learned page with its newest value first (in parallel, the ring's copy threads)
 *   and copies only the pages that changed (less ring memory, smaller loads; the compare reads both copies).
 *   SSBU_LS_DECAY=<n> (implies CMP; default off): every n captures the pages unchanged for n captures go read-only again.
 * THE PRESENTATION POOL (on with the learned set): the blocks the game's jemalloc would make for three allocation sites proven
 *   not simulation state (R4 sections 3 and 8: the graphics 16 MB buffer, an 8 MB block, the 1.4 MB blocks; list "rt_big3",
 *   built in) are served from a separate reservation of this exe that the ring never sees (not in a window, not an extra:
 *   < 16 GB): never saved, never protected; the pool's bookkeeping is saved with the state (ls_pool_meta, an extra). Hooks on
 *   jemalloc's aligned_alloc / posix_memalign / realloc / free / malloc_size in main. A free of a block that is not live there
 *   (a re-run pass freeing again what a real pass freed) is counted and ignored. SSBU_LS_ROUTE=<site file> (lines "<caller>
 *   [<caller's caller>]", hex 13.0.4 addresses) replaces the built-in list (test); SSBU_LS_ROUTE=0: no pool.
 * Hooks (r1_ring.c / r1_state.c): mark_setup (end, threads held) -> ls_after_setup(); r1_ring_capture / r1_ring_load:
 *   ls_capture / ls_each instead of rg_sweep when g_ls_active, ls_capture_end; r1_ring_rebase -> ls_rebase(); rgs_step (fight
 *   over) -> ls_fight_off(); r1_on_commit / r1_on_decommit / r1_wrote -> ls_range; ra_init: g_mark = mark_bind() || ls_bind();
 *   r1_init -> ls_init(); collect() -> ls_pool_meta; rgs_report -> ls_report(). */
static int g_ls_on = -1, g_ls_active, g_ls_cmp, g_ls_off_done;
static volatile uint8_t *g_ls_st;                       /* per ring page id (windows 1 + 2): 0 untracked, 1 read-only, 2 learned, 3 opening */
static uint8_t *g_ls_pend;                              /* SSBU_LS_LAZY, per ring page id: 1 = read-only since the base, its base bytes not yet in the shadow */
static int g_ls_lazy = -1; static volatile LONG64 g_ls_lazy_copies;
#define LS_LAZY_DEFAULT 0                               /* (not measured on Windows yet: SSBU_LS_LAZY=1 to try it) */
static size_t g_ls_n1, g_ls_np;
static uint32_t *g_ls_list; static volatile LONG g_ls_n; static LONG g_ls_cap = 1 << 21; static volatile LONG g_ls_over;
static volatile LONG64 g_ls_faults, g_ls_fault_mod[5], g_ls_wrote_open, g_ls_dec_learn, g_ls_commit_prot, g_ls_spin, g_ls_refault, g_ls_protfail;
static long long g_ls_prot_pages, g_ls_bases, g_ls_unprot_pages, g_ls_caps, g_ls_cap_pages, g_ls_cap_same; static double g_ls_prot_ms, g_ls_cmp_ms;
static HMODULE g_ls_mod_game, g_ls_mod_exe;
static volatile LONG64 g_read_retry;                    /* nnfs.c read_at: reads repeated after ERROR_NOACCESS (r1_read_retry_note) */
typedef struct { uint64_t off; char mod[40]; long long n; } LsW;
static LsW g_ls_w[64]; static int g_ls_nw; static SRWLOCK g_ls_wlock = SRWLOCK_INIT;
static int ls_marked_dll(void) {                         /* a game DLL with store marks is loaded (mark_bind's names) */
    const char *dn = getenv("SSBU_R1_GAMEDLL");
    HMODULE m = GetModuleHandleA(dn && dn[0] ? dn : "ssbu_game_r1.dll");
    if (!m && !(dn && dn[0])) m = GetModuleHandleA("ssbu_game_p15r1.dll");
    return m && GetProcAddress(m, "r1_m1") != NULL;
}
static int ls_on(void) {
    if (g_ls_on < 0) {
        const char *e = getenv("SSBU_LS"); int marked = ls_marked_dll();
        g_ls_on = !(e && e[0] == '0') && !marked && ((e && e[0]) || rgs_wanted());
        LOG("r1: LS (the learned-set save, normal game build): %s\n", g_ls_on ? "ON" : e && e[0] == '0' ? "off (SSBU_LS=0)" :
            marked ? "off: a game DLL with marks is loaded, its marks are used" : "off (no rollback ring wanted)");
    }
    return g_ls_on;
}
static int ls_bind(void) {                               /* no game DLL with marks: the exe's own map variables (nothing marks them but host code) */
    if (!ls_on()) return 0;
    pm1 = &r1_m1; pm2 = &r1_m2; plo1 = &r1_lo1; pn1 = &r1_n1; pn2 = &r1_n2;
    LOG("r1: LS: the game DLL has no marks: the ring runs on the learned page set (page protection), maps of the exe\n");
    return 1;
}
static int ls_lazy(void) {                               /* (r1_state.c save / mark_setup, r1_ring.c rebase) the base leaves the shadow to the first writes */
    if (g_ls_lazy < 0) { const char *e = getenv("SSBU_LS_LAZY"); g_ls_lazy = e && e[0] ? atoi(e) != 0 : LS_LAZY_DEFAULT; }
    return g_ls_lazy && ls_on() && pm1 == &r1_m1;
}
static uint8_t *ls_addr(size_t id);
static long ls_count(void) { LONG n = g_ls_n; return n > g_ls_cap ? g_ls_cap : n; }     /* (r1_state.c save, lazy: the pages learned during the hold) */
static uint8_t *ls_learned_addr(long k) { uint32_t v = g_ls_list[k]; return v ? ls_addr(v - 1) : NULL; }
static int ls_id(const uint8_t *a, size_t *id) {
    if (g_win1 && a >= g_win1 && a < g_win1 + g_wn1) { *id = (size_t)(a - g_win1) >> 12; return 1; }
    if ((uintptr_t)a >= WIN2 && (uintptr_t)a < WIN2 + WN2) { *id = g_ls_n1 + (((uintptr_t)a - WIN2) >> 12); return 1; }
    return 0;
}
static uint8_t *ls_addr(size_t id) { return id < g_ls_n1 ? g_win1 + id * PG : (uint8_t *)(uintptr_t)WIN2 + (id - g_ls_n1) * PG; }
static int ls_ns(size_t id) { return id < g_ls_n1 ? g_ns1[id] : g_ns2[id - g_ls_n1]; }
static uint32_t *g_ls_lc; static int g_ls_decay; static SRWLOCK g_ls_lk = SRWLOCK_INIT; static long long g_ls_decayed, g_ls_decays; static double g_ls_decay_ms;
static void ls_push(size_t id) {                         /* (shared: SSBU_LS_DECAY's compaction takes it exclusive, threads held) */
    AcquireSRWLockShared(&g_ls_lk);
    LONG k = InterlockedIncrement(&g_ls_n) - 1;
    if (k < g_ls_cap) { g_ls_lc[k] = (uint32_t)g_ls_caps; g_ls_list[k] = (uint32_t)id + 1; } else InterlockedIncrement(&g_ls_over);
    ReleaseSRWLockShared(&g_ls_lk);
}
static void ls_note_writer(EXCEPTION_POINTERS *e) {      /* who faulted: 0 game DLL, 1 a script module, 2 the exe, 3 other (system / driver / CRT) */
    HMODULE m = NULL; char mn[MAX_PATH] = "?"; uint64_t rip = e ? e->ContextRecord->Rip : 0;
    if (!rip || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)rip, &m)) { InterlockedIncrement64(&g_ls_fault_mod[4]); return; }
    int k = m == g_ls_mod_game ? 0 : m == g_ls_mod_exe ? 2 : 3;
    if (k == 3) { GetModuleFileNameA(m, mn, MAX_PATH); if (strstr(mn, "lua2cpp_")) k = 1; }
    InterlockedIncrement64(&g_ls_fault_mod[k]);
    if (k < 2) return;
    const char *b = strrchr(mn, '\\'); b = b ? b + 1 : mn; if (k == 2) b = "exe";
    uint64_t off = rip - (uint64_t)(uintptr_t)m;
    AcquireSRWLockExclusive(&g_ls_wlock);
    int i; for (i = 0; i < g_ls_nw; i++) if (g_ls_w[i].off == off && !strcmp(g_ls_w[i].mod, b)) break;
    if (i == g_ls_nw && g_ls_nw < 64) { g_ls_w[i].off = off; strncpy(g_ls_w[i].mod, b, 39); g_ls_nw++; }
    if (i < 64) g_ls_w[i].n++;
    ReleaseSRWLockExclusive(&g_ls_wlock);
}
static int ls_open(size_t id, EXCEPTION_POINTERS *e) {   /* the page writable for good, in the learned set; 0 = not ours */
    for (;;) {
        uint8_t s = g_ls_st[id];
        if (s == 0) return 0;
        if (s == 2) { DWORD old; InterlockedIncrement64(&g_ls_refault); return VirtualProtect(ls_addr(id), PG, PAGE_READWRITE, &old) ? 1 : 0; }
        if (s == 3) { InterlockedIncrement64(&g_ls_spin); YieldProcessor(); continue; }
        if (__sync_bool_compare_and_swap(&g_ls_st[id], (uint8_t)1, (uint8_t)3)) {
            if (g_ls_pend && g_ls_pend[id]) {           /* (lazy) still the base's bytes: into the shadow before anything writes it */
                memcpy(id < g_ls_n1 ? g_sh1 + id * PG : g_sh2 + (id - g_ls_n1) * PG, ls_addr(id), PG);
                g_ls_pend[id] = 0; InterlockedIncrement64(&g_ls_lazy_copies);
            }
            DWORD old; int ok = VirtualProtect(ls_addr(id), PG, PAGE_READWRITE, &old) != 0;
            if (!ok) InterlockedIncrement64(&g_ls_protfail);
            ls_push(id); MemoryBarrier(); g_ls_st[id] = 2;
            if (e) { InterlockedIncrement64(&g_ls_faults); ls_note_writer(e); }
            return ok;
        }
    }
}
static LONG CALLBACK ls_veh(EXCEPTION_POINTERS *e) {
    if (!g_ls_st || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    size_t id; if (!ls_id((const uint8_t *)(uintptr_t)e->ExceptionRecord->ExceptionInformation[1], &id)) return EXCEPTION_CONTINUE_SEARCH;
    return ls_open(id, e) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
static void ls_protect_run(size_t a, size_t b) {         /* ids [a, b): read-only (one call; per page if the run is not one region) */
    if (a >= b) return;
    DWORD old;
    if (g_ls_pend) memset(g_ls_pend + a, 1, b - a);     /* (before the protection: a write after it faults and finds the page pending) */
    if (VirtualProtect(ls_addr(a), (b - a) * PG, PAGE_READONLY, &old)) { for (size_t i = a; i < b; i++) g_ls_st[i] = 1; g_ls_prot_pages += (long long)(b - a); return; }
    for (size_t i = a; i < b; i++) { if (VirtualProtect(ls_addr(i), PG, PAGE_READONLY, &old)) { g_ls_st[i] = 1; g_ls_prot_pages++; } else g_ls_protfail++; }
}
static void ls_protect_all(void) {                       /* threads held: every committed RW simulation page of the windows (g_r = r1's ranges) */
    double t0 = now_ms(); long long before = g_ls_prot_pages;
    memset((void *)g_ls_st, 0, g_ls_np); g_ls_n = 0; g_ls_over = 0;
    if (g_ls_pend) memset(g_ls_pend, 0, g_ls_np);
    for (int i = 0; i < g_nr; i++) {
        size_t id0, idn; uint8_t *a = g_r[i].a; size_t np = g_r[i].n / PG;
        if (!ls_id(a, &id0) || !ls_id(a + (np - 1) * PG, &idn) || idn != id0 + np - 1) continue;      /* not (wholly) in one window */
        size_t run = id0;
        for (size_t id = id0; id < id0 + np; id++) if (ls_ns(id)) { ls_protect_run(run, id); run = id + 1; }
        ls_protect_run(run, id0 + np);
    }
    g_ls_prot_ms += now_ms() - t0; g_ls_bases++; g_ls_active = 1; g_ls_off_done = 0;
    LOG("r1: LS: %lld pages (%.1f MB) made read-only in %.1f ms; learned set emptied\n", g_ls_prot_pages - before, (g_ls_prot_pages - before) * (double)PG / 1048576.0, now_ms() - t0);
}
static void ls_after_setup(void) {                       /* mark_setup done, threads still held: the base is in the shadow */
    if (!ls_on() || pm1 != &r1_m1) return;               /* (a marked game DLL is loaded: the marks are used) */
    g_ls_n1 = (size_t)(g_wn1 >> 12); g_ls_np = g_ls_n1 + (size_t)(WN2 >> 12);
    if (!g_ls_st) {
        g_ls_st = VirtualAlloc(NULL, g_ls_np, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_ls_list = VirtualAlloc(NULL, (size_t)g_ls_cap * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_ls_lc = VirtualAlloc(NULL, (size_t)g_ls_cap * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (ls_lazy()) g_ls_pend = VirtualAlloc(NULL, g_ls_np, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_ls_st || !g_ls_list || !g_ls_lc || (ls_lazy() && !g_ls_pend)) { LOG("r1: LS: allocation FAILED: off\n"); g_ls_st = NULL; g_ls_on = 0; return; }
        g_ls_cmp = !(getenv("SSBU_LS_CMP") && getenv("SSBU_LS_CMP")[0] == '0');      /* (R3 net3) default ON: the same tick time, a quarter of the ring memory (SSBU_LS_CMP=0: every learned page copied) */
        if (getenv("SSBU_LS_DECAY")) { g_ls_decay = atoi(getenv("SSBU_LS_DECAY")); if (g_ls_decay > 0) g_ls_cmp = 1; }
        g_ls_mod_exe = GetModuleHandleA(NULL);
        {   const char *names[] = {getenv("SSBU_R1_GAMEDLL"), "ssbu_game.dll", "ssbu_game_p15n3.dll", "ssbu_game_eh_o1.dll", "ssbu_game_1305x.dll"};
            for (int i = 0; i < 5 && !g_ls_mod_game; i++) if (names[i] && names[i][0]) g_ls_mod_game = GetModuleHandleA(names[i]); }
        AddVectoredExceptionHandler(1, ls_veh);
    }
    ls_protect_all();
}
static void ls_each(void (*fn)(uint32_t id, int w, size_t j)) {   /* threads held: every page of the learned set */
    LONG n = g_ls_n; if (n > g_ls_cap) n = g_ls_cap;
    for (LONG k = 0; k < n; k++) {
        uint32_t v = g_ls_list[k]; if (!v) continue;     /* (being added right now by a host thread: the next capture has it) */
        size_t id = v - 1; int w = id >= g_ls_n1; size_t j = w ? id - g_ls_n1 : id;
        if (w ? g_ns2[j] : g_ns1[j]) continue;           /* not simulation (r1_nosim after it was learned) */
        if (g_audio_free && is_async(ls_addr(id))) continue;
        fn((uint32_t)id, w, j);
    }
}
/* SSBU_LS_CMP: the capture's pages compared first (the ring's copy threads); only the changed ones go to rg_cap_page.
 * SSBU_LS_DECAY=<n> (implies the compare): every n captures, the learned pages that did not change in the last n captures
 * are made read-only again and leave the set (their newest ring value is their present value); their next write faults
 * and brings them back. The set then follows the pages a fight writes NOW instead of every page it ever wrote. */
static uint32_t *g_ls_cids, *g_ls_ck; static uint8_t *g_ls_cdiff; static size_t g_ls_ccap, g_ls_cn;
static void ls_decay_run(void) {                         /* threads held, after the capture's copies */
    double t0 = now_ms(); long long nd = 0;
    AcquireSRWLockExclusive(&g_ls_lk);
    LONG n = g_ls_n; if (n > g_ls_cap) n = g_ls_cap; LONG m = 0;
    for (LONG k = 0; k < n; k++) {
        uint32_t v = g_ls_list[k]; if (!v) continue;
        size_t id = v - 1; int w = id >= g_ls_n1; size_t j = w ? id - g_ls_n1 : id; DWORD old;
        if ((uint32_t)g_ls_caps - g_ls_lc[k] >= (uint32_t)g_ls_decay && g_ls_st[id] == 2 && !ls_ns(id) && (w || !g_dc[j]) &&
            VirtualProtect(ls_addr(id), PG, PAGE_READONLY, &old)) { g_ls_st[id] = 1; nd++; continue; }
        g_ls_list[m] = v; g_ls_lc[m] = g_ls_lc[k]; m++;
    }
    g_ls_n = m;
    ReleaseSRWLockExclusive(&g_ls_lk);
    g_ls_decayed += nd; g_ls_decays++; g_ls_decay_ms += now_ms() - t0;
    if (g_ls_decays <= 3 || g_ls_decays % 20 == 0) LOG("r1: LS decay %lld: %lld pages read-only again in %.2f ms, set now %ld pages\n", g_ls_decays, nd, now_ms() - t0, (long)m);
}
static void ls_capture(void (*fn)(uint32_t id, int w, size_t j)) {
    g_ls_caps++;
    if (!g_ls_cmp) { long long b = g_rg_cur ? (long long)g_rg_cur->pages : 0; ls_each(fn); if (g_rg_cur) g_ls_cap_pages += (long long)g_rg_cur->pages - b; return; }
    double t0 = now_ms();
    if (g_ls_ccap < (size_t)g_ls_n + 1024) { g_ls_ccap = (size_t)g_ls_n * 2 + 65536; free(g_ls_cids); free(g_ls_ck); free(g_ls_cdiff); g_ls_cids = malloc(g_ls_ccap * 4); g_ls_ck = malloc(g_ls_ccap * 4); g_ls_cdiff = malloc(g_ls_ccap); }
    g_ls_cn = 0; g_ncj = 0;
    LONG n = g_ls_n; if (n > g_ls_cap) n = g_ls_cap; if ((size_t)n > g_ls_ccap) n = (LONG)g_ls_ccap;
    for (LONG k = 0; k < n; k++) {
        uint32_t v = g_ls_list[k]; if (!v) continue;
        uint32_t id = v - 1; int w = id >= g_ls_n1; size_t j = w ? id - g_ls_n1 : id;
        if (w ? g_ns2[j] : g_ns1[j]) continue;
        if (g_audio_free && is_async(ls_addr(id))) continue;
        uint8_t *src = rg_src(id);
        g_ls_ck[g_ls_cn] = (uint32_t)k;
        if (src == NULL || src == (uint8_t *)1 || (!w && g_dc[j])) { g_ls_cids[g_ls_cn++] = id | 0x80000000u; continue; }   /* zero / decommitted: taken as it is */
        g_ls_cids[g_ls_cn++] = id; cp_add(rg_addr(id), src);
    }
    g_cp_cmp = g_ls_cdiff; cp_run(); g_cp_cmp = NULL;    /* the queued (now, newest value) pairs: g_ls_cdiff[q] = 1 if they differ */
    g_ls_cmp_ms += now_ms() - t0;
    size_t q = 0;
    for (size_t i = 0; i < g_ls_cn; i++) {
        uint32_t id = g_ls_cids[i] & 0x7fffffffu; int w = id >= g_ls_n1; size_t j = w ? id - g_ls_n1 : id;
        if (!(g_ls_cids[i] & 0x80000000u) && !g_ls_cdiff[q++]) { g_ls_cap_same++; continue; }
        g_ls_lc[g_ls_ck[i]] = (uint32_t)g_ls_caps;
        fn(id, w, j); g_ls_cap_pages++;
    }
}
static void ls_capture_end(void) {                       /* r1_ring_capture, after the copies (threads still held) */
    if (g_ls_active && g_ls_decay > 0 && g_ls_caps % g_ls_decay == 0) ls_decay_run();
}
static void ls_rebase(void) {                            /* r1_ring_rebase, threads held, the new base in the shadow: protection again */
    if (!g_ls_st) return;
    ls_protect_all();
}
static void ls_fight_off(void) {                         /* the fight is over: every page still read-only writable again */
    if (!g_ls_st || g_ls_off_done) return;
    g_ls_active = 0; g_ls_off_done = 1;
    double t0 = now_ms(); long long n = 0;
    for (size_t a = 0; a < g_ls_np;) {
        if (g_ls_st[a] != 1) { a++; continue; }
        size_t b = a; while (b < g_ls_np && g_ls_st[b] == 1 && (b == a || ls_addr(b) == ls_addr(b - 1) + PG)) b++;
        DWORD old;
        if (VirtualProtect(ls_addr(a), (b - a) * PG, PAGE_READWRITE, &old)) n += (long long)(b - a);
        else for (size_t i = a; i < b; i++) if (VirtualProtect(ls_addr(i), PG, PAGE_READWRITE, &old)) n++;
        for (size_t i = a; i < b; i++) g_ls_st[i] = 0;
        a = b;
    }
    g_ls_unprot_pages += n;
    LOG("r1: LS: fight over: %lld pages writable again in %.1f ms; learned set %ld pages (%.1f MB); faults %lld (game DLL %lld, script modules %lld, exe %lld, other %lld, ?%lld)\n",
        n, now_ms() - t0, (long)g_ls_n, g_ls_n * (double)PG / 1048576.0, (long long)g_ls_faults, (long long)g_ls_fault_mod[0], (long long)g_ls_fault_mod[1], (long long)g_ls_fault_mod[2], (long long)g_ls_fault_mod[3], (long long)g_ls_fault_mod[4]);
}
static void ls_range(uint64_t a, uint64_t n, int what) { /* 0 r1_wrote: open; 1 decommit: learned; 2 commit: read-only unless learned */
    if (!g_ls_st || !n || !g_ls_active) return;
    size_t run = (size_t)-1, id = 0;
    for (uint64_t p = a & ~(uint64_t)(PG - 1); p < a + n; p += PG) {
        if (!ls_id((const uint8_t *)(uintptr_t)p, &id)) continue;
        if (ls_ns(id)) continue;
        if (what == 0) { if (g_ls_st[id] == 1 || g_ls_st[id] == 3) { ls_open(id, NULL); InterlockedIncrement64(&g_ls_wrote_open); } }
        else if (what == 1) { if (g_ls_st[id] == 1) ls_open(id, NULL); else if (g_ls_st[id] == 0) { g_ls_st[id] = 2; ls_push(id); } InterlockedIncrement64(&g_ls_dec_learn); }
        else if (g_ls_st[id] <= 1) { DWORD old; if (VirtualProtect((void *)(uintptr_t)p, PG, PAGE_READONLY, &old)) { g_ls_st[id] = 1; InterlockedIncrement64(&g_ls_commit_prot); } }   /* (a commit over committed pages makes them read-write again) */
        (void)run;
    }
}
/* ---- the presentation pool (SSBU_LS_ROUTE)
 * Its BLOCKS are never saved; its BOOKKEEPING (which block is live, the bump pointer, the size classes) is: one 2 MB region
 * that r1 copies whole as an "extra" (collect() -> ls_pool_meta), so a load puts it back with the rest of the state, as the
 * ring did for jemalloc's own metadata when these sites were only left out (rb_state_map.c SSBU_SM_XSITE, the proof): a block
 * the simulation freed after the loaded frame is live again (a second free of it in the re-run is a normal free), a block made
 * after it is free again. Blocks are found by bitmap (no free list inside the blocks: their bytes are not rolled back). */
#define LP_RES (8ull << 30)                              /* < 16 GB: r1 takes reservations of >= 16 GB as game regions */
#define LP_CH (1ull << 20)
#define LP_NCH (LP_RES / LP_CH)
#define LP_NCL 16                                        /* classes 16 B << c, c < 16 (up to 512 KB); larger: whole 1 MB chunks */
#define LP_PER 512                                       /* chunks per class at most */
#define LPM_BM (1792u << 10)
typedef struct {
    uint64_t bump, bm_used; long long live, made;
    uint32_t ncls[LP_NCL], cur[LP_NCL], list[LP_NCL][LP_PER];
    uint8_t cls[LP_NCH];                                 /* 0 unused, 1 + class, 100 large head, 101 large tail, 102 large head freed */
    uint32_t lsz[LP_NCH], bmo[LP_NCH];
    uint8_t bm[LPM_BM];
} LpMeta;
static uint8_t *g_lp; static LpMeta *g_lpm;
static SRWLOCK g_lp_lock = SRWLOCK_INIT;
static long long g_lp_n, g_lp_bytes, g_lp_frees, g_lp_double, g_lp_reallocs, g_lp_large, g_lp_full;
#define LP_SN 1024
static uint64_t g_lps[LP_SN][2]; static int g_lps_n; static long long g_lps_hit[LP_SN], g_lps_dbl[LP_SN];
static uint8_t *ls_pool_meta(size_t *n) { *n = g_lpm ? (sizeof(LpMeta) + PG - 1) & ~(size_t)(PG - 1) : 0; return (uint8_t *)g_lpm; }
static int lp_in(uint64_t p) { return g_lp && p >= (uint64_t)(uintptr_t)g_lp && p < (uint64_t)(uintptr_t)g_lp + LP_RES; }
static uint64_t lp_alloc(uint64_t n, uint64_t align) {  /* lock held; 0 = no room (the caller goes to jemalloc) */
    LpMeta *m = g_lpm;
    if (align < 16) align = 16;
    uint64_t need = n > align ? n : align; if (need < 16) need = 16;
    if (need <= (LP_CH >> 1)) {
        int c = 0; while ((16ull << c) < need) c++;
        uint64_t sz = 16ull << c, slots = LP_CH / sz, words = (slots + 63) / 64;
        for (uint32_t t = 0; t < m->ncls[c]; t++) {
            uint32_t idx = (m->cur[c] + t) % m->ncls[c], ch = m->list[c][idx];
            uint64_t *w = (uint64_t *)(m->bm + m->bmo[ch]);
            for (uint64_t k = 0; k < words; k++) {
                if (w[k] == ~0ull) continue;
                uint64_t bit = (uint64_t)__builtin_ctzll(~w[k]), slot = k * 64 + bit;
                if (slot >= slots) break;
                w[k] |= 1ull << bit; m->cur[c] = idx; m->live++; m->made++; g_lp_n++; g_lp_bytes += (long long)sz;
                return (uint64_t)(uintptr_t)g_lp + (uint64_t)ch * LP_CH + slot * sz;
            }
        }
        uint32_t bytes = (uint32_t)((words * 8 + 7) & ~7ull);
        if (m->ncls[c] >= LP_PER || m->bump >= LP_NCH || m->bm_used + bytes > LPM_BM) { g_lp_full++; return 0; }
        uint32_t ch = (uint32_t)m->bump;
        if (!VirtualAlloc(g_lp + (uint64_t)ch * LP_CH, LP_CH, MEM_COMMIT, PAGE_READWRITE)) { g_lp_full++; return 0; }
        m->bump++; m->bmo[ch] = (uint32_t)m->bm_used; m->bm_used += bytes; memset(m->bm + m->bmo[ch], 0, bytes);
        m->cls[ch] = (uint8_t)(1 + c); m->list[c][m->ncls[c]] = ch; m->cur[c] = m->ncls[c]; m->ncls[c]++;
        *(uint64_t *)(m->bm + m->bmo[ch]) = 1; m->live++; m->made++; g_lp_n++; g_lp_bytes += (long long)sz;
        return (uint64_t)(uintptr_t)g_lp + (uint64_t)ch * LP_CH;
    }
    if (align > LP_CH) return 0;
    uint32_t nch = (uint32_t)((need + LP_CH - 1) / LP_CH);
    for (uint64_t ch = 0; ch < m->bump; ch++) if (m->cls[ch] == 102 && m->lsz[ch] == nch) { m->cls[ch] = 100; m->live++; m->made++; g_lp_n++; g_lp_large++; return (uint64_t)(uintptr_t)g_lp + ch * LP_CH; }
    if (m->bump + nch > LP_NCH || !VirtualAlloc(g_lp + m->bump * LP_CH, (uint64_t)nch * LP_CH, MEM_COMMIT, PAGE_READWRITE)) { g_lp_full++; return 0; }
    uint64_t c0 = m->bump; m->bump += nch;
    m->cls[c0] = 100; for (uint32_t i = 1; i < nch; i++) m->cls[c0 + i] = 101; m->lsz[c0] = nch;
    m->live++; m->made++; g_lp_n++; g_lp_large++; g_lp_bytes += (long long)nch * (long long)LP_CH;
    return (uint64_t)(uintptr_t)g_lp + c0 * LP_CH;
}
static uint64_t lp_size(uint64_t p) {
    uint64_t off = p - (uint64_t)(uintptr_t)g_lp; uint64_t ch = off / LP_CH; uint8_t k = g_lpm->cls[ch];
    if (k >= 1 && k <= LP_NCL) return 16ull << (k - 1);
    if (k == 100) return (uint64_t)g_lpm->lsz[ch] * LP_CH;
    return 0;
}
static int lp_free(uint64_t p) {                          /* lock held; 0 = not a live block (a second free) */
    LpMeta *m = g_lpm; uint64_t off = p - (uint64_t)(uintptr_t)g_lp, ch = off / LP_CH; uint8_t k = m->cls[ch];
    if (k >= 1 && k <= LP_NCL) {
        uint64_t sz = 16ull << (k - 1), o = off % LP_CH, slot = o / sz; uint64_t *w = (uint64_t *)(m->bm + m->bmo[ch]);
        if (o % sz || !(w[slot / 64] & (1ull << (slot % 64)))) { g_lp_double++; return 0; }
        w[slot / 64] &= ~(1ull << (slot % 64)); m->live--; g_lp_frees++; return 1;
    }
    if (k == 100 && !(off % LP_CH)) { m->cls[ch] = 102; m->live--; g_lp_frees++; return 1; }
    g_lp_double++; return 0;
}
/* which guest threads make each routed site's blocks (a site the sound thread allocates from is rolled back with that thread) */
static const char *g_lpt_nm[LP_SN][4]; static long long g_lpt_n[LP_SN][4];
static __declspec(thread) const char *t_lp_name; static __declspec(thread) int t_lp_named;
static void lps_thr(int s) {                              /* lock held */
    if (!t_lp_named) {
        Ctx *tc; const char *nm; HANDLE h; DWORD me = GetCurrentThreadId(); t_lp_name = "host";
        for (int i = 0; guest_thread_info(i, &tc, &nm, &h); i++) if (h && GetThreadId(h) == me) { t_lp_name = nm ? nm : "?"; break; }
        t_lp_named = 1;
    }
    for (int k = 0; k < 4; k++) {
        if (g_lpt_nm[s][k] == t_lp_name || (g_lpt_nm[s][k] && !strcmp(g_lpt_nm[s][k], t_lp_name))) { g_lpt_n[s][k]++; return; }
        if (!g_lpt_nm[s][k]) { g_lpt_nm[s][k] = t_lp_name; g_lpt_n[s][k] = 1; return; }
    }
}
static int lps_find(uint64_t lr, uint64_t lr2) {
    for (int i = 0; i < g_lps_n; i++) if (g_lps[i][0] == lr && (!g_lps[i][1] || g_lps[i][1] == lr2)) return i;
    return -1;
}
/* which site a pool block came from (for the second-free report): a small table of block -> site */
#define LPB_N (1u << 20)
static uint64_t *g_lpb_p; static int16_t *g_lpb_s;
static void lpb_put(uint64_t p, int s) { if (!g_lpb_p) return; uint32_t h = (uint32_t)((p * 0x9e3779b97f4a7c15ull) >> 44) & (LPB_N - 1); for (int i = 0; i < 64; i++) { uint32_t k = (h + i) & (LPB_N - 1); if (!g_lpb_p[k] || g_lpb_p[k] == p) { g_lpb_p[k] = p; g_lpb_s[k] = (int16_t)s; return; } } }
static int lpb_get(uint64_t p) { if (!g_lpb_p) return -1; uint32_t h = (uint32_t)((p * 0x9e3779b97f4a7c15ull) >> 44) & (LPB_N - 1); for (int i = 0; i < 64; i++) { uint32_t k = (h + i) & (LPB_N - 1); if (g_lpb_p[k] == p) return g_lpb_s[k]; if (!g_lpb_p[k]) return -1; } return -1; }
static ImportFn g_lpo_aa, g_lpo_pm, g_lpo_ra, g_lpo_fr, g_lpo_ms;
static uint64_t ls_lr2(Ctx *c) { uint64_t fp = c->x[29]; return (fp > 0x10000 && !(fp & 7)) ? *(uint64_t *)(uintptr_t)(fp + 8) : 0; }
static void lph_aa(Ctx *c, void *u) {                    /* aligned_alloc(align, size) */
    (void)u; int s = lps_find(c->x[30], ls_lr2(c));
    if (s < 0) { g_lpo_aa(c); return; }
    AcquireSRWLockExclusive(&g_lp_lock); uint64_t r = lp_alloc(c->x[1], c->x[0]); if (r) { g_lps_hit[s]++; lps_thr(s); lpb_put(r, s); } ReleaseSRWLockExclusive(&g_lp_lock);
    if (!r) { g_lpo_aa(c); return; }
    c->x[0] = r;
}
static void lph_pm(Ctx *c, void *u) {                    /* posix_memalign(&p, align, size) */
    (void)u; int s = lps_find(c->x[30], ls_lr2(c));
    if (s < 0) { g_lpo_pm(c); return; }
    uint64_t mp = c->x[0];
    AcquireSRWLockExclusive(&g_lp_lock); uint64_t r = lp_alloc(c->x[2], c->x[1]); if (r) { g_lps_hit[s]++; lps_thr(s); lpb_put(r, s); } ReleaseSRWLockExclusive(&g_lp_lock);
    if (!r || !mp) { g_lpo_pm(c); return; }
    *(uint64_t *)(uintptr_t)mp = r; c->x[0] = 0;
}
static void lph_ra(Ctx *c, void *u) {                    /* realloc(p, size) */
    (void)u; uint64_t p = c->x[0], n = c->x[1];
    if (p && lp_in(p)) {
        AcquireSRWLockExclusive(&g_lp_lock);
        uint64_t have = lp_size(p), r = p; int s = lpb_get(p);
        if (n > have || !n) { r = lp_alloc(n ? n : 16, 16); if (r) { memcpy((void *)(uintptr_t)r, (void *)(uintptr_t)p, (size_t)(have < n ? have : n)); lp_free(p); lpb_put(r, s); } }
        g_lp_reallocs++;
        ReleaseSRWLockExclusive(&g_lp_lock);
        c->x[0] = r; return;
    }
    int s = p ? -1 : lps_find(c->x[30], ls_lr2(c));
    if (s < 0) { g_lpo_ra(c); return; }
    AcquireSRWLockExclusive(&g_lp_lock); uint64_t r = lp_alloc(n ? n : 16, 16); if (r) { g_lps_hit[s]++; lps_thr(s); lpb_put(r, s); } ReleaseSRWLockExclusive(&g_lp_lock);
    if (!r) { g_lpo_ra(c); return; }
    c->x[0] = r;
}
static void lph_fr(Ctx *c, void *u) {                    /* free(p) */
    (void)u; uint64_t p = c->x[0];
    if (!lp_in(p)) { g_lpo_fr(c); return; }
    AcquireSRWLockExclusive(&g_lp_lock); if (!lp_free(p)) { int s = lpb_get(p); if (s >= 0) g_lps_dbl[s]++; } ReleaseSRWLockExclusive(&g_lp_lock);
}
static void lph_ms(Ctx *c, void *u) {                    /* malloc_size(p) */
    (void)u; uint64_t p = c->x[0];
    if (!lp_in(p)) { g_lpo_ms(c); return; }
    AcquireSRWLockShared(&g_lp_lock); c->x[0] = lp_size(p); ReleaseSRWLockShared(&g_lp_lock);
}
static void ls_init(void) {                              /* r1_init */
    const char *fn = getenv("SSBU_LS_ROUTE");
    if (!ls_on() || (fn && (!*fn || !strcmp(fn, "0")))) return;
    if (fn) {                                            /* (test) a site file instead of the built-in list */
        FILE *f = fopen(fn, "r"); if (!f) { LOG("r1: LS: presentation pool: site file %s NOT found\n", fn); return; }
        char ln[256]; unsigned long long a, b;
        while (fgets(ln, sizeof ln, f) && g_lps_n < LP_SN) {
            if (ln[0] == '#') continue;
            int k = sscanf(ln, "%llx %llx", &a, &b); if (k < 1) continue;
            g_lps[g_lps_n][0] = GA(a); g_lps[g_lps_n][1] = k > 1 ? GA(b) : 0; g_lps_n++;
        }
        fclose(f);
    } else {                                             /* rt_big3 (R4 section 8.0 item 4: IDENTICAL on 12 matches, 11 fighters) */
        static const uint64_t big3[3][2] = {
            {0x7103816944ull, 0},                        /* the graphics 16 MB buffer */
            {0x7103559e08ull, 0x71000a3fd8ull},          /* an 8 MB block */
            {0x7103760cf8ull, 0x7100081980ull},          /* the 1.4 MB blocks */
        };
        for (int i = 0; i < 3; i++) { g_lps[g_lps_n][0] = GA(big3[i][0]); g_lps[g_lps_n][1] = big3[i][1] ? GA(big3[i][1]) : 0; g_lps_n++; }
        fn = "the built-in list (rt_big3)";
    }
    for (uint64_t hint = 0x40000000000ull; !g_lp && hint < 0x80000000000ull; hint += 0x400000000ull)      /* away from window 2 (0x7100000000 + 8 GB) */
        g_lp = VirtualAlloc((void *)(uintptr_t)hint, LP_RES, MEM_RESERVE, PAGE_READWRITE);
    for (uint64_t hint = 0x80000000000ull; !g_lpm && hint < 0xC0000000000ull; hint += 0x10000000ull)
        g_lpm = VirtualAlloc((void *)(uintptr_t)hint, sizeof(LpMeta), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_lpb_p = calloc(LPB_N, 8); g_lpb_s = calloc(LPB_N, 2);
    if (!g_lp || !g_lpm) { LOG("r1: LS: presentation pool: allocation FAILED: off\n"); g_lps_n = 0; return; }
    int h = !!hook_install(GA(0x710392dce0ull), lph_aa, NULL, &g_lpo_aa);
    h += !!hook_install(GA(0x710392dc40ull), lph_pm, NULL, &g_lpo_pm);
    h += !!hook_install(GA(0x710392dd80ull), lph_ra, NULL, &g_lpo_ra);
    h += !!hook_install(GA(0x710392e590ull), lph_fr, NULL, &g_lpo_fr);
    h += !!hook_install(GA(0x710392e8a0ull), lph_ms, NULL, &g_lpo_ms);
    LOG("r1: LS: presentation pool on: %d sites from %s, blocks %p + %.0f GB (never saved), bookkeeping %p + %.1f MB (saved with the state), jemalloc hooks %d of 5\n", g_lps_n, fn, (void *)g_lp, LP_RES / 1073741824.0, (void *)g_lpm, sizeof(LpMeta) / 1048576.0, h);
}
static void ls_report(void) {
    if (!g_ls_st && !g_lps_n) return;
    if (g_ls_st) {
        LOG("r1: LS report: bases %lld (%.1f ms protecting), pages made read-only %lld, writable again at fight ends %lld; learned set %ld pages (%.1f MB; list overflow %ld); first-write faults %lld: game DLL %lld, script modules %lld, exe %lld, other %lld, unknown %lld; repeated faults %lld, busy waits %lld, protect failures %lld; kernel-write opens (r1_wrote) %lld, decommits learned %lld, commits made read-only %lld; file reads repeated after ERROR_NOACCESS %lld\n",
            g_ls_bases, g_ls_prot_ms, g_ls_prot_pages, g_ls_unprot_pages, (long)g_ls_n, g_ls_n * (double)PG / 1048576.0, (long)g_ls_over, (long long)g_ls_faults, (long long)g_ls_fault_mod[0], (long long)g_ls_fault_mod[1], (long long)g_ls_fault_mod[2], (long long)g_ls_fault_mod[3], (long long)g_ls_fault_mod[4],
            (long long)g_ls_refault, (long long)g_ls_spin, (long long)g_ls_protfail, (long long)g_ls_wrote_open, (long long)g_ls_dec_learn, (long long)g_ls_commit_prot, (long long)g_read_retry);
        if (g_ls_pend) LOG("r1: LS lazy base: %lld pages copied into the shadow at their first write (%.1f MB)\n", (long long)g_ls_lazy_copies, g_ls_lazy_copies * (double)PG / 1048576.0);
        if (g_ls_decays) LOG("r1: LS decay every %d captures: %lld runs, %lld pages made read-only again (%.0f per run), %.3f ms per run\n", g_ls_decay, g_ls_decays, g_ls_decayed, (double)g_ls_decayed / g_ls_decays, g_ls_decay_ms / g_ls_decays);
        LOG("r1: LS captures %lld: pages taken %.0f per capture%s (compare %.3f ms per capture, %.0f unchanged pages skipped per capture)\n", g_ls_caps, g_ls_caps ? (double)g_ls_cap_pages / g_ls_caps : 0, g_ls_cmp ? " (changed only)" : " (the whole set)",
            g_ls_caps ? g_ls_cmp_ms / g_ls_caps : 0, g_ls_caps ? (double)g_ls_cap_same / g_ls_caps : 0);
        for (int i = 0; i < g_ls_nw; i++) LOG("r1: LS   writer not in the game code: %s+%llx  %lld first writes\n", g_ls_w[i].mod, (unsigned long long)g_ls_w[i].off, g_ls_w[i].n);
    }
    if (g_lps_n) {
        LOG("r1: LS presentation pool: %lld blocks made (%lld large), %.1f MB, live now %lld, chunks %llu, bitmap %.0f KB; frees %lld, frees of a block not live %lld, reallocs %lld, no room (went to jemalloc) %lld\n", g_lp_n, g_lp_large, g_lp_bytes / 1048576.0, g_lpm->live, (unsigned long long)g_lpm->bump, g_lpm->bm_used / 1024.0, g_lp_frees, g_lp_double, g_lp_reallocs, g_lp_full);
        for (int i = 0; i < g_lps_n; i++) if (g_lps_hit[i] || g_lps_dbl[i]) LOG("r1: LS   site %llx %llx: %lld blocks, %lld second frees; threads %s %lld, %s %lld, %s %lld, %s %lld\n", (unsigned long long)g_lps[i][0], (unsigned long long)g_lps[i][1], g_lps_hit[i], g_lps_dbl[i], g_lpt_nm[i][0] ? g_lpt_nm[i][0] : "-", g_lpt_n[i][0], g_lpt_nm[i][1] ? g_lpt_nm[i][1] : "-", g_lpt_n[i][1], g_lpt_nm[i][2] ? g_lpt_nm[i][2] : "-", g_lpt_n[i][2], g_lpt_nm[i][3] ? g_lpt_nm[i][3] : "-", g_lpt_n[i][3]);
    }
}

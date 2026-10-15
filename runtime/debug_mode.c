/* debug_mode.c - TEST / DEBUG FAST MODE (test-only; every feature is behind an environment variable, none set = no effect).
 * See tests\DEBUG_MODE.md. Called once from boot.c before nnMain (debug_mode_init). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

uint64_t hook_address(const char *name);              /* fighter_sdk\hook_host.c (included by nnro.c) */

static int safe_read(uint64_t addr, void *buf, size_t n) {
    SIZE_T got = 0;
    if (addr < 0x10000) return 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)addr, buf, n, &got) && got == n;
}
static uint64_t sr64(uint64_t a) { uint64_t v = 0; return safe_read(a, &v, 8) ? v : 0; }

/* ---- the game's fighter entries (layout read off the lua_bind accessors of the game's own binary):
 *   FighterManager singleton -> *mgr = impl; impl + 0x20 + 8*entry_id = FighterEntry*; entry + 0xf0 = FighterInformation;
 *   *(information + 8) = the information record (hit point 0x38, colour 0x51, cpu flag 0x92, stock 0xd8, category 0x3ac). */
static uint64_t fighter_info_record(int id) {
    static uint64_t inst;
    if (!inst) { uint64_t a = hook_address("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E"); if (!a) return 0; inst = a; }
    uint64_t mgr = sr64(inst), impl = mgr ? sr64(mgr) : 0, entry = impl ? sr64(impl + 0x20 + 8 * (uint64_t)id) : 0;
    uint64_t info = entry ? entry + 0xf0 : 0;
    return info ? sr64(info + 8) : 0;
}

/* SSBU_DEBUG_SCAN=1: once, when entry 1's record exists, every other copy of the pattern in it (HP 999.0 followed by
 * ff bytes, record + 0x40) in committed memory is printed with 0x60 bytes in front of it: finds the structure the record was built from. */
static void scan_copies(uint64_t rec) {
    static const uint8_t pat[16] = {0x00, 0xc0, 0x79, 0x44, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t *p = (uint8_t *)si.lpMinimumApplicationAddress;
    MEMORY_BASIC_INFORMATION mi;
    int hits = 0;
    while (p < (uint8_t *)si.lpMaximumApplicationAddress && VirtualQuery(p, &mi, sizeof mi)) {
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_READONLY)) && mi.RegionSize < (1ull << 31)) {
            const uint8_t *b = mi.BaseAddress;
            for (size_t i = 0; i + 16 <= mi.RegionSize; i += 4)
                if (b[i] == 0x00 && b[i + 1] == 0xc0 && memcmp(b + i, pat, 16) == 0 && (uint64_t)(b + i) != rec + 0x40 && hits < 12) {
                    hits++;
                    printf("debug scan: copy at %p (record base would be %p), region %p+%llx\n", b + i, b + i - 0x40, mi.BaseAddress, (unsigned long long)mi.RegionSize);
                    if (i >= 0x40) for (int k = -0x40; k < 0x40; k += 16) {
                        const uint8_t *q = b + i + k;
                        printf("debug scan:   %+04d %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n", k, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
                    }
                }
        }
        p = (uint8_t *)mi.BaseAddress + mi.RegionSize;
    }
    printf("debug scan: %d copies besides the record itself%c", hits, 10);
    fflush(stdout);
}

/* SSBU_DEBUG_WATCH=1: hardware write watchpoints (debug registers of every thread) on a few fields of the first two
 * records; each write prints the guest functions on the stack of the writer (finds the code that fills the record). */
#include <tlhelp32.h>
/* SSBU_DEBUG_MATCH="p1=<fighter>,p2=<fighter>,stage=<name>,form=omega,cpu=<level>,items=off|on,time=<min>|stock=<n>" */
static struct { int p[2]; char stage[48]; int omega, cpu, items, time, stock; char text[256]; } g_dm = {{-1, -1}, "", 0, -1, -1, -1, -1, ""};
static char g_dm_name[2][32];
static int g_dm_p2type = -1, g_dm_applied, g_dm_fight, g_dm_form = -1, g_dm_hazards = -1;
static FILE *g_dm_list;
static uint64_t fighter_info_record(int id);
static void hex_lines(const char *tag, uint64_t base, int n) {
    for (int k = 0; k < n; k += 16) {
        uint8_t b[16]; if (!safe_read(base + k, b, 16)) break;
        int any = 0; for (int j = 0; j < 16; j++) any |= b[j];
        if (any) printf("%s +%03x %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n", tag, k, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }
}

/* ---- SSBU_DEBUG_END="<n>s" (seconds of fight = n * 60 game frames) or "<n>" (frames): the frames presented while the game's
 * own FighterManager says the fight is on (is_ready_go, impl + 0xd2) are counted (display.c calls debug_mode_frame once per
 * presented frame); at the limit the exit request of a closed window is made (the normal shutdown path). */
int oe_exit_request(void);                             /* nnmisc.c */
static volatile LONG64 g_end_limit, g_end_fight_frames, g_end_fired;
static volatile LONG64 g_end_first_frame;
void debug_mode_frame(long long frame) {
    if (!g_end_limit && !g_dm.text[0]) return;
    uint64_t inst = hook_address("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
    uint64_t mgr = inst ? sr64(inst) : 0, impl = mgr ? sr64(mgr) : 0;
    uint8_t go = 0;
    static int was_go;
    if (!impl || !safe_read(impl + 0xd2, &go, 1) || !go) {
        if (was_go) { printf("debug match: fight off (ready-go cleared) at presented frame %lld after %lld fight frames\n", frame, (long long)g_end_fight_frames); fflush(stdout); }
        was_go = 0; return;
    }
    if (!was_go) { was_go = 1; InterlockedExchange64(&g_end_fight_frames, 0); }
    LONG64 n = InterlockedIncrement64(&g_end_fight_frames);
    if (n == 1) {
        g_end_first_frame = frame;
        uint64_t sel = GA(0x71053097c0ull) + 0x20ull * (rd32(GA(0x7105309968ull)) < 3 ? rd32(GA(0x7105309968ull)) : 0);
        uint64_t r0 = fighter_info_record(0), r1 = fighter_info_record(1);
        printf("debug match: fight %d on (the game's ready-go) at presented frame %lld: p1 kind %d (%s), p2 kind %d (%s) type %d cpu power %d | stage id 0x%x (match block + 0x420), chosen ui_stage %llx form %d (0 normal, 1 battlefield, 2 omega) flags %x/%x (%s) | rule: stock %d, item appear %d; stocks in the fighters' records %d / %d\n", g_dm_fight, frame,
               (int)(int8_t)rd8(GA(0x71052c4738ull)), g_dm_name[0], (int)(int8_t)rd8(GA(0x71052c4738ull) + 0x260), g_dm_name[1], rd8(GA(0x71052c4730ull) + 0x260 + 0xa), rd8(GA(0x71052c4730ull) + 0x260 + 0x79),
               rd32(GA(0x71052c41b0ull) + 0x420), (unsigned long long)rd64(sel), rd32(sel + 8), rd32(sel + 0xc), rd8(GA(0x7105309985ull)), g_dm.stage,
               rd8(GA(0x71052c41b0ull) + 0x4c), rd32(GA(0x71052c41b0ull) + 0x54), r0 ? (int)rd32(r0 + 0xd8) : -1, r1 ? (int)rd32(r1 + 0xd8) : -1);
        hex_lines("debug match:   stage block", GA(0x71052c41b0ull) + 0x420, 0x30);
        fflush(stdout);
    }
    if (g_end_limit && n >= g_end_limit) InterlockedExchange64(&g_end_fired, frame);
}
static int fighter_kind_by_name(const char *name) {
    char *e; long v = strtol(name, &e, 0);
    if (*name && !*e) return (int)v;
    FILE *f = fopen("..\\reference\\skyline-smash\\src\\lua_const.rs", "r");
    if (!f) return -1;
    char key[96], line[256]; snprintf(key, sizeof key, "FIGHTER_KIND_%s:", name);
    for (char *p = key; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    int kind = -1;
    while (fgets(line, sizeof line, f)) { char *s = strstr(line, key); if (s && s - line < 8) { kind = (int)strtol(strchr(s, ':') + 1, NULL, 0); break; } }
    fclose(f);
    return kind;
}
static void parse_match(const char *s) {
    snprintf(g_dm.text, sizeof g_dm.text, "%s", s);
    char buf[256]; snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        char *eq = strchr(t, '='); if (!eq) continue;
        *eq = 0; const char *k = t, *v = eq + 1;
        if (!strcmp(k, "p1")) { g_dm.p[0] = fighter_kind_by_name(v); snprintf(g_dm_name[0], sizeof g_dm_name[0], "%s", v); }
        else if (!strcmp(k, "p2") && (!strcmp(v, "cpu") || !strcmp(v, "human"))) g_dm_p2type = !strcmp(v, "cpu");
        else if (!strcmp(k, "p2")) { g_dm.p[1] = fighter_kind_by_name(v); snprintf(g_dm_name[1], sizeof g_dm_name[1], "%s", v); }
        else if (!strcmp(k, "stage")) snprintf(g_dm.stage, sizeof g_dm.stage, "%s", v);
        else if (!strcmp(k, "form")) { g_dm.omega = !strcmp(v, "omega"); g_dm_form = !strcmp(v, "omega") ? 2 : !strcmp(v, "battlefield") ? 1 : 0; }
        else if (!strcmp(k, "hazards")) g_dm_hazards = !strcmp(v, "on");
        else if (!strcmp(k, "cpu")) g_dm.cpu = atoi(v);
        else if (!strcmp(k, "items")) g_dm.items = !strcmp(v, "on");
        else if (!strcmp(k, "time")) g_dm.time = atoi(v);
        else if (!strcmp(k, "stock")) g_dm.stock = atoi(v);
    }
    printf("debug match: requested %s -> p1 kind %d, p2 kind %d, stage %s%s\n", s, g_dm.p[0], g_dm.p[1], g_dm.stage, g_dm.omega ? " (omega)" : "");
}
static void match_player_hook(Ctx *c, void *user);
static ImportFn g_orig_player_record;
static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *ep) {
    static volatile LONG shown;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *cx = ep->ContextRecord;
    if (!(cx->Dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
    int which = 0; while (!((cx->Dr6 >> which) & 1)) which++;
    uint64_t addr = (&cx->Dr0)[which];
    /* Tried and NOT working: overwriting the record's fighter field (record + 0x30 = kind + 1) here changes nothing in the
     * fight (run m1: record said 6 and 9, the fighters spawned were still kinds 0 and 8): the record is a copy made from
     * the select-screen data, not the source. */
    if (InterlockedIncrement(&shown) <= 160) {
        char line[1200]; int n = snprintf(line, sizeof line, "debug watch: write to %llx (reg %d) now %016llx by thread %lu, guest stack:", (unsigned long long)addr, which, (unsigned long long)*(uint64_t *)(uintptr_t)(addr & ~7ull), GetCurrentThreadId());
        CONTEXT c2 = *cx; uint64_t last = 0;
        for (int depth = 0, sh = 0; depth < 400 && sh < 14; depth++) {
            uint64_t g = guest_function_of(c2.Rip);
            if (g && g != last) { n += snprintf(line + n, sizeof line - n, " f_%llx", (unsigned long long)g); sh++; last = g; }
            DWORD64 ib; RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(c2.Rip, &ib, NULL);
            if (!rf) break;
            void *hd; DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c2.Rip, rf, &c2, &hd, &est, NULL);
            if (!c2.Rip) break;
        }
        {   /* the guest's own frame chain: one of the host registers holds the Ctx of this thread (x29 = frame pointer, x30 = link) */
            const DWORD64 *regs = &cx->Rax; int found = 0;
            for (int r = 0; r < 16 && !found; r++) {
                uint64_t p = regs[r], x[32];
                if (r == 4 || !safe_read(p, x, sizeof x)) continue;
                uint64_t fp = x[29], sp = x[31], pair[2];
                if (fp < sp || fp - sp > 0x100000 || (x[30] >> 32) < 0x71 || (x[30] >> 32) > 0x72 || !safe_read(fp, pair, 16) || (pair[1] >> 32) < 0x71 || (pair[1] >> 32) > 0x72) continue;
                found = 1;
                n += snprintf(line + n, sizeof line - n, " | guest pc near lr %llx, frames:", (unsigned long long)x[30]);
                for (int d = 0; d < 10 && safe_read(fp, pair, 16) && (pair[1] >> 32) >= 0x71 && (pair[1] >> 32) <= 0x72; d++) { n += snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)pair[1]); if (pair[0] <= fp) break; fp = pair[0]; }
            }
        }
        printf("%s\n", line); fflush(stdout);
    }
    cx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void watch_set(const uint64_t *addrs, int n) {
    static int veh;
    if (!veh) { veh = 1; AddVectoredExceptionHandler(1, watch_veh); }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    int set = 0;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT cx; memset(&cx, 0, sizeof cx); cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &cx)) {
            DWORD64 *dr = &cx.Dr0; DWORD64 d7 = 0;
            for (int i = 0; i < n && i < 4; i++) { dr[i] = addrs[i]; d7 |= (1ull << (2 * i)) | (1ull << (16 + 4 * i)) | (3ull << (18 + 4 * i)); }
            cx.Dr7 = d7;
            if (SetThreadContext(th, &cx)) set++;
        }
        ResumeThread(th); CloseHandle(th);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    printf("debug watch: %d watchpoints on %d threads\n", n, set); fflush(stdout);
}

int hook_install(uint64_t addr, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);

/* ---- the match set-up (from the game's own binary; see tests\DEBUG_MODE.md) */
#define MELEE_INFO   GA(0x71052c41b0ull)                 /* the game's global match block (rule at +0, players at +0x588, 0x260 each) */
#define MI_PLAYER(i) (MELEE_INFO + 0x588 + 0x260ull * (uint64_t)(i))
#define FN_ENTRY_SETUP GA(0x7100653240ull)               /* (FighterEntry *, init parameter *, flags *): copies 0x70 bytes to entry + 0x70 */
static ImportFn g_orig_entry_setup;
static void research_entry_setup(Ctx *c, void *user) {
    (void)user;
    printf("debug research: entry set-up: entry %llx, parameter %llx, called from %llx\n", (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[30]);
    hex_lines("debug research:   param", c->x[1], 0x70);
    hex_lines("debug research:   rule ", MELEE_INFO, 0x588);
    hex_lines("debug research:   pl 0 ", MI_PLAYER(0), 0x260);
    hex_lines("debug research:   pl 1 ", MI_PLAYER(1), 0x260);
    fflush(stdout);
    g_orig_entry_setup(c);
}

/* ---- SSBU_DEBUG_MATCH, route A: the function that turns one player's choice on the select screens (a panel state) into
 * that player's record of the global match block. x0 = panel state, x1 = player index, x3 = record (MELEE_INFO + 0x580 + 0x260 * i).
 * State: +0x78 player type (0 human, 1 CPU, 2 amiibo, 3 none), +0x88 the ui_chara id (0xc1 << 56 | hash40("ui_chara_<name>")),
 * +0x90 colour, +0x224 CPU level. Record: +0 ui_chara id, +8 fighter kind, +0xa type, +0x19 colour, +0x79 CPU level.
 * The choice is replaced in the STATE before the game's own function runs, so everything it derives is the game's. */
#define FN_PLAYER_RECORD GA(0x7101788260ull)
static uint64_t hash40(const char *s) {
    uint32_t crc = 0xffffffffu; size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) { crc ^= (uint8_t)s[i]; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    return ((uint64_t)n << 32) | (uint32_t)~crc;
}
static void match_player_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t st = c->x[0], rec = c->x[3]; int idx = (int)c->x[1];
    uint64_t was = rd64(st + 0x88); uint32_t type_was = rd32(st + 0x78);
    if (idx == 0 && g_dm_list) {                          /* SSBU_DEBUG_MATCH_LIST: every new match set-up takes the next line (the last one stays) */
        char line[256];
        if (fgets(line, sizeof line, g_dm_list)) { line[strcspn(line, "\r\n")] = 0; if (line[0]) { g_dm_fight++; parse_match(line); } }
    }
    if (g_dm.stock > 0 && type_was != 3 && rd8(st + 5) == 1) wr8(st + 0xa, (uint8_t)g_dm.stock);   /* state + 5: rule kind (1 = stock), + 0xa: stocks -> record + 0x18 */
    if (idx >= 0 && idx < 2 && g_dm_name[idx][0] && type_was != 3) {
        char nm[64]; snprintf(nm, sizeof nm, "ui_chara_%s", g_dm_name[idx]);
        wr64(st + 0x88, 0xc100000000000000ull | hash40(nm));
        if (idx == 1 && g_dm_p2type >= 0) wr32(st + 0x78, (uint32_t)g_dm_p2type);
        if (g_dm.cpu > 0 && rd32(st + 0x78) == 1) { wr8(st + 0x91, (uint8_t)g_dm.cpu); wr32(st + 0x224, 0xffffffffu); }   /* level 1..9; the game maps it to the power in record + 0x79 (0, 15, 21, 31, 42, 48, 60, 75, 100) */
        g_dm_applied |= 1 << idx;
    }
    if (idx == 0 && g_dm.text[0]) {
        /* the stage choice: the game reads it right after the player records, in the same function (0x7102311a24..): three
         * entries of 0x20 bytes at 0x71053097c0, the current one's index at 0x7105309968: +0 ui_stage id (0x69 << 56 | hash40),
         * +8 form (0 normal, 1 battlefield, 2 omega), +0xc a flag word, +0x10 a second id. The rule header (match block + 0..) is filled already. */
        uint32_t cur = rd32(GA(0x7105309968ull)); uint64_t sel = GA(0x71053097c0ull) + 0x20ull * (cur < 3 ? cur : 0);
        printf("debug match: stage choice of the screens: entry %u: ui_stage %llx form %u flag %x second id %llx; byte 7105309985 = %u; rule: stock %u item appear %u\n", cur,
               (unsigned long long)rd64(sel), rd32(sel + 8), rd32(sel + 0xc), (unsigned long long)rd64(sel + 0x10), rd8(GA(0x7105309985ull)), rd8(MELEE_INFO + 0x4c), rd32(MELEE_INFO + 0x54));
        if (g_dm.stage[0]) {
            const char *sn = g_dm.stage; char nm[80];
            if (!strcmp(sn, "fd") || !strcmp(sn, "final_destination")) sn = "end"; else if (!strcmp(sn, "battlefield")) sn = "battle_field";
            snprintf(nm, sizeof nm, "ui_stage_%s", sn);
            wr64(sel, 0x6900000000000000ull | hash40(nm));
        }
        if (g_dm_form >= 0) wr32(sel + 8, (uint32_t)g_dm_form);
        if (g_dm_hazards >= 0) wr8(GA(0x7105309985ull), (uint8_t)g_dm_hazards);
        if (g_dm.items >= 0) wr32(MELEE_INFO + 0x54, g_dm.items ? 2u : 0u);
    }
    g_orig_player_record(c);
    printf("debug match: player %d: screen choice %llx type %u level %u -> record %llx: ui_chara %llx, fighter kind %d, type %d, colour %d, cpu level %d\n", idx,
           (unsigned long long)was, type_was, rd32(st + 0x224), (unsigned long long)rec, (unsigned long long)rd64(rec), (int)(int8_t)rd8(rec + 8), rd8(rec + 0xa), rd8(rec + 0x19), rd8(rec + 0x79));
    if (getenv("SSBU_DEBUG_RESEARCH") && idx == 0) {
        uint64_t fp = c->x[29], pair[2];
        printf("debug research: player record builder called from %llx, frames:", (unsigned long long)c->x[30]);
        for (int d = 0; d < 12 && safe_read(fp, pair, 16); d++) { printf(" %llx", (unsigned long long)pair[1]); if (pair[0] <= fp) break; fp = pair[0]; }
        printf("\n");
    }
    fflush(stdout);
}

static DWORD WINAPI dump_thread(LPVOID arg) {
    (void)arg;
    uint64_t last[8] = {0};
    for (;;) {
        Sleep(g_dm.text[0] || g_end_limit ? 2 : 250);
        if (g_end_fired) {
            printf("debug end: %lld fight frames reached at presented frame %lld (SSBU_DEBUG_END): exit request, normal shutdown\n", (long long)g_end_fight_frames, (long long)g_end_fired);
            fflush(stdout);
            if (!oe_exit_request()) report_and_exit(0);
            return 0;
        }
        if (getenv("SSBU_DEBUG_WATCHENTRY")) {            /* research: who writes the entry pointers of the FighterManager (impl + 0x20, + 0x28) */
            static int tick;
            uint64_t inst = hook_address("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
            uint64_t mgr = inst ? sr64(inst) : 0, impl = mgr ? sr64(mgr) : 0;
            if (impl && (tick++ % 40) == 0) { uint64_t w[2] = {impl + 0x20, impl + 0x28}; watch_set(w, 2); }
            continue;
        }
        if (getenv("SSBU_DEBUG_RESEARCH")) {              /* research: who fills the global match block (kind of player 0 / 1, first rule bytes) */
            static int tick;
            if (atoi(getenv("SSBU_DEBUG_RESEARCH")) == 3 && (tick++ % 40) == 0) { uint64_t w[4] = {MELEE_INFO + 0x420, MELEE_INFO + 0x428, MELEE_INFO + 0x430, MELEE_INFO + 0x4d0}; watch_set(w, 4); }
            if (atoi(getenv("SSBU_DEBUG_RESEARCH")) == 2 && (tick++ % 40) == 0) { uint64_t w[4] = {MI_PLAYER(0), MI_PLAYER(1), MELEE_INFO, MELEE_INFO + 0x10}; watch_set(w, 4); }
        }
        for (int i = 0; i < 8; i++) {
            uint64_t rec = fighter_info_record(i);
            uint8_t b[0x400];
            if (!rec || !safe_read(rec, b, sizeof b)) continue;
            uint64_t h = 1469598103934665603ull;
            for (int k = 0; k < (int)sizeof b; k++) h = (h ^ b[k]) * 1099511628211ull;
            if (h == last[i]) continue;
            last[i] = h;
            if (getenv("SSBU_DEBUG_DUMP")) {
            printf("debug dump: entry %d record %llx\n", i, (unsigned long long)rec);
            for (int k = 0; k < 0x400; k += 16) {
                int any = 0;
                for (int j = 0; j < 16; j++) any |= b[k + j];
                if (any) printf("debug dump:   +%03x %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n", k, b[k], b[k+1], b[k+2], b[k+3], b[k+4], b[k+5], b[k+6], b[k+7], b[k+8], b[k+9], b[k+10], b[k+11], b[k+12], b[k+13], b[k+14], b[k+15]);
            }
            fflush(stdout);
            }
            if (i == 1 && getenv("SSBU_DEBUG_WATCH")) { static int done; uint64_t r0 = fighter_info_record(0); if (!done && r0) { done = 1; uint64_t w[4] = {r0 + 0x30, rec + 0x30, rec + 0x20, r0 + 0x08}; watch_set(w, 4); } }
            if (i == 1 && getenv("SSBU_DEBUG_SCAN")) { static int done; if (!done) { done = 1; scan_copies(rec); } }
        }
        if (g_dm.text[0]) {                               /* read back from the game's own entries once they have been stable for ~1 s */
            static int printed, stable, prev0 = -1, prev1 = -1;
            uint64_t r0 = fighter_info_record(0), r1 = fighter_info_record(1);
            int k0 = r0 ? (int)sr64(r0 + 0x30 - 0) & 0xffffffff : 0, k1 = r1 ? (int)sr64(r1 + 0x30) & 0xffffffff : 0;
            if (k0 && k1 && k0 == prev0 && k1 == prev1) stable++; else stable = 0;
            prev0 = k0; prev1 = k1;
            if (!printed && stable > 400 && getenv("SSBU_DEBUG_DUMP")) {   /* record + 0x30 is NOT the fighter kind (session 2: the health lines and the match block are) */ printed = 1; printf("debug match: in the fight: entry 0 kind %d, entry 1 kind %d (records at %llx, %llx; kind + 1 at record + 0x30)\n", k0 - 1, k1 - 1, (unsigned long long)r0, (unsigned long long)r1); if (getenv("SSBU_DEBUG_RESEARCH")) hex_lines("debug research:   fight rule ", MELEE_INFO, 0x580); fflush(stdout); }
        }
    }
}

void debug_mode_init(void) {
    const char *end = getenv("SSBU_DEBUG_END"), *list = getenv("SSBU_DEBUG_MATCH_LIST");
    if (end && atof(end) > 0) { g_end_limit = (LONG64)(strchr(end, 's') ? atof(end) * 60.0 : atof(end)); printf("debug end: the run ends after %lld fight frames\n", (long long)g_end_limit); }
    if (list && !(g_dm_list = fopen(list, "r"))) printf("debug match: SSBU_DEBUG_MATCH_LIST: cannot open %s\n", list);
    if (g_dm_list && !getenv("SSBU_DEBUG_MATCH")) snprintf(g_dm.text, sizeof g_dm.text, "(list)");
    if (getenv("SSBU_DEBUG_MATCH") || g_dm_list) {
        if (getenv("SSBU_DEBUG_MATCH")) parse_match(getenv("SSBU_DEBUG_MATCH"));
        printf("debug match: hook on the player record builder %llx: id %d\n", FN_PLAYER_RECORD, hook_install(FN_PLAYER_RECORD, match_player_hook, NULL, &g_orig_player_record));
    }
    if (getenv("SSBU_DEBUG_RESEARCH")) printf("debug research: hook on the entry set-up: id %d\n", hook_install(FN_ENTRY_SETUP, research_entry_setup, NULL, &g_orig_entry_setup));
    if (getenv("SSBU_DEBUG_DUMP") || g_dm.text[0] || g_end_limit ||getenv("SSBU_DEBUG_RESEARCH") || getenv("SSBU_DEBUG_WATCHENTRY")) CreateThread(NULL, 0, dump_thread, NULL, 0, NULL);
}

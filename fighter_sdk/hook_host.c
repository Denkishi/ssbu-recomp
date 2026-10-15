/* The hook table (track M): replace any lifted function at run time, by guest address or exported name.
 * #included by nnro.c (it uses nnro.c's module tables and name lookup); the API a mod sees is fighter_sdk\include\hook.h.
 *
 * MECHANISM. Lifted code calls its own functions with direct C calls (`f_7200024170(c);`), so a table consulted at
 * indirect calls does not see them. Instead of rebuilding 500 MB of lifted code with a test in front of every call,
 * the ENTRY of a hooked function is rewritten in the loaded DLL: its first 5 bytes become `jmp site`. Nothing is
 * changed in a function that has no hook, so a run with 0 hooks executes exactly the code it executed before, and
 * neither the game DLL nor a module DLL is rebuilt. Sound because (measured, see S1_fighter_source.md section 8):
 * the lifted code is compiled at -O1 without an inliner (every direct call is a `call`/`jmp` to the callee's entry),
 * and the entry block of every lifted function is straight-line code of 7 bytes or more.
 *
 *   function entry:  E9 -> site                          (5 bytes; the instructions they held are in the trampoline)
 *   site:            jmp [head]                          head = code of the newest layer (one 8-byte word: atomic)
 *   layer:           inc [calls]; mov rdx,[user]; jmp [fn]      fn = the replacement, or, when removed, `orig`
 *   trampoline:      the moved entry instructions (re-addressed), jmp back behind them      = "the original"
 *
 * A replacement is `void repl(Ctx *c, void *user)`; the original is `void orig(Ctx *c)`. A second hook on the same
 * function becomes a new layer whose original is the layer below. Removing / re-enabling a hook writes one word.
 * Only the first installation on a function writes code (5 bytes): do it while no thread can be in those bytes
 * (before the game starts, or when the module is loaded); everything afterwards is safe at any time.
 * Refused, by name, never guessed: an entry whose first instructions this file cannot move (hk_len returns 0).
 *
 *   SSBU_HOOK_SELFTEST=1     census of every function of main (and of each module when loaded) + a live test with counters
 *   SSBU_HOOK_COUNT=a,b,...  pass-through hooks on these guest addresses / exported names; calls are printed at exit
 *   SSBU_MODS=<folder>       load every *.dll of the folder and call its mod_init(api) (no folder is loaded by default yet)
 */
typedef void (*HookFn)(Ctx *, void *);
typedef struct HkSite { uint64_t addr; uint8_t *target, *mem, *head; uint8_t saved[8]; ImportFn tramp; int top, moved; } HkSite;
typedef struct { HkSite *site; uint8_t *mem, *data; HookFn repl; void *user; ImportFn orig; ImportFn *orig_out; uint64_t addr; char *name; int state; } HkLayer;
enum { HK_FREE = 0, HK_PENDING, HK_ON, HK_OFF };
#define HK_SITE_BYTES 80
#define HK_LAYER_BYTES 48
#define HK_POOL_BYTES 0x40000
#define HK_MAX_LAYERS (1 << 18)
#define HK_HASH (1u << 18)

static SRWLOCK g_hk_lock = SRWLOCK_INIT;
static HkLayer *g_hk_layer[HK_MAX_LAYERS >> 10];        /* chunks of 1024; id = index + 1 */
static int g_hk_layers, g_hk_pending;
static HkSite *g_hk_site[HK_HASH];
static struct { uint8_t *base; size_t used, data; } g_hk_pool[1024];
static int g_hk_pools;
static char g_hk_error[200];

static HkLayer *hk_layer(int id) { return id > 0 && id <= g_hk_layers ? &g_hk_layer[(id - 1) >> 10][(id - 1) & 1023] : NULL; }

/* ---------------------------------------------------------------- x86-64 instruction lengths (entry code only)
 * Returns the length of the instruction at p, 0 if it is not one this file moves. *fix = offset of a 32-bit field that
 * is relative to the end of the instruction (rip-relative operand, rel32 branch), 0 if none. *kind: 0 plain,
 * 3 jcc rel8, 4 jmp rel8, 5 the function's flow ends here (ret, jmp). `call` is refused on purpose: a return address
 * inside the trampoline would have no unwind data. */
static int hk_modrm(const uint8_t *p, int *rip) {
    int mod = p[0] >> 6, rm = p[0] & 7, n = 1;
    if (mod == 3) return 1;
    if (rm == 4) { n++; if (mod == 0 && (p[1] & 7) == 5) n += 4; }
    if (mod == 0 && rm == 5) { *rip = n; n += 4; }
    else if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    return n;
}
static int hk_len(const uint8_t *p, int *fix, int *kind) {
    int i = 0, op16 = 0, rexw = 0, rip = 0, imm = 0, modrm = 0;
    *fix = 0; *kind = 0;
    for (;; i++) {
        uint8_t b = p[i];
        if (b == 0x66) op16 = 1;
        else if (b != 0xF2 && b != 0xF3 && b != 0x2E && b != 0x3E && b != 0x36 && b != 0x26 && b != 0x64 && b != 0x65) break;
        if (i > 3) return 0;
    }
    if ((p[i] & 0xF0) == 0x40) { rexw = p[i] & 8; i++; }
    uint8_t o = p[i++];
    if (o == 0x0F) {
        uint8_t o2 = p[i++];
        if ((o2 & 0xF0) == 0x80) { *fix = i; return i + 4; }
        if (o2 == 0x0B || (o2 >= 0xC8 && o2 <= 0xCF)) return i;
        if (o2 == 0x05 || o2 == 0x34 || o2 == 0x77 || o2 == 0xA2 || o2 == 0x31 || o2 == 0x01) return 0;
        if (o2 == 0x38) { i++; modrm = 1; }
        else if (o2 == 0x3A) { i++; modrm = 1; imm = 1; }
        else { modrm = 1; if ((o2 >= 0x70 && o2 <= 0x73) || o2 == 0xA4 || o2 == 0xAC || o2 == 0xBA || o2 == 0xC2 || o2 == 0xC4 || o2 == 0xC5 || o2 == 0xC6) imm = 1; }
    } else if (o < 0x40) {
        int k = o & 7;
        if (k < 4) modrm = 1; else if (k == 4) imm = 1; else if (k == 5) imm = op16 ? 2 : 4; else return 0;
    } else if (o >= 0x50 && o <= 0x5F) { }
    else if (o == 0x63 || (o >= 0x84 && o <= 0x8F)) modrm = 1;
    else if (o == 0x68) imm = 4;
    else if (o == 0x6A) imm = 1;
    else if (o == 0x69) { modrm = 1; imm = op16 ? 2 : 4; }
    else if (o == 0x6B) { modrm = 1; imm = 1; }
    else if (o >= 0x70 && o <= 0x7F) { if (i != 1) return 0; *kind = 3; return 2; }
    else if (o == 0x80 || o == 0x83 || o == 0xC0 || o == 0xC1 || o == 0xC6) { modrm = 1; imm = 1; }
    else if (o == 0x81 || o == 0xC7) { modrm = 1; imm = op16 ? 2 : 4; }
    else if (o == 0x90 || o == 0x98 || o == 0x99 || o == 0xC9 || o == 0xCC) { }
    else if (o == 0xA8) imm = 1;
    else if (o == 0xA9) imm = op16 ? 2 : 4;
    else if (o >= 0xB0 && o <= 0xB7) imm = 1;
    else if (o >= 0xB8 && o <= 0xBF) imm = rexw ? 8 : op16 ? 2 : 4;
    else if (o == 0xC3) { *kind = 5; return i; }
    else if (o >= 0xD0 && o <= 0xD3) modrm = 1;
    else if (o == 0xE9) { *fix = i; *kind = 5; return i + 4; }
    else if (o == 0xEB) { if (i != 1) return 0; *kind = 4; return 2; }
    else if (o == 0xF6 || o == 0xF7) { modrm = 1; if (((p[i] >> 3) & 7) < 2) imm = o == 0xF6 ? 1 : op16 ? 2 : 4; }
    else if (o == 0xFE) modrm = 1;
    else if (o == 0xFF) { int r = (p[i] >> 3) & 7; modrm = 1; if (r == 2 || r == 3 || r == 5) return 0; if (r == 4) *kind = 5; }
    else return 0;
    if (modrm) { int m = hk_modrm(p + i, &rip); if (rip) *fix = i + rip; i += m; }
    return i + imm;
}

/* Move the entry instructions of `src` (at least 5 bytes) to `dst`, re-addressed; returns the number of source bytes
 * moved and sets *out to the bytes written, or 0 with a reason. */
static int hk_move(const uint8_t *src, uint8_t *dst, int *out, const char **why, int dry) {
    int n = 0, w = 0, ended = 0, nto = 0;
    const uint8_t *tos[8];
    while (n < 5) {
        int fix, kind, len;
        if (ended) {                                   /* the function is shorter than 5 bytes: only padding may follow */
            if (src[n] != 0xCC) { *why = "function shorter than 5 bytes"; return 0; }
            n++; continue;
        }
        len = hk_len(src + n, &fix, &kind);
        if (!len) { *why = "entry instruction not known to the mover"; return 0; }
        if (kind == 3 || kind == 4) {
            const uint8_t *to = src + n + 2 + (int8_t)src[n + 1];
            tos[nto++] = to;
            if (kind == 3) { dst[w++] = 0x0F; dst[w++] = (uint8_t)(0x80 | (src[n] & 0x0F)); } else dst[w++] = 0xE9;
            int32_t rel = (int32_t)(to - (dst + w + 4));
            memcpy(dst + w, &rel, 4); w += 4;
            if (kind == 4) ended = 1;
        } else {
            memcpy(dst + w, src + n, (size_t)len);
            if (fix) {
                int32_t rel; memcpy(&rel, src + n + fix, 4);
                const uint8_t *to = src + n + len + rel;
                tos[nto++] = to;
                int64_t nrel = to - (dst + w + len);
                if (!dry && nrel != (int32_t)nrel) { *why = "relative operand out of reach"; return 0; }
                rel = (int32_t)nrel; memcpy(dst + w + fix, &rel, 4);
            }
            w += len;
            if (kind == 5) ended = 1;
        }
        n += len;
    }
    for (int i = 0; i < nto; i++) if (tos[i] > src && tos[i] < src + n) { *why = "branch into the moved entry bytes"; return 0; }
    *out = w;
    return n;
}

/* Memory within reach of a rel32 from `near`: `bytes` of code and `data` bytes of data. Code and data are in
 * different halves of a pool: a word that is written on every call (the call counter) must not share a page with
 * code that runs, or the processor flushes its pipeline on every write (measured: 290 ns per call instead of 6). */
#undef near
static uint8_t *hk_alloc(const uint8_t *near, size_t bytes, size_t data, uint8_t **dp) {
    for (int i = g_hk_pools - 1; i >= 0; i--) {
        int64_t d = (int64_t)(g_hk_pool[i].base - near);
        if (d > -0x70000000ll && d < 0x70000000ll && g_hk_pool[i].used + bytes <= HK_POOL_BYTES / 2 && g_hk_pool[i].data + data <= HK_POOL_BYTES / 2) {
            uint8_t *p = g_hk_pool[i].base + g_hk_pool[i].used;
            *dp = g_hk_pool[i].base + HK_POOL_BYTES / 2 + g_hk_pool[i].data;
            g_hk_pool[i].used += bytes; g_hk_pool[i].data += data;
            return p;
        }
    }
    if (g_hk_pools >= 1024) return NULL;
    MEMORY_BASIC_INFORMATION mi;
    uintptr_t at = (uintptr_t)near;
    for (int tries = 0; tries < 4096; tries++) {       /* walk down from the image that holds `near` to the first free 64 KB boundary */
        if (!VirtualQuery((void *)at, &mi, sizeof mi)) return NULL;
        if (mi.State == MEM_FREE) {
            uintptr_t top = ((uintptr_t)mi.BaseAddress + mi.RegionSize) & ~(uintptr_t)0xffff;
            if (top >= (uintptr_t)mi.BaseAddress + HK_POOL_BYTES && top >= HK_POOL_BYTES) {
                uint8_t *p = VirtualAlloc((void *)(top - HK_POOL_BYTES), HK_POOL_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                if (p && (int64_t)(near - p) < 0x70000000ll) {
                    g_hk_pool[g_hk_pools].base = p; g_hk_pool[g_hk_pools].used = bytes; g_hk_pool[g_hk_pools].data = data; g_hk_pools++;
                    *dp = p + HK_POOL_BYTES / 2;
                    return p;
                }
                if (p) VirtualFree(p, 0, MEM_RELEASE);
            }
            if ((uintptr_t)mi.BaseAddress < 0x20000) break;
            at = (uintptr_t)mi.BaseAddress - 1;
        } else {
            uintptr_t b = (uintptr_t)(mi.AllocationBase ? mi.AllocationBase : mi.BaseAddress);
            if (b < 0x20000) break;
            at = b - 1;
        }
        if ((int64_t)((uintptr_t)near - at) > 0x60000000ll) break;
    }
    at = (uintptr_t)near;                              /* nothing below within reach (a dense block of DLLs): walk up */
    for (int tries = 0; tries < 8192; tries++) {
        if (!VirtualQuery((void *)at, &mi, sizeof mi)) return NULL;
        uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_FREE) {
            uintptr_t lo = ((uintptr_t)mi.BaseAddress + 0xffff) & ~(uintptr_t)0xffff;
            if (lo + HK_POOL_BYTES <= end && (int64_t)(lo + HK_POOL_BYTES - (uintptr_t)near) < 0x70000000ll) {
                uint8_t *p = VirtualAlloc((void *)lo, HK_POOL_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                if (p) {
                    g_hk_pool[g_hk_pools].base = p; g_hk_pool[g_hk_pools].used = bytes; g_hk_pool[g_hk_pools].data = data; g_hk_pools++;
                    *dp = p + HK_POOL_BYTES / 2;
                    return p;
                }
            }
        }
        at = end;
        if ((int64_t)(at - (uintptr_t)near) > 0x60000000ll) return NULL;
    }
    return NULL;
}

static HkSite **hk_site_slot(uint64_t addr) {
    unsigned h = (unsigned)((addr * 0x9E3779B97F4A7C15ull) >> 46) & (HK_HASH - 1);
    while (g_hk_site[h] && g_hk_site[h]->addr != addr) h = (h + 1) & (HK_HASH - 1);
    return &g_hk_site[h];
}

/* first hook on this function: trampoline, site, and the 5-byte entry jump */
static HkSite *hk_site_make(uint64_t addr, uint8_t *target) {
    const char *why = "";
    uint8_t dry[64], *data = NULL;
    int w = 0, n = hk_move(target, dry, &w, &why, 1);
    uint8_t *mem = n ? hk_alloc(target, HK_SITE_BYTES, 8, &data) : NULL;
    if (n && !mem) { snprintf(g_hk_error, sizeof g_hk_error, "no executable memory near the function at %llx", (unsigned long long)addr); return NULL; }
    if (n) n = hk_move(target, mem + 8, &w, &why, 0);
    if (!n) { snprintf(g_hk_error, sizeof g_hk_error, "function at %llx cannot be hooked: %s (first bytes %02x %02x %02x %02x %02x %02x)", (unsigned long long)addr, why, target[0], target[1], target[2], target[3], target[4], target[5]); return NULL; }
    HkSite *s = calloc(1, sizeof *s);
    s->addr = addr; s->target = target; s->mem = mem; s->head = data; s->moved = n;
    memcpy(s->saved, target, 8);
    uint8_t *t = mem + 8 + w;                          /* trampoline tail: jmp [rip+0]; <address behind the moved bytes> */
    t[0] = 0xFF; t[1] = 0x25; memset(t + 2, 0, 4);
    uint64_t back = (uint64_t)(uintptr_t)(target + n);
    memcpy(t + 6, &back, 8);
    s->tramp = (ImportFn)(void *)(mem + 8);
    int32_t hrel = (int32_t)(data - (mem + 6));
    mem[0] = 0xFF; mem[1] = 0x25; memcpy(mem + 2, &hrel, 4); mem[6] = 0xCC; mem[7] = 0xCC;      /* jmp [head] */
    uint64_t head = (uint64_t)(uintptr_t)s->tramp;     /* no layer yet: the site runs the original */
    memcpy(data, &head, 8);
    uint8_t patch[5] = {0xE9};
    int32_t rel = (int32_t)(mem - (target + 5));
    memcpy(patch + 1, &rel, 4);
    DWORD old;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) { snprintf(g_hk_error, sizeof g_hk_error, "cannot write the code page of %llx", (unsigned long long)addr); free(s); return NULL; }
    memcpy(target, patch, 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    *hk_site_slot(addr) = s;
    return s;
}

static int hk_layer_new(void) {
    if (g_hk_layers >= HK_MAX_LAYERS) return 0;
    if (!g_hk_layer[g_hk_layers >> 10]) g_hk_layer[g_hk_layers >> 10] = calloc(1024, sizeof(HkLayer));
    return ++g_hk_layers;
}

/* put layer `id` on the function at `addr` (lock held) */
static int hk_attach(int id, uint64_t addr) {
    HkLayer *l = hk_layer(id);
    ImportFn fn = function_at(addr);
    if (!fn) { snprintf(g_hk_error, sizeof g_hk_error, "%llx is not the entry of a lifted function (of a loaded module or of main)", (unsigned long long)addr); return 0; }
    HkSite *s = *hk_site_slot(addr);
    if (!s && !(s = hk_site_make(addr, (uint8_t *)(void *)fn))) return 0;
    uint8_t *d = NULL, *m = hk_alloc(s->target, HK_LAYER_BYTES, 24, &d);
    if (!m) { snprintf(g_hk_error, sizeof g_hk_error, "no executable memory near the function at %llx", (unsigned long long)addr); return 0; }
    uint64_t head; memcpy(&head, s->head, 8);
    l->site = s; l->mem = m; l->data = d; l->addr = addr; l->orig = (ImportFn)(void *)(uintptr_t)head;
    /* data: fn +0, user +8, calls +16.  code: +0 inc qword [calls]; +7 mov rdx,[user]; +14 jmp [fn] */
    static const uint8_t code[24] = {0x48, 0xFF, 0x05, 0, 0, 0, 0,  0x48, 0x8B, 0x15, 0, 0, 0, 0,  0xFF, 0x25, 0, 0, 0, 0,  0xCC, 0xCC, 0xCC, 0xCC};
    memcpy(m, code, 24);
    int32_t r1 = (int32_t)(d + 16 - (m + 7)), r2 = (int32_t)(d + 8 - (m + 14)), r3 = (int32_t)(d - (m + 20));
    memcpy(m + 3, &r1, 4); memcpy(m + 10, &r2, 4); memcpy(m + 16, &r3, 4);
    uint64_t f = (uint64_t)(uintptr_t)(void *)l->repl, u = (uint64_t)(uintptr_t)l->user, z = 0;
    memcpy(d, &f, 8); memcpy(d + 8, &u, 8); memcpy(d + 16, &z, 8);
    if (l->orig_out) *l->orig_out = l->orig;
    l->state = HK_ON;
    InterlockedExchange64((volatile LONG64 *)s->head, (LONG64)(uintptr_t)m);       /* from here on the function runs the replacement */
    s->top = id;
    return id;
}

static uint64_t hk_address_of(const char *name) {
    if (!g_main_hash) ro_init_tables();
    int mi = main_find(name);
    if (mi >= 0 && dyn_syms[mi].defined) return GUEST_BASE + dyn_syms[mi].value;
    const Export *e = exp_find(name);
    return e ? e->value : 0;
}

/* ---------------------------------------------------------------- the API (hook.h) */
int hook_install(uint64_t addr, HookFn repl, void *user, ImportFn *orig) {
    AcquireSRWLockExclusive(&g_hk_lock);
    int id = hk_layer_new();
    if (id) {
        HkLayer *l = hk_layer(id);
        l->repl = repl; l->user = user; l->orig_out = orig;
        if (!hk_attach(id, addr)) { l->state = HK_FREE; g_hk_layers--; id = 0; }
    } else snprintf(g_hk_error, sizeof g_hk_error, "hook table full");
    ReleaseSRWLockExclusive(&g_hk_lock);
    return id;
}
/* by exported (mangled) name; a name no loaded module exports yet is kept and installed when its module is loaded:
 * *orig is written then, so it must point to storage that stays (a static) */
int hook_install_name(const char *name, HookFn repl, void *user, ImportFn *orig) {
    uint64_t addr = hk_address_of(name);
    if (addr && function_at(addr)) return hook_install(addr, repl, user, orig);
    AcquireSRWLockExclusive(&g_hk_lock);
    int id = hk_layer_new();
    if (id) {
        HkLayer *l = hk_layer(id);
        l->repl = repl; l->user = user; l->orig_out = orig; l->name = _strdup(name); l->state = HK_PENDING;
        g_hk_pending++;
    }
    ReleaseSRWLockExclusive(&g_hk_lock);
    return id;
}
int hook_remove(int id) {                              /* the function runs what it ran before this hook; the layer stays */
    HkLayer *l = hk_layer(id);
    if (!l || l->state == HK_FREE) return 0;
    if (l->state == HK_PENDING) { l->state = HK_FREE; return 1; }
    InterlockedExchange64((volatile LONG64 *)l->data, (LONG64)(uintptr_t)(void *)l->orig);
    l->state = HK_OFF;
    return 1;
}
int hook_enable(int id) {
    HkLayer *l = hk_layer(id);
    if (!l || (l->state != HK_OFF && l->state != HK_ON)) return 0;
    InterlockedExchange64((volatile LONG64 *)l->data, (LONG64)(uintptr_t)(void *)l->repl);
    l->state = HK_ON;
    return 1;
}
long long hook_calls(int id) {                         /* how often the function reached this layer (also while removed) */
    HkLayer *l = hk_layer(id);
    long long n = 0;
    if (l && l->data) memcpy(&n, l->data + 16, 8);
    return n;
}
const char *hook_error(void) { return g_hk_error; }
uint64_t hook_address(const char *name) { return hk_address_of(name); }

/* nnro.c, when a module has been loaded and bound: hooks that were waiting for one of its names */
static void hook_module_loaded(NroModule *m) {
    if (!g_hk_pending) return;
    AcquireSRWLockExclusive(&g_hk_lock);
    for (int id = 1; id <= g_hk_layers; id++) {
        HkLayer *l = hk_layer(id);
        if (l->state != HK_PENDING) continue;
        for (int i = 1; i < m->sym_count; i++) {
            if (!m->syms[i].defined || strcmp(m->syms[i].name, l->name) != 0) continue;
            if (hk_attach(id, m->info->base + m->syms[i].value)) { g_hk_pending--; printf("hook: %s installed when %s was loaded\n", l->name, m->info->name); }
            else { l->state = HK_FREE; g_hk_pending--; printf("hook: %s NOT installed: %s\n", l->name, g_hk_error); }
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_hk_lock);
}

/* ---------------------------------------------------------------- mods: every DLL of the mods folder gets the API */
typedef struct {
    uint32_t size, version;
    int (*install)(uint64_t, HookFn, void *, ImportFn *);
    int (*install_name)(const char *, HookFn, void *, ImportFn *);
    int (*remove)(int);
    int (*enable)(int);
    long long (*calls)(int);
    uint64_t (*address)(const char *);
    void (*call)(Ctx *, uint64_t);
    void (*log)(const char *);
    const char *(*error)(void);
} HookApi;
static void hk_log(const char *s) { printf("%s\n", s); trace_printf("%s\n", s); }
static const HookApi g_hook_api = {sizeof(HookApi), 1, hook_install, hook_install_name, hook_remove, hook_enable, hook_calls, hook_address, call_indirect, hk_log, hook_error};

static void hook_load_mods(void) {
    char dir[MAX_PATH * 2], pat[MAX_PATH * 2];
    const char *env = getenv("SSBU_MODS");
    if (!env || !env[0] || strcmp(env, "0") == 0) return;      /* no default folder yet: several exes share this directory during development */
    snprintf(dir, sizeof dir, "%s", env);
    snprintf(pat, sizeof pat, "%s\\*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH * 3];
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        HMODULE dll = LoadLibraryA(path);
        void (*init)(const HookApi *) = dll ? (void (*)(const HookApi *))(void *)GetProcAddress(dll, "mod_init") : NULL;
        if (!init) { printf("mod: %s is not a mod (no mod_init)\n", path); if (dll) FreeLibrary(dll); continue; }
        int before = g_hk_layers;
        init(&g_hook_api);
        printf("mod: %s loaded, %d hooks\n", fd.cFileName, g_hk_layers - before);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* ---------------------------------------------------------------- SSBU_HOOK_COUNT: call counters on any function */
static int g_hk_count_id[64], g_hk_count_n;
static char g_hk_count_name[64][96];
static void hook_count_report(void) {
    for (int i = 0; i < g_hk_count_n; i++) printf("hook count: %-60s %lld calls\n", g_hk_count_name[i], hook_calls(g_hk_count_id[i]));
    fflush(stdout);
}
static void hk_pass(Ctx *c, void *user) { (*(ImportFn *)user)(c); }
static void hook_count_start(void) {
    const char *list = getenv("SSBU_HOOK_COUNT");
    static ImportFn orig[64];
    if (!list || !list[0]) return;
    while (*list && g_hk_count_n < 64) {
        size_t n = strcspn(list, ",");
        char *nm = g_hk_count_name[g_hk_count_n];
        snprintf(nm, 96, "%.*s", (int)(n < 95 ? n : 95), list);
        char *end; uint64_t a = strtoull(nm, &end, 16);
        int id = (*end == 0 && a) ? hook_install(a, hk_pass, &orig[g_hk_count_n], &orig[g_hk_count_n]) : hook_install_name(nm, hk_pass, &orig[g_hk_count_n], &orig[g_hk_count_n]);
        if (id) g_hk_count_id[g_hk_count_n++] = id; else printf("hook count: %s: %s\n", nm, g_hk_error);
        list += n; if (*list == ',') list++;
    }
    atexit(hook_count_report);
}

/* ---------------------------------------------------------------- SSBU_FIGHT_HEALTH: is a fight alive? (numbers, no screenshots)
 * S4. Pass-through hooks on five script-side functions of the game (change of motion, status request, and three
 * getters every status script calls all the time). Whenever a FIGHTER's module accessor passes through one of them
 * (object id >> 28 == 0, the public layout of skyline-smash), that fighter is sampled with the game's own getters:
 * status kind, motion kind, damage. Per fighter: script calls seen, status changes, motion changes, change_motion
 * calls made by scripts, damage now. One line per fighter every SSBU_FIGHT_HEALTH seconds (default 5) and at exit:
 *   [health] t=40.0s fighter 1 kind 2: script calls 51234, status changes 61 (1.5/s), motion changes 70, ...
 * A healthy level-3 CPU changes status more than once per second; a frozen fighter shows 0 changes and, if its
 * scripts no longer run at all, no new script calls. Works with fighter source on or off (the hooks sit in main).
 * tools: fighter_sdk\tools\probe.py health. */
typedef struct {
    uint64_t boma; int kind, status, have; uint64_t motion; float damage;
    long long calls, req_motion, req_status, status_changes, motion_changes, calls_at_print, sc_at_print;
    double first, last_sample;
} HkHealth;
static HkHealth g_hh[8];
static int g_hh_n;
static ImportFn g_hh_orig[5];
static uint64_t g_hh_damage, g_hh_kind;
static double g_hh_every, g_hh_last_print;
static SRWLOCK g_hh_lock = SRWLOCK_INIT;
static double hh_now(void) {
    static LARGE_INTEGER f; LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}
static void hh_print(double now) {
    for (int k = 0; k < g_hh_n; k++) {
        HkHealth *h = &g_hh[k];
        double t = now - h->first;
        printf("[health] t=%.1fs fighter %d kind %d: script calls %lld (+%lld), status changes %lld (%.2f/s, +%lld), motion changes %lld, change_motion calls %lld, status requests %lld, status 0x%x, motion %llx, damage %.2f%%\n",
               t, k, h->kind, h->calls, h->calls - h->calls_at_print, h->status_changes, t > 0 ? (double)h->status_changes / t : 0.0, h->status_changes - h->sc_at_print,
               h->motion_changes, h->req_motion, h->req_status, h->status, (unsigned long long)h->motion, (double)h->damage);
        h->calls_at_print = h->calls; h->sc_at_print = h->status_changes;
    }
    g_hh_last_print = now;
}
static void hh_exit(void) { if (g_hh_n) { printf("[health] at exit:\n"); hh_print(hh_now()); } fflush(stdout); }
static void hk_health(Ctx *c, void *user) {
    int which = (int)(intptr_t)user;
    uint64_t boma = c->x[0];
    g_hh_orig[which](c);
    if (!boma || (rd32(boma + 8) >> 28) != 0 || !g_hh_orig[3] || !g_hh_orig[4]) return;
    AcquireSRWLockExclusive(&g_hh_lock);
    double now = hh_now();
    int k = 0;
    while (k < g_hh_n && g_hh[k].boma != boma) k++;
    if (k == g_hh_n) {
        if (g_hh_n == 8 || (g_hh_n && now - g_hh[0].last_sample > 20.0)) {      /* a new fight: the old table is printed and dropped */
            printf("[health] new set of fighters; the last one was:\n"); hh_print(now); g_hh_n = 0; k = 0;
        }
        HkHealth *h = &g_hh[g_hh_n++];
        memset(h, 0, sizeof *h);
        h->boma = boma; h->first = now; h->kind = -1;
        if (g_hh_kind) { Ctx saved = *c; c->x[0] = boma; call_indirect(c, g_hh_kind); h->kind = (int)c->x[0]; *c = saved; }
    }
    HkHealth *h = &g_hh[k];
    h->calls++;
    if (which == 0) h->req_motion++;
    if (which == 1) h->req_status++;
    if (now - h->last_sample >= 0.012 || which < 2) {
        Ctx saved = *c;
        c->x[0] = boma; g_hh_orig[3](c); int s = (int)c->x[0];
        c->x[0] = boma; g_hh_orig[4](c); uint64_t m = c->x[0];
        float d = h->damage;
        if (g_hh_damage) { c->x[0] = boma; c->x[1] = 0; call_indirect(c, g_hh_damage); d = c->v[0].f[0]; }
        *c = saved;
        if (h->have && s != h->status) h->status_changes++;
        if (h->have && m != h->motion) h->motion_changes++;
        h->status = s; h->motion = m; h->damage = d; h->have = 1; h->last_sample = now;
    }
    if (now - g_hh_last_print >= g_hh_every) hh_print(now);
    ReleaseSRWLockExclusive(&g_hh_lock);
}
static void hook_health_start(void) {
    static const char *const names[5] = {
        "_ZN3app8lua_bind32MotionModule__change_motion_implEPNS_26BattleObjectModuleAccessorEN3phx6Hash40Effbfbb",
        "_ZN3app8lua_bind40StatusModule__change_status_request_implEPNS_26BattleObjectModuleAccessorEib",
        "_ZN3app8lua_bind24MotionModule__frame_implEPNS_26BattleObjectModuleAccessorE",
        "_ZN3app8lua_bind30StatusModule__status_kind_implEPNS_26BattleObjectModuleAccessorE",
        "_ZN3app8lua_bind30MotionModule__motion_kind_implEPNS_26BattleObjectModuleAccessorE",
    };
    const char *e = getenv("SSBU_FIGHT_HEALTH");
    if (!e || !e[0] || e[0] == '0') return;
    g_hh_every = atof(e) >= 2.0 ? atof(e) : 5.0;
    g_hh_last_print = hh_now();
    g_hh_damage = hk_address_of("_ZN3app8lua_bind25DamageModule__damage_implEPNS_26BattleObjectModuleAccessorEi");
    g_hh_kind = hk_address_of("_ZN3app7utility8get_kindEPKNS_26BattleObjectModuleAccessorE");
    int ok = 0;
    for (int i = 0; i < 5; i++) {
        if (hook_install_name(names[i], hk_health, (void *)(intptr_t)i, &g_hh_orig[i])) ok++;
        else printf("[health] %s: %s\n", names[i], g_hk_error);
    }
    printf("[health] fight health probe on: %d of 5 hooks, damage getter %s, a line per fighter every %.0f s\n", ok, g_hh_damage ? "found" : "NOT found", g_hh_every);
    if (ok < 5) { printf("[health] not all hooks: the probe is off\n"); g_hh_every = 1e30; }
    atexit(hh_exit);
}

/* ---------------------------------------------------------------- SSBU_HOOK_SELFTEST */
static void hook_census(const char *what, const FnEntry *fns, int count) {
    static uint8_t scratch[64];
    int ok = 0, bad = 0, hist[16] = {0}; const char *why = ""; int w;
    char first[3][120]; int nf = 0;
    for (int i = 0; i < count; i++) {
        const uint8_t *p = (const uint8_t *)(void *)fns[i].fn;
        if (*hk_site_slot(fns[i].addr)) { ok++; continue; }        /* already hooked: its entry is our jump */
        int n = hk_move(p, scratch + 16, &w, &why, 1);
        if (n) { ok++; hist[n < 15 ? n : 15]++; }
        else { if (nf < 3) snprintf(first[nf++], 120, "%llx: %s (%02x %02x %02x %02x %02x)", (unsigned long long)fns[i].addr, why, p[0], p[1], p[2], p[3], p[4]); bad++; }
    }
    printf("hook census: %s: %d of %d functions can be hooked, %d refused\n", what, ok, count, bad);
    for (int i = 0; i < nf; i++) printf("hook census:   refused %s\n", first[i]);
}

static long long g_hk_t_count;
static ImportFn g_hk_t_orig, g_hk_t_orig2;
static void hk_t_repl(Ctx *c, void *user) { g_hk_t_count++; g_hk_t_orig(c); c->x[0] += (uint64_t)(uintptr_t)user; }
static void hk_t_repl2(Ctx *c, void *user) { (void)user; g_hk_t_orig2(c); c->x[0] *= 2; }
static int hk_check(const char *what, int ok) { printf("hook selftest: %-64s %s\n", what, ok ? "ok" : "FAILED"); return ok ? 0 : 1; }

/* main: f_7101519430 is `x0 = ((x1 & 0xfe) != 28)`: no memory, no calls (gen_eh_o2\c_01c2.c) */
static void hook_selftest_main(Ctx *c) {
    const uint64_t A = GA(0x7101519430ull);
    ImportFn f = function_at(A);
    Ctx saved = *c; int bad = 0;
    hook_census("main", fn_table, fn_table_count);
    if (!f) { printf("hook selftest: main test function not found\n"); return; }
#define HK_RUN(v) (c->x[1] = (v), c->x[0] = 99, f(c), c->x[0])
    bad += hk_check("main, before: f(28)=0, f(5)=1", HK_RUN(28) == 0 && HK_RUN(5) == 1);
    int id = hook_install(A, hk_t_repl, (void *)(uintptr_t)10, &g_hk_t_orig);
    bad += hk_check("main, hook installed", id > 0);
    if (id > 0) {
        bad += hk_check("main, replacement runs and calls the original: f(28)=10, f(5)=11", HK_RUN(28) == 10 && HK_RUN(5) == 11 && g_hk_t_count == 2 && hook_calls(id) == 2);
        c->x[1] = 5; c->x[0] = 99; call_indirect(c, A);
        bad += hk_check("main, same through call_indirect", c->x[0] == 11 && g_hk_t_count == 3);
        int id2 = hook_install(A, hk_t_repl2, NULL, &g_hk_t_orig2);
        bad += hk_check("main, second layer on top (doubles the first's result): f(5)=22", id2 > 0 && HK_RUN(5) == 22 && g_hk_t_count == 4);
        hook_remove(id);
        bad += hk_check("main, first layer removed, second stays: f(5)=2", HK_RUN(5) == 2 && g_hk_t_count == 4 && hook_calls(id) == 5);
        hook_enable(id); hook_remove(id2);
        bad += hk_check("main, first enabled again, second removed: f(5)=11", HK_RUN(5) == 11 && g_hk_t_count == 5);
        hook_remove(id);
        bad += hk_check("main, all removed: f(28)=0, f(5)=1, counter unchanged", HK_RUN(28) == 0 && HK_RUN(5) == 1 && g_hk_t_count == 5);
        LARGE_INTEGER q, t0, t1; QueryPerformanceFrequency(&q);
        hook_enable(id); QueryPerformanceCounter(&t0);
        for (int i = 0; i < 10000000; i++) { c->x[1] = (uint64_t)i; f(c); }
        QueryPerformanceCounter(&t1); hook_remove(id);
        double hooked = (double)(t1.QuadPart - t0.QuadPart) / (double)q.QuadPart;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 10000000; i++) { c->x[1] = (uint64_t)i; f(c); }
        QueryPerformanceCounter(&t1);
        double removed = (double)(t1.QuadPart - t0.QuadPart) / (double)q.QuadPart;
        ImportFn tr = (*hk_site_slot(A))->tramp;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 10000000; i++) { c->x[1] = (uint64_t)i; tr(c); }
        QueryPerformanceCounter(&t1);
        double plain = (double)(t1.QuadPart - t0.QuadPart) / (double)q.QuadPart;
        printf("hook selftest: cost per call over 10,000,000 calls: original %.2f ns, hook removed (2 jumps more) %.2f ns, hooked with a C replacement that calls the original %.2f ns\n",
               plain * 100.0, removed * 100.0, hooked * 100.0);
    }
#undef HK_RUN
    printf("hook selftest: main: %s\n", bad ? "FAILED" : "all checks passed");
    *c = saved;
}

/* lua2cpp_common: f_7200024160 sets x0 = x8, x1 = 1 and tail-calls f_720052f180 DIRECTLY (the stub of
 * lib::L2CValue::L2CValue(bool)): the hook on the callee must be reached through the caller's direct call. */
static void hk_t_mod(Ctx *c, void *user) { uint64_t out = c->x[0]; g_hk_t_count++; g_hk_t_orig(c); if (user) wr64(out + 8, 0); }
static void hook_selftest_module(Ctx *c, NroModule *m) {
    char what[96]; snprintf(what, sizeof what, "%s", m->info->name);
    hook_census(what, m->fns, m->fn_count);
    if (strcmp(m->info->name, "lua2cpp_common") != 0) return;
    const uint64_t CALLER = 0x7200024160ull, CALLEE = 0x720052f180ull;
    ImportFn f = function_at(CALLER);
    static uint64_t val[2];
    Ctx saved = *c; int bad = 0;
    if (!f || !function_at(CALLEE)) { printf("hook selftest: module test functions not found\n"); return; }
#define HK_RUN() (val[0] = 77, val[1] = 77, c->x[8] = (uint64_t)(uintptr_t)val, f(c))
    HK_RUN();
    bad += hk_check("module, before: caller builds L2CValue(true)", (uint32_t)val[0] == 1 && (val[1] & 1) == 1);
    g_hk_t_count = 0;
    int id = hook_install(CALLEE, hk_t_mod, (void *)1, &g_hk_t_orig);
    HK_RUN();
    bad += hk_check("module, hook on the callee is reached by the caller's DIRECT call", id > 0 && g_hk_t_count == 1 && (uint32_t)val[0] == 1 && (val[1] & 1) == 0);
    hook_remove(id);
    HK_RUN();
    bad += hk_check("module, removed: original result, counter unchanged", g_hk_t_count == 1 && (uint32_t)val[0] == 1 && (val[1] & 1) == 1);
#undef HK_RUN
    printf("hook selftest: module: %s\n", bad ? "FAILED" : "all checks passed");
    *c = saved;
}

static int g_hk_selftest;
static void fighter_debug_start(void);                 /* S10: fighter_host.c, SSBU_FIGHTER_DEBUG (test only; nothing without the variable) */
static void hook_start(Ctx *c) {                       /* once, before the game starts (nnro.c module_coverage_start) */
    ro_init_tables();
    g_hk_selftest = getenv("SSBU_HOOK_SELFTEST") != NULL;
    if (g_hk_selftest) hook_selftest_main(c);
    hook_count_start();
    hook_health_start();
    fighter_debug_start();
    hook_load_mods();
}

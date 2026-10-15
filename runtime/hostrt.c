/* Host runtime for the fully lifted game: image loader, relocation, indirect
 * call dispatch, import dispatch with tracing, and crash reporting. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define FILE_OFF 0x1000ULL
#define IMAGE_SIZE 0x7500000ULL          /* the whole image with bss: 13.0.4 and 13.0.5 both end below it */

/* ---------------------------------------------------------------- game builds (docs/VERSIONS.md)
 * The layout of main is read from main.elf (load_layout): its PT_LOAD segments (nso2elf.py writes text, read-only data,
 * data with bss) and the dynamic section MOD0 points to. 13.0.4: text 0x0..0x39c7e90, read-only data from 0x39c8000, data
 * from 0x4f16000, 0x52a8570 bytes in the file; RELA 0x39c8070 / 0x7ad0d8, JMPREL 0x4175148 / 0xc600, SYMTAB 0x4197610,
 * INIT_ARRAY 0x52a7be0 / 0x990 (the constants this replaces). 13.0.5 (update v2031616): text 0x39c8440, everything after
 * it one page (0x1000) later with the same sizes.
 * GA(a): the running build's address of the 13.0.4 address a. Read-only data, data and bss moved as whole segments, so
 * those addresses move by the page; code moved by different amounts in different places (13.0.5 changed a handful of
 * functions), so a code address must be in g_code_13_0_5, where each entry was found and checked as docs/VERSIONS.md says
 * (tools/port/verify_13_0_5.py repeats the check on a dump). A code address missing from it stops the program: calling the
 * wrong function silently would be worse. */
int g_game_build = -1;                               /* set by load_layout */
MainLayout g_main;
static const uint64_t g_code_13_0_5[][2] = {         /* 13.0.4 -> 13.0.5 offsets from GUEST_BASE, sorted by the first */
    {0x0382a0, 0x0382a0},       /* FUN_71000382a0 (nvn_gl.c), 2,884 bytes in both */
    {0x047d20, 0x047d20},       /* clock read return address (nnsdk.c, fixed seed) */
    {0x138534, 0x138534},       /* clock read return address (nnsdk.c, fixed seed) */
    {0x2c55e0, 0x2c55e0},       /* nninitStartup */
    {0x2c5620, 0x2c5620},       /* nnMain */
    {0x653240, 0x653240},       /* entry set-up (debug_mode.c) */
    {0x147f330, 0x147f330},     /* path formatter (newmodule_host.c) */
    {0x1519430, 0x1519430},     /* hook self-test function (hook_host.c) */
    {0x1788260, 0x17880c0},     /* player record builder (debug_mode.c) */
    {0x17df5f0, 0x17df5b0},     /* path sub-builder (newmodule_host.c) */
    {0x17dfe50, 0x17dfe10},     /* path sub-builder 2 (newmodule_host.c) */
    {0x1ea04d0, 0x1ea04d0},     /* formatter (newmodule_host.c) */
    {0x22b67d4, 0x22b67d4},     /* worker return address (newmodule_host.c) */
    {0x353e330, 0x353e8e0},     /* path -> file index (newmodule_host.c) */
    {0x35444c0, 0x3544a70},     /* resource service inflate thread: start (loose_files.c) */
    {0x35456d8, 0x3545c88},     /* and the end of its range (loose_files.c) */
    {0x35b5c50, 0x35b6200},     /* ostringstream code (cxxrt_selftest.h) */
    {0x366d1f0, 0x366d7a0},     /* clock read return address (nnsdk.c, fixed seed) */
    {0x366d388, 0x366d938},     /* clock read return address (nnsdk.c, fixed seed) */
    {0x36f2814, 0x36f2dc4},     /* call site (os_threads.c) */
    {0x3828300, 0x38288b0},     /* function with catch (...) (cxxrt_selftest.h) */
    {0x382b690, 0x382bc40},     /* call site in it */
    {0x382cb34, 0x382d0e4},     /* its landing pad */
    {0x3839a10, 0x3839fc0},     /* function with catch (std::ios_base::failure &) (cxxrt_selftest.h) */
    {0x3839b94, 0x383a144},     /* call site in it */
    {0x3839de8, 0x383a398},     /* its landing pad */
    {0x38697e0, 0x3869d90},     /* EH main test function (nnsdk2.c) */
    {0x386fc30, 0x38701e0},     /* graphics frame end (fighter_host.c, SSBU_DETERMINISTIC) */
    {0x38fae10, 0x38fb3c0},     /* luaD_throw */
    {0x38faf00, 0x38fb4b0},     /* luaD_rawrunprotected */
    {0x392dce0, 0x392e290},     /* je_aligned_alloc (the game's memalign) */
    {0x392e590, 0x392eb40},     /* free_default (the game's free) */
    {0x39a3e70, 0x39a4420},     /* zstd: free stream (nvn_vk_store.c) */
    {0x39a4040, 0x39a45f0},     /* zstd: decompress stream (nvn_vk_store.c) */
};
uint64_t ga(uint64_t a) {
    if (g_game_build < 0) { printf("\nGA: game address %llx asked for before main.elf was read: stopping\n", (unsigned long long)a); report_and_exit(8); }
    if (g_game_build == GAME_13_0_4 || a < GUEST_BASE || a >= GUEST_BASE + IMAGE_SIZE) return a;
    uint64_t o = a - GUEST_BASE;
    if (o >= 0x39c8000) return a + 0x1000;            /* 13.0.4's read-only data, data and bss: one page later */
    int lo = 0, hi = (int)(sizeof g_code_13_0_5 / sizeof g_code_13_0_5[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_code_13_0_5[mid][0] == o) return GUEST_BASE + g_code_13_0_5[mid][1];
        if (g_code_13_0_5[mid][0] < o) lo = mid + 1; else hi = mid - 1;
    }
    printf("\nGA: the 13.0.4 code address %llx has no 13.0.5 counterpart in hostrt.c (g_code_13_0_5): stopping\n", (unsigned long long)a);
    report_and_exit(8);
    return a;
}
/* main.elf's program headers (before the image is read) and the build */
static void load_layout(FILE *f, const char *elf) {
    uint8_t eh[64], ph[56];
    uint64_t load[3][3] = {{0}};                        /* vaddr, filesz, memsz of the PT_LOADs in order */
    int n = 0;
    if (fread(eh, 1, sizeof eh, f) != sizeof eh || memcmp(eh, "\x7f" "ELF", 4)) { fprintf(stderr, "%s is not an ELF file\n", elf); exit(1); }
    uint64_t phoff; uint16_t phnum;
    memcpy(&phoff, eh + 0x20, 8); memcpy(&phnum, eh + 0x38, 2);
    for (int i = 0; i < phnum; i++) {
        uint32_t type;
        fseek(f, (long)(phoff + (uint64_t)i * 56), SEEK_SET);
        if (fread(ph, 1, sizeof ph, f) != sizeof ph) break;
        memcpy(&type, ph, 4);
        if (type == 1 && n < 3) { memcpy(&load[n][0], ph + 16, 8); memcpy(&load[n][1], ph + 32, 8); memcpy(&load[n][2], ph + 40, 8); n++; }
    }
    if (n != 3) { fprintf(stderr, "%s: %d PT_LOAD segments, 3 expected (convert the dump with tools/extract/nso2elf.py)\n", elf, n); exit(1); }
    g_main.text_size = load[0][1]; g_main.ro_start = load[1][0]; g_main.data_start = load[2][0]; g_main.file_image = load[2][0] + load[2][1];
    if (g_main.text_size == 0x39c7e90) g_game_build = GAME_13_0_4;
    else if (g_main.text_size == 0x39c8440) g_game_build = GAME_13_0_5;
    else { fprintf(stderr, "%s: code size %#llx is not a build this runtime knows (13.0.4: 0x39c7e90, 13.0.5: 0x39c8440)\n", elf, (unsigned long long)g_main.text_size); exit(1); }
    if (load[2][0] + load[2][2] > IMAGE_SIZE) { fprintf(stderr, "%s: the image (%#llx bytes with bss) does not fit IMAGE_SIZE\n", elf, (unsigned long long)(load[2][0] + load[2][2])); exit(1); }
}
/* the dynamic section, from the image in memory: MOD0 at text + 4 gives its offset */
static void load_dynamic(void) {
    uint64_t mod0 = rd32(GUEST_BASE + 4), d = mod0 + (uint64_t)(int64_t)(int32_t)rd32(GUEST_BASE + mod0 + 4);
    for (int i = 0; i < 4096; i++, d += 16) {
        int64_t tag = (int64_t)rd64(GUEST_BASE + d); uint64_t v = rd64(GUEST_BASE + d + 8);
        if (tag == 0) break;
        if (tag == 7) g_main.rela = v; else if (tag == 8) g_main.relasz = v; else if (tag == 23) g_main.jmprel = v; else if (tag == 2) g_main.pltrelsz = v;
        else if (tag == 6) g_main.symtab = v; else if (tag == 5) g_main.strtab = v; else if (tag == 25) g_main.init_array = v; else if (tag == 27) g_main.init_arraysz = v;
    }
    printf("main: build %s, text %#llx, data at %#llx, RELA %#llx / %#llx, JMPREL %#llx / %#llx, SYMTAB %#llx, INIT_ARRAY %#llx / %#llx\n",
           g_game_build == GAME_13_0_5 ? "13.0.5" : "13.0.4", (unsigned long long)g_main.text_size, (unsigned long long)g_main.data_start,
           (unsigned long long)g_main.rela, (unsigned long long)g_main.relasz, (unsigned long long)g_main.jmprel, (unsigned long long)g_main.pltrelsz,
           (unsigned long long)g_main.symtab, (unsigned long long)g_main.init_array, (unsigned long long)g_main.init_arraysz);
}

static ImportFn *g_handlers;
static unsigned long long *g_import_calls;
static int *g_import_order, g_import_order_count;
static FILE *g_trace;
static unsigned long long g_trace_seq;
static FnEntry *g_by_host;                 /* fn_table sorted by host code address */
void sdk_dispatch(Ctx *c, uint64_t addr);          /* sdklift_rt.c: code lifted from sdk.elf at 0x75xxxxxxxx */
int nro_call_indirect(Ctx *c, uint64_t addr);      /* nnro.c: run-time modules at 0x72xxxxxxxx */
uint64_t nro_guest_function_of(uint64_t rip);

void trace_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_trace) { vfprintf(g_trace, fmt, ap); fflush(g_trace); }
    va_end(ap);
}

/* ---------------------------------------------------------------- lookup */
/* P3: guest address -> lifted function through a hash table built in runtime_init (open addressing, load below 1/2);
 * before that, and as the reference, the binary search over fn_table (sorted by address). Same answer either way. */
static struct { uint64_t addr; void (*fn)(Ctx *); } *g_fn_hash;
static unsigned g_fn_mask;
static inline unsigned fn_hash(uint64_t addr) { return (unsigned)(((addr >> 2) * 0x9E3779B97F4A7C15ULL) >> 40); }
static void fn_hash_build(void) {
    unsigned size = 1024;
    while (size < (unsigned)fn_table_count * 2u) size <<= 1;
    g_fn_hash = calloc(size, sizeof *g_fn_hash);
    for (int i = 0; i < fn_table_count; i++) {
        unsigned h = fn_hash(fn_table[i].addr) & (size - 1);
        while (g_fn_hash[h].addr) h = (h + 1) & (size - 1);
        g_fn_hash[h].addr = fn_table[i].addr; g_fn_hash[h].fn = fn_table[i].fn;
    }
    g_fn_mask = size - 1;
}
static void (*lookup_guest(uint64_t addr))(Ctx *) {
    if (g_fn_mask) {
        for (unsigned h = fn_hash(addr) & g_fn_mask;; h = (h + 1) & g_fn_mask) {
            if (g_fn_hash[h].addr == addr) return g_fn_hash[h].fn;
            if (!g_fn_hash[h].addr) return NULL;
        }
    }
    int lo = 0, hi = fn_table_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fn_table[mid].addr == addr) return fn_table[mid].fn;
        if (fn_table[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static int cmp_host(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const FnEntry *)a)->fn, y = (uintptr_t)((const FnEntry *)b)->fn;
    return x < y ? -1 : x > y;
}

/* Guest function whose host code contains `rip`, or 0. */
uint64_t guest_function_of(uint64_t rip) {
    int lo = 0, hi = fn_table_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if ((uintptr_t)g_by_host[mid].fn <= rip) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0 || rip - (uintptr_t)g_by_host[best].fn > 0x400000) return nro_guest_function_of(rip);
    return g_by_host[best].addr;
}

/* ---------------------------------------------------------------- host functions handed to the guest
 * Function pointers the guest obtains at run time (for example through nvnBootstrapLoader) are
 * placeholder addresses in their own range; calling one runs the registered host handler. */
typedef struct { char name[72]; ImportFn fn; volatile LONG64 calls; LONG64 first; } HostFn;
#define MAX_HOSTFN 4096
static HostFn g_hostfn[MAX_HOSTFN];
static volatile LONG g_hostfn_count;
static volatile LONG64 g_hostfn_seq;
static SRWLOCK g_hostfn_lock = SRWLOCK_INIT;
static __declspec(thread) int t_hostfn;

static void audit_hostfn(const char *name, ImportFn fn);
static int audit_path_hostfn(const char *name);
static void audit_path_hit(const char *list, const char *name, uint64_t from);
static void audit_exit_counts(void);
uint64_t hostfn_address(const char *name, ImportFn fn) {
    AcquireSRWLockExclusive(&g_hostfn_lock);
    int i, fresh = 0;
    for (i = 0; i < g_hostfn_count; i++) if (strcmp(g_hostfn[i].name, name) == 0) break;
    if (i == g_hostfn_count && i < MAX_HOSTFN) {
        strncpy(g_hostfn[i].name, name, sizeof g_hostfn[i].name - 1);
        g_hostfn[i].fn = fn;
        g_hostfn_count = i + 1;
        fresh = 1;
    }
    ReleaseSRWLockExclusive(&g_hostfn_lock);
    if (fresh) audit_hostfn(name, fn);
    return HOSTFN_BASE + (uint64_t)i * 16;
}
const char *hostfn_current_name(void) { return g_hostfn[t_hostfn].name; }
unsigned long long hostfn_current_calls(void) { return (unsigned long long)g_hostfn[t_hostfn].calls; }

void print_hostfn_summary(FILE *out) {
    int n = g_hostfn_count, used = 0;
    int *order = malloc(sizeof(int) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) if (g_hostfn[i].calls) order[used++] = i;
    for (int i = 1; i < used; i++)
        for (int j = i; j > 0 && g_hostfn[order[j]].first < g_hostfn[order[j - 1]].first; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    fprintf(out, "\nrun-time host functions: %d handed out, %d called, in order of first call:\n", n, used);
    for (int i = 0; i < used; i++) fprintf(out, "  %8lld  %s\n", (long long)g_hostfn[order[i]].calls, g_hostfn[order[i]].name);
    free(order);
}

/* Called on every failed exclusive store; reports a loop that never succeeds. */
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {
    if (++c->ex_fail == 1000 || c->ex_fail == 100000) {
        printf("STXR failing %llu times in f_%llx: addr=%llx expected=%llx seen=%llx new=%llx (%s%d)\n",
               (unsigned long long)c->ex_fail, (unsigned long long)c->fn, (unsigned long long)a, (unsigned long long)expected,
               (unsigned long long)seen, (unsigned long long)v, bits < 0 ? "monitor lost, " : "", bits < 0 ? -bits : bits);
        trace_printf("STXR failing %llu times in f_%llx: addr=%llx expected=%llx seen=%llx new=%llx\n",
                     (unsigned long long)c->ex_fail, (unsigned long long)c->fn, (unsigned long long)a, (unsigned long long)expected,
                     (unsigned long long)seen, (unsigned long long)v);
    }
}

/* Debug aid: SSBU_ICALLS=<start s>,<dur s> records every distinct (indirect call target, return address, context)
 * seen in that window with a count and the first x0..x2; icall_dump() prints them (boot.c, at the time limit). */
#define ICALL_MAX 16384
static struct { uint64_t target, lr, ctx, x0, x1, x2; long long n; } g_icall[ICALL_MAX];
static volatile LONG g_icall_lock;
static int g_icall_mode = -1;
static ULONGLONG g_icall_start, g_icall_end;
static unsigned long long g_poke_fn, g_poke_off;
static unsigned g_poke_val;
static void icall_record(Ctx *c, uint64_t addr) {
    if (g_icall_mode < 0) {
        int s = 0, d = 0;
        const char *e = getenv("SSBU_ICALLS");
        g_icall_mode = e && sscanf(e, "%d,%d", &s, &d) == 2 && d > 0;
        g_icall_start = GetTickCount64() + (ULONGLONG)s * 1000;
        g_icall_end = g_icall_start + (ULONGLONG)d * 1000;
        /* Experiment only (changes game DATA, never use for normal runs): SSBU_EXPERIMENT_POKE=<fn hex>:<off hex>=<byte hex>
         * writes that byte at x0+off each time fn is called indirectly. */
        e = getenv("SSBU_EXPERIMENT_POKE");
        if (e && sscanf(e, "%llx:%llx=%x", &g_poke_fn, &g_poke_off, &g_poke_val) == 3) { if (!g_icall_mode) g_icall_mode = 2; }
        else g_poke_fn = 0;
    }
    if (g_poke_fn && addr == g_poke_fn && rd8(c->x[0] + g_poke_off) != (uint8_t)g_poke_val) {
        printf("[poke] f_%llx: byte at x0+0x%llx (%llx) %02x -> %02x\n", g_poke_fn, g_poke_off, (unsigned long long)(c->x[0] + g_poke_off),
               rd8(c->x[0] + g_poke_off), g_poke_val);
        wr8(c->x[0] + g_poke_off, (uint8_t)g_poke_val);
    }
    if (g_icall_mode != 1) return;
    ULONGLONG now = GetTickCount64();
    if (now < g_icall_start || now > g_icall_end) return;
    uint64_t lr = c->x[30], key = addr * 31 + lr * 7 + (uint64_t)(uintptr_t)c;
    unsigned h = (unsigned)((key >> 2) * 2654435761u) % ICALL_MAX;
    while (InterlockedCompareExchange(&g_icall_lock, 1, 0)) YieldProcessor();
    for (int probe = 0; probe < ICALL_MAX; probe++, h = (h + 1) % ICALL_MAX) {
        if (g_icall[h].target == addr && g_icall[h].lr == lr && g_icall[h].ctx == (uint64_t)(uintptr_t)c) { g_icall[h].n++; break; }
        if (!g_icall[h].target) {
            g_icall[h].target = addr; g_icall[h].lr = lr; g_icall[h].ctx = (uint64_t)(uintptr_t)c;
            g_icall[h].x0 = c->x[0]; g_icall[h].x1 = c->x[1]; g_icall[h].x2 = c->x[2]; g_icall[h].n = 1;
            break;
        }
    }
    g_icall_lock = 0;
}
uint64_t icall_first_x0(uint64_t target) {
    for (int i = 0; i < ICALL_MAX; i++) if (g_icall[i].target == target) return g_icall[i].x0;
    return 0;
}
void icall_dump(FILE *out, uint64_t main_ctx) {
    if (g_icall_mode <= 0) return;
    fprintf(out, "\n[icall] indirect calls seen in the window (M = main thread)\n");
    for (int i = 0; i < ICALL_MAX; i++)
        if (g_icall[i].target)
            fprintf(out, "[icall] %c f_%llx lr=%llx n=%lld x0=%llx x1=%llx x2=%llx\n", g_icall[i].ctx == main_ctx ? 'M' : '-',
                    (unsigned long long)g_icall[i].target, (unsigned long long)g_icall[i].lr, g_icall[i].n,
                    (unsigned long long)g_icall[i].x0, (unsigned long long)g_icall[i].x1, (unsigned long long)g_icall[i].x2);
}

void call_indirect(Ctx *c, uint64_t addr) {
    if (g_icall_mode) icall_record(c, addr);
    if (addr - GUEST_BASE < IMAGE_SIZE) {            /* P3: the common case first (a function of main); the ranges tested below do not overlap it */
        void (*fn)(Ctx *) = lookup_guest(addr);
        if (!fn) guest_trap("indirect call to an address that is not a known function", addr);
        fn(c);
        return;
    }
    if (addr >= HOSTFN_BASE && addr < HOSTFN_BASE + (uint64_t)g_hostfn_count * 16) {
        int i = (int)((addr - HOSTFN_BASE) / 16);
        if (InterlockedIncrement64(&g_hostfn[i].calls) == 1) {
            g_hostfn[i].first = InterlockedIncrement64(&g_hostfn_seq);
            if (audit_path_hostfn(g_hostfn[i].name)) audit_path_hit("run-time host function", g_hostfn[i].name, c->x[30] - 4);
        }
        t_hostfn = i;
        const char *saved_name = c->in_import;
        uint64_t saved_from = c->import_from;
        c->in_import = g_hostfn[i].name;             /* so thread dumps can name the host function */
        c->import_from = c->x[30] - 4;
        g_hostfn[i].fn(c);
        c->in_import = saved_name;
        c->import_from = saved_from;
        return;
    }
    if (addr >= IMPORT_BASE && addr < IMPORT_BASE + (uint64_t)dyn_sym_count * IMPORT_SLOT) {
        host_import(c, (int)((addr - IMPORT_BASE) / IMPORT_SLOT));
        return;
    }
    if (nro_call_indirect(c, addr)) return;          /* a function of a run-time module (nnro.c) */
    if (addr >= 0x7500000000ULL && addr < 0x7500cfb000ULL) { sdk_dispatch(c, addr); return; }   /* lifted SDK code (sdklift_rt.c), e.g. a vtable entry of an SDK object */
    void (*fn)(Ctx *) = lookup_guest(addr);
    if (!fn) guest_trap("indirect call to an address that is not a known function", addr);
    fn(c);
}

/* Register context of the guest code running on this host thread, for crash reports. */
static __declspec(thread) Ctx *t_cur_ctx;

void call_guest(Ctx *c, uint64_t addr) {
    Ctx *saved = t_cur_ctx;
    t_cur_ctx = c;
    call_indirect(c, addr);
    t_cur_ctx = saved;
}

static void dump_guest_registers(void) {
    Ctx *c = t_cur_ctx;
    if (!c) return;
    printf("last guest function entered: f_%llx\n", (unsigned long long)c->fn);
    printf("guest registers:\n");
    for (int i = 0; i < 31; i++)
        printf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 3 == 2 ? "\n" : "");
    printf("  sp  %016llx\n", (unsigned long long)c->x[31]);
}

/* ---------------------------------------------------------------- imports */
void register_imports(const ImportDef *defs, int count) {
    for (int d = 0; d < count; d++) {
        int found = 0;
        for (int i = 0; i < dyn_sym_count; i++) {
            if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, defs[d].name) == 0) { g_handlers[i] = defs[d].fn; found = 1; }
        }
        (void)found;
    }
}

/* Handler currently registered for an imported function, or NULL (not imported / still a stub). */
ImportFn import_lookup(const char *name) {
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0) return g_handlers[i];
    return NULL;
}

static uint64_t *g_import_addr;                    /* per-symbol address override, 0 = use the placeholder */
void import_set_address(const char *name, void *addr) {
    if (!g_import_addr) g_import_addr = calloc((size_t)dyn_sym_count, sizeof *g_import_addr);
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0) g_import_addr[i] = (uint64_t)(uintptr_t)addr;
}

/* Address of the placeholder that stands for a data symbol of another module (IMPORT_SLOT bytes,
 * zero-filled), so the host can give it its real value. NULL if the game does not import it. */
void *import_data(const char *name) {
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0)
            return (void *)(uintptr_t)(IMPORT_BASE + (uint64_t)i * IMPORT_SLOT);
    return NULL;
}

/* What the relocations of main use for imported symbol `sym`: its redirected address or its placeholder
 * (nnro.c gives the run-time modules the same view). */
uint64_t import_address(int sym) {
    return g_import_addr && g_import_addr[sym] ? g_import_addr[sym] : IMPORT_BASE + (uint64_t)sym * IMPORT_SLOT;
}

static int audit_strict_on(void);
static unsigned char *g_import_open;        /* per import: 1 = an OPEN row of the fake audit (SSBU_STRICT=path) */
static void audit_path_hit(const char *list, const char *name, uint64_t from);
/* P3: exact call counts cost two interlocked increments on shared cache lines per import call (290 million calls in a
 * 240 s battle run, from every thread). They are kept only when SSBU_IMPORT_COUNTS=1 or SSBU_AUDIT_COUNTS is set;
 * otherwise an import is counted (and traced) for its first three calls, as before, and then called directly. */
static int g_import_count_all = 1;
/* close4: suspension of the application by the applet manager (nnmisc.c oe_watch; suyu am/applet.cpp UpdateSuspensionStateLocked ->
 * process->Suspend). While g_guest_frozen is set every guest thread stops at its next call of, or return from, an SDK function and
 * waits here (the console's kernel stops the threads wherever they are; a thread inside a kernel call finishes it there too). */
volatile LONG g_guest_frozen;
void guest_freeze_wait(void) { while (g_guest_frozen) Sleep(5); }
/* Session 8 (track V), SSBU_IMPORT_WATCH=<part of an import's name>: the calls of that one import are timed, and for every
 * frame in which they took more than 2 ms a line says how many there were, what they cost, the slowest one (with the text
 * its third argument points at when it is a printable string: the format of a formatting function) and how many different
 * return addresses called it. Off: one compare per import call. */
static void host_import_real(Ctx *c, int sym);
static int g_watch_sym = -2;
void host_import(Ctx *c, int sym) {
    if (g_watch_sym == -2) {
        const char *e = getenv("SSBU_IMPORT_WATCH");
        int w = -1;
        if (e && e[0]) for (int i = 0; i < (int)dyn_sym_count; i++) if (strstr(dyn_syms[i].name, e)) { w = i; break; }
        if (w >= 0) printf("import_watch: watching \"%s\"\n", dyn_syms[w].name);
        g_watch_sym = w;
    }
    if (sym != g_watch_sym) { host_import_real(c, sym); return; }
    extern long long nvn_front_frame_next(void);
    static long long cur_frame, n, tks, max_tk, n_all; static char max_txt[72]; static uint64_t max_ra, ras[64]; static int nras; static uint64_t max_x1;
    static volatile LONG lock;
    LARGE_INTEGER q0, q1, fq;
    uint64_t ra = c->x[30], x1 = c->x[1], x2 = c->x[2];
    char txt[72]; txt[0] = 0;
    {   MEMORY_BASIC_INFORMATION mi;               /* the third argument as text, when it is one (read BEFORE the call: the registers change) */
        if (x2 > 0x10000 && VirtualQuery((const void *)(uintptr_t)x2, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize > x2 + 80) {
            const unsigned char *s = (const unsigned char *)(uintptr_t)x2; int k = 0;
            for (; k < 70 && s[k] >= 32 && s[k] < 127; k++) txt[k] = (char)s[k];
            txt[k] = 0;
        }
    }
    QueryPerformanceCounter(&q0);
    host_import_real(c, sym);
    QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&fq);
    long long d = q1.QuadPart - q0.QuadPart, fr = nvn_front_frame_next();
    while (InterlockedExchange(&lock, 1)) YieldProcessor();
    if (fr != cur_frame) {
        if (tks * 500 > fq.QuadPart) printf("import_watch: frame %lld: %lld calls, %.2f ms; slowest %.3f ms (argument 1 = %llu, argument 2 as text \"%s\", called from %llx); %d%s different return addresses, the first: %llx %llx %llx %llx; %lld calls since the start\n", cur_frame, n, (double)tks * 1000.0 / (double)fq.QuadPart,
                                             (double)max_tk * 1000.0 / (double)fq.QuadPart, (unsigned long long)max_x1, max_txt, (unsigned long long)max_ra, nras, nras == 64 ? "+" : "", (unsigned long long)ras[0], (unsigned long long)ras[1], (unsigned long long)ras[2], (unsigned long long)ras[3], n_all);
        cur_frame = fr; n = tks = max_tk = 0; nras = 0; memset(ras, 0, sizeof ras);
    }
    n++; n_all++; tks += d;
    if (d > max_tk) { max_tk = d; max_ra = ra; max_x1 = x1; memcpy(max_txt, txt, sizeof max_txt); }
    { int k = 0; for (; k < nras; k++) if (ras[k] == ra) break; if (k == nras && nras < 64) ras[nras++] = ra; }
    InterlockedExchange(&lock, 0);
}
static void host_import_real(Ctx *c, int sym) {
    static volatile LONG order_count;
    if (g_guest_frozen) guest_freeze_wait();
    if (!g_import_count_all && g_import_calls[sym] >= 3 && g_handlers[sym]) {
        const char *saved_name = c->in_import;
        uint64_t saved_from = c->import_from;
        c->in_import = dyn_syms[sym].name;
        c->import_from = c->x[30] - 4;
        g_handlers[sym](c);
        c->in_import = saved_name;
        c->import_from = saved_from;
        if (g_guest_frozen) guest_freeze_wait();
        return;
    }
    unsigned long long n = (unsigned long long)InterlockedIncrement64((volatile LONG64 *)&g_import_calls[sym]);
    if (n == 1) { g_import_order[InterlockedIncrement(&order_count) - 1] = sym; g_import_order_count = order_count; }
    ImportFn fn = g_handlers[sym];
    InterlockedIncrement64((volatile LONG64 *)&g_trace_seq);
    if (n == 1 && g_import_open && g_import_open[sym]) audit_path_hit("import", dyn_syms[sym].name, c->x[30] - 4);
    if (n <= 3) {
        trace_printf("%8llu %s %s  x0=%llx x1=%llx x2=%llx x3=%llx  from=%llx\n", g_trace_seq,
                     fn ? "call " : "STUB ", dyn_syms[sym].name,
                     (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                     (unsigned long long)c->x[3], (unsigned long long)c->x[30] - 4);
    }
    const char *saved_name = c->in_import;
    uint64_t saved_from = c->import_from;
    c->in_import = dyn_syms[sym].name;
    c->import_from = c->x[30] - 4;
    if (fn) fn(c);
    else {
        /* No handler: the symbol belongs to a module that is not loaded (the script modules bind theirs at nn::ro::LoadModule)
         * or was never implemented. The console would fault on the unresolved address; a silent 0 is a fake answer. */
        if (n == 1) {
            printf("import \"%s\" was called without a handler (caller 0x%llx)\n", dyn_syms[sym].name, (unsigned long long)c->x[30] - 4);
            trace_printf("import \"%s\" called without a handler\n", dyn_syms[sym].name);
        }
        if (audit_strict_on()) guest_trap("call to an import that has no handler (SSBU_STRICT)", c->x[30] - 4);
        c->x[0] = 0;
    }
    c->in_import = saved_name;
    c->import_from = saved_from;
    if (g_guest_frozen) guest_freeze_wait();
}

void print_import_summary(FILE *out) {
    int stubs = 0, impl = 0;
    for (int i = 0; i < g_import_order_count; i++) { if (g_handlers[g_import_order[i]]) impl++; else stubs++; }
    fprintf(out, "\nimports reached: %d  (implemented: %d, still stubs: %d), %llu calls in total\n",
            g_import_order_count, impl, stubs, g_trace_seq);
    if (!g_import_count_all) fprintf(out, "(call counts are off: each import is counted up to its third call only; SSBU_IMPORT_COUNTS=1 counts every call)\n");
    fprintf(out, "most called imports:\n");
    for (int k = 0; k < 14; k++) {                       /* small selection sort over the reached imports */
        int best = -1;
        for (int i = k; i < g_import_order_count; i++)
            if (best < 0 || g_import_calls[g_import_order[i]] > g_import_calls[g_import_order[best]]) best = i;
        if (best < 0) break;
        int t = g_import_order[k]; g_import_order[k] = g_import_order[best]; g_import_order[best] = t;
        fprintf(out, "  %10llu  %s%s\n", g_import_calls[g_import_order[k]], dyn_syms[g_import_order[k]].name,
                g_handlers[g_import_order[k]] ? "" : "   (stub)");
    }
    if (getenv("SSBU_IMPORT_COUNTS") && atoi(getenv("SSBU_IMPORT_COUNTS")) >= 2) {      /* D2 debug aid: every reached import with its count */
        fprintf(out, "all import counts:\n");
        for (int i = 0; i < g_import_order_count; i++) fprintf(out, "  count %10llu  %s\n", g_import_calls[g_import_order[i]], dyn_syms[g_import_order[i]].name);
    }
    fprintf(out, "unimplemented imports reached:\n");
    for (int i = 0; i < g_import_order_count; i++) {
        int s = g_import_order[i];
        if (!g_handlers[s]) fprintf(out, "  %8llu  %s\n", g_import_calls[s], dyn_syms[s].name);
    }
}

/* ---------------------------------------------------------------- traps */
void print_devinfo_summary(FILE *out);       /* nvn_stub.c */
void display_report(FILE *out);              /* display.c */

void audio_report(FILE *out);                /* nnaudio.c: underruns of the run */
void hid_report(FILE *out);                  /* nnhid.c: synthetic XInput pad (SSBU_XINPUT_SYNTH) */
void os_thread_report(FILE *out);            /* os_threads.c: every thread the game created, with its nn::os arguments */
void module_coverage_report(FILE *out);      /* nnro.c: script module functions that ran (SSBU_MODCOV=1) */

void (*g_exit_report_hook)(FILE *);          /* a test's own last line (nnsdk2.c eh_main_test) */

void report_and_exit(int code) {
    if (g_exit_report_hook) g_exit_report_hook(stdout);
    audit_exit_counts();
    print_import_summary(stdout);
    print_hostfn_summary(stdout);
    print_devinfo_summary(stdout);
    display_report(stdout);
    audio_report(stdout);
    hid_report(stdout);
    os_thread_report(stdout);
    module_coverage_report(stdout);
    if (g_trace) { print_import_summary(g_trace); print_hostfn_summary(g_trace); fclose(g_trace); }
    fflush(stdout);
    ExitProcess((UINT)code);
}

void guest_trap(const char *what, uint64_t addr) {
    printf("\nGUEST TRAP: %s (0x%llx)\n", what, (unsigned long long)addr);
    trace_printf("GUEST TRAP: %s (0x%llx)\n", what, (unsigned long long)addr);
    RaiseException(0xE0000001, 0, 0, NULL);
    report_and_exit(2);
}
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) {
    printf("\nUNTRANSLATED INSTRUCTION at 0x%llx: %08x (%s)\n", (unsigned long long)pc, insn, what);
    trace_printf("UNTRANSLATED INSTRUCTION at 0x%llx: %08x (%s)\n", (unsigned long long)pc, insn, what);
    RaiseException(0xE0000002, 0, 0, NULL);
    report_and_exit(3);
}
void guest_svc(Ctx *c, uint32_t number) { (void)c; guest_trap("svc (direct kernel call)", number); }

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    printf("\nHOST EXCEPTION 0x%08lx at host address %p\n", er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        printf("  %s address 0x%llx\n", er->ExceptionInformation[0] == 1 ? "writing" : "reading",
               (unsigned long long)er->ExceptionInformation[1]);
    dump_guest_registers();
    printf("guest call stack (innermost first):\n");
    CONTEXT ctx = *ep->ContextRecord;
    uint64_t last = 0;
    for (int depth = 0, shown = 0; depth < 400 && shown < 40; depth++) {
        uint64_t g = guest_function_of(ctx.Rip);
        if (g && g != last) { printf("  f_%llx\n", (unsigned long long)g); shown++; last = g; }
        DWORD64 image_base;
        RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &image_base, NULL);
        if (!rf) break;
        void *handler_data;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, rf, &ctx, &handler_data, &establisher, NULL);
        if (!ctx.Rip) break;
    }
    report_and_exit(4);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ---------------------------------------------------------------- loader */
void runtime_init(const char *elf, const char *trace_path) {
    g_handlers = calloc((size_t)dyn_sym_count, sizeof *g_handlers);
    g_import_calls = calloc((size_t)dyn_sym_count, sizeof *g_import_calls);
    g_import_order = calloc((size_t)dyn_sym_count, sizeof *g_import_order);
    g_by_host = malloc((size_t)fn_table_count * sizeof *g_by_host);
    memcpy(g_by_host, fn_table, (size_t)fn_table_count * sizeof *g_by_host);
    qsort(g_by_host, (size_t)fn_table_count, sizeof *g_by_host, cmp_host);
    fn_hash_build();
    {   const char *e = getenv("SSBU_IMPORT_COUNTS"), *a = getenv("SSBU_AUDIT_COUNTS");
        g_import_count_all = (e && e[0] && e[0] != '0') || (a && a[0]); }
    {   /* sanity: host code addresses must be strictly increasing after the sort */
        int bad = 0;
        for (int i = 1; i < fn_table_count; i++) if ((uintptr_t)g_by_host[i].fn <= (uintptr_t)g_by_host[i - 1].fn) bad++;
        printf("host address table: %d entries, %d out of order, span %p..%p\n", fn_table_count, bad,
               (void *)g_by_host[0].fn, (void *)g_by_host[fn_table_count - 1].fn);
    }
    g_trace = fopen(trace_path, "w");
    SetUnhandledExceptionFilter(crash_filter);

    void *mem = VirtualAlloc((void *)GUEST_BASE, IMAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem != (void *)GUEST_BASE) { fprintf(stderr, "cannot map guest image at 0x%llx\n", GUEST_BASE); exit(1); }
    /* data symbols provided by other modules: zero-filled placeholders, IMPORT_SLOT bytes each */
    void *imp = VirtualAlloc((void *)IMPORT_BASE, (SIZE_T)dyn_sym_count * IMPORT_SLOT, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (imp != (void *)IMPORT_BASE) { fprintf(stderr, "cannot map import area\n"); exit(1); }
    FILE *f = fopen(elf, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", elf); exit(1); }
    load_layout(f, elf);
    fseek(f, (long)FILE_OFF, SEEK_SET);
    if (fread(mem, 1, g_main.file_image, f) != g_main.file_image) { fprintf(stderr, "short read on image\n"); exit(1); }
    fclose(f);
    load_dynamic();

    unsigned long long rel = 0, sym_int = 0, sym_ext = 0, sym_weak = 0, other = 0;
    const uint64_t tables[2][2] = {{g_main.rela, g_main.relasz}, {g_main.jmprel, g_main.pltrelsz}};
    for (int t = 0; t < 2; t++) {
        for (uint64_t i = 0; i < tables[t][1] / 24; i++) {
            uint64_t e = GUEST_BASE + tables[t][0] + i * 24;
            uint64_t off = rd64(e), info = rd64(e + 8);
            int64_t addend = (int64_t)rd64(e + 16);
            uint32_t type = (uint32_t)info, sym = (uint32_t)(info >> 32);
            if (type == 0x403) { wr64(GUEST_BASE + off, GUEST_BASE + (uint64_t)addend); rel++; }
            else if (type == 0x101 || type == 0x401 || type == 0x402) {
                uint64_t s;
                if (dyn_syms[sym].defined) { s = GUEST_BASE + dyn_syms[sym].value; sym_int++; }
                /* An undefined WEAK symbol is null unless a module defines it. main has 14 (__rel_dyn/plt_start/end, nu::VirtualAllocHook,
                 * nu::VirtualFreeHook, 8 glslc*; .dynsym of main at 0x4197610, recomp.py) and neither sdk nor subsdk0 exports any of them
                 * (dynsym of both read 2026-10-08): the game's `if (hook)` tests are false, as on the console. */
                else if ((rd8(GUEST_BASE + g_main.symtab + (uint64_t)sym * 24 + 4) >> 4) == 2 && !(g_import_addr && g_import_addr[sym])) { s = 0; sym_weak++; }
                else { s = g_import_addr && g_import_addr[sym] ? g_import_addr[sym] : IMPORT_BASE + (uint64_t)sym * IMPORT_SLOT; sym_ext++; }
                wr64(GUEST_BASE + off, s + (type == 0x101 ? (uint64_t)addend : 0));
            } else other++;
        }
    }
    printf("image mapped at 0x%llx: %llu relative relocations, %llu to own symbols, %llu to imported symbols, %llu to undefined weak symbols (null), %llu unhandled\n",
           GUEST_BASE, rel, sym_int, sym_ext, sym_weak, other);
    printf("lifted functions: %d   dynamic symbols: %d\n", fn_table_count, dyn_sym_count);
    /* close5: page protection as the loader sets it on the console: the text and read-only data segments (program headers of
     * main.elf: R-X 0x0..0x39c7e90, R-- 0x39c8000..0x4f15f60; the data segment starts at 0x4f16000) become read-only once the
     * relocations are applied (all of them land in the data segment: the image would not load on the console otherwise), so a
     * wild write into code or constants faults instead of corrupting silently. SSBU_NO_PROTECT=1 leaves the old mapping. */
    if (!getenv("SSBU_NO_PROTECT")) {
        DWORD oldp;
        if (!VirtualProtect((void *)GUEST_BASE, g_main.data_start, PAGE_READONLY, &oldp)) { fprintf(stderr, "cannot protect the image's text and read-only data\n"); exit(1); }
        printf("image protection: 0x%llx..0x%llx read-only (text, read-only data), data and bss read-write\n", GUEST_BASE, GUEST_BASE + g_main.data_start);
    }
}

/* ---------------------------------------------------------------- fake audit (plan\FAKE_AUDIT.md)
 * A handler is "trivial" when its machine code does nothing but store constants into the guest context and
 * return: the generic ret0 / nothing / ret1 handlers, and every handler with a name of its own whose body is
 * the same thing. The test looks at the compiled code, so it does not depend on what a handler is called or
 * on which source file it lives in.
 *   SSBU_STRICT=1        any import or run-time host function (vtable slot, NVN entry) bound to a trivial
 *                        handler that the audit does not white-list as CORRECT-BY-SEMANTICS, and any function
 *                        import without a handler, stops the program with a list (exit code 5).
 *   SSBU_STRICT=report   same list, without stopping.
 *   SSBU_STRICT=path     the check of the PATH that is played: nothing stops at start-up; the program stops (exit code 5)
 *                        at the first CALL of an import or run-time host function whose audit row is open (FAKE, WRONG or
 *                        unclassified in plan\fake_audit.csv -- every open row, not only the do-nothing bodies; list
 *                        generated into fake_openlist.h by tools_audit\fake_census.py). SSBU_STRICT=pathreport lists the
 *                        calls without stopping. A run to the battle that ends by its time limit under SSBU_STRICT=path
 *                        called no fake function.
 *   SSBU_AUDIT_DUMP=<f>  write every import and its binding (handler RVA, trivial kind) to <f> after the
 *                        registrations and exit: input of tools_audit\fake_census.py.
 *   SSBU_AUDIT_COUNTS=<f> at exit, write the call count of every import and run-time host function. */
#include "fake_whitelist.h"
#include "fake_openlist.h"

/* 0 = real code; 1 = empty body; 2 = only stores constants (x0 := *x0 when *sets_x0) */
int audit_trivial(ImportFn fn, long long *x0, int *sets_x0) {
    const unsigned char *p = (const unsigned char *)fn;
    int stores = 0;
    long long rax = 0;
    *x0 = 0; *sets_x0 = 0;
    if (!p) return 0;
    for (int hop = 0; hop < 4 && p[0] == 0xE9; hop++) p = p + 5 + *(const int *)(p + 1);      /* linker thunk / tail jump */
    for (int step = 0; step < 12; step++) {
        const unsigned char *q = p;
        int w = 0;
        if (q[0] == 0xC3) return stores ? 2 : 1;
        if (q[0] == 0x48) { w = 1; q++; }
        if (q[0] == 0xC7 && (q[1] == 0x01 || q[1] == 0x41 || q[1] == 0x81)) {                   /* mov [rcx+d], imm32 */
            long long disp = 0;
            int modrm = q[1];
            q += 2;
            if (modrm == 0x41) { disp = (signed char)q[0]; q += 1; } else if (modrm == 0x81) { disp = *(const int *)q; q += 4; }
            if (disp == 0) { *x0 = w ? (long long)*(const int *)q : (long long)*(const unsigned *)q; *sets_x0 = 1; }
            q += 4; stores++; p = q; continue;
        }
        if (w && q[0] == 0x83 && q[1] == 0x21 && q[2] == 0x00) { *x0 = 0; *sets_x0 = 1; stores++; p = q + 3; continue; }   /* and qword [rcx], 0 */
        if (w && q[0] == 0xB8) { rax = *(const long long *)(q + 1); p = q + 9; continue; }      /* movabs rax, imm64 */
        if (!w && q[0] == 0xB8) { rax = *(const unsigned *)(q + 1); p = q + 5; continue; }      /* mov eax, imm32 */
        if (!w && q[0] == 0x31 && q[1] == 0xC0) { rax = 0; p = q + 2; continue; }               /* xor eax, eax */
        if (w && q[0] == 0x89 && q[1] == 0x01) { *x0 = rax; *sets_x0 = 1; stores++; p = q + 2; continue; }   /* mov [rcx], rax */
        if (!w && q[0] == 0x0F && q[1] == 0x57 && q[2] == 0xC0) { p = q + 3; continue; }        /* xorps xmm0, xmm0 */
        if (!w && q[0] == 0x0F && (q[1] == 0x11 || q[1] == 0x29) && (q[2] == 0x41 || q[2] == 0x81)) {   /* movups/movaps [rcx+d], xmm0 */
            p = q + (q[2] == 0x41 ? 4 : 7); stores++; continue;
        }
        return 0;
    }
    return 0;
}

static int audit_listed(const char *name) {
    int lo = 0, hi = (int)(sizeof audit_whitelist / sizeof audit_whitelist[0]) - 1;
    while (lo <= hi) {                                       /* the generated list is sorted with strcmp */
        int mid = (lo + hi) / 2, r = strcmp(audit_whitelist[mid], name);
        if (r == 0) return 1;
        if (r < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int g_strict = -1;                  /* 0 off, 1 stop, 2 report, 3 path (stop at the first call of an open row), 4 pathreport */
static int g_audit_started, g_audit_hostfn_bad;   /* before audit_startup, host-function findings are only counted, so the whole list is printed */
static int audit_mode(void) {
    if (g_strict < 0) {
        const char *e = getenv("SSBU_STRICT");
        g_strict = !e || !e[0] || !strcmp(e, "0") ? 0 : !strcmp(e, "report") ? 2 : !strcmp(e, "path") ? 3 : !strcmp(e, "pathreport") ? 4 : 1;
    }
    return g_strict;
}

static int audit_strict_on(void) { return audit_mode() == 1 || audit_mode() == 3; }
static int audit_open(const char *name) {
    int lo = 0, hi = (int)(sizeof audit_openlist / sizeof audit_openlist[0]) - 1;
    while (lo <= hi) {                                       /* the generated list is sorted with strcmp */
        int mid = (lo + hi) / 2, r = strcmp(audit_openlist[mid], name);
        if (r == 0) return 1;
        if (r < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
static int audit_path_hostfn(const char *name) { return audit_mode() >= 3 && audit_open(name); }
static volatile LONG g_audit_path_hits;
static void audit_path_hit(const char *list, const char *name, uint64_t from) {
    InterlockedIncrement(&g_audit_path_hits);
    printf("STRICT(path): %s \"%s\" was called (from 0x%llx) and its audit row is open (plan\\fake_audit.csv)\n", list, name, (unsigned long long)from);
    trace_printf("STRICT(path): %s \"%s\" called, audit row open\n", list, name);
    if (g_strict == 3) { printf("STRICT(path): stopping (SSBU_STRICT=pathreport lists the calls without stopping)\n"); fflush(stdout); report_and_exit(5); }
}
static void audit_describe(char *buf, size_t size, int kind, long long x0, int sets) {
    if (kind == 1) snprintf(buf, size, "empty body");
    else if (sets) snprintf(buf, size, "only returns the constant 0x%llx", (unsigned long long)x0);
    else snprintf(buf, size, "only stores constants");
}

/* A run-time host function (vtable slot, NVN entry point) was handed out: check it. */
static void audit_hostfn(const char *name, ImportFn fn) {
    long long x0; int sets, kind;
    char what[96];
    if (!audit_mode() || audit_mode() >= 3 || !fn) return;
    kind = audit_trivial(fn, &x0, &sets);
    if (!kind || audit_listed(name)) return;
    audit_describe(what, sizeof what, kind, x0, sets);
    printf("STRICT: run-time host function \"%s\" is bound to a do-nothing handler (%s) that the audit does not white-list\n", name, what);
    trace_printf("STRICT: run-time host function \"%s\": %s\n", name, what);
    g_audit_hostfn_bad++;
    if (g_strict == 1 && g_audit_started) { printf("STRICT: stopping (SSBU_STRICT=report lists everything without stopping)\n"); fflush(stdout); ExitProcess(5); }
}

static void audit_write(const char *path, int with_counts) {
    FILE *f = fopen(path, "w");
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    if (!f) { printf("audit: cannot write %s\n", path); return; }
    fprintf(f, "# list\tname\tcalls\thandler_rva\tkind\tx0\tdata_redirect\n");
    for (int i = 0; i < dyn_sym_count; i++) {
        long long x0 = 0; int sets = 0, kind;
        if (dyn_syms[i].defined || !dyn_syms[i].name[0]) continue;
        kind = g_handlers[i] ? audit_trivial(g_handlers[i], &x0, &sets) : -1;
        fprintf(f, "import\t%s\t%llu\t%llx\t%d\t%s%llx\t%d\n", dyn_syms[i].name, with_counts ? g_import_calls[i] : 0ull,
                g_handlers[i] ? (unsigned long long)((uintptr_t)g_handlers[i] - base) : 0ull, kind, sets ? "" : "-", (unsigned long long)x0,
                (int)(g_import_addr && g_import_addr[i]));
    }
    for (int i = 0; i < g_hostfn_count; i++) {
        long long x0 = 0; int sets = 0;
        int kind = g_hostfn[i].fn ? audit_trivial(g_hostfn[i].fn, &x0, &sets) : -1;
        fprintf(f, "hostfn\t%s\t%lld\t%llx\t%d\t%s%llx\t0\n", g_hostfn[i].name, with_counts ? (long long)g_hostfn[i].calls : 0ll,
                g_hostfn[i].fn ? (unsigned long long)((uintptr_t)g_hostfn[i].fn - base) : 0ull, kind, sets ? "" : "-", (unsigned long long)x0);
    }
    fclose(f);
}

static void audit_exit_counts(void) {
    const char *p = getenv("SSBU_AUDIT_COUNTS");
    if (p && p[0]) audit_write(p, 1);
}

/* Called by boot.c after the last registration. */
void audit_startup(void) {
    const char *dump = getenv("SSBU_AUDIT_DUMP");
    int bad = 0, trivial = 0, listed = 0, unbound = 0;
    for (int i = 0; i < dyn_sym_count; i++) {
        long long x0; int sets, kind;
        char what[96];
        const char *name = dyn_syms[i].name;
        if (dyn_syms[i].defined || !name[0]) continue;
        if (!g_handlers[i]) {
            if (g_import_addr && g_import_addr[i]) continue;         /* data symbol with its real value (sdkdata.c, C++ runtime tables) */
            unbound++;
            if (audit_listed(name)) { listed++; continue; }
            bad++;
            if (audit_mode() == 1 || audit_mode() == 2) printf("STRICT: import \"%s\" has no handler and no data (a call would return 0)\n", name);
            continue;
        }
        kind = audit_trivial(g_handlers[i], &x0, &sets);
        if (!kind) continue;
        trivial++;
        if (audit_listed(name)) { listed++; continue; }
        bad++;
        if (audit_mode() == 1 || audit_mode() == 2) { audit_describe(what, sizeof what, kind, x0, sets); printf("STRICT: import \"%s\" is bound to a do-nothing handler (%s)\n", name, what); }
    }
    printf("fake audit: %d imports bound to do-nothing handlers, %d without handler or data; %d white-listed as correct by semantics, %d NOT white-listed%s\n",
           trivial, unbound, listed, bad, audit_mode() ? "" : " (SSBU_STRICT=1 lists them and stops)");
    if (dump && dump[0]) { audit_write(dump, 0); fflush(stdout); ExitProcess(0); }
    if (audit_mode() >= 3) {
        int open_n = 0;
        g_import_open = calloc((size_t)dyn_sym_count, 1);
        for (int i = 0; i < dyn_sym_count; i++)
            if (!dyn_syms[i].defined && dyn_syms[i].name[0] && audit_open(dyn_syms[i].name)) { g_import_open[i] = 1; open_n++; }
        printf("STRICT(path): %d imports have an open audit row; the run %s at the first one that is called\n", open_n, g_strict == 3 ? "stops" : "reports");
    }
    g_audit_started = 1;
    bad += g_audit_hostfn_bad;
    if (bad && audit_mode() == 1) {
        printf("STRICT: %d binding(s) are neither real nor proven correct: stopping. See plan\\FAKE_AUDIT.md; regenerate the list with python tools_audit\\fake_census.py\n", bad);
        fflush(stdout);
        ExitProcess(5);
    }
}

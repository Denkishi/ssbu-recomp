/* skyline.c - Skyline for this port: the plugin loader and the API that Skyline plugins (ARCropolis, smashline, nro_hook,
 * HewDraw Remix, ...) import. #included by nnro.c (it uses the module tables, the loader and the hook table).
 *
 * On the console, Skyline (exefs subsdk9) is a C++ module that hooks the game's first nn::fs::MountRom, loads every NRO
 * under rom:/skyline/plugins with nn::ro (after registering an NRR it builds itself) and calls each plugin's exported
 * `main`. Plugins then change the game at run time: function hooks (a branch written at an address), inline hooks (a
 * callback that sees all registers before one instruction), and patches (bytes written into the game's code or data).
 *
 * Here the plugins are lifted ahead of time like the game's own script modules (tools/nro/build_plugins.py ->
 * plugin_bin\<name>.dll + plugin_bin\plugins.txt: name, fixed guest base, sizes, build id), loaded by nnro.c's loader,
 * and Skyline's exports are host functions:
 *   A64HookFunction(sym, repl, &orig)  function entry of lifted code -> the hook table (hook_install); an import of main or
 *                                      a host function (nn::ro::LoadModule ...) -> hostentry_hook (hostrt.c); any other
 *                                      address of main = a "mid-function hook": active only where the re-lifted game DLL
 *                                      (tools/lifter/relift_sites.py: site list) has a callout there (sky_site_midhook).
 *   A64InlineHook(addr, cb)            at a lifted function entry: a hook-table layer runs the callbacks, then the original;
 *                                      elsewhere: the re-lifted code calls sky_site_inline(c, addr) before the instruction.
 *   sky_memcpy(dst, src, n)            writes the bytes (through a writable view of read-only pages); a write into data
 *                                      takes effect, a write into code only where the re-lifted DLL compiled that word in.
 *   getRegionAddress, get_program_id, skyline_tcp_send_raw (log), get_plugin_addresses (NULL, as Skyline's own build).
 * Every hook / inline hook / patch a plugin makes is written to skyline_sites.log (next to the exe) in the re-lift site
 * list format (kind, address; a patch with the old and new word), marked "active" or "NOT ACTIVE", so the next re-lift
 * can cover it.
 *
 * Also here, because plugins walk them: rtld ModuleObjects for main, the SDK and every loaded module, in the lists
 * nn::ro::detail::g_pAutoLoadList / g_pManualLoadList (smashline's rtld crate reads them), nn::ro::LookupSymbol /
 * LookupModuleSymbol / LoadModuleInternal, and the kernel calls plugins make directly (svcQueryMemory, svcGetInfo,
 * svcOutputDebugString; IPC is refused).
 *
 * No plugin folder (SSBU_SD, else <exe>\..\..\sd\atmosphere\contents\01006A800016E000\romfs\skyline\plugins) = nothing
 * of this runs and the game is exactly as before. */

#ifndef SDK_BASE
#define SDK_BASE 0x7500000000ULL                  /* sdklift.h */
#define SDK_IMAGE_SIZE 0xcfb000ULL
#endif
#define IMAGE_SIZE 0x7500000ULL                   /* main's mapping (hostrt.c) */
uint64_t sdk_symbol(const char *name);          /* sdklift_rt.c */
int sdk_image_ready(void);
#define SKY_MAX_PLUGINS 64
#define SKY_PROGRAM_ID 0x01006A800016E000ULL
#define SKY_PLUGIN_DIR "atmosphere\\contents\\01006A800016E000\\romfs\\skyline\\plugins"

static void sky_find_interpreter(NroModule *m);
static void sky_sites_init(void);
static void sky_sites_add(HMODULE m, const char *what);
static NroInfo g_sky_info[SKY_MAX_PLUGINS];
static char g_sky_src[SKY_MAX_PLUGINS][MAX_PATH];
static int g_sky_count;
static char g_sky_sd[MAX_PATH];                 /* host folder that stands for sd:/ ("" = no plugins) */
static FILE *g_sky_log;
static SRWLOCK g_sky_lock = SRWLOCK_INIT;

static void sky_logf(const char *fmt, ...) {
    char line[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    printf("[skyline] %s\n", line);
    trace_printf("[skyline] %s\n", line);
}
static void sky_site(const char *fmt, ...) {     /* one line of skyline_sites.log */
    if (!g_sky_log) return;
    va_list ap; va_start(ap, fmt);
    AcquireSRWLockExclusive(&g_sky_lock);
    vfprintf(g_sky_log, fmt, ap); fputc('\n', g_sky_log); fflush(g_sky_log);
    ReleaseSRWLockExclusive(&g_sky_lock);
    va_end(ap);
}

static void exe_dir(char *out, size_t n) {
    DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n);
    while (k && out[k - 1] != '\\' && out[k - 1] != '/') k--;
    out[k] = 0;
}

/* ---------------------------------------------------------------- the plugin registry (plugin_bin\plugins.txt) */
static int hexval(int ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }
static const char *sky_bin_dir(void) { const char *b = getenv("SSBU_PLUGIN_BIN"); return b && b[0] ? b : "plugin_bin"; }
static BOOL CALLBACK sky_registry_once(PINIT_ONCE once, PVOID p, PVOID *ctx) {
    (void)once; (void)p; (void)ctx;
    char path[MAX_PATH * 2], line[MAX_PATH * 2];
    const char *sd = getenv("SSBU_SD");
    exe_dir(path, sizeof path);
    if (sd && sd[0]) snprintf(g_sky_sd, sizeof g_sky_sd, "%s", sd);
    else snprintf(g_sky_sd, sizeof g_sky_sd, "%s..\\..\\sd", path);
    {   char dir[MAX_PATH * 2]; snprintf(dir, sizeof dir, "%s\\" SKY_PLUGIN_DIR, g_sky_sd);
        DWORD a = GetFileAttributesA(dir);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (getenv("SSBU_SKYLINE") && !strcmp(getenv("SSBU_SKYLINE"), "0"))) { g_sky_sd[0] = 0; return TRUE; } }
    snprintf(path + strlen(path), sizeof path - strlen(path), "%s\\plugins.txt", sky_bin_dir());
    FILE *f = fopen(path, "r");
    if (!f) { printf("[skyline] %s is missing: no plugin has lifted code (tools/nro/build_plugins.py)\n", path); return TRUE; }
    while (fgets(line, sizeof line, f) && g_sky_count < SKY_MAX_PLUGINS) {
        char name[128], bid[64], src[MAX_PATH];
        unsigned long long base; unsigned img, file;
        src[0] = 0;
        if (line[0] == '#' || sscanf(line, "%127s %llx %x %x %63s %259[^\r\n]", name, &base, &img, &file, bid, src) < 5 || strlen(bid) != 40) continue;
        NroInfo *in = &g_sky_info[g_sky_count];
        in->name = _strdup(name); in->base = base; in->image_size = img; in->file_size = file;
        for (int k = 0; k < 20; k++) in->build_id[k] = (uint8_t)(hexval(bid[2 * k]) << 4 | hexval(bid[2 * k + 1]));
        snprintf(g_sky_src[g_sky_count], MAX_PATH, "%s", src);
        g_sky_count++;
    }
    fclose(f);
    return TRUE;
}
static int sky_registry(void) {                  /* number of registered plugins (0 when no plugin folder) */
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, sky_registry_once, NULL, NULL);
    return g_sky_sd[0] ? g_sky_count : 0;
}

/* ---------------------------------------------------------------- rtld ModuleObjects and the load lists
 * Layout (nnsdk-rs rtld.rs, smashline crates/rtld): next, prev, rela_plt, rela, module_base, dynamic, is_rela,
 * rela_plt_size, dt_init, dt_fini, hash_bucket, hash_chain, dynstr, dynsym, dynstr_size, got, rela_dyn_size,
 * rel_dyn_size, rel_count, rela_count, hash_nchain, hash_nbucket, got_stub_ptr, soname_idx, nro_size,
 * cannot_revert_symbols: 0xd0 bytes. A list is a sentinel {front, back}; g_p*LoadList point at the sentinels. */
#define MO_SIZE 0xd0
typedef struct { uint64_t front, back; } SkyList;
static SkyList g_sky_auto, g_sky_manual;
static uint64_t g_sky_pauto, g_sky_pmanual;      /* nn::ro::detail::g_pAutoLoadList / g_pManualLoadList */
static uint64_t g_sky_main_mo, g_sky_sdk_mo;

static void list_init(SkyList *l) { if (!l->front) l->front = l->back = (uint64_t)(uintptr_t)l; }
static void list_push(SkyList *l, uint64_t mo) {
    list_init(l);
    uint64_t sent = (uint64_t)(uintptr_t)l, last = l->back;
    wr64(mo, sent); wr64(mo + 8, last);
    if (last == sent) l->front = mo; else wr64(last, mo);
    l->back = mo;
}
static void list_remove(SkyList *l, uint64_t mo) {
    uint64_t sent = (uint64_t)(uintptr_t)l, next = rd64(mo), prev = rd64(mo + 8);
    if (prev == sent) l->front = next; else wr64(prev, next);
    if (next == sent) l->back = prev; else wr64(next + 8, prev);
    wr64(mo, mo); wr64(mo + 8, mo);
}
static void rtld_fill(uint64_t mo, uint64_t base, uint64_t dynamic, uint64_t size) {
    memset((void *)(uintptr_t)(mo + 0x10), 0, MO_SIZE - 0x10);
    wr64(mo + 0x20, base); wr64(mo + 0x28, dynamic); wr8(mo + 0x30, 1);
    for (uint64_t d = dynamic; rd64(d); d += 16) {
        uint64_t tag = rd64(d), val = rd64(d + 8);
        switch (tag) {
        case 23: wr64(mo + 0x10, base + val); break;            /* DT_JMPREL */
        case 2: wr64(mo + 0x38, val); break;                    /* DT_PLTRELSZ */
        case 7: wr64(mo + 0x18, base + val); break;             /* DT_RELA */
        case 8: wr64(mo + 0x80, val); break;                    /* DT_RELASZ */
        case 18: wr64(mo + 0x88, val); break;                   /* DT_RELSZ */
        case 20: wr8(mo + 0x30, val == 7); break;               /* DT_PLTREL: DT_RELA */
        case 12: wr64(mo + 0x40, base + val); break;            /* DT_INIT */
        case 13: wr64(mo + 0x48, base + val); break;            /* DT_FINI */
        case 4: {                                               /* DT_HASH: nbucket, nchain, buckets, chains */
            uint64_t h = base + val; uint32_t nb = rd32(h), nc = rd32(h + 4);
            wr64(mo + 0xa8, nb); wr64(mo + 0xa0, nc); wr64(mo + 0x50, h + 8); wr64(mo + 0x58, h + 8 + 4ull * nb);
            break; }
        case 5: wr64(mo + 0x60, base + val); break;             /* DT_STRTAB */
        case 6: wr64(mo + 0x68, base + val); break;             /* DT_SYMTAB */
        case 10: wr64(mo + 0x70, val); break;                   /* DT_STRSZ */
        case 3: wr64(mo + 0x78, base + val); break;             /* DT_PLTGOT */
        case 0x6ffffff9: wr64(mo + 0x98, val); break;           /* DT_RELACOUNT */
        case 0x6ffffffa: wr64(mo + 0x90, val); break;           /* DT_RELCOUNT */
        case 14: wr64(mo + 0xb8, val); break;                   /* DT_SONAME */
        }
    }
    wr64(mo + 0xc0, size);
}
static uint64_t image_dynamic(uint64_t base) { uint32_t mod0 = rd32(base + 4); return base + mod0 + (int32_t)rd32(base + mod0 + 4); }

/* segments for svcQueryMemory: main and the SDK from their ELF program headers, modules from their NRO header */
typedef struct { uint64_t start, end; uint32_t perm, type; } SkySeg;
static SkySeg g_sky_img_seg[2][3];               /* main, SDK: text, rodata, data+bss */
static int read_elf_segments(const char *path, uint64_t base, SkySeg seg[3]) {
    FILE *f = fopen(path, "rb");
    uint8_t h[64], ph[56];
    int n = 0;
    if (!f) return 0;
    if (fread(h, 1, 64, f) == 64 && !memcmp(h, "\x7f" "ELF", 4)) {
        uint64_t phoff; uint16_t phnum; memcpy(&phoff, h + 0x20, 8); memcpy(&phnum, h + 0x38, 2);
        for (int i = 0; i < phnum && n < 3; i++) {
            fseek(f, (long)(phoff + (uint64_t)i * 56), SEEK_SET);
            if (fread(ph, 1, 56, f) != 56) break;
            uint32_t type, flags; uint64_t va, memsz; memcpy(&type, ph, 4); memcpy(&flags, ph + 4, 4); memcpy(&va, ph + 16, 8); memcpy(&memsz, ph + 40, 8);
            if (type != 1) continue;
            seg[n].start = base + va; seg[n].end = base + ((va + memsz + 0xfff) & ~0xfffull);
            seg[n].perm = (flags & 4 ? 1 : 0) | (flags & 2 ? 2 : 0) | (flags & 1 ? 4 : 0);
            seg[n].type = n == 2 ? 4 : 3;                       /* CodeStatic, CodeMutable */
            n++;
        }
    }
    fclose(f);
    return n == 3;
}
static void nro_segments(uint64_t base, SkySeg seg[3]) {
    for (int s = 0; s < 3; s++) {
        uint64_t off = rd32(base + 0x20 + 8 * s), sz = rd32(base + 0x24 + 8 * s);
        if (s == 2) sz += rd32(base + 0x38);
        seg[s].start = base + off; seg[s].end = base + ((off + sz + 0xfff) & ~0xfffull);
        seg[s].perm = s == 0 ? 5 : s == 1 ? 1 : 3;
        seg[s].type = s == 2 ? 0x15 : 0x14;                     /* ModuleCodeMutable, ModuleCodeStatic */
    }
}

/* Called by nnro.c once main and the SDK image are mapped (first nn::ro call, or the plugin loader). */
static void sky_rtld_init(void) {
    static volatile LONG done;
    if (InterlockedExchange(&done, 1)) return;
    list_init(&g_sky_auto); list_init(&g_sky_manual);
    g_sky_pauto = (uint64_t)(uintptr_t)&g_sky_auto; g_sky_pmanual = (uint64_t)(uintptr_t)&g_sky_manual;
    char mainp[MAX_PATH], sdkp[MAX_PATH];
    snprintf(mainp, sizeof mainp, "%s", __argc > 1 ? __argv[1] : "..\\binaries\\13.0.4\\main.elf");
    const char *env = getenv("SSBU_SDK_ELF");
    if (env && env[0]) snprintf(sdkp, sizeof sdkp, "%s", env);
    else {
        snprintf(sdkp, sizeof sdkp, "%s", mainp);
        char *s1 = strrchr(sdkp, '\\'), *s2 = strrchr(sdkp, '/'); if (s2 > s1) s1 = s2;
        snprintf(s1 ? s1 + 1 : sdkp, sizeof sdkp - (size_t)(s1 ? s1 + 1 - sdkp : 0), "sdk.elf");
    }
    if (!read_elf_segments(mainp, GUEST_BASE, g_sky_img_seg[0])) sky_logf("cannot read the segments of %s", mainp);
    g_sky_main_mo = (uint64_t)(uintptr_t)calloc(1, MO_SIZE);
    rtld_fill(g_sky_main_mo, GUEST_BASE, image_dynamic(GUEST_BASE), IMAGE_SIZE);
    list_push(&g_sky_auto, g_sky_main_mo);
    if (sdk_image_ready() && read_elf_segments(sdkp, SDK_BASE, g_sky_img_seg[1])) {
        g_sky_sdk_mo = (uint64_t)(uintptr_t)calloc(1, MO_SIZE);
        rtld_fill(g_sky_sdk_mo, SDK_BASE, image_dynamic(SDK_BASE), SDK_IMAGE_SIZE);
        list_push(&g_sky_auto, g_sky_sdk_mo);
    }
}

/* nnro.c: a module was loaded / is being unloaded (under g_ro_lock) */
static void sky_module_loaded(NroModule *m, uint64_t obj) {
    sky_rtld_init();
    uint64_t base = m->info->base;
    rtld_fill(obj, base, image_dynamic(base), m->info->image_size);
    list_push(&g_sky_manual, obj);
    m->rtld_obj = obj;
    if (!m->plugin) { sky_sites_init(); sky_sites_add(m->dll, m->info->name); }      /* a module re-lifted with sites (nro_bin_sky) */
}
static void sky_module_unloaded(NroModule *m) {
    if (m->rtld_obj) list_remove(&g_sky_manual, m->rtld_obj);
    m->rtld_obj = 0;
}

/* data symbols the plugins import from the SDK (nnro.c resolve) */
static uint64_t sky_data_symbol(const char *name) {
    if (!strcmp(name, "_ZN2nn2ro6detail15g_pAutoLoadListE")) { sky_rtld_init(); return (uint64_t)(uintptr_t)&g_sky_pauto; }
    if (!strcmp(name, "_ZN2nn2ro6detail17g_pManualLoadListE")) { sky_rtld_init(); return (uint64_t)(uintptr_t)&g_sky_pmanual; }
    return 0;
}
/* SDK functions a plugin only uses as an ADDRESS (smashline finds the SDK's ModuleObject through the address of its
 * _Unwind_GetIP import): resolved to the SDK image, where call_indirect reaches the lifted SDK code if any. */
static uint64_t sky_sdk_symbol(const char *name) {
    if (!strncmp(name, "_Unwind_", 8)) return sdk_symbol(name);
    return 0;
}

/* ---------------------------------------------------------------- kernel calls of plugin code */
static int sky_find_seg(uint64_t a, SkySeg *out) {
    for (int i = 0; i < 2; i++)
        for (int s = 0; s < 3; s++)
            if (a >= g_sky_img_seg[i][s].start && a < g_sky_img_seg[i][s].end) { *out = g_sky_img_seg[i][s]; return 1; }
    NroModule *m = module_at(a);
    if (m && m->loaded) {
        SkySeg seg[3]; nro_segments(m->info->base, seg);
        for (int s = 0; s < 3; s++) if (a >= seg[s].start && a < seg[s].end) { *out = seg[s]; return 1; }
    }
    return 0;
}
static int sky_svc(Ctx *c, uint32_t n) {
    switch (n) {
    case 0x06: {                                 /* QueryMemory(MemoryInfo *out, u32 *page_info, u64 addr): w1 = page info */
        uint64_t out = c->x[0], a = c->x[2];
        SkySeg s;
        sky_rtld_init();
        memset((void *)(uintptr_t)out, 0, 0x28);
        if (sky_find_seg(a, &s)) { wr64(out, s.start); wr64(out + 8, s.end - s.start); wr32(out + 0x10, s.type); wr32(out + 0x18, s.perm); }
        else {
            MEMORY_BASIC_INFORMATION mi;
            if (VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) {
                uint32_t p = mi.Protect & 0xff, perm = p == PAGE_READONLY ? 1 : p == PAGE_READWRITE || p == PAGE_WRITECOPY ? 3 : p == PAGE_EXECUTE_READ ? 5 : p == PAGE_EXECUTE_READWRITE ? 7 : 0;
                wr64(out, (uint64_t)(uintptr_t)mi.BaseAddress); wr64(out + 8, mi.RegionSize);
                wr32(out + 0x10, mi.State == MEM_COMMIT ? 5 : 0); wr32(out + 0x18, mi.State == MEM_COMMIT ? perm : 0);   /* Heap / Unmapped */
            } else { wr64(out, a & ~0xfffull); wr64(out + 8, 0x1000); }
        }
        c->x[0] = 0; c->x[1] = 0;
        return 1; }
    case 0x29: {                                 /* GetInfo(u64 *out, u32 id0, Handle, u64 id1): value in x1 */
        uint32_t id = (uint32_t)c->x[1];
        c->x[1] = id == 18 ? SKY_PROGRAM_ID : id == 4 || id == 2 ? 0 : 0;
        c->x[0] = 0;
        return 1; }
    case 0x27:                                   /* OutputDebugString(ptr, len) */
        sky_logf("svcOutputDebugString: %.*s", (int)c->x[1], (const char *)(uintptr_t)c->x[0]);
        c->x[0] = 0; return 1;
    case 0x16: c->x[0] = 0; return 1;            /* CloseHandle */
    case 0x1f: case 0x21: case 0x7d: case 0x30: case 0x31: case 0x55:
        /* ConnectToNamedPort, SendSyncRequest, CreateResourceLimit, resource limit values, QueryIoMapping:
         * no kernel objects or services here: "not supported" (0xf001 = kernel InvalidState-ish, never retried) */
        sky_logf("kernel call 0x%x from 0x%llx refused", n, (unsigned long long)c->x[30]);
        c->x[0] = 0xe401; return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- sites compiled into a re-lifted DLL
 * A game DLL or module DLL re-lifted from a site list (tools/lifter/relift_sites.py) exports sky_site_table (kind 1 patch,
 * 2 inline, 3 midhook, 4 dynimm, 5 jtable, 6 noinline): what it has compiled in. */
typedef struct { uint64_t addr; uint32_t kind; uint32_t word; } SkySite;
static struct { const SkySite *t; int n; } g_site_tabs[16];      /* the game DLL's, then each re-lifted module DLL's */
static volatile LONG g_site_ntabs;
static void sky_sites_add(HMODULE m, const char *what) {
    const SkySite *t = m ? (const SkySite *)(void *)GetProcAddress(m, "sky_site_table") : NULL;
    const int *n = m ? (const int *)(void *)GetProcAddress(m, "sky_site_count") : NULL;
    if (!t || !n) return;
    LONG k = InterlockedIncrement(&g_site_ntabs) - 1;
    if (k >= 16) return;
    g_site_tabs[k].t = t; g_site_tabs[k].n = *n;
    sky_logf("%s has %d plugin sites compiled in", what, *n);
}
static void sky_sites_init(void) {               /* the game DLL the exe was linked against (RECOMP_DLL), whatever its name */
    static volatile LONG done;
    extern const FnEntry (*game_fn_table_p)[];   /* gamedll.c: the DLL's fn_table */
    if (InterlockedExchange(&done, 1)) return;
    HMODULE gm = NULL;
    char name[MAX_PATH] = "the game DLL";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(const void *)game_fn_table_p, &gm)) {
        DWORD k = GetModuleFileNameA(gm, name, sizeof name);
        while (k && name[k - 1] != '\\' && name[k - 1] != '/') k--;
        memmove(name, name + k, strlen(name + k) + 1);
    }
    sky_sites_add(gm, name);
}
static const SkySite *sky_compiled(uint64_t addr, uint32_t kind) {
    sky_sites_init();
    for (int k = 0; k < g_site_ntabs && k < 16; k++)
        for (int i = 0; i < g_site_tabs[k].n; i++) if (g_site_tabs[k].t[i].addr == addr && g_site_tabs[k].t[i].kind == kind) return &g_site_tabs[k].t[i];
    return NULL;
}

/* "module+offset" for the log */
static void sky_where(uint64_t a, char *out, size_t n) {
    if (a >= GUEST_BASE && a < GUEST_BASE + IMAGE_SIZE) { snprintf(out, n, "main+0x%llx", (unsigned long long)(a - GUEST_BASE)); return; }
    if (a >= SDK_BASE && a < SDK_BASE + SDK_IMAGE_SIZE) { snprintf(out, n, "sdk+0x%llx", (unsigned long long)(a - SDK_BASE)); return; }
    NroModule *m = module_at(a);
    if (m) { snprintf(out, n, "%s+0x%llx", m->info->name, (unsigned long long)(a - m->info->base)); return; }
    snprintf(out, n, "0x%llx", (unsigned long long)a);
}

/* ---------------------------------------------------------------- inline hooks */
typedef struct SkyInline { uint64_t addr; int n; uint64_t cb[16]; struct SkyInline *next; ImportFn orig; int entry; volatile LONG64 calls; } SkyInline;
#define SKY_IH_HASH 1024
static SkyInline *g_sky_ih[SKY_IH_HASH];
static SkyInline *sky_ih_find(uint64_t addr) {
    for (SkyInline *e = g_sky_ih[(addr >> 2) & (SKY_IH_HASH - 1)]; e; e = e->next) if (e->addr == addr) return e;
    return NULL;
}
/* Run the callbacks of an inline hook at `addr` on the register state in c, as Skyline's inlineHandlerImpl does: the
 * context (x0..x30, the original sp, q0..q31: 0x300 bytes) is built on the guest stack below sp, each callback gets its
 * address in x0, and x0..x30 / q0..q31 are reloaded from it afterwards (sp is not; x16/x17 keep their values here, on the
 * console they hold the stub's scratch values). The condition flags are kept (the console's stub does not save them). */
static void sky_run_inline(Ctx *c, SkyInline *e) {
    InterlockedIncrement64(&e->calls);
    for (int k = e->n - 1; k >= 0; k--) {                  /* the newest hook runs first */
        uint64_t sp = c->x[31], ctx = sp - 0x300, x16 = c->x[16], x17 = c->x[17];
        uint32_t f[4] = {c->N, c->Z, c->C, c->V};
        for (int i = 0; i < 31; i++) wr64(ctx + 8ull * i, c->x[i]);
        wr64(ctx + 0xf8, sp);
        memcpy((void *)(uintptr_t)(ctx + 0x100), c->v, 32 * 16);
        c->x[31] = ctx; c->x[0] = ctx;
        call_indirect(c, e->cb[k]);
        for (int i = 0; i < 31; i++) c->x[i] = rd64(ctx + 8ull * i);
        memcpy(c->v, (void *)(uintptr_t)(ctx + 0x100), 32 * 16);
        c->x[31] = sp; c->x[16] = x16; c->x[17] = x17;
        c->N = f[0]; c->Z = f[1]; c->C = f[2]; c->V = f[3];
    }
}
/* the re-lifted code calls this before the instruction at pc (game_shim.c / nro_shim.c host interface) */
void sky_site_inline(Ctx *c, uint64_t pc) {
    SkyInline *e = sky_ih_find(pc);
    if (e && e->n) sky_run_inline(c, e);
}
static void sky_ih_entry(Ctx *c, void *user) { SkyInline *e = user; sky_run_inline(c, e); e->orig(c); }

static void sky_A64InlineHook(Ctx *c) {
    uint64_t addr = c->x[0], cb = c->x[1];
    char where[96]; sky_where(addr, where, sizeof where);
    AcquireSRWLockExclusive(&g_sky_lock);
    SkyInline *e = sky_ih_find(addr);
    int fresh = !e;
    if (!e) {
        e = calloc(1, sizeof *e); e->addr = addr;
        e->next = g_sky_ih[(addr >> 2) & (SKY_IH_HASH - 1)];
        g_sky_ih[(addr >> 2) & (SKY_IH_HASH - 1)] = e;
    }
    if (e->n < 16) e->cb[e->n++] = cb;
    ReleaseSRWLockExclusive(&g_sky_lock);
    const char *state;
    if (sky_compiled(addr, 2)) state = "active (callout)";
    else if (function_at(addr)) {
        if (fresh) e->entry = hook_install(addr, sky_ih_entry, e, &e->orig) > 0;
        state = e->entry ? "active (entry hook)" : "NOT ACTIVE (hook table refused the entry)";
    } else state = "NOT ACTIVE (needs a callout: re-lift)";
    sky_site("inline %llx    # %s cb=%llx %s", (unsigned long long)addr, where, (unsigned long long)cb, state);
    if (state[0] == 'N') sky_logf("inline hook at %s: %s", where, state);
}

/* ---------------------------------------------------------------- function hooks */
typedef struct SkyHook { uint64_t target, repl; ImportFn orig; struct SkyHook *prev; volatile LONG64 calls; struct SkyHook *all; } SkyHook;
static SkyHook *volatile g_sky_hooks;      /* every hook, for the report at exit */
static void sky_hook_thunk(Ctx *c, void *user) { InterlockedIncrement64(&((SkyHook *)user)->calls); call_indirect(c, ((SkyHook *)user)->repl); }
static void sky_hook_orig(Ctx *c, void *user) { ((SkyHook *)user)->orig(c); }

/* mid-function hooks: a branch written at a non-entry address of main; the re-lifted code asks at that point */
typedef struct SkyMid { uint64_t addr; SkyHook *top; struct SkyMid *next; } SkyMid;
static SkyMid *g_sky_mid;
static __declspec(thread) uint64_t t_sky_mid_skip;       /* "orig" of a mid hook: run the original instruction at addr */
int sky_site_midhook(Ctx *c, uint64_t pc) {
    if (t_sky_mid_skip == pc) { t_sky_mid_skip = 0; return 0; }
    for (SkyMid *m = g_sky_mid; m; m = m->next)
        if (m->addr == pc && m->top) { call_indirect(c, m->top->repl); return 1; }
    return 0;
}
static void sky_mid_orig(Ctx *c, void *user) {           /* the previous hook at the address, or the original code from it */
    SkyHook *h = user;
    if (h->prev) { call_indirect(c, h->prev->repl); return; }
    ImportFn f = function_at(h->target);
    if (!f) guest_trap("skyline: original of a mid-function hook: no lifted entry at the address (re-lift with a midhook site)", h->target);
    t_sky_mid_skip = h->target;
    f(c);
}

static void sky_A64HookFunction(Ctx *c) {
    uint64_t sym = c->x[0], repl = c->x[1], out = c->x[2];
    char where[96], rwhere[96]; sky_where(sym, where, sizeof where); sky_where(repl, rwhere, sizeof rwhere);
    SkyHook *h = calloc(1, sizeof *h);
    h->target = sym; h->repl = repl;
    do h->all = g_sky_hooks; while (InterlockedCompareExchangePointer((void *volatile *)&g_sky_hooks, h, h->all) != h->all);
    const char *state = "active";
    uint64_t orig = 0;
    int mid = sym >= GUEST_BASE && sym < GUEST_BASE + IMAGE_SIZE && sky_compiled(sym, 3);   /* the re-lifted DLL has an entry there too */
    if (!mid && function_at(sym)) {
        if (hook_install(sym, sky_hook_thunk, h, &h->orig) > 0) orig = hostfn_closure("skyline orig", sky_hook_orig, h);
        else state = "NOT ACTIVE (hook table refused the entry)";
    } else if (!mid && hostentry_hook(sym, sky_hook_thunk, h, &orig)) state = "active (host entry)";
    else if (sym >= GUEST_BASE && sym < GUEST_BASE + IMAGE_SIZE) {
        AcquireSRWLockExclusive(&g_sky_lock);
        SkyMid *m = g_sky_mid;
        while (m && m->addr != sym) m = m->next;
        if (!m) { m = calloc(1, sizeof *m); m->addr = sym; m->next = g_sky_mid; g_sky_mid = m; }
        h->prev = m->top; m->top = h;
        ReleaseSRWLockExclusive(&g_sky_lock);
        orig = hostfn_closure("skyline mid orig", sky_mid_orig, h);
        state = mid ? "active (mid-function callout)" : "NOT ACTIVE (mid-function: needs a midhook site, re-lift)";
    } else state = "NOT ACTIVE (not lifted code of main or a module)";
    if (out) wr64(out, orig);
    char from[96]; sky_where(c->x[30], from, sizeof from);
    sky_site("%s %llx    # %s repl=%llx (%s) %s; from %s", strstr(state, "mid") ? "midhook" : sym >= GUEST_BASE && sym < GUEST_BASE + IMAGE_SIZE ? "noinline" : "hook",
             (unsigned long long)sym, where, (unsigned long long)repl, rwhere, state, from);
    if (state[0] == 'N') sky_logf("hook at %s -> %s: %s", where, rwhere, state);
}

/* ---------------------------------------------------------------- patches */
static int sky_code_range(uint64_t a) {          /* 1 = in lifted code (main text, a module's text): bytes have no effect unless compiled in */
    SkySeg s;
    return sky_find_seg(a, &s) && (s.perm & 4);
}
static void sky_memcpy_fn(Ctx *c) {
    uint64_t dst = c->x[0], src = c->x[1], n = c->x[2];
    if (!dst || !src) { c->x[0] = (uint64_t)-1; return; }
    sky_rtld_init();
    int code = sky_code_range(dst);
    size_t keep = code ? (size_t)n : 0;                       /* the old words of code, for the log */
    uint8_t *before = keep ? malloc(keep) : NULL;
    if (!before) keep = 0;
    else memcpy(before, (void *)(uintptr_t)dst, keep);
    DWORD old;
    BOOL prot = VirtualProtect((void *)(uintptr_t)dst, (SIZE_T)n, PAGE_READWRITE, &old);
    memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
    if (prot) { DWORD o2; VirtualProtect((void *)(uintptr_t)dst, (SIZE_T)n, old, &o2); }
    char where[96]; sky_where(dst, where, sizeof where);
    if (code) {
        for (uint64_t off = 0; off + 4 <= n; off += 4) {
            uint32_t was = off + 4 <= keep ? *(uint32_t *)(before + off) : 0, now = rd32(dst + off);
            const SkySite *s = sky_compiled(dst + off, 1);
            int act = s && s->word == now, dyn = sky_compiled(dst + off, 4) != NULL;
            char w2[96]; sky_where(dst + off, w2, sizeof w2);
            sky_site("patch %llx %08x %08x    # %s %s", (unsigned long long)(dst + off), was, now, w2,
                     act ? "active" : dyn ? "active (decoded at run time)" : was == now ? "unchanged" : "NOT ACTIVE (code patch: re-lift)");
            if (!act && !dyn && was != now) sky_logf("patch at %s (%08x -> %08x): NOT ACTIVE", w2, was, now);
        }
        if (n & 3) sky_site("patchbytes %llx %llu    # %s: not a whole number of words", (unsigned long long)dst, (unsigned long long)n, where);
    } else sky_site("data %llx %llu    # %s", (unsigned long long)dst, (unsigned long long)n, where);
    free(before);
    c->x[0] = 0;
}

/* ---------------------------------------------------------------- the rest of Skyline's exports */
static void sky_getRegionAddress(Ctx *c) {       /* Region: Text 0, Rodata 1, Data 2, Bss 3, Heap 4 */
    sky_rtld_init();
    unsigned r = (unsigned)(c->x[0] & 0xff);
    c->x[0] = r < 3 ? g_sky_img_seg[0][r].start : r == 3 ? g_sky_img_seg[0][2].end : 0;
}
static void sky_get_program_id(Ctx *c) { c->x[0] = SKY_PROGRAM_ID; }
static void sky_tcp_send_raw(Ctx *c) {           /* (const char *data, u64 size): println! of skyline-rs comes here */
    size_t n = (size_t)c->x[1];
    const char *p = (const char *)(uintptr_t)c->x[0];
    if (!p || !n) return;
    while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
    static ULONGLONG t0;
    if (!t0) t0 = GetTickCount64();
    double t = (double)(GetTickCount64() - t0) / 1000.0;      /* seconds since the first plugin line */
    if (n) { printf("[plugin %6.1f] %.*s\n", t, (int)n, p); trace_printf("[plugin %6.1f] %.*s\n", t, (int)n, p); }
}
static void sky_get_plugin_addresses(Ctx *c) { if (c->x[1]) wr64(c->x[1], 0); if (c->x[2]) wr64(c->x[2], 0); }
static void sky_nothing(Ctx *c) { (void)c; }

static const ImportDef sky_imports[] = {
    {"A64HookFunction", sky_A64HookFunction}, {"A64InlineHook", sky_A64InlineHook}, {"sky_memcpy", sky_memcpy_fn},
    {"getRegionAddress", sky_getRegionAddress}, {"get_program_id", sky_get_program_id}, {"skyline_tcp_send_raw", sky_tcp_send_raw},
    {"get_plugin_addresses", sky_get_plugin_addresses}, {"__custom_init", sky_nothing}, {"__custom_fini", sky_nothing},
};

/* ---------------------------------------------------------------- the loader */
typedef struct { char path[MAX_PATH]; } SkyFile;
static int sky_cmp_file(const void *a, const void *b) { return _stricmp(((const SkyFile *)a)->path, ((const SkyFile *)b)->path); }
static void sky_scan(const char *dir, SkyFile *out, int *n, int cap) {          /* recursive, like Skyline's walk */
    char pat[MAX_PATH * 2]; WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) sky_scan(p, out, n, cap);
        else if (*n < cap) snprintf(out[(*n)++].path, MAX_PATH, "%s", p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

typedef struct { Ctx *c; int loaded; } SkyBoot;
static uint64_t sky_lookup_module_symbol(NroModule *m, const char *name) {
    for (int i = 1; i < m->sym_count; i++)
        if (m->syms[i].defined && !strcmp(m->syms[i].name, name)) return m->info->base + m->syms[i].value;
    return 0;
}
static void ro_LoadModule_entry(Ctx *c);
static DWORD WINAPI sky_boot_thread(LPVOID arg) {
    SkyBoot *b = arg;
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    memset(c, 0, sizeof *c);
    uint64_t stack = (uint64_t)(uintptr_t)VirtualAlloc(NULL, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    c->x[31] = stack + 0x100000 - 0x100;
    c->tpidr = (uint64_t)(uintptr_t)VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    { uint64_t os_main_thread_type(void); wr64(c->tpidr + 0x1f8, os_main_thread_type()); }
    char dir[MAX_PATH * 2];
    snprintf(dir, sizeof dir, "%s\\" SKY_PLUGIN_DIR, g_sky_sd);
    static SkyFile files[SKY_MAX_PLUGINS];
    int nf = 0;
    sky_scan(dir, files, &nf, SKY_MAX_PLUGINS);
    qsort(files, (size_t)nf, sizeof files[0], sky_cmp_file);
    NroModule *mods[SKY_MAX_PLUGINS];
    int nm = 0;
    for (int i = 0; i < nf; i++) {
        FILE *f = fopen(files[i].path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *img = VirtualAlloc(NULL, (SIZE_T)((size + 0xfff) & ~0xfff), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        size_t got = fread(img, 1, (size_t)size, f);
        fclose(f);
        uint64_t nro = (uint64_t)(uintptr_t)img;
        if (got != (size_t)size || size < 0x80 || rd32(nro + 0x10) != 0x304f524eu) { sky_logf("%s is not an NRO: skipped", files[i].path); continue; }
        NroModule *m = module_of_image(nro);
        if (!m) { sky_logf("%s: no lifted code for this build of the plugin (build id not in plugins.txt): skipped", files[i].path); continue; }
        uint64_t module = (uint64_t)(uintptr_t)calloc(1, 0x140), bss_size = rd32(nro + 0x38);
        uint64_t bss = bss_size ? (uint64_t)(uintptr_t)VirtualAlloc(NULL, (SIZE_T)bss_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : 0;
        snprintf((char *)(uintptr_t)(module + 0x30), 0x100, "%s", m->info->name);
        c->x[0] = module; c->x[1] = nro; c->x[2] = bss; c->x[3] = bss_size; c->x[4] = 0;
        ro_LoadModule_entry(c);
        if (c->x[0] != RO_OK) { sky_logf("%s: nn::ro::LoadModule failed (0x%llx)", files[i].path, (unsigned long long)c->x[0]); continue; }
        mods[nm++] = m;
        if (!strcmp(m->info->name, "sky_smashline")) sky_find_interpreter(m);
    }
    for (int i = 0; i < nm; i++) {                 /* Skyline: every plugin is loaded before the first main runs */
        uint64_t entry = sky_lookup_module_symbol(mods[i], "main");
        if (!entry) { sky_logf("%s has no main", mods[i]->info->name); continue; }
        sky_logf("%s: main", mods[i]->info->name);
        call_guest(c, entry);
        sky_logf("%s: main returned", mods[i]->info->name);
    }
    b->loaded = nm;
    return 0;
}

static ImportFn g_sky_mountrom_orig;
static void sky_MountRom(Ctx *c) {               /* the game's first MountRom: plugins load here, the game waits */
    static volatile LONG done;
    g_sky_mountrom_orig(c);
    if (InterlockedExchange(&done, 1)) return;
    uint64_t result = c->x[0];
    if (sky_registry() > 0) {
        char log[MAX_PATH * 2]; exe_dir(log, sizeof log); strncat(log, "skyline_sites.log", sizeof log - strlen(log) - 1);
        g_sky_log = fopen(log, "w");
        if (g_sky_log) fprintf(g_sky_log, "# sites the plugins hooked / patched in this run (re-lift site list format; tools/lifter/relift_sites.py)\n");
        { uint32_t fs_mount_direct(const char *name, const char *host_dir); uint32_t r = fs_mount_direct("sd", g_sky_sd);
          sky_logf("sd:/ = %s (mount result 0x%x); %d plugins have lifted code", g_sky_sd, r, g_sky_count); }
        sky_rtld_init();
        SkyBoot b = {0};
        HANDLE h = CreateThread(NULL, (SIZE_T)64 << 20, sky_boot_thread, &b, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        sky_logf("%d plugins loaded; the game continues", b.loaded);
    }
    c->x[0] = result;
}

/* ---------------------------------------------------------------- nn::ro additions */
static void ro_LookupSymbol(Ctx *c) {            /* (uintptr_t *out, const char *name): every loaded module, then main */
    const char *name = (const char *)(uintptr_t)c->x[1];
    const Export *e = exp_find(name);
    uint64_t a = e ? e->value : 0;
    if (!a) { int mi = main_find(name); if (mi >= 0 && dyn_syms[mi].defined) a = GUEST_BASE + dyn_syms[mi].value; }
    wr64(c->x[0], a);
    c->x[0] = a ? RO_OK : 0x1416u;               /* module 22, description 10: NotFound */
}
static void ro_LookupModuleSymbol(Ctx *c) {      /* (uintptr_t *out, const Module *, const char *name) */
    uint64_t obj = rd64(c->x[1]);
    NroModule *m = obj ? module_at(obj) : NULL;
    uint64_t a = m && m->loaded ? sky_lookup_module_symbol(m, (const char *)(uintptr_t)c->x[2]) : 0;
    wr64(c->x[0], a);
    c->x[0] = a ? RO_OK : 0x1416u;
}

/* smashline's interpreter stub (nnunwind.c), found by the shape of its code, which smashline's interpreter.asm fixes:
 * entry stp x29,x30 / stp x19,x20 / str x21 (all pre-indexed -16), mov x19,x0 / mov x20,x1 / mov x21,x2, then
 * adr x0, <landing pad>; mov x1, sp; bl set_smashline_interpreter_landing; three movs; bl smashline_interpreter (its return
 * address is the stub's exit); the landing pad is a nop, resumed at +4. */
void unwind_set_interpreter(uint64_t ret_addr, uint64_t resume_lp);
void unwind_interp_call(Ctx *c, void (*orig)(Ctx *));
static ImportFn g_sky_interp_orig;
static void sky_interp_hook(Ctx *c, void *user) { (void)user; unwind_interp_call(c, g_sky_interp_orig); }
static void sky_find_interpreter(NroModule *m) {
    uint64_t base = m->info->base, tsize = rd32(base + 0x24);
    static const uint32_t head[6] = {0xa9bf7bfd, 0xa9bf53f3, 0xf81f0ff5, 0xaa0003f3, 0xaa0103f4, 0xaa0203f5};
    for (uint64_t a = base; a + 0x60 < base + tsize; a += 4) {
        if (memcmp((void *)(uintptr_t)a, head, sizeof head)) continue;
        uint32_t adr = rd32(a + 24);
        if ((adr & 0x9f00001f) != 0x10000000) continue;                     /* adr x0, <label> */
        int64_t imm = ((int64_t)(int32_t)((adr >> 5) << 13) >> 11) | ((adr >> 29) & 3);
        uint64_t lp = a + 24 + (uint64_t)imm;
        int bls = 0;
        for (uint64_t b = a + 28; b < a + 0x40; b += 4) {
            if ((rd32(b) & 0xfc000000) != 0x94000000 || ++bls < 2) continue;   /* the second bl: smashline_interpreter */
            if (rd32(lp) != 0xd503201f) break;                             /* the landing pad is a nop */
            unwind_set_interpreter(b + 4, lp + 4);
            if (hook_install(a, sky_interp_hook, NULL, &g_sky_interp_orig) > 0)
                sky_logf("smashline interpreter stub at %llx (exit %llx, landing pad %llx): exceptions of scripts land there", (unsigned long long)a, (unsigned long long)(b + 4), (unsigned long long)(lp + 4));
            return;
        }
    }
    sky_logf("smashline: interpreter stub NOT found: a script ended by an exception will stop the game");
}

/* At exit: how often the plugins' hooks and inline hooks ran (evidence that their code takes part in the game). */
static int sky_cmp_calls(const void *a, const void *b) { LONG64 x = *(const LONG64 *)b, y = *(const LONG64 *)a; return x < y ? -1 : x > y; }
extern void (*g_plugin_report_hook)(void);
static void sky_report(void) {
    typedef struct { LONG64 calls; uint64_t addr, repl; } Row;
    static Row rows[4096];
    int n = 0; LONG64 total = 0; int used = 0;
    for (SkyHook *h = g_sky_hooks; h && n < 4096; h = h->all) { rows[n].calls = h->calls; rows[n].addr = h->target; rows[n].repl = h->repl; total += h->calls; used += h->calls > 0; n++; }
    qsort(rows, (size_t)n, sizeof rows[0], sky_cmp_calls);
    printf("[skyline] function hooks: %d installed, %d called, %lld calls in all; most called:\n", n, used, (long long)total);
    for (int i = 0; i < n && i < 12 && rows[i].calls; i++) {
        char w[96], r[96]; sky_where(rows[i].addr, w, sizeof w); sky_where(rows[i].repl, r, sizeof r);
        printf("  %12lld  %s -> %s\n", (long long)rows[i].calls, w, r);
    }
    n = 0; total = 0; used = 0;
    for (int b = 0; b < SKY_IH_HASH; b++)
        for (SkyInline *e = g_sky_ih[b]; e && n < 4096; e = e->next) { rows[n].calls = e->calls; rows[n].addr = e->addr; rows[n].repl = e->cb[0]; total += e->calls; used += e->calls > 0; n++; }
    qsort(rows, (size_t)n, sizeof rows[0], sky_cmp_calls);
    printf("[skyline] inline hooks: %d installed, %d reached, %lld times in all; most reached:\n", n, used, (long long)total);
    for (int i = 0; i < n && i < 12 && rows[i].calls; i++) {
        char w[96], r[96]; sky_where(rows[i].addr, w, sizeof w); sky_where(rows[i].repl, r, sizeof r);
        printf("  %12lld  %s -> %s\n", (long long)rows[i].calls, w, r);
    }
    fflush(stdout);
}

/* nnro.c register_ro_imports (after nnfs.c registered MountRom) */
static void sky_register(void) {
    extern int (*g_svc_handler)(Ctx *, uint32_t);        /* hostrt.c guest_svc */
    register_imports(sky_imports, (int)(sizeof sky_imports / sizeof sky_imports[0]));
    if (sky_registry() <= 0) return;
    g_svc_handler = sky_svc;
    hostfn_placeholders_readable();         /* hostrt.c: plugins read "orig" addresses as data */
    g_plugin_report_hook = sky_report;      /* report_and_exit (ExitProcess skips atexit) */
    g_sky_mountrom_orig = import_lookup("_ZN2nn2fs8MountRomEPKcPvm");
    if (g_sky_mountrom_orig) { ImportDef d = {"_ZN2nn2fs8MountRomEPKcPvm", sky_MountRom}; register_imports(&d, 1); }
    else printf("[skyline] nn::fs::MountRom has no handler: plugins are not loaded\n");
}

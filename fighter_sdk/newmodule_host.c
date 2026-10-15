/* newmodule_host.c - N5 (plan\phases\N5_own_module.md): the fighter registry and SCRIPT MODULES THAT EXIST ONLY AS OUR DLL.
 * Included at the end of nnro.c (it uses that file's module table). OPT-IN: without SSBU_CHARACTERS=<folder> nothing
 * of this file runs and no hook is installed. Contains nothing of the game.
 *
 * 1. The registry. <folder>\<name>\fighter.json of every sub-folder = one new fighter (name, donor, dll, kind).
 *    Kinds are given in name order from 0x5E. The fighters' DLLs are staged into registry_bin\fighters\ (unless
 *    SSBU_FIGHTER_BIN is set: then that folder is used as it is) and SSBU_MODS defaults to registry_bin, the folder of
 *    the registry's components (kind tables, roster rows: still DLLs built with hook.h; they get the list through
 *    ssbu_registry(), fighter_sdk\include\registry.h).
 *
 * 2. A module without an NRO. The game's script module worker (main 0x71022b6620) formats
 *    "prebuilt:/nro/<build>/lua2cpp_<name>.nro" (call at 0x71022b67d0, return address 0x71022b67d4; x4 = the name,
 *    x25 = the request object, callee-saved), hashes the string, looks it up in the archive and, when it is there,
 *    loads the file and calls nn::ro::LoadModule; then request + 0x132 = 1 ("loaded"). A name the archive does not
 *    have falls through silently and the requester waits for ever (N4, run r4).
 *    The hook on the formatter recognises that call site by its return address (N4's "callee hook + return address")
 *    and answers a registered name itself: the module object is a NroModule of OURS with an address range of its own
 *    behind the registry's modules (reserved, never committed: the addresses are names of functions, not memory):
 *      - function at (range + offset) = the function of the fighter's DLL bound at that offset (fighter_host.c's
 *        dispatch, S10's synthetic addresses), else the DONOR module's function at the same offset;
 *      - the donor's module image is a dependency (its class tables and constants are data of that image): when it
 *        is not loaded the request is turned into a request for the donor's module, which the game then loads by
 *        its own path; a request for a module that is already loaded is answered "loaded".
 *    An agent the donor's factory makes for a new fighter is moved into the new module (agent_end): its class table
 *    is replaced by a copy whose donor-module entries are the new module's addresses, and every script it registers
 *    (lib::L2CAgent::sv_set_function_hash, lua2cpp::L2CAgentBase::sv_set_status_func: hooked by name) is registered
 *    under the new module's address. From then on the engine calls lua2cpp_<name> addresses for that agent.
 *    What still is the donor's: the factories themselves, the data of the image, and every function the DLL does
 *    not have as source (docs\NEW_FIGHTER.md).
 *
 * 3. Paths under the new name. The same formatter builds the engine's file paths. A formatted string that contains
 *    a new fighter's name as a path token is logged once ("[registry] path asked") and, as long as stage 2 of the
 *    loose-file layer has no row for it, rewritten to the donor's name, so the archive's file answers. */
#include <stdarg.h>
#include "include/registry.h"

#define NM_MAX 8
#define NM_VT_SLOTS 96
#define NM_WORKER_RET GA(0x71022b67d4ULL)
#define NM_FORMATTER GA(0x7101ea04d0ULL)

typedef struct NewMod {
    SsbuRegFighter f;
    char donor_module[48], module[48];
    NroInfo info;
    NroModule mod;                 /* what module_at() returns for the new range */
    NroModule *dm;                 /* the donor's module */
    FnEntry *fns; uint64_t *bind_addr;
    int active;
    uint64_t vt_src[16], vt_dst[16]; int vt_count;
    long requests, answered, substituted, agents, remapped, vt_clones, paths, paths_missing;
} NewMod;
static NewMod g_nm[NM_MAX];
static int g_nm_count, g_nm_started;
static SsbuRegFighter g_nm_list[NM_MAX];
static SRWLOCK g_nm_lock = SRWLOCK_INIT;
static __declspec(thread) NewMod *t_nm_current;
static struct { uint64_t agent; NewMod *nm; } g_nm_agent[2048];
static ImportFn g_nm_o_fmt, g_nm_o_fnhash, g_nm_o_status;

static void nm_log(const char *fmt, ...) {
    char line[700]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    printf("%s\n", line);
    if (g_nm_started) trace_printf("%s\n", line);      /* the manifests are read from a constructor: before that only the log */
}

/* ---------------------------------------------------------------- manifests */
static int nm_json_str(const char *js, const char *key, char *out, size_t n) {
    char pat[48]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    out[0] = 0;
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n') p++;
    size_t k = 0;
    if (*p == '"') { for (p++; *p && *p != '"' && k + 1 < n; p++) out[k++] = *p; }
    else for (; *p && *p != ',' && *p != '}' && *p != '\r' && *p != '\n' && *p != ' ' && k + 1 < n; p++) out[k++] = *p;
    out[k] = 0;
    return 1;
}
static int nm_cmp_fighter(const void *a, const void *b) { return strcmp(((const NewMod *)a)->f.name, ((const NewMod *)b)->f.name); }

static void nm_scan(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *root = getenv("SSBU_CHARACTERS");
    if (!root || !root[0]) return;
    char pat[MAX_PATH * 2]; WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", root);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { nm_log("[registry] SSBU_CHARACTERS=%s: no such folder", root); return; }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || g_nm_count >= NM_MAX) continue;
        char path[MAX_PATH * 2], js[4096];
        snprintf(path, sizeof path, "%s\\%s\\fighter.json", root, fd.cFileName);
        FILE *f = fopen(path, "rb");
        if (!f) continue;                              /* a folder without a manifest is a plain file mod (loose_files.c) */
        size_t n = fread(js, 1, sizeof js - 1, f); fclose(f); js[n] = 0;
        NewMod *nm = &g_nm[g_nm_count];
        char v[64];
        memset(nm, 0, sizeof *nm);
        if (!nm_json_str(js, "name", nm->f.name, sizeof nm->f.name) || !nm->f.name[0]) snprintf(nm->f.name, sizeof nm->f.name, "%.31s", fd.cFileName);
        if (!nm_json_str(js, "donor", nm->f.donor, sizeof nm->f.donor) || !nm->f.donor[0]) { nm_log("[registry] %s: no \"donor\": skipped", path); continue; }
        nm_json_str(js, "dll", nm->f.dll, sizeof nm->f.dll);
        nm->f.own_name = !(nm_json_str(js, "own_name", v, sizeof v) && (v[0] == 'f' || v[0] == '0'));
        nm->f.kind = nm_json_str(js, "kind", v, sizeof v) && v[0] >= '0' && v[0] <= '9' ? (int)strtol(v, NULL, 0) : -1;
        snprintf(nm->f.folder, sizeof nm->f.folder, "%s\\%s", root, fd.cFileName);
        snprintf(nm->module, sizeof nm->module, "lua2cpp_%s", nm->f.name);
        snprintf(nm->donor_module, sizeof nm->donor_module, "lua2cpp_%s", nm->f.donor);
        g_nm_count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(g_nm, (size_t)g_nm_count, sizeof g_nm[0], nm_cmp_fighter);
    const char *bin = getenv("SSBU_FIGHTER_BIN");
    int staged = 0;
    if (g_nm_count && !(bin && bin[0])) { CreateDirectoryA("registry_bin", NULL); CreateDirectoryA("registry_bin\\fighters", NULL); }
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        if (nm->f.kind < 0) nm->f.kind = 0x5e + i;     /* "auto": the first free kind numbers, in name order */
        if (nm->f.dll[0] && !(bin && bin[0])) {
            char src[MAX_PATH * 2], dst[MAX_PATH * 2];
            snprintf(src, sizeof src, "%s\\%s", nm->f.folder, nm->f.dll);
            snprintf(dst, sizeof dst, "registry_bin\\fighters\\%s.dll", nm->f.name);
            if (CopyFileA(src, dst, FALSE)) staged++; else nm_log("[registry] %s: cannot stage %s (Windows error %lu)", nm->f.name, src, (unsigned long)GetLastError());
        }
        g_nm_list[i] = nm->f;
        nm_log("[registry] fighter %s: kind 0x%x, donor %s, script DLL %s, folder %s, own name %d", nm->f.name, nm->f.kind, nm->f.donor,
               nm->f.dll[0] ? nm->f.dll : "(none)", nm->f.folder, nm->f.own_name);
    }
    if (g_nm_count) {
        if (!(bin && bin[0])) _putenv_s("SSBU_FIGHTER_BIN", "registry_bin\\fighters");
        const char *mods = getenv("SSBU_MODS");
        if (!(mods && mods[0])) _putenv_s("SSBU_MODS", "registry_bin");
        nm_log("[registry] %d fighter(s) from %s; %d script DLL(s) staged; fighter DLL folder %s; components from %s", g_nm_count, root, staged,
               getenv("SSBU_FIGHTER_BIN"), getenv("SSBU_MODS"));
    }
}
__attribute__((constructor)) static void nm_constructor(void) { nm_scan(); }

/* ---------------------------------------------------------------- the module object */
static NewMod *nm_find(const char *name) {
    for (int i = 0; i < g_nm_count; i++) if (strcmp(g_nm[i].f.name, name) == 0) return &g_nm[i];
    return NULL;
}
static NroModule *nm_registry_module(const char *module) {
    ro_init_tables();
    for (int i = 0; i < nro_registry_count; i++) if (strcmp(g_mods[i].info->name, module) == 0) return &g_mods[i];
    return NULL;
}
/* address range of new module number i: behind the last module of the registry, 2 MB slots */
static uint64_t nm_base(int index, uint32_t image_size) {
    uint64_t end = NRO_BASE;
    for (int i = 0; i < nro_registry_count; i++) {
        uint64_t e = nro_registry[i].base + (((uint64_t)nro_registry[i].image_size + 0xffff) & ~(uint64_t)0xffff);
        if (e > end) end = e;
    }
    end = (end + (1u << NRO_SLOT_SHIFT) - 1) & ~(uint64_t)((1u << NRO_SLOT_SHIFT) - 1);
    (void)image_size;
    return end + (uint64_t)(index + 1) * 0x1000000ULL;         /* 16 MB apart: no fighter module is larger than 4 MB */
}
static int nm_activate(NewMod *nm) {
    if (nm->active) return 1;
    if (!nm->dm) nm->dm = nm_registry_module(nm->donor_module);
    NroModule *dm = nm->dm, *m = &nm->mod;
    if (!dm || !dm->loaded || !dm->fns) return 0;
    AcquireSRWLockExclusive(&g_ro_lock);
    if (!nm->info.base) {
        nm->info = *dm->info;
        nm->info.name = nm->module;
        nm->info.base = nm_base((int)(nm - g_nm), dm->info->image_size);
        /* the range is reserved so that no allocation of the process can get these addresses; it is never committed */
        VirtualAlloc((void *)(uintptr_t)nm->info.base, dm->info->image_size, MEM_RESERVE, PAGE_NOACCESS);
        nm->fns = malloc((size_t)dm->fn_count * sizeof *nm->fns);
        for (int i = 0; i < dm->fn_count; i++) { nm->fns[i].addr = dm->fns[i].addr - dm->info->base + nm->info.base; nm->fns[i].fn = dm->fns[i].fn; }
        nm->bind_addr = malloc((size_t)dm->sym_count * sizeof *nm->bind_addr);
    }
    for (int i = 0; i < dm->sym_count; i++) {
        uint64_t a = dm->bind_addr ? dm->bind_addr[i] : 0;
        nm->bind_addr[i] = a - dm->info->base < dm->info->image_size ? a - dm->info->base + nm->info.base : a;
    }
    memset(m, 0, sizeof *m);
    m->info = &nm->info; m->dll = dm->dll;
    m->fns = nm->fns; m->fn_count = dm->fn_count; m->syms = dm->syms; m->sym_count = dm->sym_count;
    m->bind_fn = dm->bind_fn; m->bind_addr = nm->bind_addr;
    m->loaded = 1;
    for (uint64_t s = 0; s < ((uint64_t)nm->info.image_size + (1u << NRO_SLOT_SHIFT) - 1) >> NRO_SLOT_SHIFT; s++)
        g_slot[((nm->info.base - NRO_BASE) >> NRO_SLOT_SHIFT) + s] = m;
    ReleaseSRWLockExclusive(&g_ro_lock);
    nm->vt_count = 0;
    nm->active = 1;
    fighter_attach(m);                                  /* <fighter DLL folder>\<name>.dll: the fighter's own script functions */
    int own = m->fighter ? ((FsMod *)m->fighter)->ovr_count : 0;
    nm_log("[registry] module %s is OURS: no NRO; address range %llx (+0x%x, reserved, not committed); %d of its %d functions come from %s.dll, "
           "the others from the donor module %s (at %llx) at the same offset",
           nm->module, (unsigned long long)nm->info.base, nm->info.image_size, own, dm->fn_count, nm->f.name, nm->donor_module, (unsigned long long)dm->info->base);
    return 1;
}

/* ---------------------------------------------------------------- agents */
static NewMod *nm_agent_lookup(uint64_t agent) {
    NewMod *nm = NULL;
    unsigned i = (unsigned)(agent >> 4) & 2047;
    AcquireSRWLockShared(&g_nm_lock);
    for (int k = 0; k < 2048 && g_nm_agent[i].agent; k++, i = (i + 1) & 2047) if (g_nm_agent[i].agent == agent) { nm = g_nm_agent[i].nm; break; }
    ReleaseSRWLockShared(&g_nm_lock);
    return nm;
}
static void nm_agent_set(uint64_t agent, NewMod *nm) {      /* nm == NULL: forget (the slot stays in the probe chain) */
    unsigned i = (unsigned)(agent >> 4) & 2047;
    AcquireSRWLockExclusive(&g_nm_lock);
    for (int k = 0; k < 2048; k++, i = (i + 1) & 2047) {
        if (g_nm_agent[i].agent == agent) { g_nm_agent[i].nm = nm; break; }
        if (!g_nm_agent[i].agent) { if (nm) { g_nm_agent[i].agent = agent; g_nm_agent[i].nm = nm; } break; }
    }
    ReleaseSRWLockExclusive(&g_nm_lock);
}
static uint64_t nm_remap(NewMod *nm, uint64_t a) {
    NroModule *dm = nm->dm;
    if (a - dm->info->base < dm->info->image_size && fn_in_table(dm->fns, dm->fn_count, a)) return a - dm->info->base + nm->info.base;
    return a;
}
static uint64_t nm_clone_vtable(NewMod *nm, uint64_t vt) {
    NroModule *dm = nm->dm;
    if (vt - dm->info->base >= dm->info->image_size) return vt;        /* a class of another module: nothing of the donor's in it */
    for (int i = 0; i < nm->vt_count; i++) if (nm->vt_src[i] == vt) return nm->vt_dst[i];
    uint64_t *copy = calloc(NM_VT_SLOTS + 2, 8), lo = vt - 0x10, end = dm->info->base + dm->info->image_size;
    int moved = 0;
    for (int i = 0; i < NM_VT_SLOTS + 2 && lo + 8ull * i + 8 <= end; i++) {
        uint64_t v = rd64(lo + 8ull * i);
        copy[i] = i >= 2 ? nm_remap(nm, v) : v;
        if (copy[i] != v) moved++;
    }
    uint64_t dst = (uint64_t)(uintptr_t)(copy + 2);
    if (nm->vt_count < 16) { nm->vt_src[nm->vt_count] = vt; nm->vt_dst[nm->vt_count++] = dst; }
    nm->vt_clones++;
    nm_log("[registry] %s: class table %s+0x%llx copied for the new module: %d entries are now %s addresses", nm->module, nm->donor_module,
           (unsigned long long)(vt - dm->info->base), moved, nm->module);
    return dst;
}
static void nm_agent_begin(const char *name) {
    NewMod *nm = name ? nm_find(name) : NULL;
    t_nm_current = nm && nm_activate(nm) ? nm : NULL;
    if (nm && !t_nm_current) { static int said; if (!said++) nm_log("[registry] %s: the donor module %s is not loaded: the agent stays the donor's", nm->module, nm->donor_module); }
}
static void nm_agent_end(const char *name, uint64_t agent) {
    NewMod *nm = t_nm_current;
    t_nm_current = NULL;
    if (!name) { if (agent && g_nm_count) nm_agent_set(agent, NULL); return; }
    if (!nm || agent < 0x10000) return;
    uint64_t vt = rd64(agent), nv = nm_clone_vtable(nm, vt);
    if (nv != vt) wr64(agent, nv);
    nm_agent_set(agent, nm);
    if (nm->agents++ < 24) nm_log("[registry] %s: agent %llx (class table %s+0x%llx) belongs to the new module", nm->module, (unsigned long long)agent,
                                  vt - nm->dm->info->base < nm->dm->info->image_size ? nm->donor_module : "(other)", (unsigned long long)(vt - nm->dm->info->base));
}
static void nm_h_fnhash(Ctx *c, void *user) {          /* lib::L2CAgent::sv_set_function_hash(this, function, hash) */
    (void)user;
    NewMod *nm = t_nm_current ? t_nm_current : nm_agent_lookup(c->x[0]);
    if (nm && nm->active) { uint64_t a = nm_remap(nm, c->x[1]); if (a != c->x[1]) { c->x[1] = a; nm->remapped++; } }
    g_nm_o_fnhash(c);
}
static void nm_h_status(Ctx *c, void *user) {          /* lua2cpp::L2CAgentBase::sv_set_status_func(this, kind, line, function) */
    (void)user;
    NewMod *nm = t_nm_current ? t_nm_current : nm_agent_lookup(c->x[0]);
    if (nm && nm->active) { uint64_t a = nm_remap(nm, c->x[3]); if (a != c->x[3]) { c->x[3] = a; nm->remapped++; } }
    g_nm_o_status(c);
}

/* ---------------------------------------------------------------- the formatter: module requests and paths */
static uint64_t nm_hash40(const char *s) {
    uint32_t crc = 0xffffffffu; size_t n = 0;
    for (; s[n]; n++) {
        uint8_t ch = (uint8_t)s[n]; if (ch >= 'A' && ch <= 'Z') ch += 32;
        crc ^= ch; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ((uint64_t)(n & 0xff) << 32) | (uint32_t)~crc;
}
/* the archive's own file lookup (as the worker does it inline at 0x71022b68cc..): file path index, or -1 */
static int64_t nm_arc_file_index(uint64_t hash) {
    uint64_t fsinfo = rd64(GA(0x7105331f20ULL));
    if (!fsinfo) return -2;
    uint64_t holder = rd64(fsinfo + 0x78); if (!holder) return -2;
    uint64_t tab = rd64(holder); if (!tab) return -2;
    uint64_t buckets = rd64(tab + 0x50), entries = rd64(tab + 0x58);
    if (!buckets || !entries) return -2;
    uint32_t nb = rd32(buckets + 4); if (!nb) return -2;
    uint64_t b = buckets + 8 + (hash % nb) * 8;
    uint32_t start = rd32(b), count = rd32(b + 4);
    for (uint32_t i = 0; i < count; i++) {             /* the game searches the sorted range; a scan is enough for a log line */
        uint64_t e = rd64(entries + 8ull * (start + i));
        if ((e & 0xffffffffffULL) == hash) return (int64_t)(e >> 40);
    }
    return -1;
}
static int nm_token_char(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'); }
static uint64_t g_nm_seen[4096]; static int g_nm_seen_count;
int (*g_nm_stage2_has)(uint64_t path_hash);            /* set by the loose-file layer's stage 2 when it has a row for the path */
static char *nm_stristr(char *s, const char *name) {    /* the engine also writes the name with a capital (its sound-name row) */
    size_t n = strlen(name);
    for (; *s; s++) if (_strnicmp(s, name, n) == 0) return s;
    return NULL;
}
static void nm_path(char *buf, uint64_t lr) {
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        size_t ln = strlen(nm->f.name), ld = strlen(nm->f.donor);
        char *p = nm_stristr(buf, nm->f.name);
        if (!p || !strchr(buf, '/')) continue;
        char was[512]; snprintf(was, sizeof was, "%s", buf);
        uint64_t h_new = nm_hash40(buf);
        if (g_nm_stage2_has && g_nm_stage2_has(h_new)) return;      /* the path exists now: the fighter's own file */
        int swapped = 0;
        while (p) {
            int tok = (p == buf || !nm_token_char(p[-1])) && !nm_token_char(p[ln]);
            if (tok && ld <= ln) {
                int cap = p[0] >= 'A' && p[0] <= 'Z';
                memmove(p + ld, p + ln, strlen(p + ln) + 1); memcpy(p, nm->f.donor, ld);
                if (cap && p[0] >= 'a' && p[0] <= 'z') p[0] -= 32;
                swapped++; p = nm_stristr(p + ld, nm->f.name);
            } else p = nm_stristr(p + 1, nm->f.name);
        }
        if (!swapped) continue;
        {
            /* the engine's fixed strings are 256 bytes of text followed by a 16-bit length (0x71017e89c0: `strh wzr,
             * [buffer + 256]`), and it appends at that length: a shortened text needs the shorter length */
            MEMORY_BASIC_INFORMATION mi;
            uint16_t *len = (uint16_t *)(buf + 256);
            if (VirtualQuery(len, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY | PAGE_EXECUTE_READ))
                && *len == (uint16_t)strlen(was)) *len = (uint16_t)strlen(buf);
        }
        int known = 0;
        for (int k = 0; k < g_nm_seen_count; k++) if (g_nm_seen[k] == h_new) { known = 1; break; }
        if (!known) {
            if (g_nm_seen_count < 4096) g_nm_seen[g_nm_seen_count++] = h_new;
            int64_t fi = nm_arc_file_index(nm_hash40(buf));
            nm->paths++; if (fi < 0) nm->paths_missing++;
            nm_log("[registry] path asked: %s (hash %llx, by %llx) -> not in the archive -> the donor's: %s: %s", was, (unsigned long long)h_new, (unsigned long long)lr, buf,
                   fi >= 0 ? "found" : fi == -1 ? "NOT a file path of the archive (a folder, a stream, or missing)" : "(archive tables not ready)");
            if (fi >= 0) nm_log("[registry] path file index: %s = 0x%llx", buf, (unsigned long long)fi);
        }
        return;
    }
}
static void nm_h_fmt(Ctx *c, void *user) {
    (void)user;
    uint64_t buf = c->x[0], lr = c->x[30];
    if (lr == NM_WORKER_RET && c->x[4] > 0x10000) {
        const char *name = (const char *)(uintptr_t)c->x[4];
        uint64_t req = c->x[25];
        char module[64]; snprintf(module, sizeof module, "lua2cpp_%.40s", name);
        NewMod *nm = nm_find(name);
        NroModule *rm = nm ? NULL : nm_registry_module(module);
        if (nm) {
            nm->requests++;
            if (!nm->dm) nm->dm = nm_registry_module(nm->donor_module);
            if (!nm->dm) { nm_log("[registry] module request %s: the donor module %s is not a module of the game: NOT answered", module, nm->donor_module); g_nm_o_fmt(c); return; }
            if (nm->active && !nm->dm->loaded) { fighter_detach(&nm->mod); nm->active = 0; }      /* the donor's image went away with the last match */
            if (!nm->dm->loaded) {
                /* the donor's image is a dependency: this request loads it (the game's own path, its own LoadModule) */
                c->x[4] = (uint64_t)(uintptr_t)nm->f.donor;
                nm->substituted++;
                nm_log("[registry] module request %s (request object %llx): answered by OUR module (no NRO of that name); its donor image %s is not loaded: "
                       "this request loads it", module, (unsigned long long)req, nm->donor_module);
                g_nm_o_fmt(c);
                return;
            }
            *(uint8_t *)(uintptr_t)(req + 0x132) = 1;                  /* "loaded", as the worker writes it at 0x71022b6cd0 */
            *(char *)(uintptr_t)buf = 0;                               /* an empty path: the worker goes back to its queue */
            nm->answered++;
            nm_log("[registry] module request %s (request object %llx): answered by OUR module (no NRO of that name); donor image %s already loaded", module,
                   (unsigned long long)req, nm->donor_module);
            return;
        }
        if (rm && rm->loaded) {                        /* loaded earlier as a new fighter's donor: LoadModule would refuse a second load */
            *(uint8_t *)(uintptr_t)(req + 0x132) = 1;
            *(char *)(uintptr_t)buf = 0;
            nm_log("[registry] module request %s (request object %llx): the module is already loaded (as a donor): answered \"loaded\"", module, (unsigned long long)req);
            return;
        }
        g_nm_o_fmt(c);
        return;
    }
    g_nm_o_fmt(c);
    if (buf > 0x10000) nm_path((char *)(uintptr_t)buf, lr);
}

/* The other string builders that make file paths from a kind's name (found by what a run asked for and did not get):
 *   0x710147f330 (buffer of 256 bytes, format, arguments): used 12 times by the per-kind path builder 0x71017e88d0;
 *   0x71017df5f0 (buffer, kind, file type, x3): the per-kind sub-builder of the same function.
 * Both return the path in the buffer given in x0. */
/*   0x71017dfe50 (buffer, kind, file type, x3, x4): a second sub-builder of it;
 *   0x710353e330 (out, path string in x1): the path builder's last step, "path string -> file index" (entry 3:
 *   the string is looked at BEFORE the call). */
static const uint64_t g_nm_path_fn[] = {      /* 13.0.4 addresses: GA() where they are used */
    0x710147f330ULL, 0x71017df5f0ULL, 0x71017dfe50ULL, 0x710353e330ULL };
#define NM_PATH_PRE 3
static ImportFn g_nm_o_path[sizeof g_nm_path_fn / sizeof g_nm_path_fn[0]];
static void nm_h_pathfmt(Ctx *c, void *user) {
    uint64_t buf = c->x[0], lr = c->x[30];
    int i = (int)(intptr_t)user;
    if (i >= NM_PATH_PRE) { if (c->x[1] > 0x10000) nm_path((char *)(uintptr_t)c->x[1], lr); g_nm_o_path[i](c); return; }
    g_nm_o_path[i](c);
    if (buf > 0x10000) nm_path((char *)(uintptr_t)buf, lr);
}

static void nm_report(void) {
    for (int i = 0; i < g_nm_count; i++) {
        NewMod *nm = &g_nm[i];
        FsMod *fm = nm->mod.fighter;
        long long calls = 0; int called = 0;
        for (int k = 0; fm && k < fm->ovr_count; k++) { calls += fm->stat[k].calls; called += fm->stat[k].calls != 0; }
        nm_log("[registry] %s at exit: %ld module requests (%ld answered directly, %ld loaded the donor image), %ld agents moved into the module, %ld class tables copied, "
               "%ld script registrations moved to its addresses; %d functions of its DLL were called, %lld calls; %ld paths with its name asked (%ld not files of the archive)",
               nm->module, nm->requests, nm->answered, nm->substituted, nm->agents, nm->vt_clones, nm->remapped, called, calls, nm->paths, nm->paths_missing);
    }
}
static void nm_start(void) {
    if (g_nm_started || !g_nm_count) return;
    g_nm_started = 1;
    int a = hook_install(NM_FORMATTER, nm_h_fmt, NULL, &g_nm_o_fmt);
    int b = hook_install_name("_ZN3lib8L2CAgent20sv_set_function_hashEPvN3phx6Hash40E", nm_h_fnhash, NULL, &g_nm_o_fnhash);
    int d = hook_install_name("_ZN7lua2cpp12L2CAgentBase18sv_set_status_funcERKN3lib8L2CValueES4_Pv", nm_h_status, NULL, &g_nm_o_status);
    int e = 0;
    for (int i = 0; i < (int)(sizeof g_nm_path_fn / sizeof g_nm_path_fn[0]); i++) e += !!hook_install(GA(g_nm_path_fn[i]), nm_h_pathfmt, (void *)(intptr_t)i, &g_nm_o_path[i]);
    nm_log("[registry] runtime hooks: path formatter 0x%llx %s, sv_set_function_hash %s, sv_set_status_func %s, %d of %d other path builders", (unsigned long long)NM_FORMATTER,
           a ? "installed" : "FAILED", b ? "installed" : "FAILED", d ? "installed" : "FAILED", e, (int)(sizeof g_nm_path_fn / sizeof g_nm_path_fn[0]));
    atexit(nm_report);
}

static const SsbuRegistry g_nm_registry = { sizeof(SsbuRegistry), 1, 0, g_nm_list, nm_start, nm_agent_begin, nm_agent_end };
__declspec(dllexport) const SsbuRegistry *ssbu_registry(void) {
    static SsbuRegistry r;
    nm_scan();
    r = g_nm_registry;
    r.count = g_nm_count;
    return &r;
}

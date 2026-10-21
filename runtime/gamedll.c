/* Host side of the exe / ssbu_game.dll split (see game_shim.c).
 *
 * build.py compiles every host file with
 *     -Dfn_table=(*game_fn_table_p) -Dfn_table_count=(*game_fn_table_count_p)
 *     -Ddyn_syms=(*game_dyn_syms_p) -Ddyn_sym_count=(*game_dyn_sym_count_p)
 * so the tables declared in runtime.h become pointers into the DLL, filled in here before main(). */
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "game_data.h"

typedef struct {
    void (*call_indirect)(Ctx *, uint64_t);
    void (*host_import)(Ctx *, int);
    void (*guest_trap)(const char *, uint64_t);
    void (*guest_unimpl)(uint64_t, uint32_t, const char *);
    void (*guest_svc)(Ctx *, uint32_t);
    void (*stx_diag)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
    void (*site_inline)(Ctx *, uint64_t);      /* appended (game_shim.c): an older DLL copies only the fields it knows */
    int (*site_midhook)(Ctx *, uint64_t);
} GameHost;

typedef struct {
    const FnEntry *table;
    const int *table_count;
    const DynSym *syms;
    const int *sym_count;
} GameTables;

#ifdef _WIN32
/* The game DLL is loaded by name before main() (no import library): GAME_DLL_NAME (build.py: RECOMP_DLL, normally
 * ssbu_game_eh_o1.dll), or ssbu_game_sky.dll beside it, the game re-lifted with the sites of the Skyline plugins
 * (tools/lifter/relift_sites.py, scripts/mods.py), when the SD folder has plugins (skyline.c): one exe serves the normal
 * game and the modded one. SSBU_GAME_DLL=<name> names a DLL outright; SSBU_SKY_RELIFT=0 keeps the normal DLL with
 * plugins present (scripts/mods.py records their sites that way). */
#include <windows.h>
#ifndef GAME_DLL_NAME
#define GAME_DLL_NAME "ssbu_game_eh_o1.dll"
#endif
typedef void (*GameBindFn)(const GameHost *, GameTables *);
typedef void (*GameDataBindFn)(const GameDataHost *, GameData *);
int sky_plugins_present(void);                           /* skyline.c (in nnro.c): the SD folder has plugins */
int sky_use_relift(void);                                /* skyline.c: SSBU_SKY_RELIFT is not 0 */
static int g_game_dll_sky;                               /* the re-lifted DLL was loaded (for the message of a stale one) */
static HMODULE load_game_dll(void) {
    char path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    const char *env = getenv("SSBU_GAME_DLL"), *name = GAME_DLL_NAME, *why = "";
    if (env && env[0]) {
        name = env; why = " (SSBU_GAME_DLL)";
    } else if (sky_plugins_present() && sky_use_relift()) {
        snprintf(path + n, sizeof path - n, "ssbu_game_sky.dll");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { name = "ssbu_game_sky.dll"; why = " (the game re-lifted with the plugins' sites)"; }
        else why = " (plugins present, but no ssbu_game_sky.dll beside the exe: their patches and inline hooks are NOT ACTIVE; docs/SKYLINE.md)";
    }
    snprintf(path + n, sizeof path - n, "%s", name);
    HMODULE m = LoadLibraryA(path);
    if (!m) {
        fprintf(stderr, "cannot load the game DLL %s (Windows error %lu): the build is incomplete. Double-click \"Install SSBU.bat\" and press Resume.\n",
                path, (unsigned long)GetLastError());
        exit(9);
    }
    printf("game DLL: %s%s\n", name, why);
    g_game_dll_sky = !strcmp(name, "ssbu_game_sky.dll");
    return m;
}
#else
__declspec(dllimport) void game_bind(const GameHost *h, GameTables *t);
/* game_data.h: the dump-derived tables and the lifted SDK code, in the DLL too (game_data.c) */
__declspec(dllimport) void game_data_bind(const GameDataHost *h, GameData *d);
#endif
GameData game_data;
void sdk_host_call(Ctx *c, int index);                   /* sdklift_rt.c */
void sdk_trap_call(Ctx *c, const char *name);
void sdk_dispatch(Ctx *c, uint64_t addr);
void *sdk_jmp_record(Ctx *c);
void sdk_jmp_restore(Ctx *c, void *r);
void *sdk_longjmp_target(Ctx *c);

/* The site callouts of a DLL re-lifted from a site list (tools/lifter/relift_sites.py) go to the Skyline layer
 * (skyline.c, included by nnro.c). */
void sky_site_inline(Ctx *c, uint64_t addr);
int sky_site_midhook(Ctx *c, uint64_t addr);

#undef fn_table
#undef fn_table_count
#undef dyn_syms
#undef dyn_sym_count
const FnEntry (*game_fn_table_p)[];
const int *game_fn_table_count_p;
const DynSym (*game_dyn_syms_p)[];
const int *game_dyn_sym_count_p;

__attribute__((constructor)) static void bind_game(void) {
    GameHost h = {call_indirect, host_import, guest_trap, guest_unimpl, guest_svc, stx_diag, sky_site_inline, sky_site_midhook};
    GameTables t;
#ifdef _WIN32
    HMODULE m = load_game_dll();
    GameBindFn game_bind = (GameBindFn)(void *)GetProcAddress(m, "game_bind");
    GameDataBindFn game_data_bind = (GameDataBindFn)(void *)GetProcAddress(m, "game_data_bind");
    if (!game_bind || !game_data_bind) {
        fprintf(stderr, "the game DLL does not export game_bind / game_data_bind: it is not a game DLL of this project (or one built "
                        "before the data objects moved into it): build both from the same tree (scripts/setup.py)\n");
        exit(9);
    }
#endif
    game_bind(&h, &t);
    game_fn_table_p = (const FnEntry (*)[])t.table;
    game_fn_table_count_p = t.table_count;
    game_dyn_syms_p = (const DynSym (*)[])t.syms;
    game_dyn_sym_count_p = t.sym_count;
    GameDataHost dh = {GAME_DATA_VERSION, sdk_host_call, sdk_trap_call, sdk_dispatch, sdk_jmp_record, sdk_jmp_restore, sdk_longjmp_target};
    game_data_bind(&dh, &game_data);
    if (game_data.version != GAME_DATA_VERSION || game_data.size != (int)sizeof game_data) {
        fprintf(stderr, "the game DLL was built for another version of this exe (game data interface %d, this exe %d): build both "
                        "from the same tree (scripts/setup.py)\n", game_data.version, GAME_DATA_VERSION);
#ifdef _WIN32
        if (g_game_dll_sky)
            fprintf(stderr, "(ssbu_game_sky.dll, the game re-lifted with the plugins' sites, is older than the game DLL beside it: delete it, "
                            "or run python scripts/mods.py --force; \"Play SSBU.bat\" makes it again)\n");
#endif
        exit(9);
    }
}

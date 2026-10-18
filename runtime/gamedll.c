/* Host side of the exe / ssbu_game.dll split (see game_shim.c).
 *
 * build.py compiles every host file with
 *     -Dfn_table=(*game_fn_table_p) -Dfn_table_count=(*game_fn_table_count_p)
 *     -Ddyn_syms=(*game_dyn_syms_p) -Ddyn_sym_count=(*game_dyn_sym_count_p)
 * so the tables declared in runtime.h become pointers into the DLL, filled in here before main(). */
#include "runtime.h"

typedef struct {
    void (*call_indirect)(Ctx *, uint64_t);
    void (*host_import)(Ctx *, int);
    void (*guest_trap)(const char *, uint64_t);
    void (*guest_unimpl)(uint64_t, uint32_t, const char *);
    void (*guest_svc)(Ctx *, uint32_t);
    void (*stx_diag)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
    void (*site_inline)(Ctx *, uint64_t);      /* appended (HDR DLL, game_shim.c): an older DLL copies only the fields it knows */
    int (*site_midhook)(Ctx *, uint64_t);
} GameHost;

typedef struct {
    const FnEntry *table;
    const int *table_count;
    const DynSym *syms;
    const int *sym_count;
} GameTables;

__declspec(dllimport) void game_bind(const GameHost *h, GameTables *t);

/* The site callouts of a DLL re-lifted from a site list (tools/lifter/relift_sites.py) go to the Skyline layer
 * (skyline.c). Until that defines them, the linker takes these defaults (/alternatename: used only when no object
 * defines the name, so a definition in skyline.c simply wins): no callback, the original code runs. */
void sky_site_inline(Ctx *c, uint64_t addr);
int sky_site_midhook(Ctx *c, uint64_t addr);
void sky_site_inline_default(Ctx *c, uint64_t addr) { (void)c; (void)addr; }
int sky_site_midhook_default(Ctx *c, uint64_t addr) { (void)c; (void)addr; return 0; }
#pragma comment(linker, "/alternatename:sky_site_inline=sky_site_inline_default")
#pragma comment(linker, "/alternatename:sky_site_midhook=sky_site_midhook_default")

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
    game_bind(&h, &t);
    game_fn_table_p = (const FnEntry (*)[])t.table;
    game_fn_table_count_p = t.table_count;
    game_dyn_syms_p = (const DynSym (*)[])t.syms;
    game_dyn_sym_count_p = t.sym_count;
}

/* Linked into ssbu_game.dll together with the lifted game code (obj\*.obj).
 *
 * The lifted code is 700+ MB of objects that change only when the lifter changes; keeping it in a
 * DLL means the host runtime (the part edited all day) links in a second or two. The DLL cannot
 * import from the exe, so the six host functions the lifted code calls are forwarded through
 * pointers the exe hands over once at start-up (gamedll.c). */
#include "runtime.h"

typedef struct {
    void (*call_indirect)(Ctx *, uint64_t);
    void (*host_import)(Ctx *, int);
    void (*guest_trap)(const char *, uint64_t);
    void (*guest_unimpl)(uint64_t, uint32_t, const char *);
    void (*guest_svc)(Ctx *, uint32_t);
    void (*stx_diag)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
    /* appended for a DLL re-lifted from a site list (tools/lifter/relift_sites.py): callouts compiled into re-lifted
     * functions at the addresses of plugin inline hooks / mid-function hooks. An older exe hands over a struct without
     * them; only a DLL built from a site list calls them. */
    void (*site_inline)(Ctx *, uint64_t);
    int (*site_midhook)(Ctx *, uint64_t);
} GameHost;

typedef struct {
    const FnEntry *fn_table;
    const int *fn_table_count;
    const DynSym *dyn_syms;
    const int *dyn_sym_count;
} GameTables;

static GameHost host;

void call_indirect(Ctx *c, uint64_t addr) { host.call_indirect(c, addr); }
void host_import(Ctx *c, int sym) { host.host_import(c, sym); }
void guest_trap(const char *what, uint64_t addr) { host.guest_trap(what, addr); }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { host.guest_unimpl(pc, insn, what); }
void guest_svc(Ctx *c, uint32_t number) { host.guest_svc(c, number); }
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) { host.stx_diag(c, a, expected, seen, v, bits); }
/* Site callouts (relift_sites.py). sky_site_inline: before the instruction at `addr`; the callback may read and change
 * x0..x30 and v0..v31 (they are all in c here: the lifted code keeps registers in the context). sky_site_midhook: a
 * function hook written at a non-entry address; 1 = the replacement has run (as a tail call), the lifted function
 * returns at once with the current registers. */
void sky_site_inline(Ctx *c, uint64_t addr) { host.site_inline(c, addr); }
int sky_site_midhook(Ctx *c, uint64_t addr) { return host.site_midhook(c, addr); }

__declspec(dllexport) void game_bind(const GameHost *h, GameTables *t) {
    host = *h;
    t->fn_table = fn_table;
    t->fn_table_count = &fn_table_count;
    t->dyn_syms = dyn_syms;
    t->dyn_sym_count = &dyn_sym_count;
}

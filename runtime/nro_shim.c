/* Linked into nro_bin\<module>.dll together with the lifted code of one NRO (obj_nro\<module>\*.obj);
 * compiled with -DNRO_MODULE=<module>. Same idea as game_shim.c: the DLL cannot import from the exe, so
 * the host functions the lifted code calls are forwarded through pointers nnro.c hands over when it
 * loads the DLL. `nro_import` is what the module's PLT stubs call (see ro_study\recomp_nro.py). */
#include "runtime.h"

typedef struct {
    void (*call_indirect)(Ctx *, uint64_t);
    void (*host_import)(Ctx *, int);
    void (*guest_trap)(const char *, uint64_t);
    void (*guest_unimpl)(uint64_t, uint32_t, const char *);
    void (*guest_svc)(Ctx *, uint32_t);
    void (*stx_diag)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
    void (*nro_import)(Ctx *, void *, int);
    void (*catch_call)(Ctx *, void (*)(Ctx *, uint64_t));      /* nnunwind.c; absent in hosts older than the exception support */
    /* appended: the site callouts of a module re-lifted from a site list (tools/lifter/relift_sites.py --module,
     * nro_bin_sky); only such a DLL calls them, an older host does not have them */
    void (*site_inline)(Ctx *, uint64_t);
    int (*site_midhook)(Ctx *, uint64_t);
} NroHost;
typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } NroResume;
typedef struct { const FnEntry *fns; int fn_count; const DynSym *syms; int sym_count; const NroResume *resume; int resume_count; } NroTables;

#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)

struct NroModule { void *host_module; };
struct NroModule CAT(nromod_, NRO_MODULE);       /* the token the lifted PLT stubs pass to nro_import */
extern const FnEntry CAT(fn_table_, NRO_MODULE)[];
extern const int CAT(fn_table_count_, NRO_MODULE);
extern const DynSym CAT(dyn_syms_, NRO_MODULE)[];
extern const int CAT(dyn_sym_count_, NRO_MODULE);
extern const NroResume CAT(resume_table_, NRO_MODULE)[];       /* landing pads of the module (ro_study\\recomp_nro.py, eh_info) */
extern const int CAT(resume_count_, NRO_MODULE);

static NroHost host;

void call_indirect(Ctx *c, uint64_t addr) { host.call_indirect(c, addr); }
void host_import(Ctx *c, int sym) { host.host_import(c, sym); }
void guest_trap(const char *what, uint64_t addr) { host.guest_trap(what, addr); }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { host.guest_unimpl(pc, insn, what); }
void guest_svc(Ctx *c, uint32_t number) { host.guest_svc(c, number); }
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) { host.stx_diag(c, a, expected, seen, v, bits); }
void nro_import(Ctx *c, struct NroModule *m, int sym) { host.nro_import(c, m->host_module, sym); }
void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t)) { host.catch_call(c, body); }
void sky_site_inline(Ctx *c, uint64_t addr) { host.site_inline(c, addr); }      /* see game_shim.c */
int sky_site_midhook(Ctx *c, uint64_t addr) { return host.site_midhook(c, addr); }

__declspec(dllexport) void nro_bind(const NroHost *h, void *host_module, NroTables *t) {
    host = *h;
    CAT(nromod_, NRO_MODULE).host_module = host_module;
    t->fns = CAT(fn_table_, NRO_MODULE);
    t->fn_count = CAT(fn_table_count_, NRO_MODULE);
    t->syms = CAT(dyn_syms_, NRO_MODULE);
    t->sym_count = CAT(dyn_sym_count_, NRO_MODULE);
    t->resume = CAT(resume_table_, NRO_MODULE);
    t->resume_count = CAT(resume_count_, NRO_MODULE);
}

#ifdef R1_MARK          /* track R1 round 3: the dirty-page maps this module's stores mark (runtime.h R1M); set by the exe (r1_state.c) */
__declspec(dllexport) uint8_t *r1_m1, *r1_m2;
__declspec(dllexport) uint64_t r1_lo1, r1_n1, r1_n2;
#endif

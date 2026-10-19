/* C++ exception unwinding for lifted code (the lua2cpp script modules need it: a script coroutine is ended by
 * throwing lua2cpp::L2CExceptionShutdown from its yield function and catching it in the coroutine's top
 * function - [module lua2cpp_common: throw at +0x52dd20 when the agent's byte +0xc0 is set after a fiber switch,
 * catch in the function at +0x52e2f0]; without unwinding the game stopped at the start of every battle).
 *
 * How it works
 *   - nncxxrt.c (eh_runtime.h) owns the exception objects, the ABI entry points and the personality (LSDA lookup and
 *     type matching); it calls raise() here for every throw and every _Unwind_Resume.
 *   - The guest stack is real: lifted code keeps x29/x30 and the callee-saved registers exactly where the original
 *     code does, and the .eh_frame tables are mapped with the image. raise() walks the GUEST frames with the CFI of
 *     each function (CFA and saved x19..x30, d8..d15) and asks the personality about every call site. A throw first
 *     searches for a handler without touching anything (no handler = std::terminate, as on the console); then, and
 *     on every _Unwind_Resume, it enters the nearest pad.
 *   - A landing pad is code in the middle of a lifted function. The module lifter (ro_study
ecomp_nro.py) gives
 *     every function with landing pads a second entry `u_<fn>(c, landing pad)` and lists them in the module's
 *     resume table; raise() sets the frame's registers (sp, x19..x29, d8..d15 from the CFI; x0/x1 by
 *     eh_enter_pad) and calls it.
 *       clean-up pad: runs destructors and calls _Unwind_Resume -> raise() again, nested on the host stack;
 *       handler pad:  runs the handler and the REST of that function, then returns here; the host stack is then
 *                     cut back (longjmp) to the frame nro_catch_call() made when that function was entered (the
 *                     lifter routes every function whose LSDA has a catch clause through it), so the host caller
 *                     continues as if the function had returned normally. Records are keyed by the frame's CFA.
 *   - Everything lives on the guest stack and on the host stack of the thread or fiber that threw, so fibers need
 *     nothing special. The walk ends where a return address has no unwind tables (thread / fiber entry, or a host
 *     function that called back into the guest): no handler before that point = terminate.
 *   - Frames of the main executable are walked like any other but cannot be resumed: main is lifted without resume
 *     entries. A clean-up pad there is skipped with a warning; a handler there stops with a report.
 * Environment: SSBU_UNWIND_LOG=1 prints every frame of every throw (the first 3 throws are always summarised). */
#include <windows.h>
#include <setjmp.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hostrt.h"
#include "eh_runtime.h"

typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } NroResume;
int nro_unwind_module(uint64_t pc, uint64_t *base, uint64_t *size, const NroResume **tab, int *count, const char **name);   /* nnro.c */
int cxxrt_readable(uint64_t p, size_t n);               /* nncxxrt.c */

static int g_log;
static volatile LONG g_throws, g_cleanups, g_skipped;

/* ---------------------------------------------------------------- DWARF readers */
static uint64_t uleb(const uint8_t **p) { uint64_t r = 0; int s = 0; uint8_t b; do { b = *(*p)++; r |= (uint64_t)(b & 0x7f) << s; s += 7; } while (b & 0x80); return r; }
static int64_t sleb(const uint8_t **p) { int64_t r = 0; int s = 0; uint8_t b; do { b = *(*p)++; r |= (int64_t)(b & 0x7f) << s; s += 7; } while (b & 0x80); if (s < 64 && (b & 0x40)) r |= -((int64_t)1 << s); return r; }

/* DW_EH_PE value; datarel = base for 0x30. Returns 0 for "omitted" and for a null value. */
static uint64_t read_enc(const uint8_t **p, unsigned enc, uint64_t datarel) {
    if (enc == 0xff) return 0;
    const uint8_t *start = *p;
    uint64_t v;
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: memcpy(&v, *p, 8); *p += 8; break;
    case 0x01: v = uleb(p); break;
    case 0x02: { uint16_t t; memcpy(&t, *p, 2); *p += 2; v = t; break; }
    case 0x03: { uint32_t t; memcpy(&t, *p, 4); *p += 4; v = t; break; }
    case 0x09: v = (uint64_t)sleb(p); break;
    case 0x0a: { int16_t t; memcpy(&t, *p, 2); *p += 2; v = (uint64_t)(int64_t)t; break; }
    case 0x0b: { int32_t t; memcpy(&t, *p, 4); *p += 4; v = (uint64_t)(int64_t)t; break; }
    default: return 0;
    }
    if (!v) return 0;
    if ((enc & 0x70) == 0x10) v += (uint64_t)(uintptr_t)start;
    else if ((enc & 0x70) == 0x30) v += datarel;
    if (enc & 0x80) v = cxxrt_readable(v, 8) ? rd64(v) : 0;        /* indirect: the (relocated) slot holds the value */
    return v;
}

/* ---------------------------------------------------------------- frames */
typedef struct { uint64_t x[32]; uint64_t d[8]; uint64_t pc; } Regs;      /* x[19..30], x[31] = sp, d8..d15; pc = return address */
typedef struct { uint64_t start, end, lsda; const uint8_t *insns, *insns_end, *cie_insns, *cie_end; uint64_t code_align; int64_t data_align; unsigned ra; } Fde;

/* CIE and FDE at guest address `at` (EhFunction.fde). */
static int parse_fde(uint64_t at, Fde *f) {
    const uint8_t *fde = (const uint8_t *)(uintptr_t)at;
    uint32_t len, cid; memcpy(&len, fde, 4); memcpy(&cid, fde + 4, 4);
    if (len == 0 || len == 0xffffffffu || cid == 0) return 0;
    const uint8_t *cie = fde + 4 - (int32_t)cid;
    uint32_t clen; memcpy(&clen, cie, 4);
    const uint8_t *p = cie + 9;
    unsigned version = cie[8];
    const char *aug = (const char *)p;
    p += strlen(aug) + 1;
    f->code_align = uleb(&p);
    f->data_align = sleb(&p);
    f->ra = version == 1 ? *p++ : (unsigned)uleb(&p);
    unsigned lsda_enc = 0xff, fde_enc = 0;
    if (aug[0] == 'z') {
        uint64_t alen = uleb(&p);
        const uint8_t *q = p;
        for (const char *a = aug + 1; *a; a++) {
            if (*a == 'L') lsda_enc = *q++;
            else if (*a == 'R') fde_enc = *q++;
            else if (*a == 'P') { unsigned e = *q++; read_enc(&q, e & 0x7f, 0); }
            else if (*a == 'S') { }
            else return 0;
        }
        p += alen;
    } else if (aug[0]) return 0;
    f->cie_insns = p; f->cie_end = cie + 4 + clen;
    p = fde + 8;
    f->start = read_enc(&p, fde_enc, 0);
    f->end = f->start + read_enc(&p, fde_enc & 0x0f, 0);
    f->lsda = 0;
    if (aug[0] == 'z') { uint64_t alen = uleb(&p); const uint8_t *q = p; if (lsda_enc != 0xff) f->lsda = read_enc(&q, lsda_enc, 0); p += alen; }
    f->insns = p; f->insns_end = fde + 4 + len;
    return 1;
}

enum { R_SAME = 0, R_OFFSET, R_UNDEF, R_REG };
typedef struct { int cfa_reg; int64_t cfa_off; uint8_t rule[96]; int64_t val[96]; } Row;

static int run_cfi(const uint8_t *p, const uint8_t *end, const Fde *f, uint64_t *loc, uint64_t target, Row *row, const Row *init) {
    Row stack[8]; int sp = 0;
    while (p < end && *loc <= target) {
        uint8_t op = *p++;
        unsigned hi = op & 0xc0, lowbits = op & 0x3f;
        if (hi == 0x40) { *loc += lowbits * f->code_align; continue; }
        if (hi == 0x80) { uint64_t o = uleb(&p); if (lowbits < 96) { row->rule[lowbits] = R_OFFSET; row->val[lowbits] = (int64_t)o * f->data_align; } continue; }
        if (hi == 0xc0) { if (lowbits < 96 && init) { row->rule[lowbits] = init->rule[lowbits]; row->val[lowbits] = init->val[lowbits]; } continue; }
        uint64_t r, o;
        switch (op) {
        case 0x00: break;
        case 0x02: *loc += *p++ * f->code_align; break;
        case 0x03: { uint16_t d; memcpy(&d, p, 2); p += 2; *loc += d * f->code_align; break; }
        case 0x04: { uint32_t d; memcpy(&d, p, 4); p += 4; *loc += d * f->code_align; break; }
        case 0x05: r = uleb(&p); o = uleb(&p); if (r < 96) { row->rule[r] = R_OFFSET; row->val[r] = (int64_t)o * f->data_align; } break;
        case 0x06: r = uleb(&p); if (r < 96 && init) { row->rule[r] = init->rule[r]; row->val[r] = init->val[r]; } break;
        case 0x07: r = uleb(&p); if (r < 96) row->rule[r] = R_UNDEF; break;
        case 0x08: r = uleb(&p); if (r < 96) row->rule[r] = R_SAME; break;
        case 0x09: r = uleb(&p); o = uleb(&p); if (r < 96) { row->rule[r] = R_REG; row->val[r] = (int64_t)o; } break;
        case 0x0a: if (sp >= 8) return 0; stack[sp++] = *row; break;
        case 0x0b: if (sp <= 0) return 0; *row = stack[--sp]; break;
        case 0x0c: row->cfa_reg = (int)uleb(&p); row->cfa_off = (int64_t)uleb(&p); break;
        case 0x0d: row->cfa_reg = (int)uleb(&p); break;
        case 0x0e: row->cfa_off = (int64_t)uleb(&p); break;
        case 0x11: r = uleb(&p); { int64_t so = sleb(&p); if (r < 96) { row->rule[r] = R_OFFSET; row->val[r] = so * f->data_align; } } break;
        case 0x12: row->cfa_reg = (int)uleb(&p); row->cfa_off = sleb(&p) * f->data_align; break;
        case 0x13: row->cfa_off = sleb(&p) * f->data_align; break;
        case 0x2d: break;                                 /* AArch64 negate_ra_state: no pointer authentication here */
        case 0x2e: uleb(&p); break;                       /* GNU_args_size */
        default: return 0;                                /* expressions and anything else: not used by this compiler's output */
        }
    }
    return 1;
}

/* Registers of the caller of the frame `r` (pc inside FDE f). Also returns this frame's CFA. */
static int step(const Fde *f, Regs *r, uint64_t *cfa_out) {
    Row init, row;
    memset(&init, 0, sizeof init);
    init.cfa_reg = 31;
    uint64_t loc = f->start;
    uint64_t no_limit = ~(uint64_t)0 - 1;
    if (!run_cfi(f->cie_insns, f->cie_end, f, &loc, no_limit, &init, NULL)) return 0;
    row = init;
    loc = f->start;
    if (!run_cfi(f->insns, f->insns_end, f, &loc, r->pc - 1, &row, &init)) return 0;
    if (row.cfa_reg < 19 || row.cfa_reg > 31) return 0;
    uint64_t cfa = r->x[row.cfa_reg] + (uint64_t)row.cfa_off;
    Regs n = *r;
    for (int i = 19; i <= 30; i++) {
        if (row.rule[i] == R_OFFSET) { if (!cxxrt_readable(cfa + (uint64_t)row.val[i], 8)) return 0; n.x[i] = rd64(cfa + (uint64_t)row.val[i]); }
        else if (row.rule[i] == R_REG && row.val[i] >= 19 && row.val[i] <= 30) n.x[i] = r->x[row.val[i]];
    }
    for (int i = 0; i < 8; i++) if (row.rule[72 + i] == R_OFFSET) n.d[i] = rd64(cfa + (uint64_t)row.val[72 + i]);
    n.x[31] = cfa;
    n.pc = n.x[f->ra < 32 ? f->ra : 30];
    *cfa_out = cfa;
    *r = n;
    return 1;
}

/* ---------------------------------------------------------------- host frames to come back to */
typedef struct { uint64_t cfa; jmp_buf *jb; } CatchSlot;
#define CATCH_SLOTS 8192
static CatchSlot g_catch[CATCH_SLOTS];
static SRWLOCK g_catch_lock = SRWLOCK_INIT;

static void catch_put(uint64_t cfa, jmp_buf *jb) {
    unsigned h = (unsigned)((cfa >> 4) * 2654435761u) & (CATCH_SLOTS - 1), first_free = CATCH_SLOTS;
    AcquireSRWLockExclusive(&g_catch_lock);
    for (unsigned n = 0; n < 64; n++) {
        unsigned i = (h + n) & (CATCH_SLOTS - 1);
        if (g_catch[i].cfa == cfa) { first_free = i; break; }                 /* a frame at the same guest stack address replaces a stale one */
        if (g_catch[i].cfa <= 1 && first_free == CATCH_SLOTS) first_free = i;
        if (g_catch[i].cfa == 0) break;
    }
    if (first_free == CATCH_SLOTS) first_free = h;                            /* crowded by records of abandoned stacks: reuse */
    g_catch[first_free].cfa = cfa; g_catch[first_free].jb = jb;
    ReleaseSRWLockExclusive(&g_catch_lock);
}
static jmp_buf *catch_take(uint64_t cfa, jmp_buf *only) {                     /* removes the record; only != NULL: remove if it is that one */
    unsigned h = (unsigned)((cfa >> 4) * 2654435761u) & (CATCH_SLOTS - 1);
    jmp_buf *jb = NULL;
    AcquireSRWLockExclusive(&g_catch_lock);
    for (unsigned n = 0; n < 64; n++) {
        unsigned i = (h + n) & (CATCH_SLOTS - 1);
        if (g_catch[i].cfa == cfa) { if (!only || g_catch[i].jb == only) { jb = g_catch[i].jb; g_catch[i].cfa = 1; g_catch[i].jb = NULL; } break; }
        if (g_catch[i].cfa == 0) break;
    }
    ReleaseSRWLockExclusive(&g_catch_lock);
    return jb;
}

/* A record's host frame must still be on this stack (a fiber's stack is the TEB's while it runs) and above the current
 * frame: a longjmp to a frame that is gone makes Windows end the process at once, without any report (one candidate for
 * play sessions that ended silently); this turns that case into a report. */
static void catch_check(Ctx *c, jmp_buf *jb, uint64_t cfa) {
    _JUMP_BUFFER *b = (_JUMP_BUFFER *)jb;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    uint64_t now = (uint64_t)(uintptr_t)_AddressOfReturnAddress();
    if (b->Rsp > now && b->Rsp < (uint64_t)(uintptr_t)tib->StackBase) return;
    printf("\nunwind: the host frame recorded for guest frame %llx is not on this stack (its rsp %llx, this stack %p..%p, now %llx)\n",
           (unsigned long long)cfa, (unsigned long long)b->Rsp, tib->StackLimit, tib->StackBase, (unsigned long long)now);
    eh_terminate(c, "stale host frame record");
}

/* Entry of every lifted function whose LSDA has a catch clause (generated code: `nro_catch_call(c, u_<fn>)`). */
void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t)) {
    Ctx *volatile cc = c;
    jmp_buf jb;
    volatile uint64_t cfa = c->x[31];                     /* the CFA of an AArch64 frame is the stack pointer at entry */
    const char *volatile in_import = c->in_import;
    volatile uint64_t import_from = c->import_from;
    catch_put(cfa, &jb);
    if (setjmp(jb) == 0) {
        body(c, 0);
        catch_take(cfa, &jb);
    } else {                                              /* a handler of this function ran it to its end (the unwinder removed the record) */
        cc->in_import = in_import;
        cc->import_from = import_from;
    }
}

/* ---------------------------------------------------------------- smashline's ACMD interpreter (skyline.c)
 * smashline (a Skyline plugin) runs every ACMD script through an asm stub, __smashline_interpreter, that records
 * (landing pad, sp) for the current fiber and calls the script, often Rust code without unwind tables. On the console it
 * hooks libunwind's step: while that record exists, the first frame stepped gets the stub's landing pad and a personality
 * that says "handler here", so a script ended by L2CExceptionShutdown resumes in the stub, which takes its saved sp back
 * and returns normally to the game. Here: when the stub is on the throwing stack (its call's return address in a frame
 * record, or failing that in a stack slot), raise() enters the landing pad's resume entry; the stub then returns, and the
 * host stack is cut back to the record unwind_interp_call() made at the stub's entry (installed with the hook table). */
static uint64_t g_interp_ret, g_interp_lp;
/* SSBU_EH_TIMING=1 (test): where a throw that smashline's stub takes spends its time. Stages: 0 interp_on_stack, 1 the
 * stub's landing code, 2 catch_take + checks, 3 longjmp back to unwind_interp_call (the host unwind); plus the host stack
 * the longjmp cut. Slow throws (> 2 ms, first 80) get a line; every 500 throws a summary. */
static int g_ehtm = -1;
static __declspec(thread) LONG64 t_eh_t[5];
static __declspec(thread) uint64_t t_eh_rsp;
static LONG64 g_ehtm_sum[4], g_ehtm_max[4], g_ehtm_n, g_ehtm_slow;
static double g_ehtm_f;
static LONG64 ehtm_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static int ehtm_on(void) { if (g_ehtm < 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ehtm_f = 1000.0 / (double)f.QuadPart; g_ehtm = getenv("SSBU_EH_TIMING") && atoi(getenv("SSBU_EH_TIMING")); } return g_ehtm; }
static void ehtm_landed(uint64_t rsp_now) {
    t_eh_t[4] = ehtm_now();
    LONG64 d[4]; for (int i = 0; i < 4; i++) d[i] = t_eh_t[i + 1] - t_eh_t[i];
    LONG64 n = InterlockedIncrement64(&g_ehtm_n);
    for (int i = 0; i < 4; i++) { InterlockedAdd64(&g_ehtm_sum[i], d[i]); if (d[i] > g_ehtm_max[i]) g_ehtm_max[i] = d[i]; }
    double tot = (double)(t_eh_t[4] - t_eh_t[0]) * g_ehtm_f;
    if (tot > 2.0 && InterlockedIncrement64(&g_ehtm_slow) <= 80)
        printf("eh-timing: slow throw %.2f ms: stub search %.2f, landing code %.2f, record %.2f, longjmp %.2f ms; host stack cut %llu KB\n", tot,
               d[0] * g_ehtm_f, d[1] * g_ehtm_f, d[2] * g_ehtm_f, d[3] * g_ehtm_f, (unsigned long long)((rsp_now - t_eh_rsp) >> 10));
    if (n % 500 == 0)
        printf("eh-timing: %lld stub throws: average ms search %.3f landing %.3f record %.3f longjmp %.3f | worst %.2f %.2f %.2f %.2f | slow (> 2 ms) %lld\n", (long long)n,
               g_ehtm_sum[0] * g_ehtm_f / n, g_ehtm_sum[1] * g_ehtm_f / n, g_ehtm_sum[2] * g_ehtm_f / n, g_ehtm_sum[3] * g_ehtm_f / n,
               g_ehtm_max[0] * g_ehtm_f, g_ehtm_max[1] * g_ehtm_f, g_ehtm_max[2] * g_ehtm_f, g_ehtm_max[3] * g_ehtm_f, (long long)g_ehtm_slow);
}
static volatile LONG g_interp_landings;
void unwind_set_interpreter(uint64_t ret_addr, uint64_t resume_lp) { g_interp_ret = ret_addr; g_interp_lp = resume_lp; }
/* The interpreter calls in progress on each guest thread / fiber: a list through records on the host stack of
 * unwind_interp_call, its head in fiber-local storage (every guest fiber is a Windows fiber, os_threads.c; a fiber that
 * is dropped takes its list with it). interp_on_stack used to scan the whole throwing stack (up to 2 MB, one word at a
 * time) for the stub's return address, after a VirtualQuery of the stack: the guest stacks of fibers lie in the game's
 * heap, one committed region of gigabytes, which VirtualQuery walks page by page. Every L2CExceptionShutdown of HDR's
 * scripts paid 1-22 ms for it on the main thread (frames of 10-37 ms in a 120 Hz fight, up to 2.5 s of them in two
 * minutes). Now the innermost record of this fiber is the interpreter call the throw is inside of; its guest side is
 * confirmed by the stub's return address just below the stub's sp (a few hundred bytes), else the old scan decides. */
typedef struct IRec { struct IRec *prev; uint64_t cfa; } IRec;
static DWORD g_irec_fls = FLS_OUT_OF_INDEXES;
static volatile LONG g_irec_init;
static DWORD irec_slot(void) {
    if (g_irec_init != 2) {
        if (InterlockedCompareExchange(&g_irec_init, 1, 0) == 0) { g_irec_fls = FlsAlloc(NULL); InterlockedExchange(&g_irec_init, 2); }
        else while (g_irec_init != 2) SwitchToThread();
    }
    return g_irec_fls;
}
void unwind_interp_call(Ctx *c, void (*orig)(Ctx *)) {
    Ctx *volatile cc = c;
    jmp_buf jb;
    volatile uint64_t cfa = c->x[31];
    const char *volatile in_import = c->in_import;
    volatile uint64_t import_from = c->import_from;
    volatile DWORD fls = irec_slot();
    IRec rec = {fls != FLS_OUT_OF_INDEXES ? (IRec *)FlsGetValue(fls) : NULL, cfa};
    IRec *volatile prev = rec.prev;                       /* read again after a longjmp: kept in memory */
    if (fls != FLS_OUT_OF_INDEXES) FlsSetValue(fls, &rec);
    catch_put(cfa, &jb);
    if (setjmp(jb) == 0) {
        orig(c);
        catch_take(cfa, &jb);
    } else {
        if (g_ehtm > 0 && t_eh_t[0]) { ehtm_landed((uint64_t)(uintptr_t)_AddressOfReturnAddress()); t_eh_t[0] = 0; }
        cc->in_import = in_import;
        cc->import_from = import_from;
    }
    if (fls != FLS_OUT_OF_INDEXES) FlsSetValue(fls, prev);
}
static LONG64 g_iact_fast, g_iact_scan, g_iact_stale, g_iact_none;
static int interp_on_stack_scan(Ctx *c) {                /* the old way: when the record is not confirmed */
    MEMORY_BASIC_INFORMATION mi;
    uint64_t sp = c->x[31];
    if (!VirtualQuery((void *)(uintptr_t)sp, &mi, sizeof mi)) return 0;
    uint64_t top = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
    if (top - sp > 0x200000) top = sp + 0x200000;
    for (uint64_t fp = c->x[29], n = 0; fp >= sp && fp + 16 <= top && !(fp & 7) && n < 4096; n++) {      /* frame records */
        if (rd64(fp + 8) == g_interp_ret) return 1;
        uint64_t next = rd64(fp);
        if (next <= fp) break;
        fp = next;
    }
    for (uint64_t a = sp & ~7ull; a + 8 <= top; a += 8) if (rd64(a) == g_interp_ret) return 1;      /* a frame without a record */
    return 0;
}
static int interp_on_stack(Ctx *c) {
    DWORD fls = irec_slot();
    if (fls == FLS_OUT_OF_INDEXES) { InterlockedIncrement64(&g_iact_scan); return interp_on_stack_scan(c); }
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    uint64_t now = (uint64_t)(uintptr_t)_AddressOfReturnAddress(), base = (uint64_t)(uintptr_t)tib->StackBase, sp = c->x[31];
    IRec *r = (IRec *)FlsGetValue(fls);
    if (!r) {
        LONG64 k = InterlockedIncrement64(&g_iact_none);
        if (g_ehtm > 0) {                             /* SSBU_EH_TIMING (test): would the old scan have said otherwise? */
            static LONG64 differ;
            if (interp_on_stack_scan(c) && InterlockedIncrement64(&differ) <= 10) printf("eh-timing: no interpreter call recorded on this fiber, but the old scan finds the stub's return address (guest sp %llx)\n", (unsigned long long)sp);
            if (k % 100 == 0) printf("eh-timing: interpreter check: %lld outside (old scan disagrees %lld), %lld inside (confirmed), %lld unconfirmed\n",
                                     (long long)k, (long long)differ, (long long)g_iact_fast, (long long)g_iact_stale);
        }
        return 0;
    }
    if ((uint64_t)(uintptr_t)r > now && (uint64_t)(uintptr_t)r < base && r->cfa > sp && r->cfa - sp <= 0x200000) {
        uint64_t cfa = r->cfa, lo = cfa - sp > 0x10000 ? cfa - 0x10000 : sp;      /* the stub's return address in the frame it called, just below its sp */
        for (uint64_t a = (cfa - 8) & ~7ull; a >= lo && a >= sp; a -= 8)
            if (rd64(a) == g_interp_ret) {
                LONG64 k = InterlockedIncrement64(&g_iact_fast);
                if (g_ehtm > 0 && k % 500 == 0) printf("eh-timing: interpreter check: %lld inside (confirmed), %lld outside, %lld unconfirmed\n", (long long)k, (long long)g_iact_none, (long long)g_iact_stale);
                return 1;
            }
    }
    InterlockedIncrement64(&g_iact_stale);            /* not confirmed (a record left behind by a longjmp?): the old scan decides */
    return interp_on_stack_scan(c);
}

/* ---------------------------------------------------------------- the unwinder (eh_runtime.h: EhRaiseFn) */
static void (*resume_entry(uint64_t lp, const char **name))(Ctx *, uint64_t) {
    uint64_t base, size; const NroResume *tab = NULL; int count = 0;
    *name = "main";
    if (!nro_unwind_module(lp, &base, &size, &tab, &count, name)) return eh_main_resume_entry(lp);     /* main: NULL unless it was lifted with resume entries (ssbu_game_eh.dll) */
    if (!tab) return NULL;
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tab[mid].lp == lp) return tab[mid].fn;
        if (tab[mid].lp < lp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static void regs_from(Ctx *c, Regs *r) {
    memset(r, 0, sizeof *r);
    for (int i = 19; i <= 31; i++) r->x[i] = c->x[i];
    for (int i = 0; i < 8; i++) r->d[i] = c->v[8 + i].d[0];
    r->pc = c->x[30];                                     /* return address of the __cxa_throw / _Unwind_Resume call */
}

/* One frame: what its call site does with the exception, then the caller's registers. Returns 0 at the end of the
 * chain (no unwind information for the return address: a thread or fiber entry, or a host callback boundary). */
static int frame(Ctx *c, uint64_t exc, Regs *cur, EhPad *pad, uint64_t *cfa, const char **stop) {
    EhFunction fn;
    Fde f;
    if (!cur->pc || !eh_find_function(cur->pc - 4, &fn)) { *stop = "the call chain leaves the code with unwind tables (thread, fiber or host callback entry)"; return 0; }
    if (!parse_fde(fn.fde, &f)) { *stop = "FDE not understood"; return 0; }
    eh_personality(c, cur->pc - 4, exc, pad);
    pad->function = fn;
    uint64_t pc = cur->pc;
    if (!step(&f, cur, cfa)) { *stop = "CFI of a frame not understood"; return 0; }
    if (g_log) printf("unwind:   f_%llx call at %llx cfa %llx: %s\n", (unsigned long long)fn.start, (unsigned long long)pc - 4, (unsigned long long)*cfa,
                      pad->kind == EH_HANDLER ? "HANDLER" : pad->kind == EH_CLEANUP ? "cleanup" : "-");
    return 1;
}

static void eh_raise(Ctx *c, int reason) {
    uint64_t exc = eh_in_flight(c);
    Regs cur;
    EhPad pad;
    uint64_t cfa = 0;
    const char *stop = "frame limit";
    const char *owner = "?";
    if (ehtm_on()) { t_eh_t[0] = ehtm_now(); t_eh_rsp = (uint64_t)(uintptr_t)_AddressOfReturnAddress(); }
    if (g_interp_ret && interp_on_stack(c)) {            /* smashline: the interpreter stub catches it (see above) */
        if (g_ehtm > 0) t_eh_t[1] = ehtm_now();
        void (*fn)(Ctx *, uint64_t) = resume_entry(g_interp_lp, &owner);
        if (fn) {
            EhPad ip;
            memset(&ip, 0, sizeof ip);
            ip.kind = EH_HANDLER; ip.landing_pad = g_interp_lp; ip.exception = exc;
            if (InterlockedIncrement(&g_interp_landings) <= 3 || g_log)
                printf("unwind: %s thrown at 0x%llx: smashline's interpreter stub takes it (landing pad %llx)\n", eh_type_name(exc), (unsigned long long)c->x[30] - 4, (unsigned long long)g_interp_lp);
            int uncaught_before = eh_uncaught_count(c);
            eh_enter_pad(c, &ip);
            c->in_import = NULL;
            fn(c, g_interp_lp);                           /* the stub's landing code: its saved sp back, then it returns */
            if (g_ehtm > 0) t_eh_t[2] = ehtm_now();
            eh_stub_done(c, exc, uncaught_before);        /* nncxxrt.c: the exception is over for this context */
            jmp_buf *jb = catch_take(c->x[31], NULL);
            if (!jb) {
                printf("\nunwind: smashline's interpreter stub returned to sp %llx, where no entry record is\n", (unsigned long long)c->x[31]);
                eh_terminate(c, "no host frame to return to");
            }
            catch_check(c, jb, c->x[31]);
            if (g_ehtm > 0) t_eh_t[3] = ehtm_now();
            longjmp(*jb, 1);
        }
    }
    if (reason == EH_RAISE_THROW) {                       /* phase 1: is there a handler at all? (the console terminates without unwinding if not) */
        LONG seq = InterlockedIncrement(&g_throws);
        int found = 0, depth = 0;
        regs_from(c, &cur);
        for (; depth < 4096; depth++) {
            if (!frame(c, exc, &cur, &pad, &cfa, &stop)) break;
            if (pad.kind == EH_HANDLER) { found = 1; break; }
        }
        if (found && !resume_entry(pad.landing_pad, &owner)) {
            printf("\nunwind: %s thrown at 0x%llx has its handler at %llx in %s, which has no resume entry (main is lifted without them; a module needs `python build.py nro <module> --relift`)\n",
                   eh_type_name(exc), (unsigned long long)c->x[30] - 4, (unsigned long long)pad.landing_pad, owner);
            eh_terminate(c, "the handler cannot be entered");
        }
        if (!found) {
            printf("\nunwind: %s thrown at 0x%llx: no handler in %d frames (%s)\n", eh_type_name(exc), (unsigned long long)c->x[30] - 4, depth, stop);
            eh_terminate(c, "no handler");
        }
        if (g_log || seq <= 3)
            printf("unwind: throw #%ld of %s at 0x%llx: handler in %s f_%llx (landing pad %llx), %d frames up\n", (long)seq, eh_type_name(exc),
                   (unsigned long long)c->x[30] - 4, owner, (unsigned long long)pad.function.start, (unsigned long long)pad.landing_pad, depth);
    }
    /* phase 2 (and every _Unwind_Resume): the nearest pad from here */
    regs_from(c, &cur);
    for (int depth = 0; depth < 4096; depth++) {
        Regs at = cur;                                    /* registers of this frame at its call */
        if (!frame(c, exc, &cur, &pad, &cfa, &stop)) break;
        if (pad.kind == EH_NONE) continue;
        void (*fn)(Ctx *, uint64_t) = resume_entry(pad.landing_pad, &owner);
        if (!fn) {
            if (pad.kind == EH_HANDLER) eh_terminate(c, "the handler is in code without resume entries");
            if (InterlockedIncrement(&g_skipped) <= 8) printf("unwind: clean-up pad %llx in %s skipped (no resume entry)\n", (unsigned long long)pad.landing_pad, owner);
            continue;
        }
        for (int i = 19; i <= 29; i++) c->x[i] = at.x[i];
        c->x[31] = at.x[31];
        for (int i = 0; i < 8; i++) { c->v[8 + i].d[0] = at.d[i]; c->v[8 + i].d[1] = 0; }
        eh_enter_pad(c, &pad);                            /* x0, x1; the exception is no longer "in flight" */
        c->in_import = NULL;
        if (pad.kind == EH_CLEANUP) InterlockedIncrement(&g_cleanups);
        fn(c, pad.landing_pad);
        /* A clean-up pad does not come back (it ends in _Unwind_Resume, which re-enters raise()). Coming back means a
         * handler ran and the function that contains it returned to ITS caller: cut the host stack back to the frame
         * nro_catch_call() made when that function was entered. */
        jmp_buf *jb = catch_take(cfa, NULL);
        if (!jb) {
            printf("\nunwind: f_%llx handled an exception but was not entered through nro_catch_call (frame %llx)\n", (unsigned long long)pad.function.start, (unsigned long long)cfa);
            eh_terminate(c, "no host frame to return to");
        }
        catch_check(c, jb, cfa);
        longjmp(*jb, 1);
    }
    printf("\nunwind: ran out of frames while unwinding (%s)\n", stop);
    eh_terminate(c, "unwinding ran past the last frame");
}

void unwind_report(FILE *out) {
    if (g_throws) fprintf(out, "unwind: %ld exceptions thrown, %ld clean-up pads run, %ld skipped\n", (long)g_throws, (long)g_cleanups, (long)g_skipped);
    if (g_interp_landings) fprintf(out, "unwind: %ld taken by smashline's interpreter stub\n", (long)g_interp_landings);
}

void register_unwind_imports(void) {
    g_log = getenv("SSBU_UNWIND_LOG") != NULL;
    eh_set_unwinder(eh_raise);
}

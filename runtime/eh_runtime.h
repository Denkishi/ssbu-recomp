/* C++ exception RUNTIME of the port (implemented in nncxxrt.c). STABLE INTERFACE for the unwinder (nnunwind.c).
 *
 * Division of work
 *   runtime (this header): exception objects, the per-context throw state, the Itanium ABI entry points the guest
 *       calls (__cxa_throw, __cxa_rethrow, _Unwind_Resume, __cxa_begin_catch, __cxa_end_catch, ...), the table of
 *       loaded images with their .eh_frame_hdr, and the PERSONALITY as a query: "for this call-site pc and this
 *       exception, is there a landing pad, which selector, clean-up or handler?".
 *   unwinder (not here): finds the guest frames, restores the guest registers of the frame that owns the pad and
 *       transfers control to the pad (resume entries of the lifted code).
 *
 * Flow
 *   1. The guest calls __cxa_throw / __cxa_rethrow / std::rethrow_exception / a libc++ __throw_* helper. The runtime
 *      fills the exception header, makes it the exception IN FLIGHT of that Ctx and calls the registered raise
 *      function: raise(c, EH_RAISE_THROW).
 *   2. The unwinder walks the guest frames from the innermost one. For each frame with return address `ret` it asks
 *          eh_personality(c, ret - 4, eh_in_flight(c), &pad)
 *      (any pc inside the call instruction works). EH_NONE: go to the caller's frame. EH_CLEANUP / EH_HANDLER:
 *      restore that frame's registers (sp, x19..x29, d8..d15: the callee-saved state at the call), then
 *          eh_enter_pad(c, &pad)          sets c->x[0] = unwind header, c->x[1] = selector, clears "in flight"
 *      and continue execution at pad.landing_pad.
 *      eh_personality is a pure query, so a search phase (is there an EH_HANDLER anywhere up the stack?) can be run
 *      before any clean-up pad is entered, as the console's two-phase unwinder does; if there is none the console
 *      calls std::terminate WITHOUT unwinding: call eh_terminate(c, "no handler").
 *   3. A clean-up pad ends with `bl _Unwind_Resume` (x0 = the unwind header it was entered with). The runtime puts
 *      the exception in flight again and calls raise(c, EH_RAISE_RESUME): continue the walk from the frame that
 *      called _Unwind_Resume (its return address is c->x[30]; that call site may itself have a pad).
 *   4. A handler pad calls __cxa_begin_catch(x0) (returns the adjusted object pointer) and later __cxa_end_catch,
 *      or __cxa_rethrow (step 1 again). Pads of noexcept functions call __clang_call_terminate -> std::terminate,
 *      which reports and stops the process.
 *   raise() is expected not to return (it transfers control to a pad). If it returns and the exception is still in
 *   flight, or if no raise function is registered, the runtime calls eh_terminate (report: type, what(), guest call
 *   stack; exit code 3).
 *
 * Register convention at pad entry [sdk: __cxa_begin_catch 0x4c9fb8, __cxa_get_exception_ptr 0x4c9fb0,
 * __cxa_throw 0x4c9ea0]: x0 = _Unwind_Exception * = thrown object - 0x20, x1 = selector (int): 0 for a clean-up,
 * > 0 the type filter of the matching catch clause, < 0 a violated exception specification. Nothing else is set.
 * The header in front of the thrown object has libc++abi's layout (0x80 bytes: reference count at -0x80, type at
 * -0x78, destructor at -0x70, handler count at -0x50, adjusted pointer at -0x30, unwind header at -0x20).
 *
 * State is kept PER Ctx (not per host thread): every guest thread and every guest fiber has its own Ctx
 * (os_threads.c new_ctx), and fibers of one host thread share its thread-local storage. All functions may be called
 * from any host thread; a given Ctx must only be used by the thread currently running it. */
#ifndef RECOMP_EH_RUNTIME_H
#define RECOMP_EH_RUNTIME_H

#include "runtime.h"

/* ---- loaded images
 * base: load address; size: image size in bytes; eh_frame_hdr: guest address of .eh_frame_hdr, or 0 to take it from
 * the image's MOD0 header (u32 at base+4 = offset of "MOD0"; its field at +0x10 is the offset of .eh_frame_hdr
 * relative to MOD0). Call after the image is mapped AND relocated (type tables hold relocated pointers).
 * main is registered by the runtime itself at start-up. Returns 0 if the image has no usable table. */
int eh_register_image(uint64_t base, uint64_t size, uint64_t eh_frame_hdr);
void eh_unregister_image(uint64_t base);

/* Function containing pc according to .eh_frame; lsda is 0 when the function has no exception table.
 * Returns 0 when pc is in no registered image or no FDE covers it. */
typedef struct {
    uint64_t image_base;
    uint64_t start, end;             /* guest addresses of the function ([start, end)) */
    uint64_t lsda;                   /* guest address of its LSDA, or 0 */
    uint64_t fde;                    /* guest address of its FDE (CFI for those who want to unwind registers) */
} EhFunction;
int eh_find_function(uint64_t pc, EhFunction *out);

/* ---- personality */
enum { EH_NONE = 0, EH_CLEANUP = 1, EH_HANDLER = 2 };
typedef struct {
    int kind;                        /* EH_NONE: no pad for this call site and this exception */
    uint64_t landing_pad;            /* guest address to continue at */
    int64_t selector;                /* value for x1 */
    uint64_t exception;              /* the exception the answer is for (thrown object pointer) */
    uint64_t adjusted;               /* what __cxa_begin_catch will return if this handler is entered */
    EhFunction function;
} EhPad;
/* call_pc: an address inside the call instruction of the frame (return address - 4).
 * exception: the thrown object pointer (eh_in_flight(c)). Returns out->kind. Changes no state. */
int eh_personality(Ctx *c, uint64_t call_pc, uint64_t exception, EhPad *out);

/* ---- throw state of a context */
uint64_t eh_in_flight(Ctx *c);       /* thrown object pointer of the exception being propagated, or 0 */
const char *eh_type_name(uint64_t exception);        /* mangled type name, e.g. "St12out_of_range" */
/* Commit to a pad returned by eh_personality: sets x0 and x1, records the adjusted pointer for __cxa_begin_catch,
 * clears the in-flight state. The caller then restores the frame's registers and jumps to pad->landing_pad. */
void eh_enter_pad(Ctx *c, const EhPad *pad);
/* smashline's interpreter stub took the exception (nnunwind.c): its context no longer has one. */
int eh_uncaught_count(Ctx *c);
void eh_stub_done(Ctx *c, uint64_t exc, int uncaught_before);

/* ---- the unwinder's entry point */
enum { EH_RAISE_THROW = 0, EH_RAISE_RESUME = 1 };
typedef void (*EhRaiseFn)(Ctx *c, int reason);
void eh_set_unwinder(EhRaiseFn raise);

/* std::terminate as the console would end here: prints `why`, the type and what() of the exception in flight (or
 * being handled), the guest call stack, and exits the process with code 3. Never returns. */
void eh_terminate(Ctx *c, const char *why);

/* ---- resume entries of MAIN (added after the first version of this header; everything above is unchanged)
 * When the game DLL was generated with landing-pad resume entries (`RECOMP_EH=1 python recomp.py full gen_eh`, same
 * scheme as the modules: `u_<fn>(c, landing pad)`, and `nro_catch_call(c, u_<fn>)` for functions with a real catch
 * handler), this returns the entry to call for a landing pad of main, exactly like a module's resume table does;
 * NULL when the pad is not in main or the DLL in use has no resume entries (the default build today). */
typedef void (*EhResumeFn)(Ctx *c, uint64_t landing_pad);
EhResumeFn eh_main_resume_entry(uint64_t landing_pad);

/* ---- for host code that throws on behalf of the library (libc++ helpers and the like) */
uint64_t eh_allocate(size_t size);                                   /* = __cxa_allocate_exception */
void eh_throw(Ctx *c, uint64_t exception, uint64_t type_info, uint64_t destructor);   /* = __cxa_throw */

#endif

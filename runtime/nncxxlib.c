/* libc++ (std::__1) pieces beyond libcxx.c / nniostream.c: futures, locale facets, random_device, stream vtables.
 * Layouts marked [sdk] were read from the real SDK module (python sdkdis.py / sdkdata.py <symbol>).
 *
 * std::__assoc_sub_state [sdk: promise<void>::promise() allocates 0x48 bytes and initialises them; set_value,
 * __sub_wait, ~promise read the fields]:
 *   +0x00 vptr (vtable: ~D1, ~D0, __on_zero_shared, __execute)      +0x08 long shared_owners (0 = one owner)
 *   +0x10 exception_ptr                                            +0x18 mutex (0x20 bytes)
 *   +0x38 condition_variable (8 bytes)                             +0x40 unsigned state
 *   state bits: 1 = value constructed, 2 = future attached, 4 = ready, 8 = deferred
 * promise<void> and future<void> are one pointer to that state. The mutex and condition variable are driven through
 * the registered std::mutex / std::condition_variable handlers (os_threads.c), exactly like the game's inlined code.
 *
 * std::random_device [sdk]: on this platform it is NOT a hardware source: the constructor seeds a mt19937 with the
 * default seed 5489 inside the object (624 words, index at +0x9c0) and operator() is the generator's next output, so
 * every random_device yields the same sequence 3499211612, 581869302, ... The game sees the same here.
 *
 * Locale facets [sdk]: ctype<char> = { vptr, refs, const unsigned short *table, bool del }, classic table copied from
 * the SDK (mask bits: 0x1 blank, 0x2 cntrl, 0x4 punct, 0x100 upper, 0x200 lower, 0x400 alpha, 0x800 digit,
 * 0x1000 xdigit, 0x2000 space, 0x4000 print); numpunct<char> = { vptr, refs, char '.', char ',', string grouping "" }.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P64(p) ((uint64_t)(uintptr_t)(p))

/* nncxxrt.c */
uint64_t cxxrt_call(Ctx *c, uint64_t addr, uint64_t a0, uint64_t a1, uint64_t a2);
void cxxrt_guest_delete(Ctx *c, uint64_t p);
uint64_t cxxrt_allocate_exception(size_t size);
void cxxrt_exception_init(uint64_t obj, uint64_t type, uint64_t dtor);
void cxxrt_exception_release(Ctx *c, uint64_t obj);
void cxxrt_throw_report(Ctx *c, const char *how, uint64_t obj, uint64_t type);
uint64_t cxxrt_refstring_new(const char *s, size_t n);
uint64_t cxxrt_ti_logic_error(void);
uint64_t cxxrt_class_ti_vptr(int kind);
/* libcxx.c */
void cxx_string_init(Ctx *c, uint64_t str, const char *p, size_t n);

static void call_import(Ctx *c, const char *name, uint64_t a0, uint64_t a1) {
    ImportFn fn = import_lookup(name);
    if (!fn) { printf("\nnncxxlib: %s has no handler\n", name); report_and_exit(3); }
    uint64_t save[3] = {c->x[0], c->x[1], c->x[30]};
    c->x[0] = a0; c->x[1] = a1;
    fn(c);
    c->x[0] = save[0]; c->x[1] = save[1]; c->x[30] = save[2];
}
#define MUTEX_LOCK "_ZNSt3__15mutex4lockEv"
#define MUTEX_UNLOCK "_ZNSt3__15mutex6unlockEv"
#define CV_WAIT "_ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE"
#define CV_NOTIFY_ALL "_ZNSt3__118condition_variable10notify_allEv"
/* a short std::string (at most 22 characters) written in place: no allocation */
static void short_string(uint64_t s, const char *text) {
    size_t n = strlen(text);
    memset((void *)(uintptr_t)s, 0, 24);
    wr8(s, (uint8_t)(n << 1));
    memcpy((void *)(uintptr_t)(s + 1), text, n);
}

/* ================================================================ future_error / future_category */
static uint64_t g_vt_future_error[5], g_ti_future_error[3];
static uint64_t g_vt_future_category[9], g_ti_future_category[3], g_ti_error_category[2], g_future_category[1];
static const char *future_message(int ev) {
    switch (ev) {                                       /* libc++ __future_error_category::message; future_errc values */
    case 4: return "The associated promise has been destructed prior to the associated state becoming ready.";
    case 1: return "The future has already been retrieved from the promise or packaged_task.";
    case 2: return "The state of the promise has already been set.";
    case 3: return "Operation not permitted on an object without an associated state.";
    }
    return "unspecified future_errc value\n";
}
static void future_error_build(uint64_t self, int ev) {
    const char *msg = future_message(ev);
    wr64(self, P64(&g_vt_future_error[2]));
    wr64(self + 8, cxxrt_refstring_new(msg, strlen(msg)));
    wr64(self + 0x10, (uint32_t)ev);
    wr64(self + 0x18, P64(g_future_category));
}
static void future_error_ctor(Ctx *c) {                 /* future_error(error_code): the 16-byte code arrives in x1, x2 */
    uint64_t self = A0, cat = A2;
    future_error_build(self, (int)(int32_t)A1);
    wr64(self + 0x18, cat);
    RET(self);
}
static void throw_future_error(Ctx *c, int ev) {
    uint64_t obj = cxxrt_allocate_exception(0x20);
    future_error_build(obj, ev);
    cxxrt_exception_init(obj, P64(g_ti_future_error), g_vt_future_error[2]);
    cxxrt_throw_report(c, "throw by the future/promise library", obj, P64(g_ti_future_error));
}
static void future_category(Ctx *c) { RET(g_future_category); }
static void cat_dtor(Ctx *c) { RET(A0); }
static void cat_name(Ctx *c) { RET("future"); }
static void cat_default_condition(Ctx *c) { uint64_t self = A0; RET((uint32_t)A1); c->x[1] = self; }      /* error_condition { int, category * } in x0, x1 */
static void cat_equivalent_cond(Ctx *c) { RET((uint32_t)rd32(A2) == (uint32_t)A1 && rd64(A2 + 8) == A0); } /* (int code, const error_condition &) */
static void cat_equivalent_code(Ctx *c) { RET(rd64(A1 + 8) == A0 && rd32(A1) == (uint32_t)A2); }           /* (const error_code &, int cond) */
static void cat_message(Ctx *c) {                       /* string message(int) const: result through x8 */
    const char *m = future_message((int)(int32_t)A1);
    cxx_string_init(c, c->x[8], m, strlen(m));
}

/* ================================================================ __assoc_sub_state, promise<void>, future<void> */
enum { ST_OWNERS = 8, ST_EXCEPTION = 0x10, ST_MUTEX = 0x18, ST_CV = 0x38, ST_STATE = 0x40, ST_SIZE = 0x48 };
enum { S_CONSTRUCTED = 1, S_ATTACHED = 2, S_READY = 4, S_DEFERRED = 8 };
static uint64_t g_vt_assoc[6], g_ti_assoc[3], g_ti_shared_count2[2];

static void state_release(Ctx *c, uint64_t st) {        /* __shared_count::__release_shared() */
    if (InterlockedDecrement64((volatile LONG64 *)(uintptr_t)(st + ST_OWNERS)) == -1)
        cxxrt_call(c, rd64(rd64(st) + 0x10), st, 0, 0);                /* virtual __on_zero_shared() */
}
static void assoc_dtor(Ctx *c) {                        /* ~__assoc_sub_state: the members' destructors */
    uint64_t st = A0;
    cxxrt_exception_release(c, rd64(st + ST_EXCEPTION));
    wr64(st + ST_EXCEPTION, 0);
    RET(st);
}
static void assoc_dtor_delete(Ctx *c) {                 /* only objects created by promise<void>() below have this entry */
    uint64_t st = A0;
    cxxrt_exception_release(c, rd64(st + ST_EXCEPTION));
    _aligned_free((void *)(uintptr_t)st);
}
static void assoc_on_zero_shared(Ctx *c) { uint64_t st = A0; if (st) cxxrt_call(c, rd64(rd64(st) + 8), st, 0, 0); }   /* [sdk] delete this (virtual D0) */
static void assoc_execute(Ctx *c) { throw_future_error(c, 3); }                                                      /* [sdk] base __execute throws no_state */
static void assoc_sub_wait(Ctx *c) {                    /* (this, unique_lock<mutex> &lk) */
    uint64_t st = A0, lk = A1;
    uint32_t state = rd32(st + ST_STATE);
    if (state & S_READY) return;
    if (state & S_DEFERRED) {
        wr32(st + ST_STATE, state & ~(uint32_t)S_DEFERRED);
        if (!rd8(lk + 8)) { printf("\nC++ RUNTIME: unique_lock::unlock on a lock that is not owned (std::system_error)\n"); cxxrt_throw_report(c, "throw by the future library", 0, 0); }
        call_import(c, MUTEX_UNLOCK, rd64(lk), 0);
        wr8(lk + 8, 0);
        cxxrt_call(c, rd64(rd64(st) + 0x18), st, 0, 0);                /* virtual __execute() */
        return;
    }
    while (!(rd32(st + ST_STATE) & S_READY)) call_import(c, CV_WAIT, st + ST_CV, lk);
}
static void promise_ctor(Ctx *c) {
    uint64_t self = A0;
    uint8_t *st = _aligned_malloc(ST_SIZE, 16);
    memset(st, 0, ST_SIZE);
    wr64(P64(st), P64(&g_vt_assoc[2]));
    wr16(P64(st) + ST_MUTEX, 1);                                       /* [sdk] the bytes the SDK's inlined mutex / condvar */
    wr8(P64(st) + ST_CV, 1);                                           /*       initialisers write                          */
    wr64(self, P64(st));
    RET(self);
}
static void promise_get_future(Ctx *c) {                /* future<void> get_future(): result through x8 */
    uint64_t st = rd64(A0), out = c->x[8];
    if (!st) throw_future_error(c, 3);
    call_import(c, MUTEX_LOCK, st + ST_MUTEX, 0);
    uint32_t state = rd32(st + ST_STATE);
    if (state & S_ATTACHED) { call_import(c, MUTEX_UNLOCK, st + ST_MUTEX, 0); throw_future_error(c, 1); }
    InterlockedIncrement64((volatile LONG64 *)(uintptr_t)(st + ST_OWNERS));
    wr32(st + ST_STATE, state | S_ATTACHED);
    call_import(c, MUTEX_UNLOCK, st + ST_MUTEX, 0);
    wr64(out, st);
}
static void promise_set_value(Ctx *c) {
    uint64_t st = rd64(A0);
    if (!st) throw_future_error(c, 3);
    call_import(c, MUTEX_LOCK, st + ST_MUTEX, 0);
    if ((rd32(st + ST_STATE) & S_CONSTRUCTED) || rd64(st + ST_EXCEPTION)) { call_import(c, MUTEX_UNLOCK, st + ST_MUTEX, 0); throw_future_error(c, 2); }
    wr32(st + ST_STATE, rd32(st + ST_STATE) | S_CONSTRUCTED | S_READY);
    call_import(c, CV_NOTIFY_ALL, st + ST_CV, 0);
    call_import(c, MUTEX_UNLOCK, st + ST_MUTEX, 0);
}
static void promise_dtor(Ctx *c) {
    uint64_t self = A0, st = rd64(self);
    if (st) {
        /* [sdk] no value, no exception, and a future still attached: store future_error(broken_promise) for it */
        if (!(rd8(st + ST_STATE) & S_CONSTRUCTED) && !rd64(st + ST_EXCEPTION) && (int64_t)rd64(st + ST_OWNERS) >= 1) {
            uint64_t obj = cxxrt_allocate_exception(0x20);
            future_error_build(obj, 4);
            cxxrt_exception_init(obj, P64(g_ti_future_error), g_vt_future_error[2]);
            call_import(c, MUTEX_LOCK, st + ST_MUTEX, 0);
            wr64(st + ST_EXCEPTION, obj);
            wr32(st + ST_STATE, rd32(st + ST_STATE) | S_READY);
            call_import(c, CV_NOTIFY_ALL, st + ST_CV, 0);
            call_import(c, MUTEX_UNLOCK, st + ST_MUTEX, 0);
        }
        state_release(c, st);
    }
    RET(self);
}
static void future_dtor(Ctx *c) { uint64_t self = A0, st = rd64(self); if (st) state_release(c, st); RET(self); }

/* ================================================================ random_device (mt19937, see the top of the file) */
static void rd_ctor(Ctx *c) {
    uint64_t self = A0;
    uint32_t x = 5489;
    wr32(self, x);
    for (uint32_t i = 1; i < 624; i++) { x = (x ^ (x >> 30)) * 1812433253u + i; wr32(self + 4ull * i, x); }
    wr64(self + 0x9c0, 0);
    RET(self);
}
static void rd_call(Ctx *c) {
    uint64_t self = A0, i = rd64(self + 0x9c0) % 624, j = (i + 1) % 624, k = (i + 397) % 624;
    uint32_t xj = rd32(self + 4 * j);
    uint32_t y = (rd32(self + 4 * i) & 0x80000000u) | (xj & 0x7fffffffu);
    uint32_t v = rd32(self + 4 * k) ^ (y >> 1) ^ ((xj & 1) * 0x9908b0dfu);
    wr32(self + 4 * i, v);
    wr64(self + 0x9c0, j);
    v ^= v >> 11;
    v ^= (v << 7) & 0x9d2c5680u;
    v ^= (v << 15) & 0xefc60000u;
    v ^= v >> 18;
    RET(v);
}
static void rd_dtor(Ctx *c) { RET(A0); }
static void rd_entropy(Ctx *c) { vzero(&c->v[0]); c->v[0].df[0] = 0.0; }

/* ================================================================ locale facets of the classic locale */
static const uint16_t classic_table[256] = {            /* [sdk] ctype<char>::classic_table(), entries 0..127; the rest are 0 */
    0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x2003, 0x2002, 0x2002, 0x2002, 0x2002, 0x002, 0x002,
    0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002, 0x002,
    0x6001, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004,
    0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x5800, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004,
    0x4004, 0x5500, 0x5500, 0x5500, 0x5500, 0x5500, 0x5500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500,
    0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4500, 0x4004, 0x4004, 0x4004, 0x4004, 0x4004,
    0x4004, 0x5600, 0x5600, 0x5600, 0x5600, 0x5600, 0x5600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600,
    0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4600, 0x4004, 0x4004, 0x4004, 0x4004, 0x002,
};
static uint64_t g_id_ctype[2], g_id_numpunct[2];        /* the imported locale::id objects: only their addresses matter */
static uint64_t g_vt_ctype[13], g_ti_ctype[2], g_ctype[4];
static uint64_t g_vt_numpunct[10], g_ti_numpunct[2], g_numpunct[6];
static uint8_t g_classic_impl[64];

static int lower(int ch) { return ch >= 'a' && ch <= 'z'; }
static int upper(int ch) { return ch >= 'A' && ch <= 'Z'; }
static void facet_dtor(Ctx *c) { RET(A0); }             /* the facets are static: nothing to destroy or free */
static void facet_nop(Ctx *c) { (void)c; }
static void ct_toupper(Ctx *c) { int ch = (uint8_t)A1; RET(lower(ch) ? ch - 0x20 : ch); }
static void ct_toupper_range(Ctx *c) { for (uint8_t *p = (uint8_t *)(uintptr_t)A1; P64(p) < A2; p++) if (lower(*p)) *p -= 0x20; RET(A2); }
static void ct_tolower(Ctx *c) { int ch = (uint8_t)A1; RET(upper(ch) ? ch + 0x20 : ch); }
static void ct_tolower_range(Ctx *c) { for (uint8_t *p = (uint8_t *)(uintptr_t)A1; P64(p) < A2; p++) if (upper(*p)) *p += 0x20; RET(A2); }
static void ct_widen(Ctx *c) { RET((uint8_t)A1); }
static void ct_widen_range(Ctx *c) { memmove((void *)(uintptr_t)A3, (void *)(uintptr_t)A1, (size_t)(A2 - A1)); RET(A2); }     /* (low, high, dest) */
static void ct_narrow(Ctx *c) { RET((int8_t)A1 >= 0 ? (uint8_t)A1 : (uint8_t)A2); }                                          /* [sdk] non-ASCII -> default */
static void ct_narrow_range(Ctx *c) {                   /* (low, high, dfault, dest) */
    uint8_t *d = (uint8_t *)(uintptr_t)A4;
    for (const int8_t *p = (const int8_t *)(uintptr_t)A1; P64(p) < A2; p++) *d++ = *p >= 0 ? (uint8_t)*p : (uint8_t)A3;
    RET(A2);
}
static void np_decimal_point(Ctx *c) { RET(rd8(A0 + 0x10)); }
static void np_thousands_sep(Ctx *c) { RET(rd8(A0 + 0x11)); }
static void np_grouping(Ctx *c) { short_string(c->x[8], ""); }
static void np_truename(Ctx *c) { short_string(c->x[8], "true"); }
static void np_falsename(Ctx *c) { short_string(c->x[8], "false"); }
static void locale_use_facet(Ctx *c) {                  /* const facet *locale::use_facet(id &) const */
    uint64_t id = A1;
    if (id == P64(g_id_ctype)) { RET(g_ctype); return; }
    if (id == P64(g_id_numpunct)) { RET(g_numpunct); return; }
    printf("\nC++ RUNTIME: std::locale::use_facet for an id at 0x%llx that is neither ctype<char> nor numpunct<char> (std::bad_cast); guest caller 0x%llx\n",
           (unsigned long long)id, (unsigned long long)c->x[30] - 4);
    cxxrt_throw_report(c, "throw by std::locale::use_facet", 0, 0);
}
static void locale_has_facet(Ctx *c) { RET(A1 == P64(g_id_ctype) || A1 == P64(g_id_numpunct)); }

/* ================================================================ basic_string<wchar_t>
 * same representation as std::string with 4-byte characters: bit 0 of byte 0 = long, data pointer at +0x10.
 * [sdk] the destructor passes the buffer to operator delete, which is the game's own (it exports _ZdlPv). */
static void wstr_dtor(Ctx *c) { uint64_t s = A0; if (rd8(s) & 1) cxxrt_guest_delete(c, rd64(s + 0x10)); RET(s); }

/* ================================================================ basic_ostream / basic_istream vtables
 * [sdk] _ZTVNSt3__113basic_ostreamIcNS_11char_traitsIcEEEE: { vbase offset 8, 0, typeinfo, D1, D0,
 *   vcall offset -8, offset-to-top -8, typeinfo, virtual thunk D1, virtual thunk D0 } (istream: 0x10 / -0x10).
 *
 * Destructors, re-implemented from the SDK's code (fake audit, closing session):
 *   [sdk 0x464d38] ios_base::~ios_base: vptr = ios_base vtable; for i = event_size-1 .. 0: fn[i](erase_event = 0, *this, index[i])
 *       (fn array +0x38, index array +0x40, event_size +0x48); ~locale() on +0x30; free(+0x38), free(+0x40), free(+0x58), free(+0x70)
 *       with the C `free` the SDK module imports from the game.
 *   [sdk 0x464dc0] basic_ios::~basic_ios (D2) = tail call to it.
 *   [sdk 0x46bd10] ostream D1: `add x0, x0, #8; b ios_base D1`; [sdk 0x46bd30] D0: the same, then operator delete(this).
 *       istream ([sdk] _ZNSt3__113basic_istream...D1Ev): its basic_ios part is at +0x10.
 *   [sdk 0x4656c0] basic_streambuf D2: vptr = basic_streambuf vtable; ~locale() on +8.
 *   [sdk 0x46be38] ostream::sentry::~sentry: if rdbuf && state == good && (flags & unitbuf) && !uncaught_exception():
 *       rdbuf->pubsync() == -1 -> setstate(badbit).
 * The vptr written is the SDK image's own vtable (mapped by sdklift_rt.c), as on the console. */
uint64_t sdk_symbol(const char *name);                    /* sdklift_rt.c */
static uint64_t g_vt_ostream[10], g_vt_istream[10], g_ti_ostream[2], g_ti_istream[2];
static void call_handler(Ctx *c, const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("nncxxlib: no handler for %s\n", name); guest_trap("missing C++ library handler", c->x[30] - 4); }
    f(c);
}
static void ios_base_destroy(Ctx *c, uint64_t ios) {
    uint64_t vt = sdk_symbol("_ZTVNSt3__18ios_baseE");
    if (vt) wr64(ios, vt + 0x10);
    for (uint64_t n = rd64(ios + 0x48); n-- > 0;) {
        uint64_t fn = rd64(rd64(ios + 0x38) + n * 8);
        c->x[2] = rd32(rd64(ios + 0x40) + n * 4);
        c->x[0] = 0; c->x[1] = ios;
        call_guest(c, fn);
    }
    c->x[0] = ios + 0x30; call_handler(c, "_ZNSt3__16localeD1Ev");
    static const int arrays[4] = {0x38, 0x40, 0x58, 0x70};
    for (int i = 0; i < 4; i++) { c->x[0] = rd64(ios + (uint64_t)arrays[i]); if (c->x[0]) call_guest(c, GA(0x710392e590ULL)); }   /* free(NULL) does nothing */
}
static void ios_dtor(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self); RET(self); }
static void sbuf_dtor(Ctx *c) {
    uint64_t self = A0, vt = sdk_symbol("_ZTVNSt3__115basic_streambufIcNS_11char_traitsIcEEEE");
    if (vt) wr64(self, vt + 0x10);
    c->x[0] = self + 8; call_handler(c, "_ZNSt3__16localeD1Ev");
    RET(self);
}
static void sentry_dtor(Ctx *c) {
    uint64_t s = A0, os = rd64(s + 8), ios = os + rd64(rd64(os) - 0x18);
    if (rd64(ios + 0x28) && rd32(ios + 0x20) == 0 && (rd8(ios + 9) & 0x20)) {
        call_handler(c, "_ZSt18uncaught_exceptionv");
        if (!(c->x[0] & 1)) {
            ios = rd64(s + 8) + rd64(rd64(rd64(s + 8)) - 0x18);
            uint64_t sb = rd64(ios + 0x28);
            c->x[0] = sb;
            call_guest(c, rd64(rd64(sb) + 0x30));                                   /* pubsync -> virtual sync() */
            if ((uint32_t)c->x[0] == 0xffffffffu) {
                ios = rd64(s + 8) + rd64(rd64(rd64(s + 8)) - 0x18);
                c->x[0] = ios; c->x[1] = rd32(ios + 0x20) | 1;
                call_handler(c, "_ZNSt3__18ios_base5clearEj");
            }
        }
    }
    RET(s);
}
/* complete-object destructors: `this` is the stream; its basic_ios part is at +8 (ostream) or +0x10 (istream) = vtable[-3] */
static void stream_dtor(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + rd64(rd64(self) - 0x18)); RET(self); }
static void ostream_d1(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 8); RET(self); }
static void istream_d1(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 0x10); RET(self); }
static void ostream_d0(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 8); cxxrt_guest_delete(c, self); }
static void istream_d0(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 0x10); cxxrt_guest_delete(c, self); }
static void stream_dtor_delete(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + rd64(rd64(self) - 0x18)); cxxrt_guest_delete(c, self); }
/* virtual thunks: `this` is the basic_ios part; this += vcall offset (vtable[-3] of the ios vptr) gives the stream */
static void stream_thunk_dtor(Ctx *c) { uint64_t ios = A0, self = ios + rd64(rd64(ios) - 0x18); ios_base_destroy(c, ios); RET(self); }
static void stream_thunk_dtor_delete(Ctx *c) { uint64_t ios = A0, self = ios + rd64(rd64(ios) - 0x18); ios_base_destroy(c, ios); cxxrt_guest_delete(c, self); }

static void make_stream_vtable(uint64_t *vt, uint64_t *ti, const char *cls, int64_t ios_offset) {
    char name[72];
    vt[0] = (uint64_t)ios_offset; vt[1] = 0; vt[2] = P64(ti);
    snprintf(name, sizeof name, "std::%s::~%s [D1]", cls, cls); vt[3] = hostfn_address(name, stream_dtor);
    snprintf(name, sizeof name, "std::%s::~%s [D0]", cls, cls); vt[4] = hostfn_address(name, stream_dtor_delete);
    vt[5] = (uint64_t)-ios_offset; vt[6] = (uint64_t)-ios_offset; vt[7] = P64(ti);
    snprintf(name, sizeof name, "virtual thunk to std::%s::~%s [D1]", cls, cls); vt[8] = hostfn_address(name, stream_thunk_dtor);
    snprintf(name, sizeof name, "virtual thunk to std::%s::~%s [D0]", cls, cls); vt[9] = hostfn_address(name, stream_thunk_dtor_delete);
}

void cxxlib_install_data(void) {
    uint64_t vt_class = cxxrt_class_ti_vptr(1), vt_si = cxxrt_class_ti_vptr(2);
    /* future_error, future_category */
    g_ti_future_error[0] = vt_si; g_ti_future_error[1] = P64("NSt3__112future_errorE"); g_ti_future_error[2] = cxxrt_ti_logic_error();
    g_vt_future_error[0] = 0; g_vt_future_error[1] = P64(g_ti_future_error);
    g_vt_future_error[2] = hostfn_address("std::logic_error::~std::logic_error [D1]", NULL);       /* same entries as logic_error (nncxxrt.c) */
    g_vt_future_error[3] = hostfn_address("std::logic_error::~std::logic_error [D0]", NULL);
    g_vt_future_error[4] = hostfn_address("std::logic_error::what", NULL);
    g_ti_error_category[0] = vt_class; g_ti_error_category[1] = P64("NSt3__114error_categoryE");
    g_ti_future_category[0] = vt_si; g_ti_future_category[1] = P64("NSt3__123__future_error_categoryE"); g_ti_future_category[2] = P64(g_ti_error_category);
    g_vt_future_category[0] = 0; g_vt_future_category[1] = P64(g_ti_future_category);
    g_vt_future_category[2] = hostfn_address("future_category::~ [D1]", cat_dtor);
    g_vt_future_category[3] = hostfn_address("future_category::~ [D0]", cat_dtor);
    g_vt_future_category[4] = hostfn_address("future_category::name", cat_name);
    g_vt_future_category[5] = hostfn_address("error_category::default_error_condition", cat_default_condition);
    g_vt_future_category[6] = hostfn_address("error_category::equivalent(int, error_condition)", cat_equivalent_cond);
    g_vt_future_category[7] = hostfn_address("error_category::equivalent(error_code, int)", cat_equivalent_code);
    g_vt_future_category[8] = hostfn_address("future_category::message", cat_message);
    g_future_category[0] = P64(&g_vt_future_category[2]);
    import_set_address("_ZTVNSt3__112future_errorE", g_vt_future_error);
    import_set_address("_ZTINSt3__112future_errorE", g_ti_future_error);

    /* __assoc_sub_state: [sdk] { 0, typeinfo, D1, D0, __on_zero_shared, __execute } */
    g_ti_shared_count2[0] = vt_class; g_ti_shared_count2[1] = P64("NSt3__114__shared_countE");
    g_ti_assoc[0] = vt_si; g_ti_assoc[1] = P64("NSt3__117__assoc_sub_stateE"); g_ti_assoc[2] = P64(g_ti_shared_count2);
    g_vt_assoc[0] = 0; g_vt_assoc[1] = P64(g_ti_assoc);
    g_vt_assoc[2] = hostfn_address("std::__assoc_sub_state::~ [D1]", assoc_dtor);
    g_vt_assoc[3] = hostfn_address("std::__assoc_sub_state::~ [D0]", assoc_dtor_delete);
    g_vt_assoc[4] = hostfn_address("std::__assoc_sub_state::__on_zero_shared", assoc_on_zero_shared);
    g_vt_assoc[5] = hostfn_address("std::__assoc_sub_state::__execute", assoc_execute);
    import_set_address("_ZTVNSt3__117__assoc_sub_stateE", g_vt_assoc);
    import_set_address("_ZTINSt3__117__assoc_sub_stateE", g_ti_assoc);

    /* facets: [sdk] vtable orders of ctype<char> and numpunct<char> */
    static const struct { const char *name; ImportFn fn; } ct[] = {
        {"std::ctype<char>::~ [D1]", facet_dtor}, {"std::ctype<char>::~ [D0]", facet_dtor}, {"std::locale::facet::__on_zero_shared", facet_nop},
        {"std::ctype<char>::do_toupper(char)", ct_toupper}, {"std::ctype<char>::do_toupper(range)", ct_toupper_range},
        {"std::ctype<char>::do_tolower(char)", ct_tolower}, {"std::ctype<char>::do_tolower(range)", ct_tolower_range},
        {"std::ctype<char>::do_widen(char)", ct_widen}, {"std::ctype<char>::do_widen(range)", ct_widen_range},
        {"std::ctype<char>::do_narrow(char)", ct_narrow}, {"std::ctype<char>::do_narrow(range)", ct_narrow_range},
    }, np[] = {
        {"std::numpunct<char>::~ [D1]", facet_dtor}, {"std::numpunct<char>::~ [D0]", facet_dtor}, {"std::locale::facet::__on_zero_shared", facet_nop},
        {"std::numpunct<char>::do_decimal_point", np_decimal_point}, {"std::numpunct<char>::do_thousands_sep", np_thousands_sep},
        {"std::numpunct<char>::do_grouping", np_grouping}, {"std::numpunct<char>::do_truename", np_truename},
        {"std::numpunct<char>::do_falsename", np_falsename},
    };
    g_ti_ctype[0] = vt_class; g_ti_ctype[1] = P64("NSt3__15ctypeIcEE");
    g_vt_ctype[0] = 0; g_vt_ctype[1] = P64(g_ti_ctype);
    for (int i = 0; i < 11; i++) g_vt_ctype[2 + i] = hostfn_address(ct[i].name, ct[i].fn);
    g_ctype[0] = P64(&g_vt_ctype[2]); g_ctype[1] = 0x7fffffff; g_ctype[2] = P64(classic_table); g_ctype[3] = 0;
    g_ti_numpunct[0] = vt_class; g_ti_numpunct[1] = P64("NSt3__18numpunctIcEE");
    g_vt_numpunct[0] = 0; g_vt_numpunct[1] = P64(g_ti_numpunct);
    for (int i = 0; i < 8; i++) g_vt_numpunct[2 + i] = hostfn_address(np[i].name, np[i].fn);
    g_numpunct[0] = P64(&g_vt_numpunct[2]); g_numpunct[1] = 0x7fffffff; g_numpunct[2] = 0x2c2e;        /* [sdk] '.' at +0x10, ',' at +0x11 */
    import_set_address("_ZNSt3__15ctypeIcE2idE", g_id_ctype);
    import_set_address("_ZNSt3__18numpunctIcE2idE", g_id_numpunct);
    import_set_address("_ZTVNSt3__15ctypeIcEE", g_vt_ctype);
    import_set_address("_ZTVNSt3__18numpunctIcEE", g_vt_numpunct);

    /* streams */
    g_ti_ostream[0] = vt_class; g_ti_ostream[1] = P64("NSt3__113basic_ostreamIcNS_11char_traitsIcEEEE");
    g_ti_istream[0] = vt_class; g_ti_istream[1] = P64("NSt3__113basic_istreamIcNS_11char_traitsIcEEEE");
    make_stream_vtable(g_vt_ostream, g_ti_ostream, "basic_ostream", 8);
    make_stream_vtable(g_vt_istream, g_ti_istream, "basic_istream", 0x10);
    import_set_address("_ZTVNSt3__113basic_ostreamIcNS_11char_traitsIcEEEE", g_vt_ostream);
    import_set_address("_ZTVNSt3__113basic_istreamIcNS_11char_traitsIcEEEE", g_vt_istream);
    (void)g_classic_impl;
}

#define OSTR "_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE"
#define ISTR "_ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE"
/* Replace earlier registrations: use_facet used to trap; the deleting destructor did not free the object. */
static const ImportDef cxxlib_replace[] = {
    {"_ZNKSt3__16locale9use_facetERNS0_2idE", locale_use_facet},
    {OSTR "D0Ev", ostream_d0}, {ISTR "D0Ev", istream_d0}, {OSTR "D1Ev", ostream_d1}, {ISTR "D1Ev", istream_d1},
    {"_ZNSt3__19basic_iosIcNS_11char_traitsIcEEED2Ev", ios_dtor},
    {"_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEED2Ev", sbuf_dtor},
    {OSTR "6sentryD1Ev", sentry_dtor},
};
static const ImportDef cxxlib_fill[] = {
    {"_ZNKSt3__16locale9has_facetERNS0_2idE", locale_has_facet},
    {"_ZTv0_n24_" "NSt3__113basic_ostreamIcNS_11char_traitsIcEEED1Ev", stream_thunk_dtor},
    {"_ZTv0_n24_" "NSt3__113basic_ostreamIcNS_11char_traitsIcEEED0Ev", stream_thunk_dtor_delete},
    {"_ZTv0_n24_" "NSt3__113basic_istreamIcNS_11char_traitsIcEEED1Ev", stream_thunk_dtor},
    {"_ZTv0_n24_" "NSt3__113basic_istreamIcNS_11char_traitsIcEEED0Ev", stream_thunk_dtor_delete},
    {"_ZNSt3__17promiseIvEC1Ev", promise_ctor}, {"_ZNSt3__17promiseIvEC2Ev", promise_ctor},
    {"_ZNSt3__17promiseIvE10get_futureEv", promise_get_future}, {"_ZNSt3__17promiseIvE9set_valueEv", promise_set_value},
    {"_ZNSt3__17promiseIvED1Ev", promise_dtor}, {"_ZNSt3__17promiseIvED2Ev", promise_dtor},
    {"_ZNSt3__16futureIvED1Ev", future_dtor}, {"_ZNSt3__16futureIvED2Ev", future_dtor},
    {"_ZNSt3__117__assoc_sub_state10__sub_waitERNS_11unique_lockINS_5mutexEEE", assoc_sub_wait},
    {"_ZNSt3__117__assoc_sub_state9__executeEv", assoc_execute},
    {"_ZNSt3__117__assoc_sub_state16__on_zero_sharedEv", assoc_on_zero_shared},
    {"_ZNSt3__112future_errorC1ENS_10error_codeE", future_error_ctor}, {"_ZNSt3__112future_errorC2ENS_10error_codeE", future_error_ctor},
    {"_ZNSt3__115future_categoryEv", future_category},
    {"_ZNSt3__113random_deviceC1ERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE", rd_ctor},
    {"_ZNSt3__113random_deviceC2ERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE", rd_ctor},
    {"_ZNSt3__113random_deviceclEv", rd_call}, {"_ZNSt3__113random_deviceD1Ev", rd_dtor}, {"_ZNSt3__113random_deviceD2Ev", rd_dtor},
    {"_ZNKSt3__113random_device7entropyEv", rd_entropy},
    {"_ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEED2Ev", wstr_dtor},
    {"_ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEED1Ev", wstr_dtor},
};

/* test access (cxxrt_selftest.h) */
uint64_t cxxlib_test_object(int which) { return which == 0 ? P64(g_id_ctype) : which == 1 ? P64(g_id_numpunct) : which == 2 ? P64(&g_vt_ostream[3]) : P64(g_future_category); }

void register_cxxlib_imports(void) {
    register_imports(cxxlib_replace, (int)(sizeof cxxlib_replace / sizeof cxxlib_replace[0]));
    for (int i = 0; i < (int)(sizeof cxxlib_fill / sizeof cxxlib_fill[0]); i++)
        if (!import_lookup(cxxlib_fill[i].name)) register_imports(&cxxlib_fill[i], 1);
}

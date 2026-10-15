/* Host implementations of the libc++ (std::__1) functions the game imports
 * from the system module. Objects live in guest memory, so the layouts below
 * must match libc++ on AArch64 exactly.
 *
 * std::string (24 bytes, little endian, default ABI):
 *   short: byte 0 = size << 1, characters from byte 1 (up to 22 + NUL)
 *   long : u64 capacity | 1 at +0, u64 size at +8, char* data at +16
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(n) ((void *)(uintptr_t)c->x[n])

typedef uint8_t Str;                                     /* pointer to a guest std::string */

/* String buffers must come from the game's own allocator: most std::string code is inlined in
 * the game and allocates/frees through f_710392dce0(align, size) / f_710392e590(ptr) directly,
 * so a buffer created here can be freed there and vice versa. The stored capacity is the
 * allocation size (libc++ keeps "size | 1"), rounded up to 16 as the inlined code does. */
#define GAME_MEMALIGN GA(0x710392dce0ULL)
#define GAME_FREE GA(0x710392e590ULL)
static __declspec(thread) Ctx *t_ctx;                     /* context of the string op in progress */
static char *sbuf_alloc(size_t alloc) {
    Ctx *c = t_ctx;
    uint64_t save[4] = {c->x[0], c->x[1], c->x[2], c->x[30]};
    c->x[0] = 16; c->x[1] = alloc;
    call_guest(c, GAME_MEMALIGN);
    char *p = (char *)(uintptr_t)c->x[0];
    c->x[0] = save[0]; c->x[1] = save[1]; c->x[2] = save[2]; c->x[30] = save[3];
    if (!p) guest_trap("game allocator returned null for a string buffer", alloc);
    return p;
}
static void sbuf_free(char *p, const char *where) {
    (void)where;
    Ctx *c = t_ctx;
    uint64_t save[4] = {c->x[0], c->x[1], c->x[2], c->x[30]};
    c->x[0] = (uint64_t)(uintptr_t)p;
    call_guest(c, GAME_FREE);
    c->x[0] = save[0]; c->x[1] = save[1]; c->x[2] = save[2]; c->x[30] = save[3];
}

static int s_long(const Str *s) { return s[0] & 1; }
static char *s_data(Str *s) { return s_long(s) ? (char *)(uintptr_t)rd64((uint64_t)s + 16) : (char *)s + 1; }
static size_t s_size(const Str *s) { return s_long(s) ? (size_t)rd64((uint64_t)s + 8) : (size_t)(s[0] >> 1); }
static size_t s_cap(const Str *s) { return s_long(s) ? (size_t)(rd64((uint64_t)s) & ~1ull) - 1 : 22; }
static void s_set_size(Str *s, size_t n) { if (s_long(s)) wr64((uint64_t)s + 8, n); else s[0] = (uint8_t)(n << 1); }

static void s_reserve(Str *s, size_t need) {
    size_t cap = s_cap(s);
    if (need <= cap) return;
    size_t want = need > 2 * cap ? need : 2 * cap;
    size_t alloc = (want + 16) & ~(size_t)15;
    size_t size = s_size(s);
    char *p = sbuf_alloc(alloc);
    memcpy(p, s_data(s), size + 1);
    if (s_long(s)) sbuf_free(s_data(s), "reserve");
    wr64((uint64_t)s, alloc | 1);
    wr64((uint64_t)s + 8, size);
    wr64((uint64_t)s + 16, (uint64_t)p);
}
static void s_assign(Str *s, const char *p, size_t n) {
    char *copy = NULL;
    char *d = s_data(s);
    if (p >= d && p <= d + s_size(s)) { copy = malloc(n + 1); memcpy(copy, p, n); p = copy; }   /* self-assignment */
    s_reserve(s, n);
    d = s_data(s);
    memmove(d, p, n);
    d[n] = 0;
    s_set_size(s, n);
    free(copy);
}
static void s_append(Str *s, const char *p, size_t n) {
    char *copy = NULL;
    char *d = s_data(s);
    size_t size = s_size(s);
    if (p >= d && p <= d + size) { copy = malloc(n + 1); memcpy(copy, p, n); p = copy; }
    s_reserve(s, size + n);
    d = s_data(s);
    memcpy(d + size, p, n);
    d[size + n] = 0;
    s_set_size(s, size + n);
    free(copy);
}
static void s_init(Str *s, const char *p, size_t n) { memset(s, 0, 24); s_assign(s, p, n); }

/* For the other C++ runtime files (nncxxrt.c, nncxxlib.c): build / read guest std::string objects and
 * free buffers the game's inlined container code allocated. */
void cxx_string_init(Ctx *c, uint64_t str, const char *p, size_t n) { Ctx *prev = t_ctx; t_ctx = c; s_init((Str *)(uintptr_t)str, p, n); t_ctx = prev; }
const char *cxx_string_data(uint64_t str, size_t *n) { Str *s = (Str *)(uintptr_t)str; if (n) *n = s_size(s); return s_data(s); }
void cxx_game_free(Ctx *c, uint64_t p) { Ctx *prev = t_ctx; t_ctx = c; sbuf_free((char *)(uintptr_t)p, "runtime"); t_ctx = prev; }

static void str_assign_cstr(Ctx *c) { t_ctx = c; s_assign(P(0), P(1), strlen(P(1))); }
static void str_assign_n(Ctx *c) { t_ctx = c; s_assign(P(0), P(1), (size_t)A2); }
static void str_append_cstr(Ctx *c) { t_ctx = c; s_append(P(0), P(1), strlen(P(1))); }
static void str_append_n(Ctx *c) { t_ctx = c; s_append(P(0), P(1), (size_t)A2); }
static void str_push_back(Ctx *c) { t_ctx = c; char ch = (char)A1; s_append(P(0), &ch, 1); }
static void str_copy_ctor(Ctx *c) { t_ctx = c; Str *o = P(1); s_init(P(0), s_data(o), s_size(o)); }
static void str_substr_ctor(Ctx *c) { t_ctx = c;
    Str *o = P(1);
    size_t size = s_size(o), pos = (size_t)A2, n = (size_t)A3;
    if (pos > size) guest_trap("std::string substring constructor out of range", c->x[30] - 4);
    if (n > size - pos) n = size - pos;
    s_init(P(0), s_data(o) + pos, n);
}
static void str_dtor(Ctx *c) { t_ctx = c; Str *s = P(0); if (s_long(s)) sbuf_free(s_data(s), "dtor"); }
static void str_copy_assign(Ctx *c) { t_ctx = c; Str *o = P(1); if (P(0) != P(1)) s_assign(P(0), s_data(o), s_size(o)); }
static void str_reserve(Ctx *c) { t_ctx = c; s_reserve(P(0), (size_t)A1); }
static void str_resize(Ctx *c) { t_ctx = c;
    Str *s = P(0);
    size_t n = (size_t)A1, size = s_size(s);
    if (n > size) { s_reserve(s, n); memset(s_data(s) + size, (int)A2, n - size); }
    s_data(s)[n] = 0;
    s_set_size(s, n);
}
static void str_erase(Ctx *c) { t_ctx = c;
    Str *s = P(0);
    size_t size = s_size(s), pos = (size_t)A1, n = (size_t)A2;
    if (pos > size) guest_trap("std::string::erase out of range", c->x[30] - 4);
    if (n > size - pos) n = size - pos;
    char *d = s_data(s);
    memmove(d + pos, d + pos + n, size - pos - n + 1);
    s_set_size(s, size - n);
}
static void str_insert_cstr(Ctx *c) { t_ctx = c;
    Str *s = P(0);
    size_t size = s_size(s), pos = (size_t)A1;
    const char *p = P(2);
    size_t n = strlen(p);
    if (pos > size) guest_trap("std::string::insert out of range", c->x[30] - 4);
    char *copy = malloc(n + 1);
    memcpy(copy, p, n);
    s_reserve(s, size + n);
    char *d = s_data(s);
    memmove(d + pos + n, d + pos, size - pos + 1);
    memcpy(d + pos, copy, n);
    s_set_size(s, size + n);
    free(copy);
}
static void str_at(Ctx *c) { t_ctx = c;
    Str *s = P(0);
    if (A1 >= s_size(s)) guest_trap("std::string::at out of range", c->x[30] - 4);
    RET(s_data(s) + A1);
}
static void str_grow_by(Ctx *c) { t_ctx = c;                        /* __grow_by(old_cap, delta_cap, old_sz, n_copy, n_del, n_add) */
    Str *s = P(0);
    size_t old_sz = (size_t)A3, n_copy = (size_t)A4, n_del = (size_t)A5, n_add = (size_t)c->x[6];
    size_t alloc = ((size_t)A1 + (size_t)A2 + 17) & ~(size_t)15;
    char *old = s_data(s), *p = sbuf_alloc(alloc);
    if (n_copy) memcpy(p, old, n_copy);
    if (old_sz - n_del - n_copy) memcpy(p + n_copy + n_add, old + n_copy + n_del, old_sz - n_del - n_copy);
    if (s_long(s)) sbuf_free(old, "grow_by");
    wr64((uint64_t)s, alloc | 1);
    wr64((uint64_t)s + 16, (uint64_t)p);
}
static void str_find_char(Ctx *c) { t_ctx = c;
    Str *s = P(0);
    size_t size = s_size(s), pos = (size_t)A2;
    const char *d = s_data(s), *hit = pos < size ? memchr(d + pos, (int)A1, size - pos) : NULL;
    RET(hit ? (uint64_t)(hit - d) : ~0ull);
}
static void str_rfind_char(Ctx *c) { t_ctx = c;
    Str *s = P(0);
    size_t size = s_size(s), pos = (size_t)A2;
    const char *d = s_data(s);
    if (size == 0) { RET(~0ull); return; }
    for (size_t i = pos < size ? pos + 1 : size; i-- > 0;) if (d[i] == (char)A1) { RET(i); return; }
    RET(~0ull);
}
static void str_compare(Ctx *c) { t_ctx = c;                        /* compare(pos1, n1, const char *s, n2) */
    Str *s = P(0);
    size_t size = s_size(s), pos = (size_t)A1, n1 = (size_t)A2, n2 = (size_t)A4;
    if (pos > size) guest_trap("std::string::compare out of range", c->x[30] - 4);
    if (n1 > size - pos) n1 = size - pos;
    int r = memcmp(s_data(s) + pos, P(3), n1 < n2 ? n1 : n2);
    RET((int64_t)(r ? r : (n1 < n2 ? -1 : n1 > n2)));
}
static void str_plus_cstr(Ctx *c) { t_ctx = c;                      /* operator+(const char *, const string &): result via x8 */
    Str *out = (Str *)(uintptr_t)c->x[8], *rhs = P(1);
    const char *lhs = P(0);
    s_init(out, lhs, strlen(lhs));
    s_append(out, s_data(rhs), s_size(rhs));
}
static void str_to_string_int(Ctx *c) { t_ctx = c; char b[32]; int n = snprintf(b, sizeof b, "%d", (int)A0); s_init((Str *)(uintptr_t)c->x[8], b, (size_t)n); }
static void str_to_string_uint(Ctx *c) { t_ctx = c; char b[32]; int n = snprintf(b, sizeof b, "%u", (unsigned)A0); s_init((Str *)(uintptr_t)c->x[8], b, (size_t)n); }

/* ---- std::align(alignment, size, void *&ptr, size_t &space): returns the aligned pointer and
 * updates ptr/space when the block fits, otherwise returns null and leaves them unchanged. */
static void cxx_align(Ctx *c) {
    uint64_t alignment = A0, size = A1, ptr_ref = A2, space_ref = A3;
    uint64_t p = rd64(ptr_ref), space = rd64(space_ref);
    uint64_t aligned = (p + alignment - 1) & ~(alignment - 1);
    uint64_t diff = aligned - p;
    if (diff + size > space) { RET(0); return; }
    wr64(ptr_ref, aligned);
    wr64(space_ref, space - diff);
    RET(aligned);
}

/* ---- hashing support */
static void next_prime(Ctx *c) {
    uint64_t n = A0;
    if (n <= 2) { RET(2); return; }
    n |= 1;
    for (;; n += 2) {
        int prime = 1;
        for (uint64_t d = 3; d * d <= n; d += 2) if (n % d == 0) { prime = 0; break; }
        if (prime) { RET(n); return; }
    }
}

/* ---- std::sort on plain arrays (comparator object in x2 is std::less) */
#define SORT(name, T) \
    static int cmp_##name(const void *a, const void *b) { T x = *(const T *)a, y = *(const T *)b; return x < y ? -1 : x > y; } \
    static void sort_##name(Ctx *c) { T *first = P(0), *last = P(1); if (last > first) qsort(first, (size_t)(last - first), sizeof(T), cmp_##name); }
SORT(float, float) SORT(int, int32_t) SORT(uint, uint32_t) SORT(u64, uint64_t)

/* ---- shared_ptr control block: { vptr, long shared_owners, long shared_weak_owners } */
static void weak_release(Ctx *c) {                       /* __shared_weak_count::__release_weak() */
    uint64_t self = A0;
    volatile LONG64 *weak = (volatile LONG64 *)(uintptr_t)(self + 16);
    /* libc++: when the count is already 0 this is the last reference (no atomic write needed); otherwise
     * decrement atomically and destroy the block when it drops to -1 */
    if (*weak != 0 && InterlockedDecrement64(weak) != -1) return;
    uint64_t saved_lr = c->x[30];
    call_guest(c, rd64(rd64(self) + 4 * 8));             /* virtual __on_zero_shared_weak() */
    c->x[30] = saved_lr;
}
static void weak_lock(Ctx *c) {                          /* __shared_weak_count::lock() */
    uint64_t self = A0;
    volatile LONG64 *owners = (volatile LONG64 *)(uintptr_t)(self + 8);
    for (;;) {                                           /* take a strong reference unless the object is already gone */
        LONG64 seen = *owners;
        if (seen == -1) { RET(0); return; }
        if (InterlockedCompareExchange64(owners, seen + 1, seen) == seen) { RET(self); return; }
    }
}
static void keep(Ctx *c) { (void)c; }
static void ret0(Ctx *c) { RET(0); }
static void throw_trap(Ctx *c) { guest_trap("libc++ threw length_error / out_of_range", c->x[30] - 4); }

/* ---- clocks */
static void steady_now(Ctx *c) {                         /* nanoseconds */
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    /* whole seconds and the remainder separately: exact in 64 bits for any uptime */
    RET((uint64_t)(now.QuadPart / freq.QuadPart) * 1000000000ull + (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000000ull / (uint64_t)freq.QuadPart);
}
static void system_now(Ctx *c) {                         /* microseconds since 1970 */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    RET(t / 10 - 11644473600000000ULL);
}

/* ---- std::locale: there is only the classic "C" locale. A locale object is one pointer
 * to a shared implementation; every locale compares equal to the classic one, so number
 * formatting takes its fast (no digit grouping) path and never needs a numpunct facet. */
static uint8_t g_classic_locale_impl[64];
static uint64_t g_classic_locale;                       /* a std::locale object holding the impl pointer */
static void locale_ctor(Ctx *c) { wr64(A0, (uint64_t)(uintptr_t)g_classic_locale_impl); }
static void locale_classic(Ctx *c) { g_classic_locale = (uint64_t)(uintptr_t)g_classic_locale_impl; RET(&g_classic_locale); }
static void locale_eq(Ctx *c) { RET(1); }
static void locale_dtor(Ctx *c) { (void)c; }
/* locale::use_facet is in nncxxlib.c (ctype<char> and numpunct<char> facets of the classic locale) */

#define STR "NSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE"
static const ImportDef cxx_imports[] = {
    {"_Z" STR "6assignEPKc", str_assign_cstr}, {"_Z" STR "6assignEPKcm", str_assign_n},
    {"_Z" STR "6appendEPKc", str_append_cstr}, {"_Z" STR "6appendEPKcm", str_append_n},
    {"_Z" STR "9push_backEc", str_push_back},
    {"_Z" STR "C1ERKS5_", str_copy_ctor}, {"_Z" STR "C1ERKS5_mmRKS4_", str_substr_ctor},
    {"_Z" STR "D2Ev", str_dtor}, {"_Z" STR "aSERKS5_", str_copy_assign},
    {"_Z" STR "7reserveEm", str_reserve}, {"_Z" STR "6resizeEmc", str_resize}, {"_Z" STR "5eraseEmm", str_erase},
    {"_Z" STR "6insertEmPKc", str_insert_cstr}, {"_Z" STR "2atEm", str_at}, {"_Z" STR "9__grow_byEmmmmmm", str_grow_by},
    {"_ZNK" "St3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE4findEcm", str_find_char},
    {"_ZNK" "St3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5rfindEcm", str_rfind_char},
    {"_ZNK" "St3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEmmPKcm", str_compare},
    {"_ZNSt3__1plIcNS_11char_traitsIcEENS_9allocatorIcEEEENS_12basic_stringIT_T0_T1_EEPKS6_RKS9_", str_plus_cstr},
    {"_ZNSt3__19to_stringEi", str_to_string_int}, {"_ZNSt3__19to_stringEj", str_to_string_uint},
    {"_ZNSt3__112__next_primeEm", next_prime}, {"_ZNSt3__15alignEmmRPvRm", cxx_align},
    {"_ZNSt3__16__sortIRNS_6__lessIffEEPfEEvT0_S5_T_", sort_float}, {"_ZNSt3__16__sortIRNS_6__lessIiiEEPiEEvT0_S5_T_", sort_int},
    {"_ZNSt3__16__sortIRNS_6__lessIjjEEPjEEvT0_S5_T_", sort_uint}, {"_ZNSt3__16__sortIRNS_6__lessIyyEEPyEEvT0_S5_T_", sort_u64},
    {"_ZNSt3__119__shared_weak_count14__release_weakEv", weak_release}, {"_ZNSt3__119__shared_weak_count4lockEv", weak_lock},
    {"_ZNSt3__119__shared_weak_countD2Ev", keep}, {"_ZNSt3__114__shared_countD2Ev", keep},
    {"_ZNKSt3__119__shared_weak_count13__get_deleterERKSt9type_info", ret0},
    {"_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv", throw_trap},
    {"_ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv", throw_trap},
    {"_ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv", throw_trap},
    {"_ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv", throw_trap},
    {"_ZNSt3__16chrono12steady_clock3nowEv", steady_now}, {"_ZNSt3__16chrono12system_clock3nowEv", system_now},
    {"_ZNSt3__115recursive_mutexC2Ev", keep}, {"_ZNSt3__115recursive_mutexD2Ev", keep},
    {"_ZNSt3__115recursive_mutex8try_lockEv", keep}, {"_ZNSt3__15mutex8try_lockEv", keep},
    {"_ZNSt3__118condition_variable10notify_allEv", keep}, {"_ZNSt3__118condition_variable10notify_oneEv", keep},
    {"_ZNSt3__118condition_variableD1Ev", keep},
    {"pthread_mutexattr_init", ret0}, {"pthread_mutexattr_settype", ret0}, {"pthread_mutexattr_destroy", ret0},
    {"_ZNSt3__16localeC1Ev", locale_ctor}, {"_ZNSt3__16localeC1ERKS0_", locale_ctor}, {"_ZNSt3__16localeD1Ev", locale_dtor},
    {"_ZNSt3__16locale7classicEv", locale_classic}, {"_ZNKSt3__16localeeqERKS0_", locale_eq},
};

void register_cxx_imports(void) {
    register_imports(cxx_imports, (int)(sizeof cxx_imports / sizeof cxx_imports[0]));
}

/* C library additions: compiler-rt IEEE binary128 helpers (long double on AArch64), C stdio streams on top of
 * nn::fs, localeconv, gmtime, pthread_join.
 *
 * Only names that have no handler yet are installed (see register_libc2_imports).
 *
 * long double: AArch64 passes and returns binary128 in v0/v1 (q registers), so arguments are c->v[n] as 128 bits
 * and results go to c->v[0]. The arithmetic below is a self-contained soft-float (round to nearest even, the only
 * mode the game can be in: it never changes FPCR rounding through these helpers), tested by the self-test in
 * nncxxrt.c against vectors generated with exact rational arithmetic (gen_quad_tests.py -> quad_tests.h). */
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
#define RET(v) (c->x[0] = (uint64_t)(v))
#define STR(n) ((const char *)(uintptr_t)c->x[n])

/* ================================================================ binary128 soft-float */
typedef unsigned __int128 u128;
#define Q_BIAS 16383
#define Q_FRAC_MASK ((((u128)1) << 112) - 1)
#define Q_IMPLICIT (((u128)1) << 112)
#define Q_INF ((u128)0x7fff << 112)
#define Q_QNAN (Q_INF | ((u128)1 << 111))
#define Q_SIGN ((u128)1 << 127)

static int q_is_nan(u128 a) { return (a & ~Q_SIGN) > Q_INF; }
static int q_is_inf(u128 a) { return (a & ~Q_SIGN) == Q_INF; }
static int q_is_zero(u128 a) { return (a & ~Q_SIGN) == 0; }
static int clz128(u128 v) { uint64_t hi = (uint64_t)(v >> 64); return hi ? __builtin_clzll(hi) : 64 + (v ? __builtin_clzll((uint64_t)v) : 64); }

/* Rounds and packs: value = sig * 2^(exp - Q_BIAS - 115), sig normalised with its leading one at bit 115
 * (113 significand bits followed by guard, round, sticky). exp may be anything. */
static u128 q_pack(int sign, int exp, u128 sig) {
    if (exp <= 0) {                                   /* subnormal: shift right keeping a sticky bit */
        int shift = 1 - exp;
        if (shift > 116) sig = sig != 0;
        else { u128 lost = sig & (((u128)1 << shift) - 1); sig = (sig >> shift) | (lost != 0); }
        exp = 0;
    }
    unsigned grs = (unsigned)sig & 7;
    sig >>= 3;
    if (grs > 4 || (grs == 4 && (sig & 1))) sig++;
    if (exp == 0) { if (sig >> 112) exp = 1; }        /* rounded up into the normal range */
    else if (sig >> 113) { sig >>= 1; exp++; }
    if (exp >= 0x7fff) return ((u128)sign << 127) | Q_INF;
    return ((u128)sign << 127) | ((u128)exp << 112) | (sig & Q_FRAC_MASK);
}
/* Splits a finite non-zero value into exponent and a significand with the leading one at bit 112. */
static void q_unpack(u128 a, int *exp, u128 *sig) {
    int e = (int)((a >> 112) & 0x7fff);
    u128 m = a & Q_FRAC_MASK;
    if (e == 0) { int sh = clz128(m) - 15; m <<= sh; e = 1 - sh; }
    else m |= Q_IMPLICIT;
    *exp = e; *sig = m;
}

u128 q_add(u128 a, u128 b) {
    if (q_is_nan(a) || q_is_nan(b)) return Q_QNAN;
    int sa = (int)(a >> 127), sb = (int)(b >> 127);
    if (q_is_inf(a)) return q_is_inf(b) && sa != sb ? Q_QNAN : a;
    if (q_is_inf(b)) return b;
    if (q_is_zero(a)) return q_is_zero(b) ? (sa == sb ? a : 0) : b;
    if (q_is_zero(b)) return a;
    int ea, eb;
    u128 ma, mb;
    q_unpack(a, &ea, &ma); q_unpack(b, &eb, &mb);
    ma <<= 3; mb <<= 3;
    if (ea < eb || (ea == eb && ma < mb)) { int t = ea; ea = eb; eb = t; u128 m = ma; ma = mb; mb = m; t = sa; sa = sb; sb = t; }
    int d = ea - eb;
    if (d) { if (d > 116) mb = 1; else { u128 lost = mb & (((u128)1 << d) - 1); mb = (mb >> d) | (lost != 0); } }
    u128 m;
    if (sa == sb) {
        m = ma + mb;
        if (m >> 116) { m = (m >> 1) | (m & 1); ea++; }
    } else {
        m = ma - mb;
        if (m == 0) return 0;                          /* exact cancellation: +0 in round-to-nearest */
        int sh = clz128(m) - 12;                        /* bring the leading one back to bit 115 */
        m <<= sh; ea -= sh;
    }
    return q_pack(sa, ea, m);
}
u128 q_sub(u128 a, u128 b) { return q_is_nan(b) ? Q_QNAN : q_add(a, b ^ Q_SIGN); }

u128 q_mul(u128 a, u128 b) {
    if (q_is_nan(a) || q_is_nan(b)) return Q_QNAN;
    int sign = (int)((a ^ b) >> 127);
    if (q_is_inf(a) || q_is_inf(b)) return (q_is_zero(a) || q_is_zero(b)) ? Q_QNAN : ((u128)sign << 127) | Q_INF;
    if (q_is_zero(a) || q_is_zero(b)) return (u128)sign << 127;
    int ea, eb;
    u128 ma, mb;
    q_unpack(a, &ea, &ma); q_unpack(b, &eb, &mb);
    /* 113 x 113 -> 226 bits in four 64-bit limbs */
    uint64_t a0 = (uint64_t)ma, a1 = (uint64_t)(ma >> 64), b0 = (uint64_t)mb, b1 = (uint64_t)(mb >> 64);
    u128 p00 = (u128)a0 * b0, p01 = (u128)a0 * b1, p10 = (u128)a1 * b0, p11 = (u128)a1 * b1;
    u128 mid = (p00 >> 64) + (uint64_t)p01 + (uint64_t)p10;
    u128 lo = (uint64_t)p00 | (mid << 64);
    u128 hi = p11 + (p01 >> 64) + (p10 >> 64) + (mid >> 64);
    /* product = hi:lo, leading one at bit 224 or 225; keep 116 bits (to bit 115) plus sticky */
    int top = (hi >> 97) ? 225 : 224;                   /* bit 225 of the product is bit 97 of hi */
    int shift = top - 115;                              /* 109 or 110 */
    u128 m = (hi << (128 - shift)) | (lo >> shift);
    if (lo & (((u128)1 << shift) - 1)) m |= 1;
    return q_pack(sign, ea + eb - Q_BIAS + (top - 224), m);
}
u128 q_div(u128 a, u128 b) {
    if (q_is_nan(a) || q_is_nan(b)) return Q_QNAN;
    int sign = (int)((a ^ b) >> 127);
    if (q_is_inf(a)) return q_is_inf(b) ? Q_QNAN : ((u128)sign << 127) | Q_INF;
    if (q_is_inf(b)) return (u128)sign << 127;
    if (q_is_zero(b)) return q_is_zero(a) ? Q_QNAN : ((u128)sign << 127) | Q_INF;
    if (q_is_zero(a)) return (u128)sign << 127;
    int ea, eb;
    u128 ma, mb;
    q_unpack(a, &ea, &ma); q_unpack(b, &eb, &mb);
    int exp = ea - eb + Q_BIAS;
    if (ma < mb) { ma <<= 1; exp--; }                   /* now 1 <= ma/mb < 2 */
    u128 q = 0, rem = ma;
    for (int i = 0; i < 116; i++) {                     /* 116 quotient bits, leading one at bit 115 */
        q <<= 1;
        if (rem >= mb) { rem -= mb; q |= 1; }
        rem <<= 1;
    }
    if (rem) q |= 1;
    return q_pack(sign, exp, q);
}
/* -1, 0, 1, or 2 when unordered */
int q_cmp(u128 a, u128 b) {
    if (q_is_nan(a) || q_is_nan(b)) return 2;
    if (q_is_zero(a) && q_is_zero(b)) return 0;
    int sa = (int)(a >> 127), sb = (int)(b >> 127);
    if (sa != sb) return sa ? -1 : 1;
    if (a == b) return 0;
    return ((a < b) != sa) ? -1 : 1;
}
u128 q_from_u64(uint64_t v, int sign) {
    if (!v) return 0;
    int sh = __builtin_clzll(v);
    u128 m = (u128)(v << sh) << 52;                     /* leading one at bit 115 */
    return q_pack(sign, Q_BIAS + 63 - sh, m);
}
u128 q_from_i64(int64_t v) { return v < 0 ? q_from_u64((uint64_t)0 - (uint64_t)v, 1) : q_from_u64((uint64_t)v, 0); }
u128 q_from_f64(double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    int sign = (int)(bits >> 63), e = (int)((bits >> 52) & 0x7ff);
    uint64_t f = bits & 0xfffffffffffffull;
    if (e == 0x7ff) return ((u128)sign << 127) | Q_INF | (f ? ((u128)1 << 111) | ((u128)f << 60) : 0);
    if (e == 0) {
        if (!f) return (u128)sign << 127;
        int sh = __builtin_clzll(f) - 11;
        f <<= sh; e = 1 - sh; f &= 0xfffffffffffffull;
    }
    return ((u128)sign << 127) | ((u128)(e - 1023 + Q_BIAS) << 112) | ((u128)f << 60);
}
u128 q_from_f32(float f) { return q_from_f64((double)f); }       /* float -> double is exact */
/* Narrowing: rounds the 113-bit significand to `bits` (53 or 24) with the target's exponent range. */
static uint64_t q_narrow(u128 a, int bits, int ebits) {
    int sign = (int)(a >> 127), emax = (1 << ebits) - 1, bias = emax >> 1;
    uint64_t s = (uint64_t)sign << (bits - 1 + ebits);
    if (q_is_nan(a)) return s | ((uint64_t)emax << (bits - 1)) | (1ull << (bits - 2)) | (uint64_t)((a & Q_FRAC_MASK) >> (113 - bits));
    if (q_is_inf(a)) return s | ((uint64_t)emax << (bits - 1));
    if (q_is_zero(a)) return s;
    int e;
    u128 m;
    q_unpack(a, &e, &m);
    e = e - Q_BIAS + bias;
    /* keep bits+3 bits: leading one at bit (bits+2), then guard/round/sticky */
    int shift = 113 - (bits + 3);
    u128 lost = m & (((u128)1 << shift) - 1);
    uint64_t sig = (uint64_t)(m >> shift) | (lost != 0);
    if (e <= 0) {
        int sh = 1 - e;
        if (sh > bits + 3) sig = sig != 0;
        else { uint64_t l = sig & ((1ull << sh) - 1); sig = (sig >> sh) | (l != 0); }
        e = 0;
    }
    unsigned grs = (unsigned)sig & 7;
    sig >>= 3;
    if (grs > 4 || (grs == 4 && (sig & 1))) sig++;
    if (e == 0) { if (sig >> (bits - 1)) e = 1; }
    else if (sig >> bits) { sig >>= 1; e++; }
    if (e >= emax) return s | ((uint64_t)emax << (bits - 1));
    return s | ((uint64_t)e << (bits - 1)) | (sig & ((1ull << (bits - 1)) - 1));
}
double q_to_f64(u128 a) { uint64_t b = q_narrow(a, 53, 11); double d; memcpy(&d, &b, 8); return d; }
float q_to_f32(u128 a) { uint32_t b = (uint32_t)q_narrow(a, 24, 8); float f; memcpy(&f, &b, 4); return f; }
/* Conversions to integer truncate toward zero; out-of-range values saturate as compiler-rt does. */
int64_t q_to_i64(u128 a) {
    if (q_is_nan(a)) return 0;
    int sign = (int)(a >> 127), e = (int)((a >> 112) & 0x7fff) - Q_BIAS;
    if (e < 0) return 0;
    if (e >= 63) return sign ? INT64_MIN : INT64_MAX;
    uint64_t v = (uint64_t)(((a & Q_FRAC_MASK) | Q_IMPLICIT) >> (112 - e));
    return sign ? -(int64_t)v : (int64_t)v;
}
uint64_t q_to_u64(u128 a) {
    if (q_is_nan(a)) return 0;
    int sign = (int)(a >> 127), e = (int)((a >> 112) & 0x7fff) - Q_BIAS;
    if (e < 0 || sign) return 0;
    if (e >= 64) return ~0ull;
    return (uint64_t)(((a & Q_FRAC_MASK) | Q_IMPLICIT) >> (112 - e));
}
void quad_from_double(double d, uint64_t out[2]) { u128 q = q_from_f64(d); out[0] = (uint64_t)q; out[1] = (uint64_t)(q >> 64); }
double quad_to_double(const uint64_t in[2]) { return q_to_f64(((u128)in[1] << 64) | in[0]); }

static u128 qarg(Ctx *c, int n) { return ((u128)c->v[n].d[1] << 64) | c->v[n].d[0]; }
static void qret(Ctx *c, u128 v) { c->v[0].d[0] = (uint64_t)v; c->v[0].d[1] = (uint64_t)(v >> 64); }
static void tf_add(Ctx *c) { qret(c, q_add(qarg(c, 0), qarg(c, 1))); }
static void tf_sub(Ctx *c) { qret(c, q_sub(qarg(c, 0), qarg(c, 1))); }
static void tf_mul(Ctx *c) { qret(c, q_mul(qarg(c, 0), qarg(c, 1))); }
static void tf_div(Ctx *c) { qret(c, q_div(qarg(c, 0), qarg(c, 1))); }
static void tf_neg(Ctx *c) { qret(c, qarg(c, 0) ^ Q_SIGN); }
static void tf_floatdi(Ctx *c) { qret(c, q_from_i64((int64_t)A0)); }
static void tf_floatsi(Ctx *c) { qret(c, q_from_i64((int64_t)(int32_t)A0)); }
static void tf_floatundi(Ctx *c) { qret(c, q_from_u64(A0, 0)); }
static void tf_floatunsi(Ctx *c) { qret(c, q_from_u64((uint32_t)A0, 0)); }
static void tf_extenddf(Ctx *c) { qret(c, q_from_f64(c->v[0].df[0])); }
static void tf_extendsf(Ctx *c) { qret(c, q_from_f32(c->v[0].f[0])); }
static void tf_truncdf(Ctx *c) { double r = q_to_f64(qarg(c, 0)); vzero(&c->v[0]); c->v[0].df[0] = r; }
static void tf_truncsf(Ctx *c) { float r = q_to_f32(qarg(c, 0)); vzero(&c->v[0]); c->v[0].f[0] = r; }
static void tf_fixdi(Ctx *c) { RET(q_to_i64(qarg(c, 0))); }
static void tf_fixsi(Ctx *c) { int64_t v = q_to_i64(qarg(c, 0)); RET((int64_t)(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v)); }
static void tf_fixunsdi(Ctx *c) { RET(q_to_u64(qarg(c, 0))); }
static void tf_fixunssi(Ctx *c) { uint64_t v = q_to_u64(qarg(c, 0)); RET(v > 0xffffffffull ? 0xffffffffull : v); }
/* compiler-rt comparison conventions: __letf2 family returns 1 when unordered, __getf2 family returns -1 */
static void tf_cmp_le(Ctx *c) { int r = q_cmp(qarg(c, 0), qarg(c, 1)); RET((int64_t)(r == 2 ? 1 : r)); }      /* __letf2 __lttf2 __eqtf2 __netf2 __cmptf2 */
static void tf_cmp_ge(Ctx *c) { int r = q_cmp(qarg(c, 0), qarg(c, 1)); RET((int64_t)(r == 2 ? -1 : r)); }     /* __getf2 __gttf2 */
static void tf_unord(Ctx *c) { RET(q_cmp(qarg(c, 0), qarg(c, 1)) == 2); }

/* ================================================================ stdio streams
 * A guest FILE * is a pointer to the host structure below (the game only passes it back to these functions).
 * Files are opened through the registered nn::fs handlers, so "rom:/..." and the other mounts resolve exactly
 * as they do for the game's own file access. Reading only: the game's fopen users are Lua's loadfile/io library. */
typedef struct {
    uint32_t magic;                  /* 'FILE' */
    int kind;                        /* 0 nn::fs file, 1 stdin, 2 stdout, 3 stderr */
    uint64_t handle;
    int64_t pos, size;
    int eof, err, open, ungot;       /* ungot: -1 or a pushed-back byte */
} GFile;
#define GFILE_MAGIC 0x454c4946u
static GFile g_std[3] = {{GFILE_MAGIC, 1, 0, 0, 0, 0, 0, 1, -1}, {GFILE_MAGIC, 2, 0, 0, 0, 0, 0, 1, -1}, {GFILE_MAGIC, 3, 0, 0, 0, 0, 0, 1, -1}};
static uint64_t g_std_ptr[3];        /* the objects behind the imported `stdin`, `stdout`, `stderr` (FILE * variables) */
#define FS_OPEN "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"
#define FS_CLOSE "_ZN2nn2fs9CloseFileENS0_10FileHandleE"
#define FS_READ "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm"
#define FS_SIZE "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE"

static uint64_t fs_call(Ctx *c, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    ImportFn fn = import_lookup(name);
    if (!fn) { printf("\nnnlibc2: %s has no handler, needed by stdio\n", name); report_and_exit(3); }
    uint64_t save[5] = {c->x[0], c->x[1], c->x[2], c->x[3], c->x[4]};
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4;
    fn(c);
    uint64_t r = c->x[0];
    c->x[1] = save[1]; c->x[2] = save[2]; c->x[3] = save[3]; c->x[4] = save[4];
    (void)save[0];
    return r;
}
static GFile *gfile(uint64_t p) {
    GFile *f = (GFile *)(uintptr_t)p;
    return f && f->magic == GFILE_MAGIC ? f : NULL;
}
static int gfile_open(Ctx *c, GFile *f, const char *path, const char *mode) {
    uint64_t handle = 0, size = 0;
    if (!path || !mode || mode[0] != 'r' || strchr(mode, '+')) {       /* writing through stdio is not provided */
        trace_printf("     fopen(\"%s\", \"%s\"): only read modes are supported -> NULL\n", path ? path : "(null)", mode ? mode : "(null)");
        return 0;
    }
    if ((uint32_t)fs_call(c, FS_OPEN, (uint64_t)(uintptr_t)&handle, (uint64_t)(uintptr_t)path, 1 /* OpenMode_Read */, 0, 0) != 0) return 0;
    fs_call(c, FS_SIZE, (uint64_t)(uintptr_t)&size, handle, 0, 0, 0);
    f->magic = GFILE_MAGIC; f->kind = 0; f->handle = handle; f->pos = 0; f->size = (int64_t)size;
    f->eof = f->err = 0; f->open = 1; f->ungot = -1;
    return 1;
}
static void gfile_close(Ctx *c, GFile *f) {
    if (f->kind == 0 && f->open) fs_call(c, FS_CLOSE, f->handle, 0, 0, 0, 0);
    f->open = 0;
}
static size_t gfile_read(Ctx *c, GFile *f, void *dst, size_t n) {
    size_t done = 0;
    if (!n) return 0;
    if (f->ungot >= 0) { *(uint8_t *)dst = (uint8_t)f->ungot; f->ungot = -1; done = 1; }
    if (f->kind != 0 || !f->open) { if (done < n) f->eof = 1; return done; }      /* stdin has no data */
    if (done < n) {
        uint64_t got = 0;
        int64_t want = (int64_t)(n - done);
        if (want > f->size - f->pos) want = f->size - f->pos;
        if (want > 0) {
            if ((uint32_t)fs_call(c, FS_READ, (uint64_t)(uintptr_t)&got, f->handle, (uint64_t)f->pos, (uint64_t)(uintptr_t)((uint8_t *)dst + done), (uint64_t)want) != 0) { f->err = 1; got = 0; }
            f->pos += (int64_t)got;
            done += (size_t)got;
        }
        if (done < n && !f->err) f->eof = 1;
    }
    return done;
}
static void c_fopen(Ctx *c) {
    const char *path = STR(0), *mode = STR(1);
    GFile *f = calloc(1, sizeof *f);
    if (!gfile_open(c, f, path, mode)) { free(f); RET(0); return; }
    RET(f);
}
static void c_freopen(Ctx *c) {                         /* (path, mode, stream): Lua reopens a text file as binary */
    const char *path = STR(0), *mode = STR(1);
    GFile *f = gfile(A2);
    if (!f) { RET(0); return; }
    if (!path) { f->pos = 0; f->eof = f->err = 0; f->ungot = -1; RET(f); return; }   /* mode change only: restart */
    char copy[1024];
    snprintf(copy, sizeof copy, "%s", path);
    gfile_close(c, f);
    if (!gfile_open(c, f, copy, mode)) { if (f != &g_std[0] && f != &g_std[1] && f != &g_std[2]) { f->magic = 0; free(f); } RET(0); return; }
    RET(f);
}
static void c_fclose(Ctx *c) {
    GFile *f = gfile(A0);
    if (!f) { RET(-1); return; }
    gfile_close(c, f);
    if (f != &g_std[0] && f != &g_std[1] && f != &g_std[2]) { f->magic = 0; free(f); }
    RET(0);
}
static void c_fread(Ctx *c) {                           /* (ptr, size, nmemb, stream) -> complete items read */
    GFile *f = gfile(A3);
    uint64_t size = A1, count = A2;
    if (!f || !size || !count) { RET(0); return; }
    size_t got = gfile_read(c, f, (void *)(uintptr_t)A0, (size_t)(size * count));
    RET(got / size);
}
static void c_getc(Ctx *c) {
    GFile *f = gfile(A0);
    uint8_t ch;
    if (!f || gfile_read(c, f, &ch, 1) != 1) { RET(0xffffffffull); return; }       /* EOF = -1 as an int */
    RET(ch);
}
static void c_ungetc(Ctx *c) {                          /* (ch, stream) */
    GFile *f = gfile(A1);
    if (!f || (int32_t)A0 == -1 || f->ungot >= 0) { RET(0xffffffffull); return; }
    f->ungot = (int)(uint8_t)A0; f->eof = 0;
    RET((uint8_t)A0);
}
static void c_feof(Ctx *c) { GFile *f = gfile(A0); RET(f ? f->eof : 0); }
static void c_ferror(Ctx *c) { GFile *f = gfile(A0); RET(f ? f->err : 0); }
static void c_fflush(Ctx *c) { RET(0); }               /* output streams go to the trace unbuffered (see clib.c fprintf) */

/* ================================================================ localeconv: the "C" locale, musl's struct lconv */
static struct {
    uint64_t decimal_point, thousands_sep, grouping, int_curr_symbol, currency_symbol, mon_decimal_point,
             mon_thousands_sep, mon_grouping, positive_sign, negative_sign;
    uint8_t chars[14];               /* int_frac_digits ... int_n_sign_posn: all CHAR_MAX (char is unsigned on AArch64) */
} g_lconv;
static void c_localeconv(Ctx *c) {
    static const char dot[] = ".", empty[] = "";
    if (!g_lconv.decimal_point) {
        g_lconv.thousands_sep = g_lconv.grouping = g_lconv.int_curr_symbol = g_lconv.currency_symbol = g_lconv.mon_decimal_point =
            g_lconv.mon_thousands_sep = g_lconv.mon_grouping = g_lconv.positive_sign = g_lconv.negative_sign = (uint64_t)(uintptr_t)empty;
        memset(g_lconv.chars, 0xff, sizeof g_lconv.chars);
        g_lconv.decimal_point = (uint64_t)(uintptr_t)dot;
    }
    RET(&g_lconv);
}

/* ================================================================ gmtime / gmtime_r
 * musl struct tm: nine ints (sec, min, hour, mday, mon, year, wday, yday, isdst), long gmtoff at +40, const char *zone at +48. */
int libc2_gmtime(int64_t t, int32_t tm[9]) {
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    if (days < -1000000000ll || days > 1000000000ll) return 0;        /* year would overflow an int */
    tm[2] = (int32_t)(rem / 3600); tm[1] = (int32_t)(rem % 3600 / 60); tm[0] = (int32_t)(rem % 60);
    int64_t wd = (days + 4) % 7;                                      /* 1970-01-01 was a Thursday */
    tm[6] = (int32_t)(wd < 0 ? wd + 7 : wd);
    /* civil date from day count (proleptic Gregorian), era-based */
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    tm[3] = (int32_t)d; tm[4] = (int32_t)(m - 1); tm[5] = (int32_t)(y - 1900);
    tm[7] = cum[m - 1] + (int32_t)d - 1 + (leap && m > 2);
    tm[8] = 0;
    return 1;
}
static void tm_store(uint64_t dst, const int32_t tm[9]) {
    static const char utc[] = "UTC";
    memcpy((void *)(uintptr_t)dst, tm, 36);
    wr32(dst + 36, 0);
    wr64(dst + 40, 0);
    wr64(dst + 48, (uint64_t)(uintptr_t)utc);
}
static void c_gmtime(Ctx *c) {
    static __declspec(thread) uint8_t buf[56];
    int32_t tm[9];
    if (!libc2_gmtime((int64_t)rd64(A0), tm)) { RET(0); return; }
    tm_store((uint64_t)(uintptr_t)buf, tm);
    RET(buf);
}
static void c_gmtime_r(Ctx *c) {
    int32_t tm[9];
    if (!libc2_gmtime((int64_t)rd64(A0), tm)) { RET(0); return; }
    tm_store(A1, tm);
    RET(A1);
}

/* ================================================================ pthread_join
 * The game imports pthread_join but NOT pthread_create (nor any other way to obtain a pthread_t): the single caller,
 * 0x710399f994, is the thread-pool teardown of a statically linked library (it joins pool->threads[i] after
 * broadcasting two condition variables), and that pool can never have started a thread. No pthread_t can therefore
 * exist; a call with a non-null handle is reported in the trace and answered with ESRCH. */
/* Since the Skyline plugins (nnro.c) the process can hold threads of their pthread_create (os_threads.c): those are joined
 * there; any other handle is still answered ESRCH. */
int os_pthread_join(uint64_t t, uint64_t retval_out);    /* os_threads.c: errno, or -1 when t is not a pthread_create thread */
static void c_pthread_join(Ctx *c) {
    int r = os_pthread_join(A0, A1);
    if (r >= 0) { RET(r); return; }
    if (A0) trace_printf("     pthread_join(%llx): not a thread of pthread_create (the game itself never creates one) -> ESRCH\n", (unsigned long long)A0);
    if (A0 && A1) wr64(A1, 0);
    RET(3);
}

static const ImportDef libc2_imports[] = {
    {"__addtf3", tf_add}, {"__subtf3", tf_sub}, {"__multf3", tf_mul}, {"__divtf3", tf_div}, {"__negtf2", tf_neg},
    {"__floatditf", tf_floatdi}, {"__floatsitf", tf_floatsi}, {"__floatunditf", tf_floatundi}, {"__floatunsitf", tf_floatunsi},
    {"__extenddftf2", tf_extenddf}, {"__extendsftf2", tf_extendsf}, {"__trunctfdf2", tf_truncdf}, {"__trunctfsf2", tf_truncsf},
    {"__fixtfdi", tf_fixdi}, {"__fixtfsi", tf_fixsi}, {"__fixunstfdi", tf_fixunsdi}, {"__fixunstfsi", tf_fixunssi},
    {"__letf2", tf_cmp_le}, {"__lttf2", tf_cmp_le}, {"__eqtf2", tf_cmp_le}, {"__netf2", tf_cmp_le}, {"__cmptf2", tf_cmp_le},
    {"__getf2", tf_cmp_ge}, {"__gttf2", tf_cmp_ge}, {"__unordtf2", tf_unord},
    {"fopen", c_fopen}, {"freopen", c_freopen}, {"fclose", c_fclose}, {"fread", c_fread}, {"getc", c_getc}, {"fgetc", c_getc},
    {"ungetc", c_ungetc}, {"feof", c_feof}, {"ferror", c_ferror}, {"fflush", c_fflush},
    {"localeconv", c_localeconv}, {"gmtime", c_gmtime}, {"gmtime_r", c_gmtime_r}, {"pthread_join", c_pthread_join},
};

/* Called before runtime_init (from cxxrt_install_data): the imported FILE * variables. */
void libc2_install_data(void) {
    for (int i = 0; i < 3; i++) g_std_ptr[i] = (uint64_t)(uintptr_t)&g_std[i];
    import_set_address("stdin", &g_std_ptr[0]);
    import_set_address("stdout", &g_std_ptr[1]);
    import_set_address("stderr", &g_std_ptr[2]);
}

void register_libc2_imports(void) {
    int n = (int)(sizeof libc2_imports / sizeof libc2_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_lookup(libc2_imports[i].name)) register_imports(&libc2_imports[i], 1);
}

#include "nnlibc2_c5b.inc"

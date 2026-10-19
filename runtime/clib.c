/* C library functions the game imports from the system module: formatted output
 * (one engine for the whole printf family, reading AArch64 variadic arguments),
 * searching/sorting with guest comparators, and small string/ctype/math helpers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
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
#define STR(n) ((const char *)(uintptr_t)c->x[n])

/* ---------------------------------------------------------------- variadic argument sources
 * Direct call  f(named..., ...): integers continue in x<first_gp>..x7 then the stack,
 * floating point in v0..v7 then the stack.
 * va_list (AAPCS64): { void *stack; void *gr_top; void *vr_top; int gr_offs; int vr_offs; }. */
typedef struct {
    Ctx *c;
    int is_va;
    int gp, fp;                  /* next x / v register for a direct call */
    uint64_t stack;              /* next stack argument */
    uint64_t gr_top, vr_top;     /* va_list register save areas */
    int32_t gr_offs, vr_offs;
} Args;

static void args_direct(Args *a, Ctx *c, int first_gp) {
    memset(a, 0, sizeof *a);
    a->c = c; a->gp = first_gp; a->stack = c->x[31];
}
static void args_va(Args *a, Ctx *c, uint64_t va) {
    memset(a, 0, sizeof *a);
    a->c = c; a->is_va = 1;
    a->stack = rd64(va); a->gr_top = rd64(va + 8); a->vr_top = rd64(va + 16);
    a->gr_offs = (int32_t)rd32(va + 24); a->vr_offs = (int32_t)rd32(va + 28);
}
static uint64_t arg_int(Args *a) {
    uint64_t v;
    if (a->is_va) {
        if (a->gr_offs < 0) { v = rd64(a->gr_top + (uint64_t)(int64_t)a->gr_offs); a->gr_offs += 8; return v; }
    } else if (a->gp < 8) return a->c->x[a->gp++];
    v = rd64(a->stack);
    a->stack += 8;
    return v;
}
static double arg_double(Args *a) {
    double v;
    if (a->is_va) {
        if (a->vr_offs < 0) { memcpy(&v, (void *)(uintptr_t)(a->vr_top + (uint64_t)(int64_t)a->vr_offs), 8); a->vr_offs += 16; return v; }
    } else if (a->fp < 8) return a->c->v[a->fp++].df[0];
    memcpy(&v, (void *)(uintptr_t)a->stack, 8);
    a->stack += 8;
    return v;
}
/* long double (binary128): a whole q register, or 16 bytes on the stack aligned to 16 */
double quad_to_double(const uint64_t in[2]);                 /* nnlibc2.c */
static double arg_long_double(Args *a) {
    uint64_t q[2];
    if (a->is_va && a->vr_offs < 0) { memcpy(q, (void *)(uintptr_t)(a->vr_top + (uint64_t)(int64_t)a->vr_offs), 16); a->vr_offs += 16; }
    else if (!a->is_va && a->fp < 8) { q[0] = a->c->v[a->fp].d[0]; q[1] = a->c->v[a->fp].d[1]; a->fp++; }
    else { a->stack = (a->stack + 15) & ~15ull; memcpy(q, (void *)(uintptr_t)a->stack, 16); a->stack += 16; }
    return quad_to_double(q);
}

/* Formats into out (at most size bytes including the terminator); returns the full length. */
static int guest_format(char *out, size_t size, const char *fmt, Args *a) {
    size_t n = 0;
#define PUT(ch) do { if (n + 1 < size) out[n] = (char)(ch); n++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        if (p[1] == '%') { PUT('%'); p++; continue; }
        char spec[48], piece[512];
        size_t k = 0;
        spec[k++] = *p++;
        while (*p && strchr("-+ #0", *p) && k < 40) spec[k++] = *p++;
        if (*p == '*') { k += (size_t)snprintf(spec + k, 12, "%d", (int)arg_int(a)); p++; }
        else while (isdigit((unsigned char)*p) && k < 40) spec[k++] = *p++;
        if (*p == '.') {
            spec[k++] = *p++;
            if (*p == '*') { k += (size_t)snprintf(spec + k, 12, "%d", (int)arg_int(a)); p++; }
            else while (isdigit((unsigned char)*p) && k < 40) spec[k++] = *p++;
        }
        int longs = 0, halves = 0, quad = 0;
        while (*p && strchr("hlLqjzt", *p)) {
            if (*p == 'l' || *p == 'q' || *p == 'j' || *p == 'z' || *p == 't') longs++;
            else if (*p == 'h') halves++;
            else quad = 1;
            p++;
        }
        char conv = *p;
        if (!conv) break;
        piece[0] = 0;
        if (strchr("diuxXoc", conv)) {
            uint64_t v = arg_int(a);
            if (conv == 'c') { spec[k++] = 'c'; spec[k] = 0; snprintf(piece, sizeof piece, spec, (int)v); }
            else {
                spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv; spec[k] = 0;
                int sg = conv == 'd' || conv == 'i';
                if (longs) ;
                else if (halves >= 2) v = sg ? (uint64_t)(int64_t)(int8_t)v : (uint64_t)(uint8_t)v;
                else if (halves == 1) v = sg ? (uint64_t)(int64_t)(int16_t)v : (uint64_t)(uint16_t)v;
                else v = sg ? (uint64_t)(int64_t)(int32_t)v : (uint64_t)(uint32_t)v;
                snprintf(piece, sizeof piece, spec, (unsigned long long)v);
            }
        } else if (strchr("fFeEgGaA", conv)) {
            spec[k++] = conv; spec[k] = 0;
            /* a long double is formatted from its nearest double (at most 17 significant digits are exact) */
            snprintf(piece, sizeof piece, spec, quad ? arg_long_double(a) : arg_double(a));
        } else if (conv == 's') {
            const char *s = (const char *)(uintptr_t)arg_int(a);
            char *narrow = NULL;
            if (longs && s) {                                 /* %ls: wchar_t is 32 bits on the target; written as UTF-8 */
                const uint32_t *w = (const uint32_t *)s;
                size_t wn = 0, o = 0;
                while (w[wn]) wn++;
                narrow = malloc(wn * 4 + 1);
                for (size_t i = 0; i < wn; i++) {
                    uint32_t u = w[i];
                    if (u < 0x80) narrow[o++] = (char)u;
                    else if (u < 0x800) { narrow[o++] = (char)(0xc0 | (u >> 6)); narrow[o++] = (char)(0x80 | (u & 0x3f)); }
                    else if (u < 0x10000) { narrow[o++] = (char)(0xe0 | (u >> 12)); narrow[o++] = (char)(0x80 | ((u >> 6) & 0x3f)); narrow[o++] = (char)(0x80 | (u & 0x3f)); }
                    else { narrow[o++] = (char)(0xf0 | (u >> 18)); narrow[o++] = (char)(0x80 | ((u >> 12) & 0x3f)); narrow[o++] = (char)(0x80 | ((u >> 6) & 0x3f)); narrow[o++] = (char)(0x80 | (u & 0x3f)); }
                }
                narrow[o] = 0;
                s = narrow;
            }
            spec[k++] = 's'; spec[k] = 0;
            int len = snprintf(NULL, 0, spec, s ? s : "(null)");
            if (len >= 0) {
                char *big = len < (int)sizeof piece ? piece : malloc((size_t)len + 1);
                snprintf(big, (size_t)len + 1, spec, s ? s : "(null)");
                for (int i = 0; i < len; i++) PUT(big[i]);
                if (big != piece) free(big);
            }
            free(narrow);
            continue;
        } else if (conv == 'p') {
            /* musl: %p is %#.16lx -- "0x" and 16 hex digits, a null pointer as 16 zeros; only the width and '-' apply */
            char hex[24];
            unsigned long long pv = (unsigned long long)arg_int(a);
            int left = 0;
            const char *z = spec + 1;
            spec[k] = 0;
            while (*z && strchr("-+ #0", *z)) { if (*z == '-') left = 1; z++; }
            snprintf(hex, sizeof hex, pv ? "0x%016llx" : "%016llx", pv);
            snprintf(piece, sizeof piece, left ? "%-*s" : "%*s", atoi(z), hex);
        } else if (conv == 'n') {
            uint64_t dst = arg_int(a);                        /* characters written so far */
            if (dst) { if (longs) wr64(dst, n); else if (halves >= 2) wr8(dst, (uint8_t)n); else if (halves == 1) wr16(dst, (uint16_t)n); else wr32(dst, (uint32_t)n); }
        } else {
            PUT('%'); PUT(conv);
            continue;
        }
        for (const char *q = piece; *q; q++) PUT(*q);
    }
#undef PUT
    if (size) out[n < size ? n : size - 1] = 0;
    return (int)n;
}

static void c_sprintf(Ctx *c) { Args a; args_direct(&a, c, 2); RET((int64_t)guest_format((char *)(uintptr_t)A0, 1u << 20, STR(1), &a)); }
static void c_snprintf(Ctx *c) { Args a; args_direct(&a, c, 3); RET((int64_t)guest_format((char *)(uintptr_t)A0, (size_t)A1, STR(2), &a)); }
static void c_vsnprintf(Ctx *c) { Args a; args_va(&a, c, A3); RET((int64_t)guest_format((char *)(uintptr_t)A0, (size_t)A1, STR(2), &a)); }
static void c_printf(Ctx *c) {
    char buf[2048];
    Args a;
    args_direct(&a, c, 1);
    int n = guest_format(buf, sizeof buf, STR(0), &a);
    trace_printf("     guest printf: %s", buf);
    RET((int64_t)n);
}
static void c_fprintf(Ctx *c) {
    char buf[2048];
    Args a;
    args_direct(&a, c, 2);
    int n = guest_format(buf, sizeof buf, STR(1), &a);
    trace_printf("     guest fprintf: %s", buf);
    RET((int64_t)n);
}

/* ---------------------------------------------------------------- formatted input (sscanf family)
 * One engine with the C99 / musl rules: white space in the format skips any white space, other characters
 * must match, conversions d i u o x X p (with base prefixes), a e f g (decimal, hex floats, inf, nan),
 * s, c, [set] (ranges, ^, leading ]), n, %%; `*` suppression, field widths, hh h l ll j z t L modifiers.
 * Returns the number of assignments, or -1 when the input ends before the first conversion completes.
 * Every destination is a guest pointer taken from the AArch64 variadic arguments. */
void quad_from_double(double d, uint64_t out[2]);            /* nnlibc2.c: long double is IEEE binary128 on AArch64 */

static int scan_float_token(const unsigned char *s, int width, char *tok, int tokmax) {
    /* longest prefix of s (at most width characters) that is a valid strtod subject; returns its length */
    int i = 0, n = 0, digits = 0;
#define TAKE() do { if (n < tokmax - 1) tok[n++] = (char)s[i]; i++; } while (0)
    if (i < width && (s[i] == '+' || s[i] == '-')) TAKE();
    if (i + 2 < width + 0 && (s[i] | 0x20) == 'i' && (s[i + 1] | 0x20) == 'n' && (s[i + 2] | 0x20) == 'f') {
        TAKE(); TAKE(); TAKE();
        if (i + 4 < width && _strnicmp((const char *)s + i, "inity", 5) == 0) { TAKE(); TAKE(); TAKE(); TAKE(); TAKE(); }
        tok[n] = 0; return i;
    }
    if (i + 2 < width && (s[i] | 0x20) == 'n' && (s[i + 1] | 0x20) == 'a' && (s[i + 2] | 0x20) == 'n') {
        TAKE(); TAKE(); TAKE();
        if (i < width && s[i] == '(') {
            int j = i + 1;
            while (j < width && (isalnum(s[j]) || s[j] == '_')) j++;
            if (j < width && s[j] == ')') i = j + 1;                 /* the payload is accepted and ignored */
        }
        tok[n] = 0; return i;
    }
    int hex = 0;
    if (i + 1 < width && s[i] == '0' && (s[i + 1] | 0x20) == 'x' &&
        (isxdigit(s[i + 2]) || (s[i + 2] == '.' && isxdigit(s[i + 3]))) && i + 2 < width) { TAKE(); TAKE(); hex = 1; }
    while (i < width && (hex ? isxdigit(s[i]) : isdigit(s[i]))) { TAKE(); digits++; }
    if (i < width && s[i] == '.') {
        int save_i = i, save_n = n;
        TAKE();
        int frac = 0;
        while (i < width && (hex ? isxdigit(s[i]) : isdigit(s[i]))) { TAKE(); frac++; }
        if (!digits && !frac) { i = save_i; n = save_n; }
        digits += frac;
    }
    if (!digits) { tok[0] = 0; return 0; }
    if (i < width && (s[i] | 0x20) == (hex ? 'p' : 'e')) {
        int j = i + 1;
        if (j < width && (s[j] == '+' || s[j] == '-')) j++;
        if (j < width && isdigit(s[j])) {
            while (i < j) TAKE();
            while (i < width && isdigit(s[i])) TAKE();
        }
    }
#undef TAKE
    tok[n] = 0;
    return i;
}

static int guest_scan(const char *input, const char *fmt, Args *a) {
    const unsigned char *s = (const unsigned char *)input, *s0 = s;
    const unsigned char *p = (const unsigned char *)fmt;
    int matches = 0, input_failure = 0;
    for (; *p; p++) {
        if (isspace(*p)) {
            while (isspace(p[1])) p++;
            while (isspace(*s)) s++;
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            if (*p == '%') { p++; while (isspace(*s)) s++; }
            if (!*s) { input_failure = 1; break; }
            if (*s != *p) break;
            s++;
            continue;
        }
        p++;
        int suppress = 0, width = 0, have_width = 0, size = 0;       /* size: -2 hh, -1 h, 0, 1 l, 2 ll/j/z/t, 3 L */
        if (*p == '*') { suppress = 1; p++; }
        while (isdigit(*p)) { width = width * 10 + (*p - '0'); have_width = 1; p++; }
        if (*p == 'm') p++;                                          /* allocating conversions are not supported: treated as plain */
        for (;; p++) {
            if (*p == 'h') size = size == -1 ? -2 : -1;
            else if (*p == 'l') size = size == 1 ? 2 : 1;
            else if (*p == 'j' || *p == 'z' || *p == 't') size = 2;
            else if (*p == 'L') size = 3;
            else break;
        }
        int conv = *p;
        if (!conv) break;
        if (conv == 'n') {
            if (!suppress) {
                uint64_t dst = arg_int(a);
                int64_t v = (int64_t)(s - s0);
                if (size == -2) wr8(dst, (uint8_t)v); else if (size == -1) wr16(dst, (uint16_t)v);
                else if (size >= 1) wr64(dst, (uint64_t)v); else wr32(dst, (uint32_t)v);
            }
            continue;
        }
        if (conv != 'c' && conv != '[') while (isspace(*s)) s++;
        if (!*s) { input_failure = 1; break; }
        if (!have_width) width = conv == 'c' ? 1 : 0x7fffffff;
        if (conv == 'c' || conv == 's' || conv == '[') {
            unsigned char set[256];
            if (conv == 'c') memset(set, 1, sizeof set);
            else if (conv == 's') { memset(set, 1, sizeof set); set[' '] = 0; for (int ch = 9; ch <= 13; ch++) set[ch] = 0; }
            else {
                int invert = 0;
                p++;
                if (*p == '^') { invert = 1; p++; }
                memset(set, invert, sizeof set);
                if (*p == ']') { set[']'] = (unsigned char)!invert; p++; }
                else if (*p == '-') { set['-'] = (unsigned char)!invert; p++; }
                for (; *p && *p != ']'; p++) {
                    if (*p == '-' && p[1] && p[1] != ']') {
                        for (int ch = p[-1]; ch <= p[1]; ch++) set[ch] = (unsigned char)!invert;
                        p++;
                    } else set[*p] = (unsigned char)!invert;
                }
                if (!*p) return matches ? matches : -1;                 /* malformed set */
            }
            set[0] = 0;
            uint64_t dst = suppress ? 0 : arg_int(a);
            int n = 0;
            if (conv == 'c') {                                         /* exactly `width` characters, no terminator */
                int avail = 0;
                while (avail < width && s[avail]) avail++;
                if (avail < width) { input_failure = 1; break; }
                if (dst) memcpy((void *)(uintptr_t)dst, s, (size_t)width);
                s += width;
            } else {
                while (n < width && set[s[n]]) n++;
                if (n == 0) break;                                     /* matching failure */
                if (dst) { memcpy((void *)(uintptr_t)dst, s, (size_t)n); wr8(dst + (uint64_t)n, 0); }
                s += n;
            }
            if (!suppress) matches++;
            continue;
        }
        if (strchr("diouxXp", conv)) {
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'o' ? 8 : conv == 'i' ? 0 : 16;
            int i = 0, neg = 0, digits = 0;
            uint64_t v = 0;
            int overflow = 0;
            if (i < width && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
            if ((base == 0 || base == 16) && i < width && s[i] == '0') {
                if (i + 1 < width && (s[i + 1] | 0x20) == 'x') {
                    if (i + 2 < width && isxdigit(s[i + 2])) { i += 2; base = 16; }
                    else { i++; digits = 1; base = base ? base : 8; s += i; goto int_done; }   /* "0x" alone reads the 0 */
                } else if (base == 0) base = 8;
            }
            if (base == 0) base = 10;
            for (; i < width; i++) {
                int ch = s[i], d = isdigit(ch) ? ch - '0' : isalpha(ch) ? (ch | 0x20) - 'a' + 10 : 99;
                if (d >= base) break;
                if (v > (~0ull - (uint64_t)d) / (uint64_t)base) overflow = 1; else v = v * (uint64_t)base + (uint64_t)d;
                digits++;
            }
            if (!digits) break;                                        /* matching failure */
            s += i;
        int_done:
            if (overflow) v = ~0ull;
            else if (neg) v = (uint64_t)(-(int64_t)v);
            if (!suppress) {
                uint64_t dst = arg_int(a);
                if (conv == 'p' || size >= 1) wr64(dst, v);
                else if (size == -2) wr8(dst, (uint8_t)v);
                else if (size == -1) wr16(dst, (uint16_t)v);
                else wr32(dst, (uint32_t)v);
                matches++;
            }
            continue;
        }
        if (strchr("aAeEfFgG", conv)) {
            char tok[512];
            int len = scan_float_token(s, width, tok, (int)sizeof tok);
            if (!len) break;
            s += len;
            if (!suppress) {
                uint64_t dst = arg_int(a);
                if (size == 0 || size < 0) { float f = strtof(tok, NULL); memcpy((void *)(uintptr_t)dst, &f, 4); }
                else if (size == 3) { uint64_t q[2]; quad_from_double(strtod(tok, NULL), q); memcpy((void *)(uintptr_t)dst, q, 16); }
                else { double d = strtod(tok, NULL); memcpy((void *)(uintptr_t)dst, &d, 8); }
                matches++;
            }
            continue;
        }
        break;                                                         /* unknown conversion */
    }
    return matches == 0 && input_failure ? -1 : matches;
}
static void c_sscanf(Ctx *c) { Args a; args_direct(&a, c, 2); RET((int64_t)guest_scan(STR(0), STR(1), &a)); }
static void c_vsscanf(Ctx *c) { Args a; args_va(&a, c, A2); RET((int64_t)guest_scan(STR(0), STR(1), &a)); }
/* host-side entry for self-tests: scans with the variadic arguments taken from c exactly as a guest call would pass them */
int clib_guest_sscanf(Ctx *c) { c_sscanf(c); return (int)(int32_t)c->x[0]; }

/* ---------------------------------------------------------------- searching and sorting
 * The comparator is guest code: int (*)(const void *, const void *). */
static __declspec(thread) Ctx *t_cmp_ctx;
static __declspec(thread) uint64_t t_cmp_fn;
static int guest_compare(const void *a, const void *b) {
    Ctx *c = t_cmp_ctx;
    c->x[0] = (uint64_t)(uintptr_t)a;
    c->x[1] = (uint64_t)(uintptr_t)b;
    call_guest(c, t_cmp_fn);
    return (int)(int32_t)c->x[0];
}
static void c_bsearch(Ctx *c) {                           /* (key, base, nmemb, size, compar) */
    uint64_t key = A0, base = A1, count = A2, size = A3, saved_lr = c->x[30], saved_sp = c->x[31];
    Ctx *prev_ctx = t_cmp_ctx;
    uint64_t prev_fn = t_cmp_fn;
    t_cmp_ctx = c; t_cmp_fn = A4;
    uint64_t lo = 0, hi = count, found = 0;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2, elem = base + mid * size;
        int r = guest_compare((void *)(uintptr_t)key, (void *)(uintptr_t)elem);
        if (r == 0) { found = elem; break; }
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    t_cmp_ctx = prev_ctx; t_cmp_fn = prev_fn;
    c->x[30] = saved_lr; c->x[31] = saved_sp;
    RET(found);
}
static void c_qsort(Ctx *c) {                             /* (base, nmemb, size, compar) */
    uint64_t saved_lr = c->x[30], saved_sp = c->x[31];
    void *base = (void *)(uintptr_t)A0;
    size_t count = (size_t)A1, size = (size_t)A2;
    Ctx *prev_ctx = t_cmp_ctx;
    uint64_t prev_fn = t_cmp_fn;
    t_cmp_ctx = c; t_cmp_fn = A3;
    qsort(base, count, size, guest_compare);
    t_cmp_ctx = prev_ctx; t_cmp_fn = prev_fn;
    c->x[30] = saved_lr; c->x[31] = saved_sp;
}

/* ---------------------------------------------------------------- strings, ctype, numbers */
static void c_atoi(Ctx *c) { RET((int64_t)atoi(STR(0))); }
static void c_atol(Ctx *c) { RET((int64_t)strtoll(STR(0), NULL, 10)); }
static void c_atof(Ctx *c) { double r = atof(STR(0)); vzero(&c->v[0]); c->v[0].df[0] = r; }
static void c_strtoul(Ctx *c) { char *e = NULL; unsigned long long v = strtoull(STR(0), &e, (int)A2); if (A1) wr64(A1, (uint64_t)(uintptr_t)e); RET(v); }
static void c_strtof(Ctx *c) { char *e = NULL; float r = strtof(STR(0), &e); if (A1) wr64(A1, (uint64_t)(uintptr_t)e); vzero(&c->v[0]); c->v[0].f[0] = r; }
static void c_tolower(Ctx *c) { RET((int64_t)tolower((int)A0)); }
static void c_toupper(Ctx *c) { RET((int64_t)toupper((int)A0)); }
#define CTYPE(n, fn) static void n(Ctx *c) { int v = (int)A0; RET(v >= 0 && v < 256 ? (fn(v) != 0) : 0); }
CTYPE(c_isalnum, isalnum) CTYPE(c_iscntrl, iscntrl) CTYPE(c_ispunct, ispunct) CTYPE(c_isspace, isspace) CTYPE(c_isxdigit, isxdigit)
static void c_strcasecmp(Ctx *c) { RET((int64_t)_stricmp(STR(0), STR(1))); }
static void c_strncasecmp(Ctx *c) { RET((int64_t)_strnicmp(STR(0), STR(1), (size_t)A2)); }
static void c_strncat(Ctx *c) { strncat((char *)(uintptr_t)A0, STR(1), (size_t)A2); }
static void c_strpbrk(Ctx *c) { RET(strpbrk(STR(0), STR(1))); }
static void c_strspn(Ctx *c) { RET(strspn(STR(0), STR(1))); }
static void c_strcspn(Ctx *c) { RET(strcspn(STR(0), STR(1))); }
static void c_strcoll(Ctx *c) { RET((int64_t)strcmp(STR(0), STR(1))); }
static void c_strdup(Ctx *c) {                            /* freed by the imported free(): same heap */
    size_t n = strlen(STR(0)) + 1;
    char *p = _aligned_malloc(n, 16);
    memcpy(p, STR(0), n);
    RET(p);
}
static void c_strerror(Ctx *c) { static char msg[] = "error"; RET(msg); }
static void c_memset_s(Ctx *c) { memset((void *)(uintptr_t)A0, (int)A2, (size_t)(A3 < A1 ? A3 : A1)); RET(0); }
/* [sdk rand 0x4fb588, srand 0x4fb578] musl's generator: one 64-bit state shared by all threads, 31-bit results
 * (the host CRT's rand() only yields 15 bits and has a per-thread state) */
static uint64_t g_rand_seed;
static void c_rand(Ctx *c) { g_rand_seed = 6364136223846793005ull * g_rand_seed + 1; RET(g_rand_seed >> 33); }
static void c_srand(Ctx *c) { g_rand_seed = (uint32_t)((uint32_t)A0 - 1); }
static void c_time(Ctx *c) { int64_t t = (int64_t)time(NULL); if (A0) wr64(A0, (uint64_t)t); RET(t); }
static void c_clock(Ctx *c) { RET((int64_t)clock() * 1000); }               /* CLOCKS_PER_SEC is 1,000,000 on the target */
static void c_getenv(Ctx *c) { RET(0); }
/* atexit(fn): the functions run in reverse order of registration when the program ends normally, that is when nnMain
 * returns (boot.c calls clib_run_atexit). musl accepts any number of them (it allocates more blocks). */
static uint64_t *g_atexit;
static int g_atexit_count, g_atexit_cap;
static volatile LONG g_atexit_lock;
static void c_atexit(Ctx *c) {
    while (InterlockedExchange(&g_atexit_lock, 1)) SwitchToThread();
    if (g_atexit_count == g_atexit_cap) { g_atexit_cap = g_atexit_cap ? g_atexit_cap * 2 : 32; g_atexit = realloc(g_atexit, (size_t)g_atexit_cap * sizeof *g_atexit); }
    g_atexit[g_atexit_count++] = A0;
    InterlockedExchange(&g_atexit_lock, 0);
    RET(0);
}
void clib_run_atexit(Ctx *c) {
    for (;;) {
        uint64_t fn = 0;
        while (InterlockedExchange(&g_atexit_lock, 1)) SwitchToThread();
        if (g_atexit_count > 0) fn = g_atexit[--g_atexit_count];
        InterlockedExchange(&g_atexit_lock, 0);
        if (!fn) break;
        call_guest(c, fn);
    }
}
static void m_exp2(Ctx *c) { double r = exp2(c->v[0].df[0]); vzero(&c->v[0]); c->v[0].df[0] = r; }
static void m_erfcf(Ctx *c) { float r = erfcf(c->v[0].f[0]); vzero(&c->v[0]); c->v[0].f[0] = r; }
static void m_modff(Ctx *c) { float ip, r = modff(c->v[0].f[0], &ip); memcpy((void *)(uintptr_t)A0, &ip, 4); vzero(&c->v[0]); c->v[0].f[0] = r; }
static void m_frexpf(Ctx *c) { int e; float r = frexpf(c->v[0].f[0], &e); wr32(A0, (uint32_t)e); vzero(&c->v[0]); c->v[0].f[0] = r; }

/* ---------------------------------------------------------------- aligned allocation for the Skyline plugins
 * The game DEFINES malloc / free / realloc / calloc / aligned_alloc itself (13.0.4: [game] 0x710353be80 malloc, 0x710353bf00
 * free, 0x710353c0b0 aligned_alloc: lifted code over the game's own heap, f_710392dce0(align, size) / f_710392e590(ptr)),
 * and nnro.c binds a plugin's malloc / free to those. The plugins free game buffers and the game frees theirs, so their
 * memalign / posix_memalign (names the game does not have; boot.c's host _aligned_malloc versions would hand out blocks
 * that the game's free cannot release) are the game's aligned_alloc too, found by its name in main's symbol table (any
 * game build). Nested guest call: x0..x30 saved around it. */
static uint64_t game_aligned_alloc(Ctx *c, uint64_t align, uint64_t size) {
    static uint64_t fn;
    if (!fn && !(fn = main_export("aligned_alloc"))) guest_trap("memalign: main defines no aligned_alloc", c->x[30] - 4);
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = align; c->x[1] = size;
    call_guest(c, fn);
    uint64_t p = c->x[0];
    memcpy(c->x, saved, sizeof saved);
    return p;
}
static void c_memalign(Ctx *c) { RET(game_aligned_alloc(c, A0, A1)); }           /* (size_t align, size_t size) */
static void c_posix_memalign(Ctx *c) {                                            /* (void **out, size_t align, size_t size) -> errno */
    uint64_t out = A0, align = A1;
    if (align < 8 || (align & (align - 1))) { RET(22); return; }                    /* EINVAL: not a power of two multiple of sizeof(void *) */
    uint64_t p = game_aligned_alloc(c, align, A2);
    if (!p) { RET(12); return; }                                                    /* ENOMEM, *out untouched */
    wr64(out, p);
    RET(0);
}

static const ImportDef clib_imports[] = {
    {"sprintf", c_sprintf}, {"snprintf", c_snprintf}, {"vsnprintf", c_vsnprintf}, {"printf", c_printf}, {"fprintf", c_fprintf},
    {"_ZN2nn4util8SNPrintfEPcmPKcz", c_snprintf}, {"_ZN2nn4util9VSNPrintfEPcmPKcSt9__va_list", c_vsnprintf},
    {"sscanf", c_sscanf}, {"vsscanf", c_vsscanf},
    {"bsearch", c_bsearch}, {"qsort", c_qsort},
    {"atoi", c_atoi}, {"atol", c_atol}, {"atof", c_atof}, {"strtoul", c_strtoul}, {"strtoull", c_strtoul}, {"strtof", c_strtof},
    {"tolower", c_tolower}, {"toupper", c_toupper}, {"isalnum", c_isalnum}, {"iscntrl", c_iscntrl}, {"ispunct", c_ispunct},
    {"isspace", c_isspace}, {"isxdigit", c_isxdigit},
    {"strcasecmp", c_strcasecmp}, {"strncasecmp", c_strncasecmp}, {"strncat", c_strncat}, {"strpbrk", c_strpbrk},
    {"strspn", c_strspn}, {"strcspn", c_strcspn}, {"strcoll", c_strcoll}, {"strdup", c_strdup}, {"strerror", c_strerror},
    {"memset_s", c_memset_s}, {"rand", c_rand}, {"srand", c_srand}, {"time", c_time}, {"clock", c_clock},
    {"getenv", c_getenv}, {"atexit", c_atexit},
    {"exp2", m_exp2}, {"erfcf", m_erfcf}, {"modff", m_modff}, {"frexpf", m_frexpf},
    {"memalign", c_memalign}, {"posix_memalign", c_posix_memalign},     /* plugins only: replace boot.c's host-heap versions */
};

void register_clib_imports(void) {
    register_imports(clib_imports, (int)(sizeof clib_imports / sizeof clib_imports[0]));
}

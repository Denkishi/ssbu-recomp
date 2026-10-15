/* Self-test of the C / C++ runtime layer, included at the end of nncxxrt.c.
 *   SSBU_CXXRT_SELFTEST=1  tests that need no game state (the image is mapped and relocated, nothing has run)
 *   SSBU_CXXRT_SELFTEST=2  additionally runs nninitStartup and the static constructors first, then the tests that
 *                          need the game's allocator (std::string results, the game's own ostringstream code)
 * Exit code = number of failed checks. */
#include "quad_tests.h"

typedef unsigned __int128 tu128;
tu128 q_add(tu128 a, tu128 b); tu128 q_sub(tu128 a, tu128 b); tu128 q_mul(tu128 a, tu128 b); tu128 q_div(tu128 a, tu128 b);
tu128 q_from_i64(int64_t v); tu128 q_from_f64(double d); double q_to_f64(tu128 a); float q_to_f32(tu128 a); int64_t q_to_i64(tu128 a);
int q_cmp(tu128 a, tu128 b);
int libc2_gmtime(int64_t t, int32_t tm[9]);
uint64_t cxxlib_test_object(int which);
extern uint64_t g_main_stack_base, g_main_stack_size;

static int st_fail, st_checks;
#define CHECK(cond, ...) do { st_checks++; if (!(cond)) { st_fail++; printf("  FAILED: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Ctx *st_ctx(void) {
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    memset(c, 0, sizeof *c);
    uint8_t *stack = VirtualAlloc(NULL, 4 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    c->x[31] = P64(stack) + (4 << 20) - 0x1000;
    c->tpidr = P64(VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return c;
}
static uint64_t st_import(Ctx *c, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    ImportFn fn = import_lookup(name);
    if (!fn) { st_fail++; printf("  FAILED: no handler for %s\n", name); return 0; }
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    fn(c);
    return c->x[0];
}

/* ---- binary128 */
static void st_quad(void) {
    int bad = 0;
    for (int i = 0; i < (int)(sizeof quad_vecs / sizeof quad_vecs[0]); i++) {
        const QuadVec *v = &quad_vecs[i];
        tu128 a = ((tu128)v->a[1] << 64) | v->a[0], b = ((tu128)v->b[1] << 64) | v->b[0], want = ((tu128)v->r[1] << 64) | v->r[0];
        tu128 got = v->op == 0 ? q_add(a, b) : v->op == 1 ? q_sub(a, b) : v->op == 2 ? q_mul(a, b) : q_div(a, b);
        int nan_ok = (want & ~((tu128)1 << 127)) > ((tu128)0x7fff << 112) && (got & ~((tu128)1 << 127)) > ((tu128)0x7fff << 112);
        if (got != want && !nan_ok) {
            if (bad++ < 5) printf("  quad %s #%d: %016llx%016llx op %016llx%016llx = %016llx%016llx, expected %016llx%016llx\n", "add\0sub\0mul\0div" + v->op * 4, i,
                                  (unsigned long long)v->a[1], (unsigned long long)v->a[0], (unsigned long long)v->b[1], (unsigned long long)v->b[0],
                                  (unsigned long long)(got >> 64), (unsigned long long)got, (unsigned long long)v->r[1], (unsigned long long)v->r[0]);
        }
    }
    CHECK(bad == 0, "%d of %d binary128 add/sub/mul/div vectors differ", bad, (int)(sizeof quad_vecs / sizeof quad_vecs[0]));
    bad = 0;
    for (int i = 0; i < (int)(sizeof quad_convs / sizeof quad_convs[0]); i++) {
        const QuadConv *v = &quad_convs[i];
        tu128 q = ((tu128)v->q[1] << 64) | v->q[0];
        double d = q_to_f64(q); float f = q_to_f32(q);
        uint64_t db; uint32_t fb;
        memcpy(&db, &d, 8); memcpy(&fb, &f, 4);
        if (db != v->d || fb != v->f || q_to_i64(q) != v->i) {
            if (bad++ < 5) printf("  quad conv #%d: %016llx%016llx -> double %016llx (want %016llx) float %08x (want %08x) int %lld (want %lld)\n", i,
                                  (unsigned long long)v->q[1], (unsigned long long)v->q[0], (unsigned long long)db, (unsigned long long)v->d, fb, v->f,
                                  (long long)q_to_i64(q), (long long)v->i);
        }
    }
    CHECK(bad == 0, "%d binary128 -> double/float/int64 conversions differ", bad);
    bad = 0;
    for (int i = 0; i < (int)(sizeof quad_froms / sizeof quad_froms[0]); i++) {
        const QuadFrom *v = &quad_froms[i];
        double d;
        memcpy(&d, &v->d, 8);
        tu128 qi = q_from_i64(v->i), qd = q_from_f64(d);
        if (qi != (((tu128)v->q[1] << 64) | v->q[0]) || qd != (((tu128)v->dq[1] << 64) | v->dq[0])) bad++;
    }
    CHECK(bad == 0, "%d int64/double -> binary128 conversions differ", bad);
    /* a few values known by heart: 1.0L, 2.0L, 1.0L/3.0L, comparisons */
    tu128 one = q_from_i64(1), three = q_from_i64(3), third = q_div(one, three);
    CHECK((uint64_t)(one >> 64) == 0x3fff000000000000ull && (uint64_t)one == 0, "1.0L encoding");
    CHECK((uint64_t)(third >> 64) == 0x3ffd555555555555ull && (uint64_t)third == 0x5555555555555555ull, "1.0L/3.0L = %016llx%016llx", (unsigned long long)(third >> 64), (unsigned long long)third);
    CHECK(q_cmp(third, one) == -1 && q_cmp(one, third) == 1 && q_cmp(one, one) == 0 && q_cmp(one, ((tu128)0x7fff8 << 108)) == 2, "binary128 comparisons");
    /* through the import handlers, as the game calls them (71032c7264..: __floatditf, __subtf3, __multf3, __lttf2) */
    Ctx *c = st_ctx();
    st_import(c, "__floatditf", (uint64_t)-7, 0, 0, 0);
    c->v[1] = c->v[0];
    st_import(c, "__floatditf", 5, 0, 0, 0);
    st_import(c, "__subtf3", 0, 0, 0, 0);                              /* 5 - (-7) = 12 */
    CHECK(c->v[0].d[1] == 0x4002800000000000ull && c->v[0].d[0] == 0, "__floatditf/__subtf3 through the handlers: %016llx", (unsigned long long)c->v[0].d[1]);
    c->v[1] = c->v[0];
    st_import(c, "__multf3", 0, 0, 0, 0);                              /* 144 */
    CHECK(c->v[0].d[1] == 0x4006200000000000ull, "__multf3 through the handler: %016llx", (unsigned long long)c->v[0].d[1]);
    CHECK((int32_t)st_import(c, "__lttf2", 0, 0, 0, 0) > 0, "__lttf2(144, 12) must be > 0");
    c->v[0] = c->v[1]; c->v[1].d[1] = 0x4006200000000000ull; c->v[1].d[0] = 0;
    CHECK((int32_t)st_import(c, "__lttf2", 0, 0, 0, 0) < 0, "__lttf2(12, 144) must be < 0");
}

/* ---- sscanf / printf through the AArch64 variadic reader */
static int st_scan(Ctx *c, const char *in, const char *fmt, int n, ...) {
    va_list ap;
    va_start(ap, n);
    uint64_t sp = c->x[31];
    for (int i = 0; i < n; i++) {
        uint64_t p = P64(va_arg(ap, void *));
        if (i < 6) c->x[2 + i] = p; else wr64(sp + 8ull * (uint64_t)(i - 6), p);
    }
    va_end(ap);
    c->x[0] = P64(in); c->x[1] = P64(fmt);
    ImportFn fn = import_lookup("sscanf");
    if (!fn) { st_fail++; printf("  FAILED: sscanf has no handler\n"); return -99; }
    fn(c);
    return (int)(int32_t)c->x[0];
}
static void st_scanf(void) {
    Ctx *c = st_ctx();
    float f[3] = {0}; double d = 0; int i[8] = {0}; char s1[64] = "", s2[64] = "", s3[64] = "", ch = 0; long long ll = 0; unsigned long long ull = 0;
    uint32_t bits; uint64_t bits64, q[2];
    /* formats the game uses (found by scanning the sscanf call sites of main) */
    CHECK(st_scan(c, "1.5,-2.25,3e2", "%f,%f,%f", 3, &f[0], &f[1], &f[2]) == 3 && f[0] == 1.5f && f[1] == -2.25f && f[2] == 300.0f, "%%f,%%f,%%f");
    CHECK(st_scan(c, "0.1, 7", "%f,%f,%f", 3, &f[0], &f[1], &f[2]) == 2 && f[1] == 7.0f, "%%f,%%f,%%f with two fields must return 2");
    CHECK(st_scan(c, "key = value;rest", "%1023[^;\r\n=] =%4999[^;\r\n]", 2, s1, s2) == 2 && !strcmp(s1, "key ") && !strcmp(s2, " value"), "config line: '%s' '%s'", s1, s2);
    CHECK(st_scan(c, "HTTP/1.1 200 OK", "HTTP/1.%d %d", 2, &i[0], &i[1]) == 2 && i[0] == 1 && i[1] == 200, "HTTP/1.%%d %%d");
    CHECK(st_scan(c, "HTTP/1.1 404 Not Found", " HTTP/%1d.%1d%c%3d", 4, &i[0], &i[1], &ch, &i[2]) == 4 && i[0] == 1 && i[1] == 1 && ch == ' ' && i[2] == 404, "status line");
    CHECK(st_scan(c, "HTTP/2 301", " HTTP/2 %d", 1, &i[0]) == 1 && i[0] == 301, "HTTP/2 %%d");
    CHECK(st_scan(c, "RTSP/1.0 200", " RTSP/%d.%d %3d", 3, &i[0], &i[1], &i[2]) == 3 && i[2] == 200, "RTSP");
    CHECK(st_scan(c, "12:34:56 GMT", "%02d:%02d:%02d%n", 4, &i[0], &i[1], &i[2], &i[3]) == 3 && i[0] == 12 && i[2] == 56 && i[3] == 8, "time with %%n (n=%d)", i[3]);
    CHECK(st_scan(c, "12:34", "%02d:%02d%n", 3, &i[0], &i[1], &i[3]) == 2 && i[3] == 5, "hh:mm with %%n");
    CHECK(st_scan(c, "https://example.org/x", "%15[^?&/:]://%c", 2, s1, &ch) == 2 && !strcmp(s1, "https") && ch == 'e', "scheme");
    CHECK(st_scan(c, "http://host.name:80/path?q", "%15[^\n:]://%[^\n/?:]%[^\n]", 3, s1, s2, s3) == 3 && !strcmp(s2, "host.name") && !strcmp(s3, ":80/path?q"), "url split '%s' '%s'", s2, s3);
    CHECK(st_scan(c, "host/path", "%[^\n/?:]%[^\n]", 2, s1, s2) == 2 && !strcmp(s1, "host") && !strcmp(s2, "/path"), "host/path");
    CHECK(st_scan(c, "file:///c/dir", "%15[^\n/:]:%3[/]%[^\n/?#]%[^\n]", 4, s1, s2, s3, s3 + 32) == 4 && !strcmp(s2, "///") && !strcmp(s3, "c"), "slashes '%s' '%s'", s2, s3);
    CHECK(st_scan(c, "[fe80::1]:80", "[%*45[0123456789abcdefABCDEF:.]%c", 1, &ch) == 1 && ch == ']', "bracketed IPv6 with suppression");
    CHECK(st_scan(c, "3.25", "%f", 1, &f[0]) == 1 && f[0] == 3.25f, "%%f");
    /* general conformance */
    CHECK(st_scan(c, "0x1Ag -077 0", "%i%c %i %i", 4, &i[0], &ch, &i[1], &i[2]) == 4 && i[0] == 26 && ch == 'g' && i[1] == -63 && i[2] == 0, "%%i bases: %d %d", i[0], i[1]);
    CHECK(st_scan(c, "ff 0XFF 777", "%x %X %o", 3, &i[0], &i[1], &i[2]) == 3 && i[0] == 255 && i[1] == 255 && i[2] == 511, "%%x %%X %%o");
    i[0] = -1; CHECK(st_scan(c, "300", "%hhd", 1, &i[0]) == 1 && i[0] == (int)0xffffff2c, "%%hhd stores one byte (%08x)", (unsigned)i[0]);
    i[0] = -1; CHECK(st_scan(c, "70000", "%hd", 1, &i[0]) == 1 && i[0] == (int)0xffff1170, "%%hd stores two bytes (%08x)", (unsigned)i[0]);
    CHECK(st_scan(c, "-9223372036854775808 18446744073709551615", "%lld %llu", 2, &ll, &ull) == 2 && ll == (-9223372036854775807ll - 1) && ull == ~0ull, "64-bit integers");
    CHECK(st_scan(c, "1 2 3 4 5 6 7 8", "%d %d %d %d %d %d %d %d", 8, &i[0], &i[1], &i[2], &i[3], &i[4], &i[5], &i[6], &i[7]) == 8 && i[5] == 6 && i[6] == 7 && i[7] == 8, "eight arguments (two on the stack)");
    CHECK(st_scan(c, "", "%d", 1, &i[0]) == -1, "empty input must return EOF");
    CHECK(st_scan(c, "   ", "%d", 1, &i[0]) == -1, "blank input must return EOF");
    CHECK(st_scan(c, "abc", "%d", 1, &i[0]) == 0, "matching failure must return 0");
    CHECK(st_scan(c, "5 x", "%d %d", 2, &i[0], &i[1]) == 1, "second conversion fails -> 1");
    CHECK(st_scan(c, "abcdefgh", "%5s%n", 2, s1, &i[0]) == 1 && !strcmp(s1, "abcde") && i[0] == 5, "%%5s");
    memset(s1, '#', 8); CHECK(st_scan(c, "xyzw", "%3c", 1, s1) == 1 && !memcmp(s1, "xyz#", 4), "%%3c writes no terminator");
    CHECK(st_scan(c, "50% off", "%d%% %s", 2, &i[0], s1) == 2 && i[0] == 50 && !strcmp(s1, "off"), "%%%%");
    CHECK(st_scan(c, "a-c]x", "%[]a-c-]", 1, s1) == 1 && !strcmp(s1, "a-c]"), "set with ] and - ('%s')", s1);
    CHECK(st_scan(c, "12345", "%2d%*2d%d", 2, &i[0], &i[1]) == 2 && i[0] == 12 && i[1] == 5, "widths and suppression");
    /* floating point: correctly rounded, single precision rounded once */
    CHECK(st_scan(c, "0.1", "%f", 1, &f[0]) == 1 && (memcpy(&bits, &f[0], 4), bits) == 0x3dcccccdu, "0.1f = %08x", bits);
    CHECK(st_scan(c, "16777217", "%f", 1, &f[0]) == 1 && f[0] == 16777216.0f, "16777217 -> float");
    CHECK(st_scan(c, "0.1 1e308 4.9e-324", "%lf %le %lg", 3, &d, &bits64, &q[0]) == 3 && (memcpy(&bits64, &d, 8), bits64) == 0x3fb999999999999aull && q[0] == 1, "doubles");
    CHECK(st_scan(c, "0x1.8p1 -inf nan", "%f %f %f", 3, &f[0], &f[1], &f[2]) == 3 && f[0] == 3.0f && f[1] < 0 && f[1] * 0 != 0 && f[2] != f[2], "hex float, inf, nan");
    CHECK(st_scan(c, "1.e5x", "%f%c", 2, &f[0], &ch) == 2 && f[0] == 1e5f && ch == 'x', "1.e5");
    CHECK(st_scan(c, "2.5e+z", "%f%c", 2, &f[0], &ch) == 2 && f[0] == 2.5f && ch == 'e', "an incomplete exponent is not consumed");
    CHECK(st_scan(c, "1.5", "%Lf", 1, q) == 1 && q[1] == 0x3fff800000000000ull && q[0] == 0, "%%Lf stores binary128");

    /* printf family through the same reader */
    char out[256];
    ImportFn sn = import_lookup("snprintf");
    c->x[0] = P64(out); c->x[1] = sizeof out; c->x[2] = P64("%d|%5s|%-4x|%c|%lld|%hhd|%05.1f|%s");
    c->x[3] = (uint64_t)-42; c->x[4] = P64("ab"); c->x[5] = 0xbeef; c->x[6] = 'Z'; c->x[7] = 0x123456789all;
    wr64(c->x[31], 300); wr64(c->x[31] + 8, P64("tail"));
    c->v[0].df[0] = 2.25;
    sn(c);
    CHECK(!strcmp(out, "-42|   ab|beef|Z|78187493530|44|002.2|tail") || !strcmp(out, "-42|   ab|beef|Z|78187493530|44|002.3|tail"), "snprintf: %s", out);
    c->x[0] = P64(out); c->x[1] = sizeof out; c->x[2] = P64("%p %Lf %.3Lg");
    c->x[3] = 0x7100001000ull;
    c->v[0].d[0] = 0; c->v[0].d[1] = 0x3fff800000000000ull;            /* 1.5L */
    c->v[1].d[0] = 0x5555555555555555ull; c->v[1].d[1] = 0x3ffd555555555555ull;   /* 1/3 */
    sn(c);
    CHECK(!strcmp(out, "0x0000007100001000 1.500000 0.333"), "snprintf %%p %%Lf: %s", out);
    c->x[0] = P64(out); c->x[1] = 8; c->x[2] = P64("%s"); c->x[3] = P64("0123456789");
    sn(c);
    CHECK((int)c->x[0] == 10 && !strcmp(out, "0123456"), "snprintf truncation: %d '%s'", (int)c->x[0], out);
}

/* ---- gmtime, localeconv, random_device */
static void st_libc(void) {
    Ctx *c = st_ctx();
    int32_t tm[9];
    CHECK(libc2_gmtime(0, tm) && tm[5] == 70 && tm[4] == 0 && tm[3] == 1 && tm[6] == 4 && tm[7] == 0, "gmtime(0)");
    CHECK(libc2_gmtime(1700000000, tm) && tm[5] == 123 && tm[4] == 10 && tm[3] == 14 && tm[2] == 22 && tm[1] == 13 && tm[0] == 20 && tm[6] == 2 && tm[7] == 317, "gmtime(1700000000): %d-%d-%d %d:%d:%d wday %d yday %d", tm[5], tm[4], tm[3], tm[2], tm[1], tm[0], tm[6], tm[7]);
    CHECK(libc2_gmtime(-1, tm) && tm[5] == 69 && tm[4] == 11 && tm[3] == 31 && tm[2] == 23 && tm[0] == 59 && tm[6] == 3 && tm[7] == 364, "gmtime(-1)");
    CHECK(libc2_gmtime(951782400, tm) && tm[5] == 100 && tm[4] == 1 && tm[3] == 29 && tm[7] == 59 && tm[6] == 2, "gmtime(2000-02-29)");
    int64_t t = 1700000000;
    uint64_t r = st_import(c, "gmtime", P64(&t), 0, 0, 0);
    CHECK(r && rd32(r + 20) == 123 && rd32(r + 16) == 10 && rd64(r + 40) == 0 && !strcmp((const char *)(uintptr_t)rd64(r + 48), "UTC"), "gmtime through the handler (struct tm layout)");
    r = st_import(c, "localeconv", 0, 0, 0, 0);
    CHECK(r && !strcmp((const char *)(uintptr_t)rd64(r), ".") && !strcmp((const char *)(uintptr_t)rd64(r + 8), "") && rd8(r + 80) == 0xff, "localeconv");
    uint8_t *rd = calloc(1, 0x9c8);
    st_import(c, "_ZNSt3__113random_deviceC1ERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE", P64(rd), 0, 0, 0);
    uint32_t a = (uint32_t)st_import(c, "_ZNSt3__113random_deviceclEv", P64(rd), 0, 0, 0);
    uint32_t b = (uint32_t)st_import(c, "_ZNSt3__113random_deviceclEv", P64(rd), 0, 0, 0);
    uint32_t last = 0;
    for (int k = 2; k < 10000; k++) last = (uint32_t)st_import(c, "_ZNSt3__113random_deviceclEv", P64(rd), 0, 0, 0);
    CHECK(a == 3499211612u && b == 581869302u && last == 4123659995u, "random_device = mt19937(5489): %u %u ... %u (10000th must be 4123659995)", a, b, last);
    free(rd);
}

/* ---- guard variables under contention */
#define ST_GUARDS 400
static uint64_t st_guards[ST_GUARDS];
static volatile LONG st_guard_inits[ST_GUARDS];
static HANDLE st_go;
static DWORD WINAPI st_guard_thread(LPVOID arg) {
    Ctx *c = st_ctx();
    (void)arg;
    WaitForSingleObject(st_go, INFINITE);
    for (int g = 0; g < ST_GUARDS; g++) {
        if (rd8(P64(&st_guards[g]))) continue;                         /* the inline fast path of the compiler */
        if ((int)st_import(c, "__cxa_guard_acquire", P64(&st_guards[g]), 0, 0, 0)) {
            LONG seen = st_guard_inits[g];
            if ((g & 7) == 0) Sleep(0);
            st_guard_inits[g] = seen + 1;                              /* a deliberately racy "constructor" */
            st_import(c, (g % 5 == 4 && seen == 0) ? "__cxa_guard_abort" : "__cxa_guard_release", P64(&st_guards[g]), 0, 0, 0);
        }
    }
    return 0;
}
static void st_guard(void) {
    HANDLE th[12];
    st_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < 12; i++) th[i] = CreateThread(NULL, 0, st_guard_thread, NULL, 0, NULL);
    SetEvent(st_go);
    WaitForMultipleObjects(12, th, TRUE, INFINITE);
    int bad = 0;
    for (int g = 0; g < ST_GUARDS; g++) {
        /* every fifth guard's first initialisation is aborted, so it must have run exactly twice; the others once */
        int want = g % 5 == 4 ? 2 : 1;
        if (st_guard_inits[g] != want || (st_guards[g] & 0xffff) != 1) bad++;
    }
    CHECK(bad == 0, "%d of %d guard variables were initialised a wrong number of times under 12 threads", bad, ST_GUARDS);
}

/* ---- exception objects and the imported vtables */
static int st_dtor_calls;
static void st_exc_dtor(Ctx *c) { st_dtor_calls++; (void)c; }
static void st_exceptions(void) {
    Ctx *c = st_ctx();
    uint64_t obj[4] = {0}, copy[4] = {0};
    st_import(c, "_ZNSt11logic_errorC2EPKc", P64(obj), P64("index out of range"), 0, 0);
    obj[0] = P64(&g_vt_out_of_range[2]);                               /* what the game's inlined out_of_range constructor stores */
    const char *w = (const char *)(uintptr_t)cxxrt_call(c, rd64(obj[0] + 0x10), P64(obj), 0, 0);
    CHECK(w && !strcmp(w, "index out of range"), "out_of_range::what() through the imported vtable");
    CHECK(rd64(obj[0] - 8) == P64(g_ti_out_of_range) && rd64(obj[0] - 16) == 0, "vtable header (offset-to-top, typeinfo)");
    st_import(c, "_ZNSt11logic_errorC2ERKS_", P64(copy), P64(obj), 0, 0);
    CHECK(copy[1] == obj[1] && rd32(obj[1] - 8) == 1, "logic_error copy shares the reference-counted string");
    cxxrt_call(c, rd64(obj[0]), P64(obj), 0, 0);                       /* virtual ~D1 */
    CHECK(rd32(copy[1] - 8) == 0 && !strcmp((const char *)(uintptr_t)copy[1], "index out of range"), "string survives the first destructor");
    st_import(c, "_ZNSt12out_of_rangeD1Ev", P64(copy), 0, 0, 0);
    CHECK(ti_upcast(P64(g_ti_out_of_range), 0x1000, P64(g_ti_exception)) == 0x1000 && ti_upcast(P64(g_ti_bad_alloc), 0x1000, P64(g_ti_logic_error)) == 0, "typeinfo hierarchy walk");
    uint64_t e = st_import(c, "__cxa_allocate_exception", 24, 0, 0, 0);
    CHECK(e && (e & 15) == 0 && rd64(e) == 0 && rd64(e + 16) == 0, "__cxa_allocate_exception returns zeroed, aligned memory");
    cxxrt_exception_init(e, P64(g_ti_bad_alloc), hostfn_address("selftest exception destructor", st_exc_dtor));
    uint64_t p1 = e, p2 = 0;
    st_import(c, "_ZNSt13exception_ptrC1ERKS_", P64(&p2), P64(&p1), 0, 0);
    st_import(c, "_ZNSt13exception_ptrD1Ev", P64(&p1), 0, 0, 0);
    CHECK(p2 == e && st_dtor_calls == 0, "exception_ptr copy keeps the object alive");
    st_import(c, "_ZNSt13exception_ptrD1Ev", P64(&p2), 0, 0, 0);
    CHECK(st_dtor_calls == 1, "last exception_ptr destroys the exception (destructor calls: %d)", st_dtor_calls);
    uint64_t ba[1];
    st_import(c, "_ZNSt9bad_allocC1Ev", P64(ba), 0, 0, 0);
    CHECK(!strcmp((const char *)(uintptr_t)cxxrt_call(c, rd64(ba[0] + 0x10), P64(ba), 0, 0), "std::bad_alloc"), "bad_alloc::what()");
}

/* ---- facets, stream vtables */
static void st_facets(void) {
    Ctx *c = st_ctx();
    uint64_t loc = 0;
    uint64_t ct = st_import(c, "_ZNKSt3__16locale9use_facetERNS0_2idE", P64(&loc), cxxlib_test_object(0), 0, 0);
    CHECK(ct && (uint8_t)cxxrt_call(c, rd64(rd64(ct) + 0x38), ct, 'q', 0) == 'q', "ctype<char>::do_widen through the vtable");
    CHECK((uint8_t)cxxrt_call(c, rd64(rd64(ct) + 0x18), ct, 'q', 0) == 'Q' && (uint8_t)cxxrt_call(c, rd64(rd64(ct) + 0x28), ct, 'Q', 0) == 'q', "do_toupper / do_tolower");
    CHECK((uint8_t)cxxrt_call(c, rd64(rd64(ct) + 0x48), ct, 0xe9, '?') == '?', "do_narrow of a non-ASCII character gives the default");
    const uint16_t *tab = (const uint16_t *)(uintptr_t)rd64(ct + 0x10);
    CHECK((tab['A'] & 0x100) && (tab['a'] & 0x200) && (tab['5'] & 0x800) && (tab[' '] & 0x2000) && !(tab['a'] & 0x100), "classic table mask bits");
    uint64_t np = st_import(c, "_ZNKSt3__16locale9use_facetERNS0_2idE", P64(&loc), cxxlib_test_object(1), 0, 0);
    uint8_t str[24];
    CHECK(np && (uint8_t)cxxrt_call(c, rd64(rd64(np) + 0x18), np, 0, 0) == '.' && (uint8_t)cxxrt_call(c, rd64(rd64(np) + 0x20), np, 0, 0) == ',', "numpunct<char> decimal point / thousands separator");
    c->x[8] = P64(str);
    cxxrt_call(c, rd64(rd64(np) + 0x30), np, 0, 0);
    CHECK(str[0] == 8 && !memcmp(str + 1, "true", 5), "numpunct<char>::do_truename returns a short std::string");
    /* a plain std::ostream as the game lays it out: primary vptr at +0, basic_ios part at +8 */
    uint64_t vt = cxxlib_test_object(2), os[32] = {0};
    os[0] = vt; os[1] = vt + 0x28;
    CHECK(rd64(vt - 0x18) == 8 && rd64(vt - 0x10) == 0 && rd64(os[1] - 0x18) == (uint64_t)-8, "basic_ostream vtable: vbase offset 8, vcall offset -8");
    CHECK(cxxrt_call(c, rd64(os[1]), P64(&os[1]), 0, 0) == P64(os), "virtual thunk to ~basic_ostream adjusts this by -8");
    CHECK(P64(os) + rd64(rd64(P64(os)) - 0x18) == P64(&os[1]), "the stream finds its basic_ios through vtable[-3]");
}

/* ---- promise<void> / future<void> */
static uint64_t st_promise[1];
static DWORD WINAPI st_setter(LPVOID arg) {
    Ctx *c = st_ctx();
    (void)arg;
    Sleep(60);
    st_import(c, "_ZNSt3__17promiseIvE9set_valueEv", P64(st_promise), 0, 0, 0);
    return 0;
}
static void st_future(void) {
    Ctx *c = st_ctx();
    uint64_t fut[1] = {0}, lk[2];
    st_import(c, "_ZNSt3__17promiseIvEC1Ev", P64(st_promise), 0, 0, 0);
    uint64_t st = st_promise[0];
    CHECK(st && rd64(st + 8) == 0 && rd32(st + 0x40) == 0, "promise<void>() creates an empty shared state");
    c->x[8] = P64(fut);
    st_import(c, "_ZNSt3__17promiseIvE10get_futureEv", P64(st_promise), 0, 0, 0);
    CHECK(fut[0] == st && rd64(st + 8) == 1 && (rd32(st + 0x40) & 2), "get_future attaches and adds an owner");
    HANDLE th = CreateThread(NULL, 0, st_setter, NULL, 0, NULL);
    /* future<void>::get() as the game inlines it: lock the state's mutex, __sub_wait, unlock */
    st_import(c, "_ZNSt3__15mutex4lockEv", st + 0x18, 0, 0, 0);
    lk[0] = st + 0x18; lk[1] = 1;
    DWORD t0 = GetTickCount();
    st_import(c, "_ZNSt3__117__assoc_sub_state10__sub_waitERNS_11unique_lockINS_5mutexEEE", st, P64(lk), 0, 0);
    DWORD waited = GetTickCount() - t0;
    CHECK((rd32(st + 0x40) & 5) == 5 && waited >= 30, "__sub_wait blocks until set_value from another thread (waited %lu ms)", (unsigned long)waited);
    st_import(c, "_ZNSt3__15mutex6unlockEv", st + 0x18, 0, 0, 0);
    WaitForSingleObject(th, INFINITE);
    st_import(c, "_ZNSt3__17promiseIvED1Ev", P64(st_promise), 0, 0, 0);
    CHECK(rd64(st + 8) == 0 && rd64(st + 0x10) == 0, "~promise after set_value drops one owner and stores no exception");
    st_import(c, "_ZNSt3__16futureIvED1Ev", P64(fut), 0, 0, 0);       /* frees the state through the vtable */
    /* broken promise */
    st_import(c, "_ZNSt3__17promiseIvEC1Ev", P64(st_promise), 0, 0, 0);
    st = st_promise[0];
    c->x[8] = P64(fut);
    st_import(c, "_ZNSt3__17promiseIvE10get_futureEv", P64(st_promise), 0, 0, 0);
    st_import(c, "_ZNSt3__17promiseIvED1Ev", P64(st_promise), 0, 0, 0);
    uint64_t e = rd64(st + 0x10);
    const char *w = e ? (const char *)(uintptr_t)cxxrt_call(c, rd64(rd64(e) + 0x10), e, 0, 0) : NULL;
    CHECK(e && (rd32(st + 0x40) & 4) && rd32(e + 0x10) == 4 && rd64(e + 0x18) == cxxlib_test_object(3) && w && !strncmp(w, "The associated promise has been destructed", 42),
          "a destroyed promise leaves future_error(broken_promise) in the state: %s", w ? w : "(none)");
    st_import(c, "_ZNSt3__16futureIvED1Ev", P64(fut), 0, 0, 0);
}

/* ---- __dynamic_cast on the game's own RTTI
 * Typeinfo objects and vtables are found in the relocated image: a typeinfo starts with one of the three imported
 * __cxxabiv1 table addresses; a vtable holds { offset-to-top, typeinfo } before its address point. */
#define ST_DATA_START (GUEST_BASE + g_main.data_start)
#define ST_DATA_END (GUEST_BASE + (g_main.file_image & ~0xfffull))
static uint64_t st_find_vptr(uint64_t ti, int64_t offset_to_top) {
    for (uint64_t a = ST_DATA_START + 8; a < ST_DATA_END - 16; a += 8)
        if (rd64(a) == ti && (int64_t)rd64(a - 8) == offset_to_top && rd64(a + 8) >= GUEST_BASE && rd64(a + 8) < GUEST_BASE + g_main.ro_start) return a + 8;
    return 0;
}
static void st_dyncast(void) {
    int si_done = 0, vmi_done = 0, n_class = 0, n_si = 0, n_vmi = 0;
    uint64_t other = 0;
    for (uint64_t a = ST_DATA_START; a < ST_DATA_END - 64; a += 8) {
        int kind = rd64(a) == P64(&g_vt_class[2]) ? TI_CLASS : rd64(a) == P64(&g_vt_si[2]) ? TI_SI : rd64(a) == P64(&g_vt_vmi[2]) ? TI_VMI : 0;
        if (!kind || !cxxrt_readable(rd64(a + 8) & 0x7fffffffffffffffull, 4)) continue;
        if (kind == TI_CLASS) { n_class++; if (!other && st_find_vptr(a, 0)) other = a; }
        if (kind == TI_SI) n_si++;
        if (kind == TI_VMI) n_vmi++;
        if (kind == TI_SI && !si_done && other) {
            /* D : B : ... two levels, both with vtables */
            uint64_t d = a, b = rd64(a + 16);
            if (ti_kind(b) != TI_SI) continue;
            uint64_t root = rd64(b + 16), vd = st_find_vptr(d, 0), vb = st_find_vptr(b, 0);
            if (!vd || !vb || ti_same(root, other) || ti_same(b, other)) continue;
            uint64_t obj_d[2] = {vd, 0}, obj_b[2] = {vb, 0};
            printf("  single inheritance: %s : %s : %s (unrelated: %s)\n", cxxrt_type_name(d), cxxrt_type_name(b), cxxrt_type_name(root), cxxrt_type_name(other));
            CHECK(cxxrt_dynamic_cast(P64(obj_d), root, d) == P64(obj_d), "root -> most derived");
            CHECK(cxxrt_dynamic_cast(P64(obj_d), root, b) == P64(obj_d), "root -> middle class");
            CHECK(cxxrt_dynamic_cast(P64(obj_d), b, d) == P64(obj_d), "middle -> most derived");
            CHECK(cxxrt_dynamic_cast(P64(obj_b), root, d) == 0, "an object of the middle class is not the derived class");
            CHECK(cxxrt_dynamic_cast(P64(obj_d), root, other) == 0, "cast to an unrelated class fails");
            si_done = 1;
        }
        if (kind == TI_VMI && !vmi_done) {
            uint32_t count = rd32(a + 20);
            if (count < 2) continue;
            int64_t of0 = (int64_t)rd64(a + 32), of1 = (int64_t)rd64(a + 48);
            uint64_t b0 = rd64(a + 24), b1 = rd64(a + 40);
            if ((of0 & 1) || (of1 & 1) || (of0 >> 8) != 0 || (of1 >> 8) <= 0 || (of1 >> 8) > 0x400 || ti_same(b0, b1)) continue;
            uint64_t off = (uint64_t)(of1 >> 8), v0 = st_find_vptr(a, 0), v1 = st_find_vptr(a, -(int64_t)off);
            if (!v0 || !v1 || ti_kind(b0) == TI_OTHER || ti_kind(b1) == TI_OTHER) continue;
            uint64_t *obj = calloc(1, off + 64);
            obj[0] = v0;
            wr64(P64(obj) + off, v1);
            printf("  multiple inheritance: %s : %s (+0), %s (+0x%llx)\n", cxxrt_type_name(a), cxxrt_type_name(b0), cxxrt_type_name(b1), (unsigned long long)off);
            CHECK(cxxrt_dynamic_cast(P64(obj) + off, b1, a) == P64(obj), "second base -> derived subtracts the base offset");
            CHECK(cxxrt_dynamic_cast(P64(obj), b0, a) == P64(obj), "first base -> derived");
            CHECK(cxxrt_dynamic_cast(P64(obj), b0, b1) == P64(obj) + off, "cross-cast first base -> second base");
            CHECK(cxxrt_dynamic_cast(P64(obj) + off, b1, b0) == P64(obj), "cross-cast second base -> first base");
            if (other && !ti_upcast(a, 0x1000, other)) CHECK(cxxrt_dynamic_cast(P64(obj) + off, b1, other) == 0, "cast to an unrelated class fails");
            free(obj);
            vmi_done = 1;
        }
    }
    printf("  game RTTI seen: %d root classes, %d single-inheritance, %d multiple/virtual-inheritance typeinfo objects\n", n_class, n_si, n_vmi);
    CHECK(si_done && vmi_done, "could not find suitable classes in the game's RTTI (single %d, multiple %d)", si_done, vmi_done);
    CHECK(n_si > 1000, "the imported __cxxabiv1 typeinfo vtables are not what the game's typeinfo objects point to");
}

/* ---- tests that need the game's allocator (mode 2) */
static void st_with_game_heap(Ctx *c) {
    uint8_t str[24], str2[24];
    uint64_t obj[4];
    /* std::string built by libcxx.c, read back, used by logic_error(const string &), freed by the game's allocator */
    cxx_string_init(c, P64(str), "a string that is longer than the 22 characters of the short form", 64);
    size_t n;
    CHECK((str[0] & 1) && !strncmp(cxx_string_data(P64(str), &n), "a string that", 13) && n == 64, "long std::string from the game allocator");
    st_import(c, "_ZNSt11logic_errorC2ERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE", P64(obj), P64(str), 0, 0);
    CHECK(!strcmp((const char *)(uintptr_t)obj[1], "a string that is longer than the 22 characters of the short form"), "logic_error(const std::string &)");
    st_import(c, "_ZNSt11logic_errorD2Ev", P64(obj), 0, 0, 0);
    st_import(c, "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev", P64(str), 0, 0, 0);
    /* error_category::message through the vtable returns a std::string */
    uint64_t cat = cxxlib_test_object(3);
    c->x[8] = P64(str2);
    cxxrt_call(c, rd64(rd64(cat) + 0x30), cat, 2, 0);
    CHECK(!strcmp(cxx_string_data(P64(str2), &n), "The state of the promise has already been set."), "future_category().message(promise_already_satisfied)");
    st_import(c, "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev", P64(str2), 0, 0, 0);
    /* a wide string as the game's inlined code builds it: buffer from the game's operator new, freed by the imported destructor */
    uint64_t wbuf = cxxrt_call(c, cxxrt_main_symbol("_Znwm"), 64, 0, 0);
    uint64_t wstr[3] = {65, 3, wbuf};
    CHECK(wbuf != 0, "the game's operator new works");
    st_import(c, "_ZNSt3__112basic_stringIwNS_11char_traitsIwEENS_9allocatorIwEEED2Ev", P64(wstr), 0, 0, 0);
}

/* The game's own function f_71035b5c50(std::string *out, unsigned long value): it builds a std::ostringstream on its
 * stack (ios_base::init, basic_streambuf(), the game's stringbuf vtable), does `os << value` (imported operator<<,
 * sentry, xsputn/overflow through the game's vtable), then stringbuf::str() (f_7102d099c0) and destroys the stream. */
#define ST_GAME_STREAM_TEST 1
static void st_game_stream(Ctx *c) {
    static const uint64_t values[] = {0, 7, 1234567890123ull, 18446744073709551615ull};
    static const char *const expect[] = {"0", "7", "1234567890123", "18446744073709551615"};
    printf("the game's ostringstream code (f_71035b5c50)\n");
    for (int i = 0; i < 4; i++) {
        uint8_t out[24];
        size_t n = 0;
        memset(out, 0xee, sizeof out);
        uint64_t sp = c->x[31];
        cxxrt_call(c, GA(GUEST_BASE + 0x35b5c50ull), P64(out), values[i], 0);
        const char *s = cxx_string_data(P64(out), &n);
        CHECK(c->x[31] == sp && n == strlen(expect[i]) && !memcmp(s, expect[i], n), "ostringstream << %llu gave '%.*s'", (unsigned long long)values[i], (int)n, s);
        st_import(c, "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev", P64(out), 0, 0, 0);
    }
}

/* ---- exception runtime (eh_runtime.h): personality, images, throw / catch / rethrow bookkeeping */
#include "eh_tests.h"
#include <setjmp.h>
static jmp_buf st_raise_jmp;
static int st_raise_reason, st_raise_calls;
static void st_raise(Ctx *c, int reason) { (void)c; st_raise_reason = reason; st_raise_calls++; longjmp(st_raise_jmp, 1); }
#define ST_RAISES(stmt) do { if (setjmp(st_raise_jmp) == 0) { stmt; CHECK(0, "the unwinder hook was not called"); } } while (0)
static int st_eh_dtors;
static void st_eh_dtor(Ctx *c) { st_eh_dtors++; (void)c; }

static void st_eh_tables(void) {
    Ctx *c = st_ctx();
    /* every call-site record of main, decoded independently by gen_eh_tests.py from the ELF file */
    uint64_t a = cxxrt_allocate_exception(16), b = cxxrt_allocate_exception(32);
    cxxrt_exception_init(a, P64(g_ti_out_of_range), 0);
    cxxrt_exception_init(b, P64(g_ti_ios_failure), 0);
    int bad = 0, n = (int)(sizeof eh_vecs / sizeof eh_vecs[0]);
    for (int i = 0; i < n; i++) {
        const EhVec *v = &eh_vecs[i];
        EhPad pa, pb;
        int ka = eh_personality(c, v->pc, a, &pa), kb = eh_personality(c, v->pc, b, &pb);
        uint64_t want_a = v->kind_a ? v->pad : 0, want_b = v->kind_b ? v->pad : 0;
        if (ka != v->kind_a || kb != v->kind_b || pa.selector != v->sel_a || pb.selector != v->sel_b || pa.landing_pad != want_a || pb.landing_pad != want_b) {
            if (bad++ < 5) printf("  pc %llx: out_of_range -> kind %d sel %lld pad %llx (want %d %d %llx); ios failure -> kind %d sel %lld pad %llx (want %d %d %llx)\n",
                                  (unsigned long long)v->pc, ka, (long long)pa.selector, (unsigned long long)pa.landing_pad, v->kind_a, v->sel_a, (unsigned long long)want_a,
                                  kb, (long long)pb.selector, (unsigned long long)pb.landing_pad, v->kind_b, v->sel_b, (unsigned long long)want_b);
        }
    }
    CHECK(bad == 0, "%d of %d personality answers over main's call-site records differ from the tables decoded offline", bad, n);
    bad = 0;
    for (int i = 0; i < (int)(sizeof eh_fn_vecs / sizeof eh_fn_vecs[0]); i++) {
        EhFunction f;
        const EhFnVec *v = &eh_fn_vecs[i];
        if (!eh_find_function(v->start, &f) || f.start != v->start || f.end != v->end || f.lsda != v->lsda) bad++;
        if (!eh_find_function(v->end - 4, &f) || f.start != v->start) bad++;
    }
    CHECK(bad == 0, "%d function look-ups through .eh_frame_hdr differ", bad);
    EhFunction f;
    CHECK(!eh_find_function(0x7200000000ull, &f) && !eh_find_function(GUEST_BASE + 0x4000000ull, &f), "an address outside any function must not be found");
    /* type matching: a class derived from ios_base::failure is caught by `catch (ios_base::failure &)` at 0x7103839de8;
     * a class with that one as second base at +0x10 is caught with the pointer adjusted */
    static uint64_t ti_derived[3], ti_multi[7];
    ti_derived[0] = P64(&g_vt_si[2]); ti_derived[1] = P64("10TestDerived"); ti_derived[2] = P64(g_ti_ios_failure);
    ti_multi[0] = P64(&g_vt_vmi[2]); ti_multi[1] = P64("9TestMulti"); ti_multi[2] = 0x0000000200000000ull;
    ti_multi[3] = P64(g_ti_streambuf); ti_multi[4] = 2; ti_multi[5] = P64(ti_derived); ti_multi[6] = (0x10 << 8) | 2;
    EhPad pad;
    cxxrt_exception_init(b, P64(ti_derived), 0);
    CHECK(eh_personality(c, GA(0x7103839b94ull), b, &pad) == EH_HANDLER && pad.selector == 1 && pad.landing_pad == GA(0x7103839de8ull) && pad.adjusted == b, "derived class caught by a base-class handler");
    cxxrt_exception_init(b, P64(ti_multi), 0);
    CHECK(eh_personality(c, GA(0x7103839b94ull), b, &pad) == EH_HANDLER && pad.adjusted == b + 0x10, "second base at +0x10: adjusted pointer %llx (object %llx)", (unsigned long long)pad.adjusted, (unsigned long long)b);
    cxxrt_exception_init(b, P64(g_ti_bad_alloc), 0);
    CHECK(eh_personality(c, GA(0x7103839b94ull), b, &pad) == EH_CLEANUP && pad.selector == 0 && pad.landing_pad == GA(0x7103839de8ull), "an unrelated type only gets the clean-up of that pad");
    CHECK(eh_personality(c, GA(0x710382b690ull), b, &pad) == EH_HANDLER && pad.landing_pad == GA(0x710382cb34ull) && pad.function.start == GA(0x7103828300ull), "catch (...) in f_7103828300");
    exc_free(a); exc_free(b);
}

static void st_eh_flow(void) {
    Ctx *c = st_ctx();
    EhPad pad;
    EhState *st = eh_state(c);
    uint64_t dtor = hostfn_address("selftest eh destructor", st_eh_dtor);
    EhRaiseFn installed = g_eh_raise;
    eh_set_unwinder(st_raise);
    /* throw -> catch (...) -> rethrow -> clean-up (end_catch) -> outer catch -> end_catch destroys */
    uint64_t e = st_import(c, "__cxa_allocate_exception", 16, 0, 0, 0);
    st_raise_calls = 0;
    ST_RAISES(st_import(c, "__cxa_throw", e, P64(g_ti_out_of_range), dtor, 0));
    CHECK(st_raise_calls == 1 && st_raise_reason == EH_RAISE_THROW && eh_in_flight(c) == e && st->uncaught == 1 && !strcmp(eh_type_name(e), "St12out_of_range"), "__cxa_throw puts the exception in flight and calls the unwinder");
    CHECK(eh_personality(c, GA(0x710382b690ull), e, &pad) == EH_HANDLER, "handler found");
    eh_enter_pad(c, &pad);
    CHECK(c->x[0] == e - 0x20 && c->x[1] == 1 && eh_in_flight(c) == 0, "pad entry: x0 = unwind header (object - 0x20), x1 = selector");
    CHECK(rd64(c->x[0]) == 0x434c4e47432b2b00ull, "exception class CLNGC++ in the unwind header");
    uint64_t hdr = c->x[0];
    c->x[0] = hdr; abi_get_exception_ptr(c);                           /* not an import of the game: called directly */
    CHECK(c->x[0] == e, "__cxa_get_exception_ptr");
    CHECK(st_import(c, "__cxa_begin_catch", hdr, 0, 0, 0) == e && exc_header(e)->handlers == 1 && st->uncaught == 0 && st->caught == e, "__cxa_begin_catch returns the object, handler count 1");
    ST_RAISES(st_import(c, "__cxa_rethrow", 0, 0, 0, 0));
    CHECK(exc_header(e)->handlers == -1 && eh_in_flight(c) == e && st->uncaught == 1, "__cxa_rethrow: handler count negated, in flight again");
    eh_personality(c, GA(0x710382b690ull), e, &pad); eh_enter_pad(c, &pad);                 /* the clean-up of the catch block */
    st_import(c, "__cxa_end_catch", 0, 0, 0, 0);
    CHECK(st_eh_dtors == 0 && exc_header(e) && exc_header(e)->handlers == 0 && st->caught == 0, "__cxa_end_catch of a rethrown exception keeps it alive");
    ST_RAISES(st_import(c, "_Unwind_Resume", hdr, 0, 0, 0));
    CHECK(st_raise_reason == EH_RAISE_RESUME && eh_in_flight(c) == e, "_Unwind_Resume continues the propagation");
    eh_personality(c, GA(0x710382b690ull), e, &pad); eh_enter_pad(c, &pad);                 /* the outer handler */
    st_import(c, "__cxa_begin_catch", hdr, 0, 0, 0);
    /* nested: a second exception thrown and caught while the first is being handled */
    uint64_t e2 = st_import(c, "__cxa_allocate_exception", 8, 0, 0, 0);
    ST_RAISES(st_import(c, "__cxa_throw", e2, P64(g_ti_bad_alloc), dtor, 0));
    eh_personality(c, GA(0x710382b690ull), e2, &pad); eh_enter_pad(c, &pad);
    st_import(c, "__cxa_begin_catch", e2 - 0x20, 0, 0, 0);
    CHECK(st->caught == e2 && exc_header(e2)->next == e, "caught exceptions stack");
    st_import(c, "__cxa_end_catch", 0, 0, 0, 0);
    CHECK(st_eh_dtors == 1 && st->caught == e, "inner exception destroyed at its end_catch");
    /* exception_ptr keeps the outer one alive past its handler; rethrow_exception throws a dependent exception */
    uint64_t ep = 0;
    c->x[8] = P64(&ep);
    eptr_current(c);                                                    /* std::current_exception: not an import of the game */
    st_import(c, "__cxa_end_catch", 0, 0, 0, 0);
    CHECK(ep == e && st_eh_dtors == 1 && st->caught == 0 && exc_header(e)->refs == 1, "std::current_exception shares ownership");
    ST_RAISES(st_import(c, "_ZSt17rethrow_exceptionSt13exception_ptr", P64(&ep), 0, 0, 0));
    uint64_t dep = eh_in_flight(c);
    CHECK(dep && dep != e && exc_header(dep)->primary == e && exc_header(e)->refs == 2, "rethrow_exception: a dependent exception in flight");
    CHECK(eh_personality(c, GA(0x7103839b94ull), dep, &pad) == EH_CLEANUP, "the dependent exception has the type of the original (out_of_range)");
    eh_personality(c, GA(0x710382b690ull), dep, &pad); eh_enter_pad(c, &pad);
    CHECK(st_import(c, "__cxa_begin_catch", dep - 0x20, 0, 0, 0) == e, "catching a dependent exception yields the original object");
    st_import(c, "__cxa_end_catch", 0, 0, 0, 0);
    CHECK(!exc_header(dep) && st_eh_dtors == 1 && exc_header(e)->refs == 1, "end_catch frees the dependent exception only");
    st_import(c, "_ZNSt13exception_ptrD1Ev", P64(&ep), 0, 0, 0);
    CHECK(st_eh_dtors == 2 && !exc_header(e), "the last exception_ptr destroys the object");
    /* the libc++ helpers really throw */
    ST_RAISES(st_import(c, "_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv", 0, 0, 0, 0));
    uint64_t le = eh_in_flight(c);
    CHECK(le && !strcmp(eh_type_name(le), "St12length_error") && !strcmp((const char *)(uintptr_t)cxxrt_call(c, rd64(rd64(le) + 0x10), le, 0, 0), "vector"), "__throw_length_error throws std::length_error(vector)");
    CHECK(eh_personality(c, GA(0x7103839b94ull), le, &pad) == EH_CLEANUP && eh_personality(c, GA(0x710382b690ull), le, &pad) == EH_HANDLER, "and the personality classifies it");
    eh_enter_pad(c, &pad);
    st_import(c, "__cxa_begin_catch", le - 0x20, 0, 0, 0);
    st_import(c, "__cxa_end_catch", 0, 0, 0, 0);
    CHECK(!exc_header(le) && st->uncaught == 0 && st->caught == 0, "its destructor (the imported vtable's) ran and the state is clean");
    /* state is per context: another Ctx sees nothing */
    Ctx *c2 = st_ctx();
    ST_RAISES(st_import(c, "_ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv", 0, 0, 0, 0));
    CHECK(eh_in_flight(c) && !eh_in_flight(c2), "throw state belongs to the Ctx (guest thread or fiber), not to the host thread");
    eh_personality(c, GA(0x710382b690ull), eh_in_flight(c), &pad); eh_enter_pad(c, &pad);
    st_import(c, "__cxa_begin_catch", c->x[0], 0, 0, 0); st_import(c, "__cxa_end_catch", 0, 0, 0, 0);
    eh_set_unwinder(installed);
}

/* A module's tables: lua2cpp_common mapped from its .nro at its load address and relocated here (the self-test
 * exits afterwards, nnro.c never sees it). Expected values hand-decoded with ro_study\nro_eh.py. */
static void st_eh_module(void) {
    const uint64_t base = 0x7200000000ull;
    Ctx *c = st_ctx();
    FILE *f = fopen("ro_study\\nro\\lua2cpp_common.nro", "rb");
    if (!f) { printf("  (ro_study\\nro\\lua2cpp_common.nro not found: module tables not tested)\n"); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *mem = VirtualAlloc((void *)(uintptr_t)base, 0x900000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (P64(mem) != base || fread(mem, 1, (size_t)size, f) != (size_t)size) { fclose(f); CHECK(0, "cannot map the module at its base"); return; }
    fclose(f);
    uint64_t mod0 = base + rd32(base + 4), rela = 0, relasz = 0, symtab = 0, relocs = 0;
    for (uint64_t dyn = mod0 + (uint64_t)(int64_t)(int32_t)rd32(mod0 + 4); rd64(dyn); dyn += 16) {
        if (rd64(dyn) == 7) rela = rd64(dyn + 8); else if (rd64(dyn) == 8) relasz = rd64(dyn + 8); else if (rd64(dyn) == 6) symtab = rd64(dyn + 8);
    }
    for (uint64_t i = 0; i < relasz / 24; i++) {
        uint64_t e = base + rela + i * 24, off = rd64(e), info = rd64(e + 8), addend = rd64(e + 16);
        if ((uint32_t)info == 0x403) { wr64(base + off, base + addend); relocs++; }
        else if ((uint32_t)info == 0x101 || (uint32_t)info == 0x401) {                 /* symbols the module defines itself */
            uint64_t sym = base + symtab + (info >> 32) * 24;
            if (rd16(sym + 6)) { wr64(base + off, base + rd64(sym + 8) + ((uint32_t)info == 0x101 ? addend : 0)); relocs++; }
        }
    }
    CHECK(eh_register_image(base, 0x900000, 0), "eh_register_image(lua2cpp_common) through its MOD0");
    EhFunction fn;
    CHECK(eh_find_function(0x720052dd20ull, &fn) && fn.start == 0x720052dc70ull && fn.end == 0x720052dd24ull && fn.lsda == 0, "the throwing function f_720052dc70 has no LSDA");
    CHECK(eh_find_function(0x720052e3dcull, &fn) && fn.start == 0x720052e2f0ull && fn.end == 0x720052e4e4ull && fn.lsda == 0x720079cee0ull, "f_720052e2f0 and its LSDA");
    uint64_t shutdown_ti = rd64(0x7200800948ull);                       /* the GOT slot its type table names */
    printf("  type caught by f_720052e2f0: %s\n", cxxrt_type_name(shutdown_ti));
    CHECK(!strcmp(cxxrt_type_name(shutdown_ti), "N7lua2cpp12_GLOBAL__N_120L2CExceptionShutdownE"), "the catch clause names L2CExceptionShutdown");
    uint64_t e = cxxrt_allocate_exception(8), other = cxxrt_allocate_exception(16);
    EhPad pad;
    cxxrt_exception_init(e, shutdown_ti, 0);
    cxxrt_exception_init(other, P64(g_ti_out_of_range), 0);
    CHECK(eh_personality(c, 0x720052e3dcull, e, &pad) == EH_HANDLER && pad.selector == 1 && pad.landing_pad == 0x720052e4bcull && pad.adjusted == e, "L2CExceptionShutdown at 0x720052e3dc: handler, selector 1, pad 0x720052e4bc");
    CHECK(eh_personality(c, 0x720052e3f0ull, e, &pad) == EH_HANDLER && pad.landing_pad == 0x720052e4a8ull, "second protected range of the same function");
    CHECK(eh_personality(c, 0x720052e3dcull, other, &pad) == EH_CLEANUP && pad.selector == 0 && pad.landing_pad == 0x720052e4bcull, "another type gets that pad as a clean-up (action chain [catch, cleanup])");
    CHECK(eh_personality(c, 0x72005250d4ull, e, &pad) == EH_CLEANUP && pad.landing_pad == 0x7200525148ull, "intermediate frame f_7200525090: clean-up pad 0x7200525148");
    CHECK(eh_personality(c, 0x72005250fcull, e, &pad) == EH_CLEANUP && pad.landing_pad == 0x7200525144ull, "clean-up pad 0x7200525144");
    CHECK(eh_personality(c, 0x7200525090ull, e, &pad) == EH_NONE && eh_personality(c, 0x720052dd1cull, e, &pad) == EH_NONE, "call sites without a pad, and a function without tables");
    CHECK(eh_personality(c, 0x72004f6078ull, e, &pad) == EH_CLEANUP && pad.landing_pad == 0x72004f61a4ull, "intermediate frame f_72004f6000: clean-up pad 0x72004f61a4");
    CHECK(eh_personality(c, 0x720052da78ull, other, &pad) == EH_HANDLER && pad.landing_pad == 0x720052dc14ull && pad.selector == 1, "catch (...) in f_720052da50");
    eh_unregister_image(base);
    CHECK(!eh_find_function(0x720052e3dcull, &fn), "eh_unregister_image");
    exc_free(e); exc_free(other);
    printf("  (%llu relocations applied to the test mapping)\n", (unsigned long long)relocs);
}

/* ---- the real unwinder on real code of main (needs a game DLL lifted with resume entries: ssbu_game_eh.dll)
 * f_7103839a10(const char *text, float *out) is the game's string -> float conversion (boost::lexical_cast): it reads
 * the number with the imported `istream >> float` inside `try { } catch (std::ios_base::failure &) { return false; }`
 * (handler pad 0x7103839de8). Here the imported operator is replaced for one call by a function that throws
 * ios_base::failure, so the exception starts in a host handler, passes the guest frame of f_7103839a10, is matched by
 * the personality, and the handler plus the rest of the function run through the resume entry. */
void register_unwind_imports(void);                      /* nnunwind.c */
static void st_throwing_extract(Ctx *c) { cxxrt_throw_ios_failure(c, "selftest"); }
static void st_main_unwind(Ctx *c) {
    static const char *const op = "_ZNSt3__113basic_istreamIcNS_11char_traitsIcEEErsERf";
    printf("unwinding through main (f_7103839a10: catch (std::ios_base::failure &))\n");
    if (!eh_main_resume_entry(GA(0x7103839de8ull))) { printf("  (this game DLL was lifted without resume entries: not tested)\n"); return; }
    register_unwind_imports();
    float out = -1.0f;
    uint64_t r = cxxrt_call(c, GA(0x7103839a10ull), P64("2.5"), P64(&out), 0);
    CHECK((uint8_t)r == 1 && out == 2.5f, "control: the conversion works when nothing throws (returned %d, value %g)", (int)(uint8_t)r, out);
    ImportDef swap = {op, st_throwing_extract}, back = {op, import_lookup(op)};
    register_imports(&swap, 1);
    uint64_t sp = c->x[31];
    for (int i = 19; i <= 29; i++) c->x[i] = 0x1111000000000000ull + (uint64_t)i;
    for (int i = 8; i < 16; i++) c->v[i].d[0] = 0x2222000000000000ull + (uint64_t)i;
    out = -1.0f;
    r = cxxrt_call(c, GA(0x7103839a10ull), P64("7.25"), P64(&out), 0);
    register_imports(&back, 1);
    int regs_ok = c->x[31] == sp;
    for (int i = 19; i <= 29; i++) if (c->x[i] != 0x1111000000000000ull + (uint64_t)i) regs_ok = 0;
    for (int i = 8; i < 16; i++) if (c->v[i].d[0] != 0x2222000000000000ull + (uint64_t)i) regs_ok = 0;
    EhState *st = eh_state(c);
    CHECK((uint8_t)r == 0, "the handler ran and the function returned false (returned %d)", (int)(uint8_t)r);
    CHECK(regs_ok, "sp, x19..x29 and d8..d15 are what they were before the call");
    CHECK(!st->in_flight && !st->caught && st->uncaught == 0, "no exception left in flight or caught (the handler's __cxa_end_catch destroyed it)");
    out = -1.0f;
    r = cxxrt_call(c, GA(0x7103839a10ull), P64("-0.125"), P64(&out), 0);
    CHECK((uint8_t)r == 1 && out == -0.125f, "the function works again afterwards (returned %d, value %g)", (int)(uint8_t)r, out);
}

static DWORD WINAPI st_main(LPVOID arg) {
    int mode = atoi(getenv("SSBU_CXXRT_SELFTEST"));
    (void)arg;
    printf("\n=== C/C++ runtime self-test (mode %d)\n", mode);
    printf("binary128 soft-float\n"); st_quad();
    printf("sscanf / printf\n"); st_scanf();
    printf("gmtime, localeconv, random_device\n"); st_libc();
    printf("guard variables\n"); st_guard();
    printf("exception classes and vtables\n"); st_exceptions();
    printf("locale facets and stream vtables\n"); st_facets();
    printf("promise / future\n"); st_future();
    printf("__dynamic_cast on the game's RTTI\n"); st_dyncast();
    printf("exception tables of main (personality)\n"); st_eh_tables();
    printf("throw / catch / rethrow bookkeeping\n"); st_eh_flow();
    printf("exception tables of a module (lua2cpp_common)\n"); st_eh_module();
    if (mode >= 2) {
        Ctx *c = st_ctx();
        g_main_stack_base = c->x[31] - (4 << 20) + 0x1000; g_main_stack_size = 4 << 20;
        printf("booting the game far enough to have its allocator (nninitStartup + static constructors)\n");
        call_guest(c, GA(GUEST_BASE + 0x2c55e0ULL));
        for (uint64_t i = 0; i < g_main.init_arraysz / 8; i++) { uint64_t fn = rd64(GUEST_BASE + g_main.init_array + i * 8); if (fn) call_guest(c, fn); }
        printf("std::string, error messages, wide strings on the game heap\n"); st_with_game_heap(c);
#ifdef ST_GAME_STREAM_TEST
        st_game_stream(c);
#endif
        st_main_unwind(c);
    }
    printf("=== self-test: %d checks, %d failed\n", st_checks, st_fail);
    fflush(stdout);
    return 0;
}
static void cxxrt_selftest(void) {
    HANDLE t = CreateThread(NULL, (SIZE_T)1 << 30, st_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    WaitForSingleObject(t, INFINITE);
    ExitProcess((UINT)st_fail);
}

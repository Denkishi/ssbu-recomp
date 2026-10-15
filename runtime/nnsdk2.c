/* Entry point of the SDK families added on 2026-10-08, and their host-side self-test.
 *
 *   sdklift_rt.c / sdklift_gen.c   code lifted out of the real SDK module (pure functions, exact behaviour)
 *   nnmii.c      nn::mii Database (service side driven by lifted SDK code), AppendMii
 *   nnapplet.c   library applets: nn::web, nn::swkbd, nn::ec shop pages, amiibo settings; nn::nfp; nn::oe volume
 *   nnalbum.c    nn::album (screenshots, movie maker)
 *   nnnet.c      nn::ssl C API, nn::socket leftovers, nn::bcat, nn::friends, nn::account, nn::ldn, nn::ec accessor, glslc
 *   nnfsdir.c    nn::fs directory functions
 *
 * SSBU_SDK_SELFTEST=1: run the tests below right after registration and exit (exit code = failures).
 * SSBU_DUMP_UNREG2=<file>: names of SDK imports (nn..., glslc...) still without a handler after this
 * file's registrations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

void register_nnmii_imports(void);
void register_nnapplet_imports(void);
void register_nnalbum_imports(void);
void register_nnnet_imports(void);
void register_nnfsdir_imports(void);
int nnmii_selftest(Ctx *c);
int nnapplet_selftest(Ctx *c);
int nnalbum_selftest(Ctx *c);
int nnnet_selftest(Ctx *c);
int nnfsdir_selftest(Ctx *c);
int nnfs_selftest(Ctx *c);
int bcat_selftest(Ctx *c);
int nnhid_selftest(Ctx *c);
int nnro_selftest(Ctx *c);
int nvn_sdk_selftest(Ctx *c);
int nntime_selftest(Ctx *c);
int libc2_stdio_selftest(Ctx *c);       /* nnlibc2_c5b.inc: musl's stdio (close5) */
void libc2_install_musl_stdio(void);
void nnnet_register_final(void);

#define P(v) ((uint64_t)(uintptr_t)(v))
#include "sdktest_data.h"

static ImportFn handler(const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("  no handler registered for %s\n", name); report_and_exit(3); }
    return f;
}

/* ---- nn::util::DecompressZlib(void *dst, size_t dstSize, const void *src, size_t srcSize, void *work, size_t workSize) -> bool */
static int zlib_selftest(Ctx *c) {
    ImportFn f = handler("_ZN2nn4util14DecompressZlibEPvmPKvmS1_m");
    int bad = 0, n = (int)(sizeof zvecs / sizeof zvecs[0]);
    size_t work_size = 0x10000;
    uint8_t *work = malloc(work_size), *out = malloc(0x20000);
    for (int i = 0; i < n; i++) {
        memset(out, 0xcd, 0x20000);
        c->x[0] = P(out); c->x[1] = zvecs[i].plain_size; c->x[2] = P(zvecs[i].comp); c->x[3] = zvecs[i].comp_size;
        c->x[4] = P(work); c->x[5] = work_size;
        f(c);
        int ok = (c->x[0] & 1) && memcmp(out, zvecs[i].plain, zvecs[i].plain_size) == 0;
        for (int k = 0; k < 32; k++) if (out[zvecs[i].plain_size + k] != 0xcd) ok = 0;         /* nothing past dstSize */
        printf("  zlib: vector %-7s %5u -> %5u bytes: %s\n", zvecs[i].name, zvecs[i].comp_size, zvecs[i].plain_size, ok ? "ok" : "FAILED");
        bad += !ok;
    }
    c->x[0] = P(out); c->x[1] = ZVEC_CORRUPT_PLAIN_SIZE; c->x[2] = P(zvec_corrupt); c->x[3] = sizeof zvec_corrupt; c->x[4] = P(work); c->x[5] = work_size;
    f(c);
    printf("  zlib: corrupted stream is refused: %s\n", (c->x[0] & 1) ? "FAILED (accepted)" : "ok");
    bad += (int)(c->x[0] & 1);
    c->x[0] = P(out); c->x[1] = 100; c->x[2] = P(zvecs[0].comp); c->x[3] = zvecs[0].comp_size; c->x[4] = P(work); c->x[5] = work_size;
    memset(out, 0xcd, 0x20000);
    f(c);
    int ok = !(c->x[0] & 1);
    for (int k = 0; k < 32; k++) if (out[100 + k] != 0xcd) ok = 0;
    printf("  zlib: output buffer too small is refused without overrun: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    free(work); free(out);
    return bad;
}

/* ---- nn::image */
#define IMG(n) "_ZN2nn5image" n
static int jpeg_decode(Ctx *c, const uint8_t *data, size_t size, uint8_t **pixels, int *w, int *h, const char *label) {
    uint8_t *dec = calloc(1, 0x200);
    memset(dec + 0x100, 0x77, 0x100);
    c->x[0] = P(dec); handler(IMG("11JpegDecoderC1Ev"))(c);
    c->x[0] = P(dec); c->x[1] = P(data); c->x[2] = size; handler(IMG("11JpegDecoder12SetImageDataEPKvm"))(c);
    c->x[0] = P(dec); handler(IMG("11JpegDecoder7AnalyzeEv"))(c);
    int status = (int)c->x[0];
    if (status) { printf("  jpeg: %s: Analyze status %d\n", label, status); free(dec); return 1; }
    c->x[0] = P(dec); handler("_ZNK2nn5image11JpegDecoder20GetAnalyzedDimensionEv")(c);
    *w = (int)(uint32_t)c->x[0]; *h = (int)(c->x[0] >> 32);
    c->x[0] = P(dec); handler("_ZNK2nn5image11JpegDecoder25GetAnalyzedWorkBufferSizeEv")(c);
    size_t work_size = (size_t)c->x[0];
    if (*w <= 0 || *h <= 0 || *w > 4096 || *h > 4096 || work_size > (64u << 20)) { printf("  jpeg: %s: implausible analysis %dx%d work %zu\n", label, *w, *h, work_size); free(dec); return 1; }
    uint8_t *work = malloc(work_size + 64);
    size_t pix_size = (size_t)*w * (size_t)*h * 4;
    *pixels = malloc(pix_size + 64);
    memset(*pixels + pix_size, 0x3c, 64);
    memset(work + work_size, 0x3c, 64);
    c->x[0] = P(dec); c->x[1] = P(*pixels); c->x[2] = pix_size; c->x[3] = 1; c->x[4] = P(work); c->x[5] = work_size;
    handler(IMG("11JpegDecoder6DecodeEPvmiS2_m"))(c);
    status = (int)c->x[0];
    int bad = status != 0;
    if (status) printf("  jpeg: %s: Decode status %d\n", label, status);
    for (int k = 0; k < 64; k++) if ((*pixels)[pix_size + k] != 0x3c || work[work_size + k] != 0x3c) { printf("  jpeg: %s: wrote past a buffer\n", label); bad = 1; break; }
    for (int k = 0; k < 0x100; k++) if (dec[0x100 + k] != 0x77) { printf("  jpeg: %s: decoder object larger than 0x100\n", label); bad = 1; break; }
    c->x[0] = P(dec); handler(IMG("11JpegDecoderD1Ev"))(c);
    free(work); free(dec);
    return bad;
}
static int jpeg_selftest(Ctx *c) {
    int bad = 0, n = (int)(sizeof jvecs / sizeof jvecs[0]);
    for (int i = 0; i < n; i++) {
        uint8_t *px = NULL;
        int w = 0, h = 0;
        if (jpeg_decode(c, jvecs[i].data, jvecs[i].size, &px, &w, &h, jvecs[i].name)) { bad++; continue; }
        int maxd = 0, alpha_ok = 1;
        long long sum = 0;
        if (w != jvecs[i].width || h != jvecs[i].height) { printf("  jpeg: %s: %dx%d, expected %dx%d\n", jvecs[i].name, w, h, jvecs[i].width, jvecs[i].height); bad++; free(px); continue; }
        for (int k = 0; k < w * h; k++) {
            for (int ch = 0; ch < 3; ch++) { int dlt = abs((int)px[k * 4 + ch] - (int)jvecs[i].rgb[k * 3 + ch]); if (dlt > maxd) maxd = dlt; sum += dlt; }
            if (px[k * 4 + 3] != 0xff) alpha_ok = 0;
        }
        /* Pillow (libjpeg-turbo) smooths subsampled chroma ("fancy upsampling"); the SDK's decoder replicates it.
         * Full-resolution chroma (4:4:4) has no such step and must agree to the last bit. */
        int subsampled = strcmp(jvecs[i].name, "j444") != 0;
        int ok = maxd <= (subsampled ? 16 : 0) && alpha_ok;
        printf("  jpeg: decode %s %dx%d vs Pillow: max channel difference %d, mean %.3f, alpha %s: %s\n", jvecs[i].name, w, h, maxd,
               (double)sum / (w * h * 3), alpha_ok ? "0xff" : "NOT 0xff", ok ? "ok" : "FAILED");
        bad += !ok;
        free(px);
    }
    /* corrupt data must give an error status, not a crash (libjpeg's error exit goes through setjmp/longjmp) */
    {
        uint8_t *copy = malloc(jvecs[0].size), *px = NULL;
        int w, h;
        memcpy(copy, jvecs[0].data, jvecs[0].size);
        for (unsigned k = jvecs[0].size / 2; k < jvecs[0].size / 2 + 40 && k < jvecs[0].size; k++) copy[k] = 0xff;
        copy[jvecs[0].size / 2 + 41] = 0xd9;
        int r = jpeg_decode(c, copy, jvecs[0].size / 2 + 42, &px, &w, &h, "truncated (expected to fail)");
        printf("  jpeg: truncated/corrupt stream handled without crash: %s\n", r ? "error status returned" : "decoded (libjpeg tolerated it)");
        free(copy); free(px);
    }
    /* encode -> decode round trip: 40x24 RGBA gradient, quality 95, both pixel formats' default sampling */
    {
        int w = 40, h = 24;
        uint8_t *src = malloc((size_t)w * h * 4), *enc = calloc(1, 0x200), *jpg = malloc(1 << 16);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint8_t *p = src + (y * w + x) * 4;
            p[0] = (uint8_t)(x * 255 / (w - 1)); p[1] = (uint8_t)(y * 255 / (h - 1)); p[2] = (uint8_t)((x + y) * 255 / (w + h - 2)); p[3] = 0xff;
        }
        uint32_t dim[2] = {(uint32_t)w, (uint32_t)h};
        uint64_t out_size = 0;
        memset(enc + 0x100, 0x77, 0x100);
        c->x[0] = P(enc); handler(IMG("11JpegEncoderC1Ev"))(c);
        c->x[0] = P(enc); c->x[1] = P(src); c->x[2] = 0; c->x[3] = P(dim); c->x[4] = 1;
        handler(IMG("11JpegEncoder12SetPixelDataEPKvNS0_11PixelFormatERKNS0_9DimensionEi"))(c);
        c->x[0] = P(enc); c->x[1] = 95; handler(IMG("11JpegEncoder10SetQualityEi"))(c);
        c->x[0] = P(enc); handler(IMG("11JpegEncoder7AnalyzeEv"))(c);
        int status = (int)c->x[0];
        c->x[0] = P(enc); handler("_ZNK2nn5image11JpegEncoder25GetAnalyzedWorkBufferSizeEv")(c);
        size_t work_size = (size_t)c->x[0];
        uint8_t *work = malloc(work_size + 64);
        memset(jpg, 0x3c, 1 << 16);
        if (!status && work_size < (64u << 20)) {
            c->x[0] = P(enc); c->x[1] = P(&out_size); c->x[2] = P(jpg); c->x[3] = (1 << 16) - 64; c->x[4] = P(work); c->x[5] = work_size; c->x[6] = 0;
            handler(IMG("11JpegEncoder6EncodeEPmPvmS3_mPNS0_11ExifBuilderE"))(c);
            status = (int)c->x[0];
        }
        int ok = status == 0 && out_size > 100 && out_size < (1 << 16) - 64 && jpg[0] == 0xff && jpg[1] == 0xd8 && jpg[out_size - 2] == 0xff && jpg[out_size - 1] == 0xd9;
        for (int k = 0; k < 64; k++) if (jpg[(1 << 16) - 64 + k] != 0x3c) ok = 0;
        for (int k = 0; k < 0x100; k++) if (enc[0x100 + k] != 0x77) ok = 0;
        printf("  jpeg: encode 40x24 RGBA32 q95: status %d, %llu bytes, SOI/EOI %s\n", status, (unsigned long long)out_size, ok ? "ok" : "FAILED");
        if (ok) {
            uint8_t *px = NULL;
            int dw = 0, dh = 0, maxd = 0;
            if (jpeg_decode(c, jpg, (size_t)out_size, &px, &dw, &dh, "round trip") || dw != w || dh != h) ok = 0;
            else for (int k = 0; k < w * h * 4; k++) { int dlt = abs((int)px[k] - (int)src[k]); if (dlt > maxd) maxd = dlt; }
            if (ok && maxd > 24) ok = 0;
            printf("  jpeg: round trip decode %dx%d: max channel difference %d: %s\n", dw, dh, maxd, ok ? "ok" : "FAILED");
            free(px);
        }
        bad += !ok;
        c->x[0] = P(enc); handler(IMG("11JpegEncoderD1Ev"))(c);
        free(work); free(src); free(enc); free(jpg);
    }
    return bad;
}

/* SSBU_DUMP_UNREG2 */
static void dump_unregistered(void) {
    const char *path = getenv("SSBU_DUMP_UNREG2");
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < dyn_sym_count; i++) {
        const char *n = dyn_syms[i].name;
        if (dyn_syms[i].defined || !n[0] || import_lookup(n)) continue;
        if (strstr(n, "lua2cpp") || strstr(n, "L2C")) continue;
        if (strncmp(n, "_ZN2nn", 6) && strncmp(n, "_ZNK2nn", 7) && strncmp(n, "nn", 2) && strncmp(n, "glslc", 5)) continue;
        int is_data = 0;
        for (int k = 0; k < sdk_data_count; k++) if (strcmp(sdk_data[k].name, n) == 0) is_data = 1;
        if (!is_data) fprintf(f, "%s\n", n);
    }
    fclose(f);
}

/* ---- C library / libm / small SDK classes that run as lifted sdk.elf code (sdklift_install.h): each is called
 * through the handler the game gets and compared with an independent reference (the host C library, a closed
 * formula, a published test vector). Math: musl and the Microsoft CRT are not bit-identical, so the test only
 * demands agreement within 4 ulp and prints how many results differ in the last bits (that number is what the
 * game no longer sees with the lifted code). */
#include <math.h>
static float callf1(Ctx *c, ImportFn f, float a) { memset(&c->v[0], 0, 16); c->v[0].f[0] = a; f(c); return c->v[0].f[0]; }
static float callf2(Ctx *c, ImportFn f, float a, float b) { memset(&c->v[0], 0, 32); c->v[0].f[0] = a; c->v[1].f[0] = b; f(c); return c->v[0].f[0]; }
static double calld1(Ctx *c, ImportFn f, double a) { memset(&c->v[0], 0, 16); c->v[0].df[0] = a; f(c); return c->v[0].df[0]; }
static double calld2(Ctx *c, ImportFn f, double a, double b) { memset(&c->v[0], 0, 32); c->v[0].df[0] = a; c->v[1].df[0] = b; f(c); return c->v[0].df[0]; }
static long long ulp_f(float a, float b) {
    int32_t x, y;
    if (a != a && b != b) return 0;
    if (a != a || b != b) return 1 << 30;
    memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    if (x < 0) x = (int32_t)0x80000000 - x;
    if (y < 0) y = (int32_t)0x80000000 - y;
    return llabs((long long)x - (long long)y);
}
static long long ulp_d(double a, double b) {
    int64_t x, y;
    if (a != a && b != b) return 0;
    if (a != a || b != b) return 1 << 30;
    memcpy(&x, &a, 8); memcpy(&y, &b, 8);
    if (x < 0) x = (int64_t)0x8000000000000000ull - x;
    if (y < 0) y = (int64_t)0x8000000000000000ull - y;
    long long d = (long long)(x - y);
    return d < 0 ? -d : d;
}
static void selftest_cmp_int(Ctx *c) { int a = (int)rd32(c->x[0]), b = (int)rd32(c->x[1]); c->x[0] = (uint64_t)(int64_t)((a > b) - (a < b)); }
static int libc_selftest(Ctx *c) {
    int bad = 0;
    static const struct { const char *name; float (*ref)(float); float lo, hi; } f1[] = {
        {"sinf", sinf, -20, 20}, {"cosf", cosf, -20, 20}, {"tanf", tanf, -1.5f, 1.5f}, {"asinf", asinf, -1, 1}, {"acosf", acosf, -1, 1},
        {"atanf", atanf, -50, 50}, {"expf", expf, -20, 20}, {"exp2f", exp2f, -30, 30}, {"logf", logf, 1e-6f, 1e6f}, {"log2f", log2f, 1e-6f, 1e6f},
        {"log10f", log10f, 1e-6f, 1e6f}, {"sqrtf", sqrtf, 0, 1e6f}, {"floorf", floorf, -1000, 1000}, {"ceilf", ceilf, -1000, 1000}, {"erfcf", erfcf, -3, 6},
    };
    static const struct { const char *name; float (*ref)(float, float); float lo, hi; } f2[] = {
        {"atan2f", atan2f, -10, 10}, {"powf", powf, 0.01f, 12}, {"fmodf", fmodf, -100, 100},
    };
    static const struct { const char *name; double (*ref)(double); double lo, hi; } d1[] = { {"sin", sin, -20, 20}, {"sqrt", sqrt, 0, 1e9}, {"exp2", exp2, -30, 30} };
    static const struct { const char *name; double (*ref)(double, double); double lo, hi; } d2[] = { {"pow", pow, 0.01, 12}, {"fmod", fmod, -100, 100} };
    enum { N = 4000 };
    for (unsigned k = 0; k < sizeof f1 / sizeof f1[0]; k++) {
        ImportFn f = handler(f1[k].name);
        long long worst = 0; int differ = 0;
        for (int i = 0; i <= N; i++) {
            float x = f1[k].lo + (f1[k].hi - f1[k].lo) * (float)i / N;
            long long u = ulp_f(callf1(c, f, x), f1[k].ref(x));
            if (u) differ++;
            if (u > worst) worst = u;
        }
        printf("  libm: %-7s %d of %d results differ from the host CRT in the last bits (largest %lld ulp): %s\n", f1[k].name, differ, N + 1, worst, worst <= 4 ? "ok" : "FAILED");
        bad += worst > 4;
    }
    for (unsigned k = 0; k < sizeof f2 / sizeof f2[0]; k++) {
        ImportFn f = handler(f2[k].name);
        long long worst = 0; int differ = 0, n = 0;
        for (int i = 0; i <= 60; i++) for (int j = 0; j <= 60; j++, n++) {
            float x = f2[k].lo + (f2[k].hi - f2[k].lo) * (float)i / 60, y = f2[k].lo + (f2[k].hi - f2[k].lo) * (float)j / 61 + 0.0131f;
            if (!strcmp(f2[k].name, "powf")) y = (y - 6) * 1.5f;
            long long u = ulp_f(callf2(c, f, x, y), f2[k].ref(x, y));
            if (u) differ++;
            if (u > worst) worst = u;
        }
        printf("  libm: %-7s %d of %d results differ from the host CRT in the last bits (largest %lld ulp): %s\n", f2[k].name, differ, n, worst, worst <= 4 ? "ok" : "FAILED");
        bad += worst > 4;
    }
    for (unsigned k = 0; k < sizeof d1 / sizeof d1[0]; k++) {
        ImportFn f = handler(d1[k].name);
        long long worst = 0; int differ = 0;
        for (int i = 0; i <= N; i++) {
            double x = d1[k].lo + (d1[k].hi - d1[k].lo) * (double)i / N;
            long long u = ulp_d(calld1(c, f, x), d1[k].ref(x));
            if (u) differ++;
            if (u > worst) worst = u;
        }
        printf("  libm: %-7s %d of %d results differ from the host CRT in the last bits (largest %lld ulp): %s\n", d1[k].name, differ, N + 1, worst, worst <= 4 ? "ok" : "FAILED");
        bad += worst > 4;
    }
    for (unsigned k = 0; k < sizeof d2 / sizeof d2[0]; k++) {
        ImportFn f = handler(d2[k].name);
        long long worst = 0; int differ = 0, n = 0;
        for (int i = 0; i <= 60; i++) for (int j = 0; j <= 60; j++, n++) {
            double x = d2[k].lo + (d2[k].hi - d2[k].lo) * (double)i / 60, y = d2[k].lo + (d2[k].hi - d2[k].lo) * (double)j / 61 + 0.0131;
            if (!strcmp(d2[k].name, "pow")) y = (y - 6) * 1.5;
            long long u = ulp_d(calld2(c, f, x, y), d2[k].ref(x, y));
            if (u) differ++;
            if (u > worst) worst = u;
        }
        printf("  libm: %-7s %d of %d results differ from the host CRT in the last bits (largest %lld ulp): %s\n", d2[k].name, differ, n, worst, worst <= 4 ? "ok" : "FAILED");
        bad += worst > 4;
    }
    {   /* strings and memory against the host C library */
        static char a[64] = "the quick brown fox", b[64], m[64];
        int ok = 1;
        c->x[0] = P(a); handler("strlen")(c); ok &= c->x[0] == strlen(a);
        c->x[0] = P(b); c->x[1] = P(a); handler("strcpy")(c); ok &= !strcmp(a, b);
        c->x[0] = P(a); c->x[1] = P("the quick brown fo"); handler("strcmp")(c); ok &= (int)c->x[0] > 0;
        c->x[0] = P(a); c->x[1] = P("brown"); handler("strstr")(c); ok &= c->x[0] == P(a + 10);
        c->x[0] = P(a); c->x[1] = 'q'; handler("strchr")(c); ok &= c->x[0] == P(a + 4);
        for (int i = 0; i < 64; i++) m[i] = (char)i;
        c->x[0] = P(m + 3); c->x[1] = P(m); c->x[2] = 40; handler("memmove")(c);
        for (int i = 0; i < 40; i++) ok &= m[3 + i] == (char)i;
        c->x[0] = P(m); c->x[1] = 0x5a; c->x[2] = 64; handler("memset")(c);
        for (int i = 0; i < 64; i++) ok &= m[i] == 0x5a;
        c->x[0] = P(b); c->x[1] = P(a); c->x[2] = 20; handler("memcmp")(c); ok &= (int)c->x[0] == 0;
        c->x[0] = P("  -1234xyz"); handler("atoi")(c); ok &= (int)c->x[0] == -1234;
        c->x[0] = P("-0x1f"); c->x[1] = 0; c->x[2] = 0; handler("strtol")(c); ok &= (int64_t)c->x[0] == -31;
        c->x[0] = P("3.25e2"); handler("atof")(c); ok &= c->v[0].df[0] == 325.0;
        c->x[0] = 'Q'; handler("tolower")(c); ok &= (int)c->x[0] == 'q';
        printf("  libc: strlen strcpy strcmp strstr strchr memmove memset memcmp atoi strtol atof tolower: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
    }
    {   /* snprintf with integer, string and floating-point arguments (AArch64 variadic call: x3.., v0..) */
        char out[128], ref[128];
        memset(&c->v[0], 0, 32);
        c->x[0] = P(out); c->x[1] = sizeof out; c->x[2] = P("%d|%5.2f|%s|%x|%lld|%e|%g"); c->x[3] = (uint64_t)(int64_t)-42; c->x[4] = P("str"); c->x[5] = 0xbeef;
        c->x[6] = (uint64_t)-1234567890123ll; c->v[0].df[0] = 3.14159; c->v[1].df[0] = 12345.678; c->v[2].df[0] = 0.0001;
        handler("snprintf")(c);
        snprintf(ref, sizeof ref, "%d|%5.2f|%s|%x|%lld|%e|%g", -42, 3.14159, "str", 0xbeef, -1234567890123ll, 12345.678, 0.0001);
        int ok = !strcmp(out, ref) && (int)c->x[0] == (int)strlen(ref);
        printf("  libc: snprintf \"%s\": %s\n", out, ok ? "ok" : "FAILED");
        bad += !ok;
    }
    {   /* musl's rand: seed = 6364136223846793005 * seed + 1, result = seed >> 33; srand(s) sets seed = s - 1 */
        uint64_t seed = 12345 - 1;
        int ok = 1;
        c->x[0] = 12345; handler("srand")(c);
        for (int i = 0; i < 8; i++) { seed = 6364136223846793005ull * seed + 1; handler("rand")(c); ok &= (uint32_t)c->x[0] == (uint32_t)(seed >> 33); }
        printf("  libc: srand(12345) + 8 x rand follow musl's generator: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
    }
    {   /* qsort with a guest callback, errno location, tick conversion, calendar, MD5, frame heap */
        int v[9] = {5, -3, 9, 0, 7, 7, -8, 2, 1}, ok = 1;
        c->x[0] = P(v); c->x[1] = 9; c->x[2] = 4; c->x[3] = hostfn_address("audit self-test comparator", selftest_cmp_int);
        handler("qsort")(c);
        for (int i = 1; i < 9; i++) ok &= v[i - 1] <= v[i];
        printf("  libc: qsort through a guest callback: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
        handler("__errno_location")(c);
        ok = c->x[0] == c->tpidr + 0x1d8;
        printf("  libc: errno lives in the thread's own block (thread pointer + 0x1d8): %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
        c->x[0] = 19200000ull * 3; handler("_ZN2nn2os17ConvertToTimeSpanENS0_4TickE")(c); ok = c->x[0] == 3000000000ull;
        c->x[0] = 2500000000ull; handler("_ZN2nn2os13ConvertToTickENS_8TimeSpanE")(c); ok &= c->x[0] == 48000000ull;
        c->x[0] = 0x7fffffffffull; handler("_ZN2nn2os17ConvertToTimeSpanENS0_4TickE")(c); ok &= c->x[0] == (uint64_t)(((unsigned __int128)0x7fffffffffull * 1000000000ull) / 19200000ull);
        printf("  nn::os: tick <-> time span conversions are exact integers: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
        int64_t posix = 1700000000;                          /* 2023-11-14 22:13:20 UTC, a Tuesday */
        c->x[0] = P(&posix); handler("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE")(c);
        uint64_t cal = c->x[0];
        ok = (uint16_t)cal == 2023 && (uint8_t)(cal >> 16) == 11 && (uint8_t)(cal >> 24) == 14 && (uint8_t)(cal >> 32) == 22 && (uint8_t)(cal >> 40) == 13 && (uint8_t)(cal >> 48) == 20;
        c->x[0] = P(&cal); handler("_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE")(c); ok &= (int64_t)c->x[0] == posix;
        c->x[0] = 2023; c->x[1] = 11; c->x[2] = 14; handler("_ZN2nn4time12GetDayOfWeekEiii")(c); ok &= (int)c->x[0] == 2;
        c->x[0] = 2023; c->x[1] = 2; c->x[2] = 29; handler("_ZN2nn4time11IsValidDateEiii")(c); ok &= !(c->x[0] & 1);
        printf("  nn::time: calendar <-> posix in UTC, day of week, date validity: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
        uint8_t *md5 = calloc(1, 0x400), digest[16];
        static const uint8_t md5_abc[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
        c->x[0] = P(md5); handler("_ZN2nn6crypto6detail7Md5Impl10InitializeEv")(c);
        c->x[0] = P(md5); c->x[1] = P("abc"); c->x[2] = 3; handler("_ZN2nn6crypto6detail7Md5Impl6UpdateEPKvm")(c);
        c->x[0] = P(md5); c->x[1] = P(digest); c->x[2] = 16; handler("_ZN2nn6crypto6detail7Md5Impl7GetHashEPvm")(c);
        ok = !memcmp(digest, md5_abc, 16);
        printf("  nn::crypto: MD5(\"abc\") = RFC 1321 test vector: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
        free(md5);
        uint8_t *region = _aligned_malloc(0x4000, 0x1000);
        c->x[0] = P(region); c->x[1] = 0x4000; c->x[2] = 0; handler("_ZN2nn4lmem15CreateFrameHeapEPvmi")(c);
        uint64_t heap = c->x[0], p1, p2;
        c->x[0] = heap; c->x[1] = 0x100; c->x[2] = 16; handler("_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi")(c); p1 = c->x[0];
        c->x[0] = heap; c->x[1] = 0x100; c->x[2] = 64; handler("_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi")(c); p2 = c->x[0];
        ok = heap && p1 >= P(region) && p2 >= p1 + 0x100 && p2 + 0x100 <= P(region) + 0x4000 && !(p1 & 15) && !(p2 & 63);
        c->x[0] = heap; c->x[1] = 0x8000; c->x[2] = 16; handler("_ZN2nn4lmem21AllocateFromFrameHeapEPNS0_6detail8HeapHeadEmi")(c); ok &= c->x[0] == 0;
        c->x[0] = heap; handler("_ZN2nn4lmem16DestroyFrameHeapEPNS0_6detail8HeapHeadE")(c);
        printf("  nn::lmem: frame heap inside the caller's region, aligned blocks, too-large request refused: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
        _aligned_free(region);
    }
    return bad;
}

/* ---------------------------------------------------------------- under the lifted nn::mem::StandardAllocator
 * (sdklift_roots.txt `cut` lines). The allocator itself (nlib nmalloc, 86 functions) is the SDK's code; these are the
 * functions below it that end in kernel calls or in musl's per-thread data.
 *   pthread_once            musl: control word 0 = not run, 1 = running, 2 = done; the routine runs exactly once
 *   pthread_key_*           on the host's nn::os TLS slots (the console's musl does the same through __nnmusl_AllocateTlsSlot)
 *   nn::os::AllocateMemoryBlock / FreeMemoryBlock   [sdk 0x3326b0 / 0x3326f0] pages of the memory heap; failure = 0x1003
 *                           ([sdk 0x3355d8] os OutOfMemory). Host: committed pages from Windows.
 *   nn::os::SetMemoryPermission   [sdk 0x339730] accepts 0 (none), 1 (read), 3 (read-write) -> VirtualProtect
 *   nn::os::SetMemoryAttribute    cache attribute of a mapping (svc 3): no counterpart for ordinary host pages
 *   nn::os::IsVirtualAddressMemoryEnabled   [sdk 0x336af0] kernel GetInfo(16): true for a 64-bit application
 * The game gives its StandardAllocator a buffer (trace: Initialize(this, buffer, 0x100000) from 0x71036f8780), so the
 * memory-block functions are only reached by the self-test's buffer-less allocator. */
static void pt_once(Ctx *c) {
    volatile LONG *ctl = (volatile LONG *)(uintptr_t)c->x[0];
    uint64_t fn = c->x[1];
    for (;;) {
        LONG v = InterlockedCompareExchange(ctl, 1, 0);
        if (v == 2) break;
        if (v == 0) { call_guest(c, fn); InterlockedExchange(ctl, 2); break; }
        SwitchToThread();
    }
    c->x[0] = 0;
}
static void pt_key_create(Ctx *c) { uint64_t key = c->x[0]; handler("_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE")(c); (void)key; c->x[0] = 0; }
static void pt_getspecific(Ctx *c) { c->x[0] &= 0xffffffffu; handler("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE")(c); }
static void pt_setspecific(Ctx *c) { c->x[0] &= 0xffffffffu; handler("_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm")(c); c->x[0] = 0; }
static void os_AllocateMemoryBlock(Ctx *c) {
    void *p = VirtualAlloc(NULL, (SIZE_T)c->x[1], MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) wr64(c->x[0], P(p));
    c->x[0] = p ? 0 : 0x1003;
}
static void os_FreeMemoryBlock(Ctx *c) { VirtualFree((void *)(uintptr_t)c->x[0], 0, MEM_RELEASE); }
static void os_SetMemoryPermission(Ctx *c) {
    DWORD old, prot = (uint32_t)c->x[2] == 3 ? PAGE_READWRITE : (uint32_t)c->x[2] == 1 ? PAGE_READONLY : PAGE_NOACCESS;
    if ((uint32_t)c->x[2] != 0 && (uint32_t)c->x[2] != 1 && (uint32_t)c->x[2] != 3) guest_trap("nn::os::SetMemoryPermission: invalid permission", c->x[30] - 4);
    { void cxxrt_mem_changed(void); cxxrt_mem_changed(); }      /* nncxxrt.c: remembered readable checks are forgotten (P3) */
    if (!VirtualProtect((void *)(uintptr_t)c->x[0], (SIZE_T)c->x[1], prot, &old)) guest_trap("nn::os::SetMemoryPermission: VirtualProtect failed", c->x[0]);
}
static void os_SetMemoryAttribute(Ctx *c) { (void)c; }
static void os_FreeAddressRegion2(Ctx *c) { VirtualFree((void *)(uintptr_t)c->x[0], 0, MEM_RELEASE); c->x[0] = 0; }
static void os_IsVammEnabled(Ctx *c) { c->x[0] = 1; }
static void install_mem_overrides(void) {
    sdk_host_override("pthread_once", pt_once);
    sdk_host_override("pthread_key_create", pt_key_create);
    sdk_host_override("pthread_getspecific", pt_getspecific);
    sdk_host_override("pthread_setspecific", pt_setspecific);
    sdk_host_override("_ZN2nn2os19AllocateMemoryBlockEPmm", os_AllocateMemoryBlock);
    sdk_host_override("_ZN2nn2os15FreeMemoryBlockEmm", os_FreeMemoryBlock);
    sdk_host_override("_ZN2nn2os19SetMemoryPermissionEmmNS0_16MemoryPermissionE", os_SetMemoryPermission);
    sdk_host_override("_ZN2nn2os18SetMemoryAttributeEmmNS0_15MemoryAttributeE", os_SetMemoryAttribute);
    sdk_host_override("_ZN2nn2os17FreeAddressRegionEm", os_FreeAddressRegion2);
    sdk_host_override("_ZN2nn2os29IsVirtualAddressMemoryEnabledEv", os_IsVammEnabled);
}

/* ---- nn::mem::NumberLineAllocator and StandardAllocator: the lifted SDK classes, driven like the game drives them */
static void st_alloc_cb(Ctx *c) { c->x[0] = P(_aligned_malloc((size_t)c->x[0], 16)); }      /* void *(*)(size_t, void *) */
static void st_free_cb(Ctx *c) { _aligned_free((void *)(uintptr_t)c->x[0]); }               /* void (*)(void *, void *) */
static int mem_selftest(Ctx *c) {
    int bad = 0;
    #define MEMCHECK(cond, text) do { if (!(cond)) { printf("  FAIL mem: %s\n", text); bad++; } } while (0)
    ImportFn nla_init = sdk_lifted("_ZN2nn3mem19NumberLineAllocator10InitializeEPFPvmS2_ES2_PFvS2_S2_ES2_b"), nla_add = sdk_lifted("_ZN2nn3mem19NumberLineAllocator8AddRangeEii"),
             nla_alloc = sdk_lifted("_ZN2nn3mem19NumberLineAllocator8AllocateEPii"), nla_free = sdk_lifted("_ZN2nn3mem19NumberLineAllocator4FreeEi");
    if (!nla_init || !nla_add || !nla_alloc || !nla_free) { printf("  FAIL mem: NumberLineAllocator is not lifted\n"); return 1; }
    uint64_t *obj = _aligned_malloc(0x40, 16);
    int32_t *ids = _aligned_malloc(64, 16);
    memset(obj, 0, 0x40);
    c->x[0] = P(obj); c->x[1] = hostfn_address("selftest alloc callback", st_alloc_cb); c->x[2] = 0; c->x[3] = hostfn_address("selftest free callback", st_free_cb); c->x[4] = 0; c->x[5] = 0;
    nla_init(c);
    MEMCHECK(obj[0] != 0, "NumberLineAllocator::Initialize left the object empty");
    c->x[0] = P(obj); c->x[1] = 0x100; c->x[2] = 0x400; nla_add(c);
    int ok_add = (int)(c->x[0] & 1);
    for (int i = 0; i < 4; i++) { c->x[0] = P(obj); c->x[1] = P(&ids[i]); c->x[2] = 16 << i; nla_alloc(c); MEMCHECK(c->x[0] & 1, "NumberLineAllocator::Allocate failed"); }
    for (int i = 0; i < 4; i++) for (int j = 0; j < i; j++) MEMCHECK(ids[i] >= ids[j] + (16 << j) || ids[j] >= ids[i] + (16 << i), "NumberLineAllocator: ranges overlap");
    for (int i = 0; i < 4; i++) MEMCHECK(ids[i] >= 0x100 && ids[i] + (16 << i) <= 0x100 + 0x400 + 0x100, "NumberLineAllocator: id outside the added range");
    int first = ids[0];
    c->x[0] = P(obj); c->x[1] = (uint64_t)ids[0]; nla_free(c);
    c->x[0] = P(obj); c->x[1] = P(&ids[4]); c->x[2] = 16; nla_alloc(c);
    printf("  NumberLineAllocator (lifted): AddRange(0x100, 0x400) -> %d; Allocate 16/32/64/128 -> %d %d %d %d; Free(%d) + Allocate(16) -> %d\n", ok_add, first, ids[1], ids[2], ids[3], first, ids[4]);
    c->x[0] = P(obj); c->x[1] = P(&ids[5]); c->x[2] = 0x1000; nla_alloc(c);
    MEMCHECK(!(c->x[0] & 1), "NumberLineAllocator: a request larger than the range succeeded");

    ImportFn sa_ctor = sdk_lifted("_ZN2nn3mem17StandardAllocatorC1Ev"), sa_init = sdk_lifted("_ZN2nn3mem17StandardAllocator10InitializeEPvm"), sa_alloc = sdk_lifted("_ZN2nn3mem17StandardAllocator8AllocateEmm"),
             sa_free = sdk_lifted("_ZN2nn3mem17StandardAllocator4FreeEPv"), sa_total = sdk_lifted("_ZNK2nn3mem17StandardAllocator16GetTotalFreeSizeEv"),
             sa_realloc = sdk_lifted("_ZN2nn3mem17StandardAllocator10ReallocateEPvm"), sa_fin = sdk_lifted("_ZN2nn3mem17StandardAllocator8FinalizeEv");
    if (!sa_ctor || !sa_init || !sa_alloc || !sa_free || !sa_total || !sa_realloc || !sa_fin) { printf("  FAIL mem: StandardAllocator is not lifted\n"); return bad + 1; }
    for (int mode = 0; mode < 2; mode++) {                       /* 0: on a buffer, as the game does; 1: buffer-less (virtual address memory) */
        uint8_t *sa = _aligned_malloc(0x400, 16), *buf = mode ? NULL : VirtualAlloc(NULL, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        memset(sa, 0, 0x400);
        c->x[0] = P(sa); sa_ctor(c);
        c->x[0] = P(sa); c->x[1] = P(buf); c->x[2] = 0x100000; sa_init(c);
        MEMCHECK(sa[0] == 1, "StandardAllocator::Initialize did not mark the object initialised");
        uint64_t free0 = 0, free1 = 0, free2 = 0;               /* GetTotalFreeSize of a buffer-less allocator asks the kernel (trap, sdklift_roots.txt) */
        if (!mode) { c->x[0] = P(sa); sa_total(c); free0 = c->x[0]; }
        c->x[0] = P(sa); c->x[1] = 0x1f0; c->x[2] = 0x40; sa_alloc(c); uint64_t p1 = c->x[0];
        c->x[0] = P(sa); c->x[1] = 0x5007; c->x[2] = 0x40; sa_alloc(c); uint64_t p2 = c->x[0];
        MEMCHECK(p1 && p2 && !(p1 & 0x3f) && !(p2 & 0x3f), "StandardAllocator::Allocate: null or misaligned block");
        if (!mode) MEMCHECK(p1 >= P(buf) && p2 + 0x5007 <= P(buf) + 0x100000 && (p2 >= p1 + 0x1f0 || p1 >= p2 + 0x5007), "StandardAllocator: block outside the buffer or overlapping");
        if (p1 && p2) { memset((void *)(uintptr_t)p1, 0xa5, 0x1f0); memset((void *)(uintptr_t)p2, 0x5a, 0x5007); }
        if (!mode) { c->x[0] = P(sa); sa_total(c); free1 = c->x[0]; }
        c->x[0] = P(sa); c->x[1] = p1; c->x[2] = 0x3000; sa_realloc(c); uint64_t p3 = c->x[0];
        MEMCHECK(p3 && *(uint8_t *)(uintptr_t)p3 == 0xa5 && *(uint8_t *)(uintptr_t)(p3 + 0x1ef) == 0xa5, "StandardAllocator::Reallocate lost the contents");
        c->x[0] = P(sa); c->x[1] = p3; sa_free(c);
        c->x[0] = P(sa); c->x[1] = p2; sa_free(c);
        if (!mode) { c->x[0] = P(sa); sa_total(c); free2 = c->x[0]; }
        if (!mode) MEMCHECK(free1 < free0 && free2 > free1 && free2 <= free0, "StandardAllocator::GetTotalFreeSize does not follow the allocations");   /* one management page stays in use */
        printf("  StandardAllocator (lifted, %s): free %llu -> %llu after 2 blocks -> %llu after freeing them\n", mode ? "no buffer" : "1 MiB buffer",
               (unsigned long long)free0, (unsigned long long)free1, (unsigned long long)free2);
        c->x[0] = P(sa); sa_fin(c);
        MEMCHECK(sa[0] == 0, "StandardAllocator::Finalize left the object initialised");
    }
    #undef MEMCHECK
    return bad;
}

static void selftest(void) {
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    uint8_t *stack = VirtualAlloc(NULL, 8 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memset(c, 0, sizeof *c);
    c->x[31] = P(stack + (8 << 20) - 0x1000);
    c->tpidr = P(VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));   /* the thread's own block: lifted libc keeps errno there */
    int bad = 0;
    printf("SDK self-test (lifted sdk.elf code + offline service answers)\n");
    if (!sdk_image_ready()) { printf("  sdk.elf is not loaded: nothing can be tested\n"); fflush(stdout); ExitProcess(99); }
    int missing = sdk_missing_host_callees(stdout);
    printf("  sdklift: %d functions lifted, %d host callees, %d without a handler\n", sdk_fn_count, sdk_host_name_count, missing);
    bad += missing;
    bad += libc_selftest(c);
    bad += mem_selftest(c);
    bad += zlib_selftest(c);
    bad += jpeg_selftest(c);
    bad += nnmii_selftest(c);
    bad += nnnet_selftest(c);
    bad += nnapplet_selftest(c);
    bad += nnalbum_selftest(c);
    bad += nnfsdir_selftest(c);
    bad += nnfs_selftest(c);
    bad += bcat_selftest(c);
    bad += nnhid_selftest(c);
    bad += nnro_selftest(c);
    bad += nvn_sdk_selftest(c);
    bad += nntime_selftest(c);
    bad += libc2_stdio_selftest(c);
    { int libc2_cxx_selftest(Ctx *c); bad += libc2_cxx_selftest(c); }      /* nnlibc2_c5b.inc: libc++ leftovers (close5) */
    { int close5_lead_selftest(Ctx *c); bad += close5_lead_selftest(c); }  /* nnmisc.c (close5): pctl answers, image protection */
    printf("SDK self-test: %d failure(s)\n", bad);
    fflush(stdout);
    ExitProcess((UINT)bad);
}

/* Imports whose host re-implementation is replaced by the real code lifted from sdk.elf (plan\FAKE_AUDIT.md, part 2;
 * list generated by tools_audit\lift_select.py). Debug aids: SSBU_LIFT_INSTALL=0 keeps every host handler;
 * SSBU_LIFT_SKIP=<text>[,<text>...] keeps the host handler of the names containing one of the texts. */
#include "sdklift_install.h"
static void install_lifted(void) {
    int n = (int)(sizeof sdk_install_names / sizeof sdk_install_names[0]), ok = 0, skipped = 0;
    const char *off = getenv("SSBU_LIFT_INSTALL"), *skip = getenv("SSBU_LIFT_SKIP");
    if (!sdk_image_ready() || (off && atoi(off) == 0)) return;
    for (int i = 0; i < n; i++) {
        int skip_this = 0;
        if (skip && skip[0]) {
            char buf[1024];
            snprintf(buf, sizeof buf, "%s", skip);
            for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) if (strstr(sdk_install_names[i], t)) skip_this = 1;
        }
        if (skip_this) { skipped++; continue; }
        if (sdk_install(sdk_install_names[i])) ok++;
        else printf("sdklift: %s is in sdklift_install.h but was not lifted or is not an import\n", sdk_install_names[i]);
    }
    printf("sdklift: %d host re-implementations replaced by the real SDK code (%d kept by SSBU_LIFT_SKIP)\n", ok, skipped);
}

/* SSBU_EH_MAIN_TEST=1 (bounded round 2026-10-09; audit row "exceptions: handler in main, rethrow, nested, terminate"):
 * one exception thrown THROUGH LIFTED MAIN CODE with the real unwinder, then the program ends (expected exit code 3).
 *   The game function [game 0x71038697e0] (Ghidra: FUN_71038697e0, 148 bytes; found with the exception tables: one of the 82
 *   landing pads of main that end a catch with __cxa_end_catch) is the copy-assignment of a callable holder: word 0 = a
 *   pointer to its manager table (bit 0 = "trivially copyable"), words 1..3 = storage. For a non-trivial source it stores
 *   the table pointer in the target and calls manager(src + 8, dst + 8, 0) at 0x710386984c inside a try block whose
 *   handler (landing pad 0x7103869874) is `catch (...) { target word 0 = 0; throw; }`.
 *   The test gives it a source whose manager is the game's own imported throw function
 *   std::__vector_base_common<true>::__throw_length_error (4,680 call sites in main), reached through main's import
 *   placeholder exactly as the game reaches it.
 * Expected, in this order in the log:
 *   unwind: throw #1 of St12length_error at 0x710386984c: handler in main f_71038697e0 (landing pad 7103869874)
 *   (the pad runs in lifted main code: __cxa_begin_catch, the clean-up store, __cxa_rethrow)
 *   unwind: St12length_error thrown at ...: no handler in N frames        <- the rethrow, searched from the pad's frame
 *   std::terminate: no handler / exception type: St12length_error / what(): vector
 *   EH main test: ... target word 0 = 0 (the handler's clean-up ran)
 * Not covered by this test: a handler in main that swallows the exception and continues (no such small function was
 * found: the scan is tools_audit/patches/close6/eh_scan.py), nested throws inside a clean-up. */
static uint64_t g_ehm_self[4], g_ehm_other[4], g_ehm_table[2];
extern void (*g_exit_report_hook)(FILE *);
uint64_t import_address(int sym);
static void eh_main_test_report(FILE *out) {
    fprintf(out, "EH main test: after the throw the target's word 0 = %llx (0 = the handler in main ran its clean-up `target = empty` before rethrowing; "
                 "it was %llx while the manager was being called)\n", (unsigned long long)g_ehm_self[0], (unsigned long long)P(g_ehm_table));
}
static void eh_main_test(void) {
    static const char thrower[] = "_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv";
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    uint8_t *stack = VirtualAlloc(NULL, 8 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    int sym = -1;
    memset(c, 0, sizeof *c);
    c->x[31] = P(stack + (8 << 20) - 0x1000);
    c->tpidr = P(VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    for (int i = 1; i < dyn_sym_count; i++) if (!dyn_syms[i].defined && strcmp(dyn_syms[i].name, thrower) == 0) { sym = i; break; }
    if (sym < 0 || !import_lookup(thrower)) { printf("EH main test: main does not import %s or it has no handler\n", thrower); fflush(stdout); ExitProcess(98); }
    g_ehm_table[0] = import_address(sym);                     /* the manager: main's own import placeholder of the throw function */
    g_ehm_other[0] = P(g_ehm_table);                          /* bit 0 clear: not trivially copyable -> the manager is called */
    memset(g_ehm_self, 0, sizeof g_ehm_self);
    g_exit_report_hook = eh_main_test_report;
    printf("EH main test: calling the game function %#llx (target %llx, source %llx, manager = import placeholder %llx of %s)\n",
           (unsigned long long)GA(0x71038697e0ULL), (unsigned long long)P(g_ehm_self), (unsigned long long)P(g_ehm_other), (unsigned long long)g_ehm_table[0], thrower);
    fflush(stdout);
    c->x[0] = P(g_ehm_self); c->x[1] = P(g_ehm_other); c->x[30] = 0;
    call_guest(c, GA(0x71038697e0ULL));
    printf("EH main test: FAILED: the function returned (no exception left it); target word 0 = %llx\n", (unsigned long long)g_ehm_self[0]);
    fflush(stdout);
    ExitProcess(97);
}

void register_sdk2_imports(void) {
    register_sdklift_imports();
    register_nnmii_imports();
    register_nnapplet_imports();
    register_nnalbum_imports();
    register_nnnet_imports();
    register_nnfsdir_imports();
    if (sdk_image_ready()) sdk_install("_ZN2nn4util14DecompressZlibEPvmPKvmS1_m");   /* replaces nnsdk.c's trap with the SDK's zlib */
    install_mem_overrides();
    install_lifted();
    libc2_install_musl_stdio();
    nnnet_register_final();
    dump_unregistered();
    if (getenv("SSBU_SDK_SELFTEST")) selftest();
    if (getenv("SSBU_EH_MAIN_TEST")) eh_main_test();
}

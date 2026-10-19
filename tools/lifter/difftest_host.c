/* Host half of the differential test: runs each lifted instruction from the
 * same pseudo-random state difftest.py gives to Unicorn and prints checksums. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "runtime.h"

#define REGION 0x7200000000ULL
#define REGION_SIZE 0x400000ULL
#define CODE (REGION + 0x200000ULL)
#define DATA (REGION + 0x300000ULL)
#define WIN_LO (DATA - 0x1000ULL)
#define WIN_HI (DATA + 0x12000ULL)

typedef struct { void (*fn)(Ctx *); uint32_t word; int mem, rn, rm, fpmode, k, frm; } Test;
extern const Test tests[];
extern const int test_count;

void call_indirect(Ctx *c, uint64_t a) { (void)c; (void)a; }
void guest_trap(const char *what, uint64_t addr) { (void)what; (void)addr; }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { (void)pc; (void)insn; (void)what; }
void guest_svc(Ctx *c, uint32_t n) { (void)c; (void)n; }
void stx_diag(Ctx *c, uint64_t a, uint64_t e, uint64_t s, uint64_t v, int bits) { (void)c; (void)a; (void)e; (void)s; (void)v; (void)bits; }   /* exclusive sequences */

static uint64_t rng_state;
static void rng_seed(uint64_t seed) { rng_state = seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL; }
static uint64_t rng_next(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint32_t crc_table[256];
static uint32_t crc32_of(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

/* Floating-point special operands: the same tables and the same rule as SPECIAL / special_v in difftest.py. */
static const uint32_t special32[] = {0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x40200000u, 0xc0200000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7fc12345u, 0x7f800001u, 0xffa00000u, 0x00000001u, 0x007fffffu, 0x80000400u, 0x00800000u, 0x7f7fffffu, 0x477fe000u, 0x477ff000u, 0x477fefffu, 0x33800000u, 0x33000000u, 0x33000001u, 0x38800000u, 0x387fffffu, 0x3f801000u, 0x3f803000u, 0x3f801001u};
static const uint64_t special64[] = {0x0000000000000000ull, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull, 0x4004000000000000ull, 0xc004000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff8000000012345ull, 0x7ff0000000000001ull, 0xfff4000000000000ull, 0x0000000000000001ull, 0x000fffffffffffffull, 0x8000000000000400ull, 0x0010000000000000ull, 0x7fefffffffffffffull, 0x40effc0000000000ull, 0x40effe0000000000ull, 0x40effdffffffffffull, 0x3e70000000000000ull, 0x3e60000000000000ull, 0x3e60000000000001ull, 0x3f10000000000000ull, 0x3f0fffffffffffffull, 0x3ff0020000000000ull, 0x3ff0060000000000ull, 0x3ff0020000001000ull};
static const uint16_t special16[] = {0x0000, 0x8000, 0x3c00, 0xbc00, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7e01, 0x7c01, 0xfd55, 0x0001, 0x03ff, 0x8200, 0x0400, 0x7bff, 0x3555, 0xc123};
static void special_state(Ctx *c, const Test *t) {
    int n = t->fpmode == 32 ? (int)(sizeof special32 / sizeof special32[0]) : t->fpmode == 64 ? (int)(sizeof special64 / sizeof special64[0]) : (int)(sizeof special16 / sizeof special16[0]);
    int ia = t->k % n, ib = (t->k / n) % n;
    for (int r = 0; r < 32; r++) {
        int b = r == t->frm ? ib : r == t->rn ? ia : (t->k + r) % n;
        for (int l = 0; l < 128 / t->fpmode; l++) {
            int i = (b + 7 * l) % n;
            if (t->fpmode == 32) c->v[r].w[l] = special32[i]; else if (t->fpmode == 64) c->v[r].d[l] = special64[i]; else c->v[r].h[l] = special16[i];
        }
    }
}

int main(int argc, char **argv) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    uint8_t *mem = VirtualAlloc((void *)REGION, REGION_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem != (uint8_t *)REGION) { fprintf(stderr, "cannot map test region\n"); return 1; }
    rng_seed(0xABCDEF);
    for (uint64_t i = 0; i < REGION_SIZE / 8; i++) { uint64_t v = rng_next(); memcpy(mem + i * 8, &v, 8); }
    size_t win = (size_t)(WIN_HI - WIN_LO);
    uint8_t *pristine = malloc(win);
    memcpy(pristine, (void *)WIN_LO, win);

    int only = argc > 1 ? atoi(argv[1]) : -1;
    for (int i = 0; i < test_count; i++) {
        if (only >= 0 && i != only) continue;
        const Test *t = &tests[i];
        static Ctx c;
        memset(&c, 0, sizeof c);
        rng_seed((uint64_t)i);
        for (int r = 0; r < 31; r++) c.x[r] = rng_next();
        c.x[31] = DATA + 0x800;
        for (int r = 0; r < 32; r++)
            for (int l = 0; l < 4; l++) c.v[r].f[l] = (float)((double)((int64_t)(rng_next() % 200001) - 100000) / 64.0);
        set_nzcv(&c, (int)(rng_next() & 0xf));
        if (t->mem) {
            uint64_t base = DATA + 0x100 * (rng_next() % 8);
            uint64_t index = rng_next() % 0x40;
            if (t->rn != 31) c.x[t->rn] = base;
            if (t->rm >= 0 && t->rm < 31 && t->rm != t->rn) c.x[t->rm] = index;
        }
        if (t->fpmode) special_state(&c, t);
        memcpy((void *)WIN_LO, pristine, win);
        memcpy((void *)CODE, &t->word, 4);
        t->fn(&c);

        uint8_t regs[32 * 8 + 32 * 16 + 4];
        memcpy(regs, c.x, 32 * 8);
        memcpy(regs + 32 * 8, c.v, 32 * 16);
        uint32_t flags = (c.N << 3) | (c.Z << 2) | (c.C << 1) | c.V;
        memcpy(regs + 32 * 8 + 32 * 16, &flags, 4);
        if (only >= 0) {
            for (int r = 0; r < 31; r++) printf("x%d %llx\n", r, (unsigned long long)c.x[r]);
            printf("sp %llx\n", (unsigned long long)c.x[31]);
            for (int r = 0; r < 32; r++) printf("v%d %016llx%016llx\n", r, (unsigned long long)c.v[r].d[1], (unsigned long long)c.v[r].d[0]);
            printf("nzcv %x\n", flags);
        } else {
            printf("%d %08x %08x\n", i, crc32_of(regs, sizeof regs), crc32_of((void *)WIN_LO, win));
        }
    }
    return 0;
}

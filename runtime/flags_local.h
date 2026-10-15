/* Lifted code with the condition flags in a local (tools\lifter\flags_local.py): each lifted function keeps N, Z, C, V in
 * `fl_`, a local whose address never escapes, so the compiler holds them in registers and drops the ones nobody reads.
 * They come from the context at the function's entry and after every call, and go back to it before anything that
 * leaves the function (a call, a return, a jump to another lifted function, a trap): every function boundary sees the
 * same flags as before. The helpers are those of runtime.h with the flags passed by pointer. */
#ifndef FLAGS_LOCAL_H
#define FLAGS_LOCAL_H
typedef struct { uint32_t N, Z, C, V; } FlagsL;
#define FL_IN() (fl_.N = c->N, fl_.Z = c->Z, fl_.C = c->C, fl_.V = c->V)
#define FL_OUT() (c->N = fl_.N, c->Z = fl_.Z, c->C = fl_.C, c->V = fl_.V)
static inline uint64_t adds64f(FlagsL *f, uint64_t a, uint64_t b, uint32_t cin) {
    uint64_t r = a + b + cin;
    f->N = (uint32_t)(r >> 63); f->Z = r == 0; f->C = (r < a) || (cin && r == a); f->V = (uint32_t)(((a ^ r) & (b ^ r)) >> 63);
    return r;
}
static inline uint32_t adds32f(FlagsL *f, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t wide = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)wide;
    f->N = r >> 31; f->Z = r == 0; f->C = (uint32_t)(wide >> 32); f->V = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}
static inline uint64_t logic64f(FlagsL *f, uint64_t r) { f->N = (uint32_t)(r >> 63); f->Z = r == 0; f->C = 0; f->V = 0; return r; }
static inline uint32_t logic32f(FlagsL *f, uint32_t r) { f->N = r >> 31; f->Z = r == 0; f->C = 0; f->V = 0; return r; }
static inline void set_nzcvf(FlagsL *f, int v) { f->N = (v >> 3) & 1; f->Z = (v >> 2) & 1; f->C = (v >> 1) & 1; f->V = v & 1; }
static inline int condf(const FlagsL *f, int cc) {
    int r;
    switch (cc >> 1) {
    case 0: r = f->Z; break;
    case 1: r = f->C; break;
    case 2: r = f->N; break;
    case 3: r = f->V; break;
    case 4: r = f->C && !f->Z; break;
    case 5: r = f->N == f->V; break;
    case 6: r = f->N == f->V && !f->Z; break;
    default: return 1;
    }
    return (cc & 1) ? !r : r;
}
#endif

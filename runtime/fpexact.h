/* Bit-exact AArch64 behaviour for the few floating-point operations where
 * x86 differs: reciprocal estimates and the sign of generated NaNs.
 * The estimate algorithms follow the pseudocode in the Arm architecture manual. */
#ifndef RECOMP_FPEXACT_H
#define RECOMP_FPEXACT_H

/* x86 produces a negative quiet NaN for invalid operations, AArch64 a positive one. */
static inline float fnan32(float r) {
    uint32_t b;
    memcpy(&b, &r, 4);
    if (b == 0xffc00000u) { b = 0x7fc00000u; memcpy(&r, &b, 4); }
    return r;
}
static inline double fnan64(double r) {
    uint64_t b;
    memcpy(&b, &r, 8);
    if (b == 0xfff8000000000000ull) { b = 0x7ff8000000000000ull; memcpy(&r, &b, 8); }
    return r;
}

static inline uint32_t recip_estimate(uint32_t a) {            /* a in 256..511 */
    a = a * 2 + 1;
    uint32_t b = (1u << 19) / a;
    return (b + 1) / 2;
}
static inline uint32_t recip_sqrt_estimate(uint32_t a) {       /* a in 128..511 */
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    /* The manual's loop (b = 512; while (a * (b + 1)^2 < 2^28) b++; return (b + 1) / 2) took up to ~500 steps and was
     * 8.7% of the game thread in a 2-fighter match (2026-10-15): k = b + 1 is found from sqrt and corrected exactly
     * instead (identical for every a, checked for 1..4095). */
    uint64_t k = (uint64_t)sqrt((double)(1u << 28) / a);
    if (k < 513) k = 513;
    while (k > 513 && (uint64_t)a * (k - 1) * (k - 1) >= (1ull << 28)) k--;
    while ((uint64_t)a * k * k < (1ull << 28)) k++;
    return (uint32_t)(k / 2);
}

/* Generic implementations on (sign, biased exponent, fraction left-aligned to 52 bits). */
static inline uint64_t fp_recpe_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = bits >> (ebits + fbits);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1;
    uint64_t signbit = sign << (ebits + fbits);
    if (exp == emax) {
        if (frac) return bits | (1ull << (fbits - 1));          /* NaN: quieten */
        return signbit;                                         /* infinity -> zero */
    }
    if (exp == 0 && frac == 0) return signbit | ((uint64_t)emax << fbits);       /* zero -> infinity */
    if (exp == 0 && frac < (1ull << (fbits - 2))) return signbit | ((uint64_t)emax << fbits);  /* overflow */
    uint64_t f52 = frac << (52 - fbits);
    if (exp == 0) {
        if ((f52 >> 51) == 0) { exp = -1; f52 = (f52 << 2) & ((1ull << 52) - 1); }
        else f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled = 0x100 | (uint32_t)(f52 >> 44);
    int64_t rexp = (dbl ? 2045 : 253) - exp;
    uint64_t rf = (uint64_t)(recip_estimate(scaled) & 0xff) << 44;
    if (rexp == 0) rf = (1ull << 51) | (rf >> 1);
    else if (rexp == -1) { rf = (1ull << 50) | (rf >> 2); rexp = 0; }
    return signbit | ((uint64_t)rexp << fbits) | (rf >> (52 - fbits));
}
static inline uint64_t fp_rsqrte_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = bits >> (ebits + fbits);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1;
    uint64_t default_nan = ((uint64_t)emax << fbits) | (1ull << (fbits - 1));
    if (exp == emax && frac) return bits | (1ull << (fbits - 1));                 /* NaN: quieten */
    if (exp == 0 && frac == 0) return (sign << (ebits + fbits)) | ((uint64_t)emax << fbits);  /* zero -> infinity */
    if (sign) return default_nan;                                                 /* negative -> NaN */
    if (exp == emax) return 0;                                                    /* +infinity -> +0 */
    uint64_t f52 = frac << (52 - fbits);
    if (exp == 0) {
        while ((f52 >> 51) == 0) { f52 = (f52 << 1) & ((1ull << 52) - 1); exp--; }
        f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled = (exp & 1) == 0 ? 0x100 | (uint32_t)(f52 >> 44) : 0x80 | (uint32_t)(f52 >> 45);
    int64_t rexp = ((dbl ? 3068 : 380) - exp) / 2;
    uint64_t est = recip_sqrt_estimate(scaled) & 0xff;
    return ((uint64_t)rexp << fbits) | (est << (fbits - 8));
}

static inline float recpe_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_recpe_bits(b, 0); memcpy(&x, &b, 4); return x; }
static inline double recpe_d(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_recpe_bits(b, 1); memcpy(&x, &b, 8); return x; }
static inline float rsqrte_f(float x) { uint32_t b; memcpy(&b, &x, 4); b = (uint32_t)fp_rsqrte_bits(b, 0); memcpy(&x, &b, 4); return x; }
static inline double rsqrte_d(double x) { uint64_t b; memcpy(&b, &x, 8); b = fp_rsqrte_bits(b, 1); memcpy(&x, &b, 8); return x; }

/* FCVT between half precision (IEEE binary16, FPCR.AHP = 0) and single / double: FPConvert of the Arm manual with
 * round-to-nearest-even and FPCR.DN = 0. A NaN keeps its sign and the top bits of its payload and becomes quiet
 * (FPConvertNaN). Done on the bit patterns, so there is one rounding and no dependence on the host's conversions. */
static inline uint16_t fp_to_half_bits(uint64_t bits, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint16_t sign = (uint16_t)(((bits >> (ebits + fbits)) & 1) << 15);
    int64_t exp = (int64_t)((bits >> fbits) & ((1u << ebits) - 1));
    uint64_t frac = bits & ((1ull << fbits) - 1);
    int64_t emax = (1 << ebits) - 1, bias = emax >> 1;
    if (exp == emax) {
        if (frac) return (uint16_t)(sign | 0x7e00 | ((frac >> (fbits - 10)) & 0x1ff));   /* NaN: quiet, payload bits below the quiet bit */
        return (uint16_t)(sign | 0x7c00);                                                 /* infinity */
    }
    if (exp == 0 && frac == 0) return sign;
    uint64_t m;                                    /* value = m * 2^(e - fbits), bit fbits of m set */
    int64_t e;
    if (exp) { m = frac | (1ull << fbits); e = exp - bias; }
    else { m = frac; e = 1 - bias; while (!(m >> fbits)) { m <<= 1; e--; } }
    if (e > 15) return (uint16_t)(sign | 0x7c00);                                         /* overflow: infinity (round to nearest) */
    int64_t sh = fbits - 10 + (e < -14 ? -14 - e : 0);                                    /* below 2^-14 the result is subnormal */
    if (sh > fbits + 1) return sign;                                                      /* less than half the smallest subnormal */
    uint64_t q = m >> sh, rem = m & ((1ull << sh) - 1), half = 1ull << (sh - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    uint32_t r = e < -14 ? (uint32_t)q : (uint32_t)(((uint64_t)(e + 15) << 10) + (q - 0x400));   /* a carry out of the fraction moves into the exponent */
    if (r >= 0x7c00) r = 0x7c00;
    return (uint16_t)(sign | r);
}
static inline uint64_t fp_from_half_bits(uint16_t h, int dbl) {
    int ebits = dbl ? 11 : 8, fbits = dbl ? 52 : 23;
    uint64_t sign = (uint64_t)(h >> 15) << (ebits + fbits), frac = h & 0x3ff, emax = (1u << ebits) - 1;
    int64_t exp = (h >> 10) & 31, bias = (int64_t)(emax >> 1), e;
    if (exp == 31) {
        if (frac) return sign | (emax << fbits) | (1ull << (fbits - 1)) | ((frac & 0x1ff) << (fbits - 10));
        return sign | (emax << fbits);
    }
    if (exp == 0) {
        if (!frac) return sign;
        e = -14;
        while (!(frac & 0x400)) { frac <<= 1; e--; }
        frac &= 0x3ff;
    } else e = exp - 15;
    return sign | ((uint64_t)(e + bias) << fbits) | (frac << (fbits - 10));
}
static inline uint16_t f32_to_half(float x) { uint32_t b; memcpy(&b, &x, 4); return fp_to_half_bits(b, 0); }
static inline uint16_t f64_to_half(double x) { uint64_t b; memcpy(&b, &x, 8); return fp_to_half_bits(b, 1); }
static inline float half_to_f32(uint16_t h) { uint32_t b = (uint32_t)fp_from_half_bits(h, 0); float x; memcpy(&x, &b, 4); return x; }
static inline double half_to_f64(uint16_t h) { uint64_t b = fp_from_half_bits(h, 1); double x; memcpy(&x, &b, 8); return x; }

/* FMAX / FMIN / FMAXNM / FMINNM (FPMax, FPMin, FPMaxNum, FPMinNum of the Arm manual, FPCR.DN = 0), on bit patterns.
 * A signalling NaN operand wins (first operand first) and is returned quiet; then a quiet NaN (first operand first),
 * except that the NM forms return the other operand when exactly one operand is a quiet NaN and none is signalling.
 * +0 is greater than -0. The C library's fmax / fmin do neither. */
static inline uint32_t fp_maxmin32_bits(uint32_t a, uint32_t b, int is_min, int num) {
    int na = (a & 0x7fffffffu) > 0x7f800000u, nb = (b & 0x7fffffffu) > 0x7f800000u;
    if (na | nb) {
        int sa = na && !(a & 0x00400000u), sb = nb && !(b & 0x00400000u);
        if (num && !sa && !sb && na != nb) return na ? b : a;
        return (sa ? a : sb ? b : na ? a : b) | 0x00400000u;
    }
    if (((a | b) & 0x7fffffffu) == 0) return is_min ? (a | b) : (a & b);
    float fa, fb;
    memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
    return (is_min ? fa < fb : fa > fb) ? a : b;
}
static inline uint64_t fp_maxmin64_bits(uint64_t a, uint64_t b, int is_min, int num) {
    int na = (a & 0x7fffffffffffffffull) > 0x7ff0000000000000ull, nb = (b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull;
    if (na | nb) {
        int sa = na && !(a & 0x0008000000000000ull), sb = nb && !(b & 0x0008000000000000ull);
        if (num && !sa && !sb && na != nb) return na ? b : a;
        return (sa ? a : sb ? b : na ? a : b) | 0x0008000000000000ull;
    }
    if (((a | b) & 0x7fffffffffffffffull) == 0) return is_min ? (a | b) : (a & b);
    double fa, fb;
    memcpy(&fa, &a, 8); memcpy(&fb, &b, 8);
    return (is_min ? fa < fb : fa > fb) ? a : b;
}
static inline float fp_maxmin32(float x, float y, int is_min, int num) {
    uint32_t a, b;
    memcpy(&a, &x, 4); memcpy(&b, &y, 4);
    a = fp_maxmin32_bits(a, b, is_min, num);
    memcpy(&x, &a, 4);
    return x;
}
static inline double fp_maxmin64(double x, double y, int is_min, int num) {
    uint64_t a, b;
    memcpy(&a, &x, 8); memcpy(&b, &y, 8);
    a = fp_maxmin64_bits(a, b, is_min, num);
    memcpy(&x, &a, 8);
    return x;
}

#endif

// examples/tinyllm/src/fixedmath.h — the integer-only math the inference core is built from.
// Engine-free POD header (plain <stdint.h>, like miracle-player's viz.h/particles.h): no `scalar`,
// no float, ever — so every result is byte-identical on the PC (float) and GBA (fixed16) scalar
// tiers, which is what `make determinism` gates.
//
// THE RULE THAT SHAPES THIS FILE: **ARM7TDMI has no divide instruction.** A `/` on the frame path
// becomes a call to __aeabi_idiv (a ~40-cycle software loop). So every reciprocal here goes
// through q_recip() — a Newton-Raphson iteration built from multiplies and shifts only — and the
// two transcendentals the transformer needs (exp for softmax, SiLU for the FFN) are ROM lookup
// tables with a power-of-two step, so the index/fraction split is a shift.
//
// Conventions:
//   * `_q16` suffix = Q16.16 fixed point (the activation domain: range +-32768, step 1.5e-5).
//   * `_q30` suffix = Q30 (the quantization-scale domain: weight scales measure ~1e-4, which
//     Q16.16 would round to 0.9% relative error — see examples/tinyllm/README.md).
//   * Right shifts of signed values are arithmetic (floor) on every compiler we build with, and
//     Python's `>>` agrees — the exporter's bit-exact reference relies on that. LEFT-shifting a
//     negative is UB (and UBSan flags it), so this file multiplies instead.
//
// Every routine here is pinned by tests/unit/test_tinyllm_math.cpp against an error bound.
#ifndef TINYLLM_FIXEDMATH_H
#define TINYLLM_FIXEDMATH_H

#include <stdint.h>

namespace tinyllm {

inline constexpr int32_t kQ16One = 1 << 16;

// Count leading zeros of a NON-ZERO word. __builtin_clz is UB at 0, and the portable fallback
// keeps this header compiling anywhere (it is only ever hit at -O0 on a non-GCC toolchain).
inline int q_clz32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(v);
#else
    int n = 0;
    if (!(v & 0xFFFF0000u)) { n += 16; v <<= 16; }
    if (!(v & 0xFF000000u)) { n += 8;  v <<= 8;  }
    if (!(v & 0xF0000000u)) { n += 4;  v <<= 4;  }
    if (!(v & 0xC0000000u)) { n += 2;  v <<= 2;  }
    if (!(v & 0x80000000u)) { n += 1; }
    return n;
#endif
}

// A division-free reciprocal: 1/d ~= m * 2^-sh, with m in (2^30, 2^31] so callers keep ~31 bits
// of mantissa and pick their own output scale by choosing the final shift.
struct QRecip {
    int64_t m  = 0;
    int32_t sh = 0;
};

// Newton-Raphson seed constants for 1/f on f in [0.5, 1) (the classic 48/17 - 32/17 f), in Q30.
// Both exceed INT32_MAX, so they are int64 literals — they are compile-time constants, not
// runtime divisions.
inline constexpr int64_t kRecipSeedA = (48LL << 30) / 17;   // 3031741620
inline constexpr int64_t kRecipSeedB = (32LL << 30) / 17;   // 2021161080

// Reciprocal of a non-zero unsigned integer. Four Newton iterations take the seed's ~2^-4 error
// to well below the 2^-31 the mantissa can hold, so the result is correct to its last bit or one
// ulp below it (test_tinyllm_math pins that). d == 0 returns {0,0}; callers must not divide by 0.
inline QRecip q_recip(uint32_t d) {
    QRecip r;
    if (d == 0) return r;
    const int lz = q_clz32(d);
    const int64_t b = int64_t(uint32_t(d << lz));       // normalized to [2^31, 2^32)
    // x approximates 2^62 / b, held in Q30 (so x lands in (2^30, 2^31]).
    int64_t x = kRecipSeedA - ((kRecipSeedB * b) >> 32);
    for (int i = 0; i < 4; ++i) {
        const int64_t fx = (b * x) >> 32;               // b*x <= 2^63 - checked, never overflows
        x = (x * ((2LL << 30) - fx)) >> 30;
    }
    r.m  = x;
    r.sh = 62 - lz;                                     // 1/d = x * 2^-(62-lz)
    return r;
}

inline int32_t q_sat32(int64_t v) {
    if (v >  2147483647LL) return  2147483647;
    if (v < -2147483647LL - 1) return -2147483647 - 1;
    return int32_t(v);
}

// n / d, both Q16.16, result Q16.16. Saturates rather than wrapping when the quotient does not
// fit (1/x for x below ~1.5e-5). d must be non-zero.
inline int32_t q_div(int32_t n_q16, int32_t d_q16) {
    if (d_q16 == 0) return n_q16 >= 0 ? 2147483647 : (-2147483647 - 1);
    const int      neg = ((n_q16 < 0) != (d_q16 < 0));
    const int64_t  n   = n_q16 < 0 ? -int64_t(n_q16) : int64_t(n_q16);
    const uint32_t d   = uint32_t(d_q16 < 0 ? -int64_t(d_q16) : int64_t(d_q16));
    const QRecip   r   = q_recip(d);
    // result = n * 2^16 / d = n * m * 2^-(sh-16); sh-16 is in [15, 46], always a right shift.
    const int64_t  q   = (n * r.m) >> (r.sh - 16);
    return q_sat32(neg ? -q : q);
}

// a * b, both Q16.16, result Q16.16 (truncating toward negative infinity, like every shift here).
inline int32_t q_mul(int32_t a_q16, int32_t b_q16) {
    return q_sat32((int64_t(a_q16) * int64_t(b_q16)) >> 16);
}

// acc * s / 2^30, saturating. `acc` is a wide matvec accumulator, so the plain product can leave
// int64 for inputs that would saturate the int32 result anyway; the normalizing loop trades low
// bits of `acc` for headroom BEFORE multiplying instead of overflowing. It runs zero times for
// every value a real model produces (acc stays well under 2^31), so it costs one compare per
// output row — and being plain integer code it stays byte-identical on both scalar tiers.
inline int32_t q_scale_q30(int64_t acc, int32_t s_q30) {
    if (acc == 0 || s_q30 == 0) return 0;
    const bool neg = acc < 0;
    int64_t a  = neg ? -acc : acc;
    int32_t sh = 30;
    while (a >= (int64_t(1) << 31)) { a >>= 1; --sh; }
    if (sh < 0) return neg ? (-2147483647 - 1) : 2147483647;
    const int64_t p = (a * int64_t(s_q30)) >> sh;
    return q_sat32(neg ? -p : p);
}

// floor(sqrt(n)) for n < 2^62. Bit-by-bit restoring square root: shifts, compares, subtracts.
inline uint32_t q_isqrt64(uint64_t n) {
    uint64_t res = 0;
    uint64_t bit = 1ULL << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        const uint64_t t = res + bit;
        if (n >= t) { n -= t; res = (res >> 1) + bit; }
        else        { res >>= 1; }
        bit >>= 2;
    }
    return uint32_t(res);
}

// sqrt of a non-negative Q16.16, result Q16.16.
inline int32_t q_sqrt(int32_t a_q16) {
    if (a_q16 <= 0) return 0;
    return int32_t(q_isqrt64(uint64_t(uint32_t(a_q16)) << 16));
}

// 1/sqrt(a) for a > 0, Q16.16 in and out. Two exact-ish steps (integer sqrt, then the Newton
// reciprocal) rather than one fused approximation, so the error is bounded by q_div's.
inline int32_t q_rsqrt(int32_t a_q16) {
    const int32_t s = q_sqrt(a_q16);
    if (s <= 0) return 2147483647;
    return q_div(kQ16One, s);
}

// --- LUT evaluation -----------------------------------------------------------------------------
// Both tables live in the model blob (cartridge ROM), are Q16.16, and have kExpLutN+1 /
// kSiluLutN+1 entries over a power-of-two-stepped domain, so this is a shift + a mask + one
// linear interpolation. `lut` must be the tensor payload; see llm_format.h for the geometry.

// exp(x) for x <= 0, Q16.16. Callers subtract the row max first (softmax range reduction), so the
// domain is exactly [-16, 0]; below that exp is under 1.1e-7 and rounds to zero in Q16.16 anyway.
inline int32_t q_exp_neg(const int32_t* lut, int32_t x_q16) {
    if (x_q16 >= 0) return kQ16One;
    if (x_q16 <= -(16 << 16)) return 0;                 // guard before the add, not after
    const int32_t t    = x_q16 + (16 << 16);            // shift the domain to (0, 16<<16)
    const int32_t i    = t >> 11;                       // step = 1/32 = 2048 in Q16.16
    const int32_t frac = t & 2047;
    if (i >= 512) return lut[512];
    return lut[i] + int32_t(((int64_t(lut[i + 1]) - int64_t(lut[i])) * frac) >> 11);
}

// SiLU(x) = x * sigmoid(x), Q16.16. Outside the tabulated [-16, 16] the function is flat (0) or
// the identity, so both tails are exact rather than clamped to a table endpoint.
inline int32_t q_silu(const int32_t* lut, int32_t x_q16) {
    if (x_q16 >=  (16 << 16)) return x_q16;
    if (x_q16 <= -(16 << 16)) return 0;
    const int32_t t    = x_q16 + (16 << 16);            // [0, 32<<16]
    const int32_t i    = t >> 12;                       // step = 1/16 = 4096 in Q16.16
    const int32_t frac = t & 4095;
    if (i >= 512) return lut[512];
    return lut[i] + int32_t(((int64_t(lut[i + 1]) - int64_t(lut[i])) * frac) >> 12);
}

// --- int8 activation quantization ----------------------------------------------------------------
// One scale for the whole vector (not per group): the vectors quantized here are always freshly
// RMSNorm'd or a SwiGLU product, whose within-vector dynamic range is narrow, and hoisting the
// scale out of the matvec removes an int64 multiply per group per output row. Weight scales stay
// per group, where the range actually varies. Measured cost is in the README.
struct QQuant {
    int32_t scale_q30 = 0;   // the real scale = max|x| / 127, in Q30
};

// Quantize `n` Q16.16 values into `out` (int8), zero-filling `out[n .. stride)` so a padded matvec
// row reads defined zeros. Returns the scale. An all-zero input yields scale 0 (the matvec then
// contributes nothing, which is the arithmetically correct answer).
inline QQuant q_quantize(const int32_t* x, int32_t n, int32_t stride, int8_t* out) {
    QQuant q;
    int32_t maxabs = 0;
    for (int32_t i = 0; i < n; ++i) {
        const int32_t a = x[i] < 0 ? -x[i] : x[i];
        if (a > maxabs) maxabs = a;
    }
    for (int32_t i = n; i < stride; ++i) out[i] = 0;
    if (maxabs <= 0) {
        for (int32_t i = 0; i < n; ++i) out[i] = 0;
        return q;
    }
    const QRecip r = q_recip(uint32_t(maxabs));
    // scale = maxabs/127 as Q30 = maxabs_q16 * 2^14 / 127.
    const QRecip r127 = q_recip(127);
    q.scale_q30 = q_sat32((int64_t(maxabs) * r127.m) >> (r127.sh - 14));
    // Multiplier for x -> int8: xq = x_q16 * 127 / maxabs_q16 = x_q16 * mult * 2^-msh.
    // 127*m needs 38 bits, so drop 7 to keep the mantissa inside 31 bits.
    const int64_t mult = (r.m * 127) >> 7;
    const int32_t msh  = r.sh - 7;
    const int64_t half = 1LL << (msh - 1);
    for (int32_t i = 0; i < n; ++i) {
        const int32_t v = x[i];
        const int64_t a = (v < 0 ? -int64_t(v) : int64_t(v));
        int64_t       m = ((a * mult) + half) >> msh;    // round-half-up on the magnitude, so the
        if (m > 127) m = 127;                            // mapping stays symmetric about zero
        out[i] = int8_t(v < 0 ? -m : m);
    }
    return q;
}

} // namespace tinyllm
#endif // TINYLLM_FIXEDMATH_H

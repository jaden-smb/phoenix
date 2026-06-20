// phx/core/fixed.cpp — Q16.16 transcendentals via small LUTs + Newton refinement.
// On ARM7TDMI there is no FPU and no hardware divide, so these are the ONLY sanctioned
// paths for sin/cos/sqrt/reciprocal. The tables are computed once at static-init on the
// host; on GBA they are baked into ROM as `const` (see note at bottom).
#include "phx/core/fixed.h"

namespace phx {

// 256-entry sine table over one full turn [0,1). sin_lut[i] = sin(2*pi*i/256) in Q16.16.
// Generated at startup here for clarity; on GBA this becomes a const array in ROM.
namespace {
constexpr int kSinBits = 8;
constexpr int kSinSize = 1 << kSinBits;          // 256
int32_t g_sin_lut[kSinSize];

struct SinLutInit {
    SinLutInit() {
        // host-side double math is fine: this runs offline / at static init, never per-frame.
        const double tau = 6.283185307179586476925286766559;
        for (int i = 0; i < kSinSize; ++i) {
            double s = __builtin_sin(tau * double(i) / double(kSinSize));
            g_sin_lut[i] = int32_t(s * double(fixed16::kOne));
        }
    }
} g_sin_lut_init;
} // namespace

fixed16 fx_sin(fixed16 turns) {
    // wrap turns into [0,1) then index the table with linear interpolation.
    int32_t frac = turns.raw & (fixed16::kOne - 1);             // keep fractional turn
    int32_t idx_fp = frac >> (fixed16::kShift - kSinBits);       // 0..255 in fixed
    int32_t i0 = idx_fp & (kSinSize - 1);
    int32_t i1 = (i0 + 1) & (kSinSize - 1);
    int32_t t  = (frac << kSinBits) & (fixed16::kOne - 1);       // sub-index fraction Q16.16
    int32_t a  = g_sin_lut[i0];
    int32_t b  = g_sin_lut[i1];
    int32_t lerp = a + int32_t((int64_t(b - a) * t) >> fixed16::kShift);
    return fixed16::from_raw(lerp);
}

fixed16 fx_cos(fixed16 turns) {
    return fx_sin(fixed16::from_raw(turns.raw + (fixed16::kOne >> 2)));   // cos(x) = sin(x + 1/4 turn)
}

fixed16 fx_sqrt(fixed16 v) {
    if (v.raw <= 0) return fixed16::from_raw(0);
    // integer sqrt of the 32.32 value (v.raw << 16), bit-by-bit — branch-light, no FPU.
    uint64_t n = (uint64_t(uint32_t(v.raw)) << fixed16::kShift);
    uint64_t res = 0;
    uint64_t bit = uint64_t(1) << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; }
        else                {                 res >>= 1; }
        bit >>= 2;
    }
    return fixed16::from_raw(int32_t(res));
}

fixed16 fx_rcp(fixed16 v) {
    // reciprocal without hardware divide: seed from a bit-width estimate, then 2 Newton
    // iterations  x = x*(2 - v*x).  Accurate to ~Q16.16 for the engine's input range.
    if (v.raw == 0) return fixed16::from_raw(0x7FFFFFFF);        // saturate; callers guard
    int sign = v.raw < 0 ? -1 : 1;
    uint32_t a = uint32_t(sign < 0 ? -v.raw : v.raw);

    // initial estimate: 1/a ~ 2^(2*16 - msb*2) — coarse but fast to converge.
    int msb = 31 - __builtin_clz(a);
    int32_t x = fixed16::kOne >> (msb - fixed16::kShift > 0 ? (msb - fixed16::kShift) : 0);
    if (x == 0) x = 1;

    fixed16 vf = fixed16::from_raw(int32_t(a));
    fixed16 xf = fixed16::from_raw(x);
    const fixed16 two = fixed16::from_int(2);
    for (int i = 0; i < 4; ++i)                                  // converges quickly
        xf = xf * (two - vf * xf);
    return fixed16::from_raw(sign * xf.raw);
}

} // namespace phx

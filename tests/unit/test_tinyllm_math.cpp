// tests/unit/test_tinyllm_math.cpp — the division-free fixed-point primitives the tinyllm
// inference core is built from (examples/tinyllm/src/fixedmath.h).
//
// These matter more than usual: ARM7TDMI has no divide instruction, so every reciprocal in the
// transformer goes through a Newton-Raphson iteration written here, and both scalar tiers must
// produce the SAME integers (`make determinism`). Each routine is pinned against an explicit
// error bound rather than "looks about right".
#include "../phx_test.h"
#include "../../examples/tinyllm/src/fixedmath.h"
#include "../../examples/tinyllm/src/llm_format.h"   // kExpLutN / kSiluLutN geometry

#include <cmath>
#include <cstdlib>

using namespace tinyllm;

namespace {

// A deterministic value generator — no <random>, so the cases are identical everywhere.
struct Xs { uint32_t s = 0x9E3779B9u;
            uint32_t next() { uint32_t x = s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return s = x; } };

double as_real(int32_t q16) { return double(q16) / 65536.0; }

} // namespace

PHX_TEST(tinyllm_clz_matches_bit_width) {
    for (int b = 0; b < 32; ++b) {
        const uint32_t v = 1u << b;
        CHECK_EQ(q_clz32(v), 31 - b);
        if (b > 0) CHECK_EQ(q_clz32(v | (v - 1)), 31 - b);
    }
}

PHX_TEST(tinyllm_recip_is_within_two_ulp) {
    // q_recip returns 1/d ~= m * 2^-sh with m in (2^30, 2^31]. Check the reconstruction against
    // exact rational arithmetic over the whole exponent range, not just small values.
    Xs r;
    int worst = 0;
    for (int i = 0; i < 4000; ++i) {
        uint32_t d = (i < 64) ? uint32_t(i + 1) : (r.next() | 1u);
        const QRecip q = q_recip(d);
        CHECK(q.m > (int64_t(1) << 30) - 1);
        CHECK(q.m <= (int64_t(1) << 31));
        // exact = floor(2^sh / d). Four Newton steps land within 2 ULP of a 31-bit mantissa --
        // 9.3e-10 relative, measured as the true worst case over a 20 M-divisor sweep. (The
        // truncating `>> 32` inside the iteration biases upward, which is why the error is
        // two-sided rather than converging strictly from below.)
        const int64_t exact = (int64_t(1) << q.sh) / int64_t(d);
        const int64_t err = exact - q.m < 0 ? q.m - exact : exact - q.m;
        if (err > 2) {
            std::printf("      recip(%u): m=%lld exact=%lld sh=%d\n",
                        unsigned(d), (long long)q.m, (long long)exact, int(q.sh));
        }
        CHECK(err <= 2);
        if (int(err) > worst) worst = int(err);
    }
    CHECK(worst <= 2);
}

PHX_TEST(tinyllm_div_matches_exact_quotient) {
    Xs r;
    for (int i = 0; i < 4000; ++i) {
        int32_t n = int32_t(r.next() % 40000000u) - 20000000;
        int32_t d = int32_t(r.next() % 20000000u) + 1;
        if (i & 1) d = -d;
        const int32_t got = q_div(n, d);
        // The exact Q16.16 quotient, truncated toward zero the way q_div's sign handling is.
        const int64_t mag = (int64_t(std::abs(int64_t(n))) << 16) / std::abs(int64_t(d));
        const int64_t want = ((n < 0) != (d < 0)) ? -mag : mag;
        CHECK(std::llabs(int64_t(got) - want) <= 1);
    }
    // Exact cases the transformer actually relies on.
    CHECK_EQ(q_div(kQ16One, 64 * kQ16One), kQ16One / 64);
    CHECK_EQ(q_div(kQ16One, kQ16One), kQ16One);
    CHECK_EQ(q_div(0, kQ16One), 0);
    // 1/x saturates rather than wrapping when the quotient leaves int32.
    CHECK_EQ(q_div(kQ16One, 1), 2147483647);
}

PHX_TEST(tinyllm_isqrt_is_exact_floor) {
    Xs r;
    for (int i = 0; i < 3000; ++i) {
        const uint64_t n = (i < 100) ? uint64_t(i)
                                     : ((uint64_t(r.next()) << 20) | r.next()) & ((1ULL << 61) - 1);
        const uint32_t s = q_isqrt64(n);
        CHECK(uint64_t(s) * s <= n);
        CHECK((uint64_t(s) + 1) * (uint64_t(s) + 1) > n);
    }
}

PHX_TEST(tinyllm_rsqrt_tracks_libm) {
    for (int i = 1; i <= 2000; ++i) {
        const int32_t a = int32_t(i) * 1234;            // 0.019 .. 37.6 in Q16.16
        const double  want = 1.0 / std::sqrt(as_real(a));
        CHECK_NEAR(as_real(q_rsqrt(a)), want, want * 1e-4 + 1e-4);
    }
    CHECK_EQ(q_rsqrt(kQ16One), kQ16One);
    CHECK_EQ(q_rsqrt(4 * kQ16One), kQ16One / 2);
}

PHX_TEST(tinyllm_scale_q30_saturates_instead_of_wrapping) {
    // The normal path: acc small enough that the plain product fits.
    CHECK_EQ(q_scale_q30(int64_t(1) << 30, 1 << 30), 1 << 30);
    CHECK_EQ(q_scale_q30(-(int64_t(1) << 30), 1 << 30), -(1 << 30));
    CHECK_EQ(q_scale_q30(0, 1 << 30), 0);
    CHECK_EQ(q_scale_q30(12345, 0), 0);
    // Negation happens BEFORE the shift (truncation toward zero), matching the reference. If it
    // floored instead, this would be -1.
    CHECK_EQ(q_scale_q30(-1, 1 << 29), 0);
    // Way past int32: saturate, never wrap.
    CHECK_EQ(q_scale_q30(int64_t(1) << 60, 1 << 30), 2147483647);
    CHECK_EQ(q_scale_q30(-(int64_t(1) << 60), 1 << 30), -2147483647 - 1);
}

PHX_TEST(tinyllm_exp_lut_bound) {
    // Build the table exactly as the exporter does, then hold the interpolated lookup to the
    // accuracy softmax actually needs.
    static int32_t lut[kExpLutN + 1];
    for (int i = 0; i <= kExpLutN; ++i)
        lut[i] = int32_t(std::floor(std::exp(-16.0 + i / 32.0) * 65536.0 + 0.5));

    double worst = 0.0;
    for (int i = 0; i <= 16 * 64; ++i) {
        const int32_t x = -int32_t(i) * 1024;           // -16 .. 0 in 1/64 steps
        const double got = as_real(q_exp_neg(lut, x));
        const double want = std::exp(as_real(x));
        const double d = std::fabs(got - want);
        if (d > worst) worst = d;
    }
    // Linear interpolation over a 1/32 step: the bound is (step^2/8)*max|exp''| = 1.2e-4.
    CHECK(worst < 1.3e-4);
    CHECK_EQ(q_exp_neg(lut, 0), kQ16One);
    CHECK_EQ(q_exp_neg(lut, 1234), kQ16One);            // positive input clamps to 1.0
    CHECK_EQ(q_exp_neg(lut, -(16 << 16)), 0);           // below the domain underflows to 0
    CHECK_EQ(q_exp_neg(lut, -2000000000), 0);           // and cannot overflow the domain shift
}

PHX_TEST(tinyllm_silu_lut_bound) {
    static int32_t lut[kSiluLutN + 1];
    for (int i = 0; i <= kSiluLutN; ++i) {
        const double x = -16.0 + i / 16.0;
        lut[i] = int32_t(std::floor(x / (1.0 + std::exp(-x)) * 65536.0 + 0.5));
    }
    double worst = 0.0;
    for (int i = -16 * 64; i <= 16 * 64; ++i) {
        const int32_t x = int32_t(i) * 1024;
        const double got = as_real(q_silu(lut, x));
        const double want = as_real(x) / (1.0 + std::exp(-as_real(x)));
        const double d = std::fabs(got - want);
        if (d > worst) worst = d;
    }
    CHECK(worst < 3.0e-4);
    // Both tails are exact rather than clamped to a table endpoint.
    CHECK_EQ(q_silu(lut, 20 << 16), 20 << 16);
    CHECK_EQ(q_silu(lut, -(20 << 16)), 0);
}

PHX_TEST(tinyllm_quantize_round_trips_within_int8_resolution) {
    Xs r;
    static int32_t x[172];
    static int8_t  q[192];
    for (int trial = 0; trial < 200; ++trial) {
        const int32_t n = 64 + int32_t(trial % 109);
        const int32_t scale = 1 + int32_t(r.next() % 400000u);
        int32_t maxabs = 0;
        for (int32_t i = 0; i < n; ++i) {
            x[i] = int32_t(r.next() % uint32_t(2 * scale + 1)) - scale;
            if (std::abs(x[i]) > maxabs) maxabs = std::abs(x[i]);
        }
        const int32_t stride = ((n + 31) / 32) * 32;
        const QQuant qq = q_quantize(x, n, stride, q);
        // The padding must be defined zeros or the padded matvec reads garbage.
        for (int32_t i = n; i < stride; ++i) CHECK_EQ(int(q[i]), 0);
        if (maxabs == 0) { CHECK_EQ(qq.scale_q30, 0); continue; }
        // Every code is in range, and the largest magnitude uses the full +-127 rail.
        int32_t maxcode = 0;
        for (int32_t i = 0; i < n; ++i) {
            CHECK(q[i] >= -127 && q[i] <= 127);
            if (std::abs(int(q[i])) > maxcode) maxcode = std::abs(int(q[i]));
        }
        CHECK_EQ(maxcode, 127);
        // Dequantizing lands within half a step of the original.
        const double sc = double(qq.scale_q30) / double(1 << 30);
        const double step = double(maxabs) / 127.0 / 65536.0;
        for (int32_t i = 0; i < n; ++i)
            CHECK(std::fabs(double(q[i]) * sc - as_real(x[i])) <= step * 0.5 + 1e-6);
    }
}

PHX_TEST(tinyllm_quantize_handles_all_zero_input) {
    static int32_t x[64] = {};
    static int8_t  q[64];
    for (int i = 0; i < 64; ++i) q[i] = 42;
    const QQuant qq = q_quantize(x, 64, 64, q);
    CHECK_EQ(qq.scale_q30, 0);
    for (int i = 0; i < 64; ++i) CHECK_EQ(int(q[i]), 0);
}

PHX_TEST(tinyllm_quantize_is_symmetric_about_zero) {
    // +v and -v must produce exactly opposite codes; an asymmetric rounding rule would bias
    // every matvec and show up as slow drift over a generation.
    static int32_t xp[32], xn[32];
    static int8_t  qp[32], qn[32];
    for (int i = 0; i < 32; ++i) { xp[i] = (i + 1) * 971; xn[i] = -xp[i]; }
    q_quantize(xp, 32, 32, qp);
    q_quantize(xn, 32, 32, qn);
    for (int i = 0; i < 32; ++i) CHECK_EQ(int(qp[i]), -int(qn[i]));
}

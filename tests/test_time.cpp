// tests/test_time.cpp — the fixed-step accumulator: step counting, clamp, alpha.
#include "phx_test.h"
#include "phx/core/time.h"

using namespace phx;

PHX_TEST(accumulator_single_step) {
    StepAccumulator acc; acc.configure(60);
    CHECK_EQ(acc.advance(acc.step_ns), 1);
    CHECK_EQ((long long)acc.acc, 0LL);
}

PHX_TEST(accumulator_multi_step_and_remainder) {
    StepAccumulator acc; acc.configure(60);
    uint64_t s = acc.step_ns;
    int steps = acc.advance(s * 2 + s / 2);     // 2.5 steps
    CHECK_EQ(steps, 2);
    CHECK_EQ((long long)acc.acc, (long long)(s / 2)); // half a step left over
}

PHX_TEST(accumulator_spiral_clamp) {
    StepAccumulator acc; acc.configure(60, /*max_catchup*/5);
    int steps = acc.advance(acc.step_ns * 1000);  // a giant stall
    CHECK_EQ(steps, 5);                            // never catch up more than the clamp
}

PHX_TEST(accumulator_alpha_range) {
    StepAccumulator acc; acc.configure(60);
    acc.advance(acc.step_ns / 2);                  // half a step, no full step
    double a = s_to_double(acc.alpha());           // tier-agnostic (float or fixed16)
    CHECK(a >= 0.0 && a < 1.0);
    CHECK_NEAR(a, 0.5, 0.01);
}

PHX_TEST(accumulator_no_step_below_threshold) {
    StepAccumulator acc; acc.configure(60);
    // halves sum exactly (step_ns is even), avoiding integer-floor surprises.
    CHECK_EQ(acc.advance(acc.step_ns / 2), 0);     // not enough for a step yet
    CHECK_EQ(acc.advance(acc.step_ns / 2), 1);     // crosses exactly one step
    CHECK_EQ((long long)acc.acc, 0LL);
}

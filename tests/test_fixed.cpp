// tests/test_fixed.cpp — Q16.16 fixed-point arithmetic and transcendentals.
#include "phx_test.h"
#include "phx/core/fixed.h"

using namespace phx;

PHX_TEST(fixed_int_roundtrip) {
    CHECK_EQ(fixed16::from_int(3).to_int(), 3);
    CHECK_EQ(fixed16::from_int(-7).to_int(), -7);
    CHECK_EQ(fixed16::from_int(0).raw, 0);
    CHECK_EQ(fixed16::from_int(1).raw, fixed16::kOne);
}

PHX_TEST(fixed_add_sub) {
    fixed16 a = fixed16::from_int(5);
    fixed16 b = fixed16::from_int(3);
    CHECK_EQ((a + b).to_int(), 8);
    CHECK_EQ((a - b).to_int(), 2);
    CHECK_EQ((-a).to_int(), -5);
}

PHX_TEST(fixed_mul_div) {
    fixed16 a = fixed16::from_int(6);
    fixed16 b = fixed16::from_int(7);
    CHECK_EQ((a * b).to_int(), 42);

    fixed16 half = fixed16::from_raw(fixed16::kOne / 2);
    CHECK_EQ((a * half).to_int(), 3);                 // 6 * 0.5 = 3

    fixed16 q = fixed16::from_int(10) / fixed16::from_int(4);   // 2.5
    CHECK_EQ(q.to_int(), 2);
    CHECK_NEAR(double(q.raw) / fixed16::kOne, 2.5, 0.001);
}

PHX_TEST(fixed_sqrt) {
    CHECK_NEAR(double(fx_sqrt(fixed16::from_int(16)).raw) / fixed16::kOne, 4.0, 0.01);
    CHECK_NEAR(double(fx_sqrt(fixed16::from_int(2)).raw)  / fixed16::kOne, 1.41421, 0.01);
    CHECK_EQ(fx_sqrt(fixed16::from_int(0)).raw, 0);
}

PHX_TEST(fixed_sin_cos) {
    // turns in [0,1): sin(0)=0, sin(1/4)=1, sin(1/2)=0, cos(0)=1
    CHECK_NEAR(double(fx_sin(fixed16::from_raw(0)).raw) / fixed16::kOne, 0.0, 0.02);
    CHECK_NEAR(double(fx_sin(fixed16::from_raw(fixed16::kOne / 4)).raw) / fixed16::kOne, 1.0, 0.02);
    CHECK_NEAR(double(fx_sin(fixed16::from_raw(fixed16::kOne / 2)).raw) / fixed16::kOne, 0.0, 0.02);
    CHECK_NEAR(double(fx_cos(fixed16::from_raw(0)).raw) / fixed16::kOne, 1.0, 0.02);
}

PHX_TEST(fixed_reciprocal) {
    CHECK_NEAR(double(fx_rcp(fixed16::from_int(4)).raw) / fixed16::kOne, 0.25, 0.01);
    CHECK_NEAR(double(fx_rcp(fixed16::from_int(2)).raw) / fixed16::kOne, 0.5,  0.01);
}

PHX_TEST(fixed_compare) {
    CHECK(fixed16::from_int(3) < fixed16::from_int(4));
    CHECK(fixed16::from_int(4) >= fixed16::from_int(4));
    CHECK(fixed16::from_int(2) != fixed16::from_int(3));
}

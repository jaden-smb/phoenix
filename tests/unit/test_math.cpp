// tests/test_math.cpp — scalar-generic 2D math: Vec2, AABB, Mat3 (docs/01-core.md).
// Uses s_to_double()/CHECK_NEAR so the same assertions run bit-honestly on both scalar
// tiers (float on PC/PSP, fixed16 on the gba_sim host build).
#include "phx_test.h"
#include "phx/core/math.h"

using namespace phx;

PHX_TEST(vec2_add_sub_dot) {
    vec2 a{ s_from_int(3), s_from_int(4) };
    vec2 b{ s_from_int(1), s_from_int(2) };
    vec2 sum = a + b;
    vec2 dif = a - b;
    CHECK_NEAR(s_to_double(sum.x), 4.0, 0.001);
    CHECK_NEAR(s_to_double(sum.y), 6.0, 0.001);
    CHECK_NEAR(s_to_double(dif.x), 2.0, 0.001);
    CHECK_NEAR(s_to_double(dif.y), 2.0, 0.001);
    CHECK_NEAR(s_to_double(a.dot(b)), 3.0 * 1.0 + 4.0 * 2.0, 0.001);
    CHECK_NEAR(s_to_double(length(vec2{ s_from_int(3), s_from_int(4) })), 5.0, 0.01);
}

PHX_TEST(aabb_overlaps_and_center) {
    aabb a = aabb::from_center({ s_from_int(0), s_from_int(0) }, { s_from_int(2), s_from_int(2) });
    aabb b = aabb::from_center({ s_from_int(3), s_from_int(0) }, { s_from_int(2), s_from_int(2) });
    aabb c = aabb::from_center({ s_from_int(10), s_from_int(10) }, { s_from_int(1), s_from_int(1) });
    CHECK(a.overlaps(b));      // edges touch/overlap at x=1..1
    CHECK(!a.overlaps(c));     // far apart

    vec2 center = a.center();
    CHECK_NEAR(s_to_double(center.x), 0.0, 0.001);
    CHECK_NEAR(s_to_double(center.y), 0.0, 0.001);
    vec2 half = a.half_extent();
    CHECK_NEAR(s_to_double(half.x), 2.0, 0.001);
    CHECK_NEAR(s_to_double(half.y), 2.0, 0.001);
}

PHX_TEST(mat3_identity_apply_is_noop) {
    mat3 id;   // default ctor = identity
    vec2 p{ s_from_int(5), s_from_int(-3) };
    vec2 r = id.apply(p);
    CHECK_NEAR(s_to_double(r.x), 5.0, 0.001);
    CHECK_NEAR(s_to_double(r.y), -3.0, 0.001);
}

PHX_TEST(mat3_translate) {
    mat3 t = mat3::translate({ s_from_int(10), s_from_int(-4) });
    vec2 r = t.apply({ s_from_int(1), s_from_int(2) });
    CHECK_NEAR(s_to_double(r.x), 11.0, 0.001);
    CHECK_NEAR(s_to_double(r.y), -2.0, 0.001);
}

PHX_TEST(mat3_scale) {
    mat3 s = mat3::scale({ s_from_int(2), s_from_int(3) });
    vec2 r = s.apply({ s_from_int(4), s_from_int(5) });
    CHECK_NEAR(s_to_double(r.x), 8.0, 0.001);
    CHECK_NEAR(s_to_double(r.y), 15.0, 0.001);
}

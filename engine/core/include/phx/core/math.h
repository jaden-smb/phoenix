// phx/core/math.h — scalar-generic 2D math. `scalar` is float on PC/PSP, fixed16 on GBA.
// Gameplay writes `phx::vec2`, never raw float, so one codebase serves every tier.
#ifndef PHX_CORE_MATH_H
#define PHX_CORE_MATH_H

#include "phx/core/types.h"
#include "phx/core/fixed.h"
#include "phx/core/caps.h"     // defines PHX_CAPS_HAS_FLOAT_HW for the active tier

namespace phx {

#if PHX_CAPS_HAS_FLOAT_HW
using scalar = float;
inline scalar s_sqrt(scalar v) { return __builtin_sqrtf(v); }
inline scalar s_from_int(int v) { return scalar(v); }
inline int    s_to_int(scalar v) { return int(v); }
inline scalar s_half(scalar v) { return v * 0.5f; }
// A scalar as a Q16.16 fixed-point integer. Used by the renderer for tier-EXACT zoom math:
// for integer (or any dyadic) zoom values this yields the SAME int32 on the float and fixed
// tiers, so the software/GU rasterizers produce bit-identical pixels regardless of `scalar`.
inline int32_t s_to_q16(scalar v) { return int32_t(v * 65536.0f + (v >= 0 ? 0.5f : -0.5f)); }
inline scalar  s_from_q16(int32_t q) { return scalar(q) / 65536.0f; }  // exact for dyadic values
#else
using scalar = fixed16;
inline scalar s_sqrt(scalar v) { return fx_sqrt(v); }
inline scalar s_from_int(int v) { return fixed16::from_int(v); }
inline int    s_to_int(scalar v) { return v.to_int(); }
inline scalar s_half(scalar v) { return fixed16::from_raw(v.raw >> 1); }
inline int32_t s_to_q16(scalar v) { return v.raw; }   // fixed16 is already Q16.16
inline scalar  s_from_q16(int32_t q) { return fixed16::from_raw(q); }
#endif

template <class T>
struct Vec2 {
    T x{}, y{};
    constexpr Vec2() = default;
    constexpr Vec2(T x_, T y_) : x(x_), y(y_) {}

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
    friend constexpr Vec2 operator*(Vec2 a, T s)    { return { a.x * s,   a.y * s   }; }
    friend constexpr Vec2 operator-(Vec2 a)         { return { -a.x, -a.y }; }
    Vec2& operator+=(Vec2 b) { x += b.x; y += b.y; return *this; }
    Vec2& operator-=(Vec2 b) { x -= b.x; y -= b.y; return *this; }

    constexpr T dot(Vec2 b) const { return x * b.x + y * b.y; }
    constexpr T len2()      const { return x * x + y * y; }
};

template <class T>
struct AABB {
    Vec2<T> min, max;
    constexpr bool overlaps(const AABB& o) const {
        return !(max.x < o.min.x || min.x > o.max.x ||
                 max.y < o.min.y || min.y > o.max.y);
    }
    Vec2<T> center() const {
        return { s_half(min.x + max.x), s_half(min.y + max.y) };
    }
    Vec2<T> half_extent() const {
        return { s_half(max.x - min.x), s_half(max.y - min.y) };
    }
    static constexpr AABB from_center(Vec2<T> c, Vec2<T> half) {
        return { { c.x - half.x, c.y - half.y }, { c.x + half.x, c.y + half.y } };
    }
};

// 2D affine matrix (row-major 3x3 with implicit last row). Used by 2.5D / camera.
template <class T>
struct Mat3 {
    T m[9] = { T(1),T(0),T(0),  T(0),T(1),T(0),  T(0),T(0),T(1) };
    static Mat3 translate(Vec2<T> t);
    static Mat3 scale(Vec2<T> s);
    Vec2<T> apply(Vec2<T> p) const { return { m[0]*p.x + m[1]*p.y + m[2],
                                              m[3]*p.x + m[4]*p.y + m[5] }; }
};

// Concrete engine aliases.
using vec2  = Vec2<scalar>;
using aabb  = AABB<scalar>;
using mat3  = Mat3<scalar>;
using vec2i = Vec2<int32_t>;   // pixel/tile coordinates (always integer)

inline scalar length(vec2 v) { return s_sqrt(v.len2()); }

// Host/tools-only: convert a scalar to double for printing/tests. Never used in engine
// runtime hot paths (the GBA build must stay double-free), but tier-agnostic in tests.
#if PHX_CAPS_HAS_FLOAT_HW
inline double s_to_double(scalar v) { return double(v); }
#else
inline double s_to_double(scalar v) { return double(v.raw) / double(fixed16::kOne); }
#endif

} // namespace phx
#endif // PHX_CORE_MATH_H

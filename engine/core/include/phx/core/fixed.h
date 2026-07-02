// phx/core/fixed.h — Q16.16 signed fixed-point for the GBA (no FPU) and any tier
// that selects software math. Operators compile to ARM MUL/shift sequences.
//
// On ARM7TDMI there is NO hardware divide: operator/ and fx_rcp are the ONLY
// sanctioned division paths, both LUT/Newton accelerated and budgeted.
#ifndef PHX_CORE_FIXED_H
#define PHX_CORE_FIXED_H

#include "phx/core/types.h"

namespace phx {

struct fixed16 {
    int32_t raw = 0;
    static constexpr int kShift = 16;
    static constexpr int32_t kOne = 1 << kShift;

    constexpr fixed16() = default;
    constexpr explicit fixed16(int32_t raw_value) : raw(raw_value) {}

    // × kOne, not << kShift: left-shifting a negative is UB in C++17 (UBSan-caught); the
    // multiply is well-defined over the whole valid ±32767 input range and compiles the same.
    static constexpr fixed16 from_int(int32_t i)   { return fixed16(i * kOne); }
    static constexpr fixed16 from_raw(int32_t r)   { return fixed16(r); }
    // approximate float construction is compile-time only (used in tuning constants);
    // gameplay never produces float at runtime on GBA.
    static constexpr fixed16 from_float(double f)  { return fixed16(int32_t(f * kOne)); }

    constexpr int32_t to_int()   const { return raw >> kShift; }       // floor
    constexpr int32_t round()    const { return (raw + (kOne >> 1)) >> kShift; }

    constexpr fixed16 operator-() const { return fixed16(-raw); }

    friend constexpr fixed16 operator+(fixed16 a, fixed16 b) { return fixed16(a.raw + b.raw); }
    friend constexpr fixed16 operator-(fixed16 a, fixed16 b) { return fixed16(a.raw - b.raw); }
    friend constexpr fixed16 operator*(fixed16 a, fixed16 b) {
        return fixed16(int32_t((int64_t(a.raw) * b.raw) >> kShift));    // 64-bit intermediate
    }
    friend constexpr fixed16 operator/(fixed16 a, fixed16 b) {
        return fixed16(int32_t((int64_t(a.raw) * kOne) / b.raw));       // see header note; × not
    }                                                                   // << (negative-shift UB)
    friend constexpr fixed16 operator*(fixed16 a, int32_t s) { return fixed16(a.raw * s); }

    fixed16& operator+=(fixed16 b) { raw += b.raw; return *this; }
    fixed16& operator-=(fixed16 b) { raw -= b.raw; return *this; }
    fixed16& operator*=(fixed16 b) { *this = *this * b; return *this; }

    friend constexpr bool operator< (fixed16 a, fixed16 b) { return a.raw <  b.raw; }
    friend constexpr bool operator<=(fixed16 a, fixed16 b) { return a.raw <= b.raw; }
    friend constexpr bool operator> (fixed16 a, fixed16 b) { return a.raw >  b.raw; }
    friend constexpr bool operator>=(fixed16 a, fixed16 b) { return a.raw >= b.raw; }
    friend constexpr bool operator==(fixed16 a, fixed16 b) { return a.raw == b.raw; }
    friend constexpr bool operator!=(fixed16 a, fixed16 b) { return a.raw != b.raw; }
};

constexpr fixed16 abs(fixed16 v) { return v.raw < 0 ? -v : v; }

// Transcendental helpers — implemented in fixed.cpp with 256-entry LUTs.
// Declared here; on GBA these are the hot paths the renderer/physics rely on.
fixed16 fx_sqrt(fixed16 v);          // Newton + LUT seed
fixed16 fx_sin(fixed16 turns);       // turns in [0,1); 256-entry sine LUT
fixed16 fx_cos(fixed16 turns);
fixed16 fx_rcp(fixed16 v);           // reciprocal: LUT seed + 1 Newton step (no HW div)

// Literal suffix for fixed constants:  fixed16 g = 9_fx; (rare — usually _fxf below)
constexpr fixed16 operator""_fx(unsigned long long i) { return fixed16::from_int(int32_t(i)); }
constexpr fixed16 operator""_fxf(long double f)       { return fixed16::from_float(double(f)); }

} // namespace phx
#endif // PHX_CORE_FIXED_H

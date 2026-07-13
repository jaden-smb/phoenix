// phx/core/types.h — zero-dependency foundation: fixed-width ints, Result, TypeId, hashing.
// This header has NO dependencies (not even <cstdint> obligations beyond the standard ints)
// so the platform C seam can include it freely.
#ifndef PHX_CORE_TYPES_H
#define PHX_CORE_TYPES_H

#include <cstdint>
#include <cstddef>

#include "phx/core/assert.h"

namespace phx {

// ---- Status / Result -------------------------------------------------------
// Fallible APIs return Status or Result<T>. NO C++ exceptions cross a boundary.
enum class Status : int16_t {
    Ok = 0,
    OutOfMemory,
    NotFound,
    BadArg,
    IoError,
    Unsupported,
    Overflow,
    Corrupt,       // structurally valid but a content integrity check failed (e.g. bad checksum)
};

template <class T>
struct Result {
    Status st;
    T      val;

    constexpr bool ok() const { return st == Status::Ok; }
    constexpr explicit operator bool() const { return ok(); }

    T&       unwrap()       { PHX_ASSERT_MSG(ok(), "Result::unwrap on error"); return val; }
    const T& unwrap() const { PHX_ASSERT_MSG(ok(), "Result::unwrap on error"); return val; }
    T        value_or(T fallback) const { return ok() ? val : fallback; }

    static constexpr Result good(T v) { return { Status::Ok, static_cast<T&&>(v) }; }
    static constexpr Result fail(Status s) { return { s, T{} }; }
};

// ---- Compile-time type identity (no RTTI) ----------------------------------
// Each T gets a stable id within a process. Used by ECS component registries.
using TypeId = uint32_t;
namespace detail { inline TypeId next_type_id() { static TypeId n = 0; return n++; } }
template <class T> inline TypeId type_id() { static const TypeId id = detail::next_type_id(); return id; }

// ---- FNV-1a hashing (asset names, zone ids) --------------------------------
using NameHash = uint32_t;
constexpr NameHash kFnvOffset = 2166136261u;
constexpr NameHash kFnvPrime  = 16777619u;

constexpr NameHash fnv1a(const char* s, NameHash h = kFnvOffset) {
    return (*s == '\0') ? h : fnv1a(s + 1, (h ^ NameHash(uint8_t(*s))) * kFnvPrime);
}
constexpr NameHash fnv1a_n(const char* s, size_t n, NameHash h = kFnvOffset) {
    return (n == 0) ? h : fnv1a_n(s + 1, n - 1, (h ^ NameHash(uint8_t(*s))) * kFnvPrime);
}

// User-defined literal: "player.png"_hash  -> compile-time constant
constexpr NameHash operator""_hash(const char* s, size_t n) { return fnv1a_n(s, n); }

// ---- A non-owning view over contiguous memory (no STL dep) -----------------
template <class T>
struct Span {
    T*     ptr = nullptr;
    size_t len = 0;
    constexpr T*     begin() const { return ptr; }
    constexpr T*     end()   const { return ptr + len; }
    constexpr size_t size()  const { return len; }
    constexpr bool   empty() const { return len == 0; }
    constexpr T&     operator[](size_t i) const { return ptr[i]; }
};

// ---- small helpers ---------------------------------------------------------
template <class T> constexpr T min2(T a, T b) { return a < b ? a : b; }
template <class T> constexpr T max2(T a, T b) { return a > b ? a : b; }
template <class T> constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

constexpr size_t align_up(size_t n, size_t a) { return (n + (a - 1)) & ~(a - 1); }

#define PHX_CONCAT_(a, b) a##b
#define PHX_CONCAT(a, b)  PHX_CONCAT_(a, b)

} // namespace phx
#endif // PHX_CORE_TYPES_H

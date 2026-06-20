// phx/core/assert.h — assertion macros. Programmer errors trap+log in debug and are
// compiled out in release; recoverable failures use Status/Result instead (never assert).
#ifndef PHX_CORE_ASSERT_H
#define PHX_CORE_ASSERT_H

namespace phx::detail {
// Defined in assert.cpp (host) / per-platform; declared here to avoid a stdio include
// leaking into every translation unit. On GBA this hits a BIOS breakpoint.
[[noreturn]] void assert_fail(const char* expr, const char* file, int line, const char* msg);
}

#if defined(PHX_BUILD_RELEASE)
    #define PHX_ASSERT(cond)            ((void)0)
    #define PHX_ASSERT_MSG(cond, msg)   ((void)0)
#else
    #define PHX_ASSERT(cond) \
        do { if (!(cond)) ::phx::detail::assert_fail(#cond, __FILE__, __LINE__, ""); } while (0)
    #define PHX_ASSERT_MSG(cond, msg) \
        do { if (!(cond)) ::phx::detail::assert_fail(#cond, __FILE__, __LINE__, (msg)); } while (0)
#endif

// PHX_VERIFY is ALWAYS checked (even in release) — for invariants whose failure must
// never be silently skipped. Evaluates `cond` exactly once.
#define PHX_VERIFY(cond) \
    do { if (!(cond)) ::phx::detail::assert_fail(#cond, __FILE__, __LINE__, "VERIFY"); } while (0)

#endif // PHX_CORE_ASSERT_H

// phx/core/src/assert.cpp — host/desktop assertion handler. Console backends provide
// their own assert_fail (GBA: BIOS breakpoint; PSP: pspDebugScreen + sceKernelExitGame).
#include "phx/core/assert.h"

#include <cstdio>
#include <cstdlib>

namespace phx::detail {

[[noreturn]] void assert_fail(const char* expr, const char* file, int line, const char* msg) {
    std::fprintf(stderr, "\n[phx] ASSERT FAILED: %s\n  at %s:%d\n", expr, file, line);
    if (msg && msg[0]) std::fprintf(stderr, "  note: %s\n", msg);
    std::fflush(stderr);
    std::abort();
}

} // namespace phx::detail

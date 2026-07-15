// tests/test_version.cpp — the version header: macro/accessor/string consistency.
// The build system and release workflow parse version.h textually (CMake regex, Makefile
// sed), so this locks the composed forms to the three component ints they parse.
#include "phx_test.h"
#include "phx/core/version.h"

#include <cstring>

PHX_TEST(version_components_sane) {
    CHECK(phx::version_major() >= 0);
    CHECK(phx::version_minor() >= 0 && phx::version_minor() <= 99);   // NUMBER slot width
    CHECK(phx::version_patch() >= 0 && phx::version_patch() <= 99);
    CHECK_EQ(phx::version_major(), PHX_VERSION_MAJOR);
    CHECK_EQ(phx::version_minor(), PHX_VERSION_MINOR);
    CHECK_EQ(phx::version_patch(), PHX_VERSION_PATCH);
}

PHX_TEST(version_string_matches_components) {
    // Re-parse "MAJOR.MINOR.PATCH" and compare against the ints — catches any future
    // hand-editing of the composed string.
    const char* s = phx::version_string();
    int maj = 0, min = 0, pat = 0, part = 0;
    for (const char* p = s; *p; ++p) {
        if (*p == '.') { ++part; continue; }
        CHECK(*p >= '0' && *p <= '9');
        int* dst = (part == 0) ? &maj : (part == 1) ? &min : &pat;
        *dst = *dst * 10 + (*p - '0');
    }
    CHECK_EQ(part, 2);                       // exactly two dots
    CHECK_EQ(maj, PHX_VERSION_MAJOR);
    CHECK_EQ(min, PHX_VERSION_MINOR);
    CHECK_EQ(pat, PHX_VERSION_PATCH);
    CHECK(std::strlen(s) >= 5);              // shortest legal form "0.0.0"
}

PHX_TEST(version_number_ordering) {
    CHECK_EQ(PHX_VERSION_NUMBER,
             PHX_VERSION_MAJOR * 10000 + PHX_VERSION_MINOR * 100 + PHX_VERSION_PATCH);
    CHECK(PHX_VERSION_AT_LEAST(0, 0, 0));
    CHECK(PHX_VERSION_AT_LEAST(PHX_VERSION_MAJOR, PHX_VERSION_MINOR, PHX_VERSION_PATCH));
    CHECK(!PHX_VERSION_AT_LEAST(PHX_VERSION_MAJOR + 1, 0, 0));
}

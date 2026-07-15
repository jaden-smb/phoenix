// phx/core/version.h — the engine version, single source of truth.
//
// Everything that needs a version derives it from the three integers below:
//   - the root CMakeLists.txt parses them into project(... VERSION) / CPack
//   - the Makefile parses them into PHX_VERSION (dist tarball/zip names, `make version`)
//   - the release workflow refuses to publish a tag that doesn't match them
// Bump ONLY here (see RELEASING.md for the release checklist); the string and the
// comparable number are composed by the preprocessor so they can never drift.
//
// Pre-1.0 semver: MINOR bumps may break API/asset formats, PATCH bumps are fixes.
#pragma once

#define PHX_VERSION_MAJOR 0
#define PHX_VERSION_MINOR 1
#define PHX_VERSION_PATCH 0

// "MAJOR.MINOR.PATCH" — composed, not hand-written, so it can't disagree with the ints.
#define PHX_VERSION_STR_(x) #x
#define PHX_VERSION_STR(x) PHX_VERSION_STR_(x)
#define PHX_VERSION_STRING            \
  PHX_VERSION_STR(PHX_VERSION_MAJOR)  \
  "." PHX_VERSION_STR(PHX_VERSION_MINOR) "." PHX_VERSION_STR(PHX_VERSION_PATCH)

// Monotonic single-integer form for compile-time comparisons (one decimal slot of
// headroom per component: 0.1.0 -> 100, 1.2.34 -> 1020034... caps at 99 per component).
#define PHX_VERSION_NUMBER \
  (PHX_VERSION_MAJOR * 10000 + PHX_VERSION_MINOR * 100 + PHX_VERSION_PATCH)
#define PHX_VERSION_AT_LEAST(maj, min, pat) \
  (PHX_VERSION_NUMBER >= ((maj) * 10000 + (min) * 100 + (pat)))

namespace phx {

// Runtime accessors (constexpr, header-only — nothing to link, safe on every tier).
constexpr int         version_major()  { return PHX_VERSION_MAJOR; }
constexpr int         version_minor()  { return PHX_VERSION_MINOR; }
constexpr int         version_patch()  { return PHX_VERSION_PATCH; }
constexpr const char* version_string() { return PHX_VERSION_STRING; }

}  // namespace phx

# engine/platform/CMakeLists.txt — exactly ONE backend compiles per build (docs/02 §1).
# Desktop (linux/windows): the SDL2 backend if SDL2 is found, else the headless `null`
# backend (so a build always succeeds, just without a window). Consoles (gba/psp) will select
# their native backend when those land; until then they fall back to `null` too. There is no
# runtime platform branching — the linker picks the one translation unit that exports the seam.
add_library(phx_platform STATIC)
target_include_directories(phx_platform PUBLIC include)
target_link_libraries(phx_platform PUBLIC phx_core)

set(PHX_PLATFORM_BACKEND "null")
if(PHX_TARGET STREQUAL "linux" OR PHX_TARGET STREQUAL "windows")
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    set(PHX_PLATFORM_BACKEND "sdl")
  endif()
endif()

if(PHX_PLATFORM_BACKEND STREQUAL "sdl")
  target_sources(phx_platform PRIVATE src/sdl/sdl_platform.cpp)
  target_compile_definitions(phx_platform PRIVATE PHX_HAVE_SDL=1)
  target_link_libraries(phx_platform PUBLIC SDL2::SDL2)
else()
  target_sources(phx_platform PRIVATE src/null/null_platform.cpp)
endif()

message(STATUS "Phoenix platform backend: ${PHX_PLATFORM_BACKEND}")

# Phoenix Engine — root build. One tree -> four runtime binaries + host tools.
# See docs/07-build-system.md for the full strategy.
cmake_minimum_required(VERSION 3.20)
project(PhoenixEngine LANGUAGES C CXX)

# ---- target selection ------------------------------------------------------
set(PHX_TARGET "linux" CACHE STRING "Target platform: linux | windows | gba | psp")
set_property(CACHE PHX_TARGET PROPERTY STRINGS linux windows gba psp)
message(STATUS "Phoenix target: ${PHX_TARGET}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/phx_module.cmake)
include(cmake/caps_select.cmake)   # sets PHX_TARGET_* defines + PHX_RENDER_TIER

# ---- per-target compile policy --------------------------------------------
if(PHX_TARGET STREQUAL "gba" OR PHX_TARGET STREQUAL "psp")
  # Consoles: no exceptions, no RTTI — matches the engine's no-throw contract.
  add_compile_options(-fno-exceptions -fno-rtti)
  add_compile_definitions(PHX_NO_EXCEPTIONS=1)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- modules (each is a STATIC lib; only the matching backend compiles) ----
add_subdirectory(engine/core)
add_subdirectory(engine/memory)
add_subdirectory(engine/platform)
add_subdirectory(engine/render)
add_subdirectory(engine/ecs)
add_subdirectory(engine/input)
add_subdirectory(engine/audio)
add_subdirectory(engine/resource)
add_subdirectory(engine/scene)
add_subdirectory(engine/physics)
add_subdirectory(engine/anim)
add_subdirectory(engine/ui)

# ---- the example game ------------------------------------------------------
add_subdirectory(examples/platformer)

# ---- host-only tools (always built for the host, even when cross-compiling) -
if(NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(tools/phxpack)
  add_subdirectory(tools/phxsprite)
  add_subdirectory(tools/phxtile)
  add_subdirectory(tools/phxsnd)
  add_subdirectory(tools/phxbin)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- architectural law: acyclic module dependency graph (CI gate) ----------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_custom_target(depcheck ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/common/depcheck.py ${CMAKE_SOURCE_DIR}/engine
    COMMENT "Verifying acyclic module dependency graph (docs/00 §3)")
endif()

# examples/emberwing/CMakeLists.txt — Emberwing: Cinder Hollow, the second full-game example.
# Same sources build for all four targets. (On the host without CMake, the Makefile builds
# this example as `make emberwing` / `make emberwing-sdl`; consoles via `make gba-emberwing`
# / `make psp-emberwing`.)
add_executable(emberwing
  src/main.cpp
  src/systems.cpp
  src/scenes.cpp
)
# main.cpp self-bakes the bundle via the host BundleWriter, so the example needs the phxpack
# writer headers on the include path (and the engine resource headers they reference).
target_include_directories(emberwing PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/phxpack
  ${CMAKE_SOURCE_DIR}/tools/common
  ${CMAKE_SOURCE_DIR}/engine/resource/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(emberwing PRIVATE
  phx_core phx_memory phx_platform phx_render phx_ecs phx_input
  phx_resource phx_scene phx_physics phx_anim phx_ui phx_audio phx_runtime
)

# --- per-target packaging ----------------------------------------------------
if(PHX_TARGET STREQUAL "gba")
  phx_gba_rom(emberwing)
elseif(PHX_TARGET STREQUAL "psp")
  phx_psp_eboot(emberwing "Emberwing")
endif()
# NOTE: the runnable console artifacts (bundle embedded + console entry points gba_main.cpp /
# psp_main.cpp) are produced by the Makefile: `make gba-emberwing` / `make psp-emberwing`.

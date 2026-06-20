# examples/platformer/CMakeLists.txt — the same sources build for all four targets.
# (On the host without CMake, the Makefile builds this example as `make platformer`.)
add_executable(platformer
  src/main.cpp
  src/systems.cpp
)
# Links the engine modules the example uses. (phx_audio exists now; add it here once the
# example triggers SFX through a real audio device — see STATUS "remaining glue".)
# main.cpp self-bakes the demo bundle via the host BundleWriter, so the example needs the
# phxpack writer headers on the include path.
target_include_directories(platformer PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/phxpack
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(platformer PRIVATE
  phx_core phx_memory phx_platform phx_render phx_ecs
  phx_input phx_resource phx_scene phx_physics phx_anim phx_ui phx_runtime
)

# --- per-target packaging ----------------------------------------------------
# A real project bakes authored assets offline with `phxpack` into assets.phxp; this demo
# synthesizes its bundle at startup (src/bake.h) so it stays self-contained. The packaging
# hooks below still apply when shipping a prebaked bundle.
if(PHX_TARGET STREQUAL "gba")
  phx_gba_rom(platformer)                          # link bundle into the ROM, emit .gba
elseif(PHX_TARGET STREQUAL "psp")
  phx_psp_eboot(platformer "Phoenix Platformer")   # -> EBOOT.PBP (mksfo + pack-pbp)
endif()
# PC: the exe writes/loads platformer.phxp in the working dir alongside it.

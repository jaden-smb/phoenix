# examples/miracle-player/CMakeLists.txt — the "A Small Miracle" music visualizer. The same
# sources build for host + console; only the entry + linked backend differ.
# (On the host without CMake, the Makefile builds this as `make miracle`; the loadable GBA ROM —
# with the tier-0 bundle embedded via bin2s — is `make gba-miracle-ppu`, the real console path.)
add_executable(miracle
  src/main.cpp
  src/miracle.cpp
)
# main.cpp self-bakes the bundle via the host BundleWriter + the phxviz analysis, so the example
# needs the phxpack/phxviz/common headers on the include path (and the resource headers).
target_include_directories(miracle PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/phxpack
  ${CMAKE_SOURCE_DIR}/tools/phxviz
  ${CMAKE_SOURCE_DIR}/tools/common
  ${CMAKE_SOURCE_DIR}/engine/resource/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(miracle PRIVATE
  phx_core phx_memory phx_platform phx_render phx_ecs phx_input
  phx_resource phx_scene phx_physics phx_anim phx_ui phx_audio phx_runtime
)

# --- per-target packaging ----------------------------------------------------
# The runnable console artifact (tier-0 bundle embedded + the PPU entry) is produced by the
# Makefile: `make gba-miracle-ppu`. This target builds the portable engine for the console but,
# like the platformer example, does not embed the multi-MB song bundle — prefer the Makefile for
# a loadable ROM. (No PSP entry today; the GBA PPU ROM is the shipping console build.)
if(PHX_TARGET STREQUAL "gba")
  phx_gba_rom(miracle)
endif()
# PC: the exe writes/loads miracle.phxp in the working dir (bakes the synthetic tone if the song
# WAV isn't present at build/miracle.wav).

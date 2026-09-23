# examples/tinyllm/CMakeLists.txt — a quantized transformer language model running on a Game Boy
# Advance. The same sources build for host and console; only the entry + linked backend differ.
# (On the host without CMake, the Makefile builds this as `make tinyllm` / `make tinyllm-sdl`; the
# loadable GBA ROM — with the tier-0 bundle embedded via bin2s — is `make gba-tinyllm-ppu`, the
# real console path.)
add_executable(tinyllm
  src/main.cpp
  src/tinyllm.cpp
  src/llm.cpp
)
# main.cpp self-bakes the bundle via the host BundleWriter, so the example needs the phxpack
# headers on its include path (and the resource headers).
target_include_directories(tinyllm PRIVATE
  ${CMAKE_SOURCE_DIR}/tools/phxpack
  ${CMAKE_SOURCE_DIR}/tools/common
  ${CMAKE_SOURCE_DIR}/engine/resource/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tinyllm PRIVATE
  phx_core phx_memory phx_platform phx_render phx_ecs phx_input
  phx_resource phx_scene phx_physics phx_anim phx_ui phx_audio phx_runtime
)

# --- per-target packaging ----------------------------------------------------
# The runnable console artifact (tier-0 bundle embedded + the PPU entry) is produced by the
# Makefile: `make gba-tinyllm-ppu`. This target builds the portable engine for the console but,
# like the platformer example, does not embed the model bundle — prefer the Makefile for a
# loadable ROM. (No PSP entry today; the GBA PPU ROM is the shipping console build.)
if(PHX_TARGET STREQUAL "gba")
  phx_gba_rom(tinyllm)
endif()
# PC: the exe writes/loads tinyllm.phxp in the working dir, baking the generated fixture model
# when no .phxllm has been exported (see examples/tinyllm/README.md).

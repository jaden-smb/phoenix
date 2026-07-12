# tests/CMakeLists.txt — the host test suites, registered with CTest (run with `ctest`).
# Mirrors `make check`: one unit runner (self-registering PHX_TESTs) + the headless
# integration binaries, each linking the engine module libraries. This is the default
# (headless) configuration: null platform + software render backend — no display needed.
#
# Note: the GBA-PPU (`make ppu`) and PSP-GU (`make gu`) host-model checks deliberately swap
# the render backend, which is incompatible with CMake's one-backend-per-build configure; they
# are compiled here as standalone executables (front end + the one backend, no phx_render lib)
# so `ctest` covers the same ground as `make check`.

set(_phxpack_inc ${CMAKE_SOURCE_DIR}/tools/phxpack)
set(_render_src  ${CMAKE_SOURCE_DIR}/engine/render/src)

# Several integration tests bake a bundle to "build/<name>.phxp" (a path relative to the cwd)
# and mount it back. Run every test from the CMake build tree and give it that "build/" subdir,
# so artifacts stay inside the build directory instead of the source tree.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/build)
set(_test_wd ${CMAKE_BINARY_DIR})

# ---- unit suite (foundation) ----------------------------------------------
add_executable(phx_tests
  unit/main.cpp
  unit/test_fixed.cpp unit/test_memory.cpp unit/test_ecs.cpp unit/test_time.cpp
  unit/test_input.cpp unit/test_physics.cpp unit/test_anim.cpp unit/test_scene.cpp
  unit/test_ui.cpp unit/test_audio.cpp unit/test_stream.cpp unit/test_lz.cpp
  unit/test_png.cpp unit/test_json.cpp unit/test_wav.cpp unit/test_cmdqueue.cpp
)
target_include_directories(phx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${_phxpack_inc})
target_link_libraries(phx_tests PRIVATE
  phx_core phx_memory phx_ecs phx_physics phx_anim phx_scene phx_audio phx_input
  phx_ui phx_resource)
add_test(NAME unit COMMAND phx_tests WORKING_DIRECTORY ${_test_wd})

# ---- headless integration binaries ----------------------------------------
# The full gameplay/render/resource stack on the null platform + software backend. Linking the
# superset of module libs is harmless — the static linker pulls only the members each test uses.
set(_game_libs
  phx_runtime phx_scene phx_physics phx_anim phx_ui
  phx_render phx_ecs phx_input phx_resource phx_audio
  phx_platform phx_memory phx_core)

# name -> source file
set(_itests
  smoke:smoke_app
  render:render_test
  playable:playable_test
  physics:physics_test
  anim:anim_test
  scene:scene_test
  ui:ui_test
  platformer:platformer_test
  audio:audio_test
  texcache:texcache_test
  png:png_test
  sprite:sprite_test
  tiled:tiled_test
  resource:resource_test
  pipeline:pipeline_test)

foreach(spec ${_itests})
  string(REPLACE ":" ";" parts ${spec})
  list(GET parts 0 tname)
  list(GET parts 1 tsrc)
  add_executable(it_${tname} suites/${tsrc}.cpp)
  target_include_directories(it_${tname} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${_phxpack_inc}
    ${CMAKE_SOURCE_DIR}/tools/common                # bake.h -> debug_font.h
    ${CMAKE_SOURCE_DIR}/tools/phxtmap               # pipeline_test: the phxtmap editor model
    ${CMAKE_SOURCE_DIR}/examples/platformer/src)   # platformer_test: components.h / bake.h
  target_link_libraries(it_${tname} PRIVATE ${_game_libs})
  add_test(NAME ${tname} COMMAND it_${tname} WORKING_DIRECTORY ${_test_wd})
endforeach()

# The platformer integration test drives the real example game object, so it also needs the
# example's gameplay translation unit compiled in (the rest of the example is header-only).
target_sources(it_platformer PRIVATE ${CMAKE_SOURCE_DIR}/examples/platformer/src/systems.cpp)
target_include_directories(it_platformer PRIVATE ${CMAKE_SOURCE_DIR}/engine/resource/include)

# pipeline_test is in-process; it also drops author-source fixtures into build/. The CLI smoke
# below then runs the REAL converter + assembler binaries over those fixtures (proving the
# executables link/parse/run), so it must run after pipeline.
add_test(NAME tools_cli
  COMMAND ${CMAKE_COMMAND}
    -DPHXSPRITE=$<TARGET_FILE:phxsprite> -DPHXTILE=$<TARGET_FILE:phxtile>
    -DPHXSND=$<TARGET_FILE:phxsnd>       -DPHXBIN=$<TARGET_FILE:phxbin>
    -DPHXPACK=$<TARGET_FILE:phxpack>     -DWD=${CMAKE_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_tools.cmake
  WORKING_DIRECTORY ${_test_wd})
set_tests_properties(tools_cli PROPERTIES DEPENDS pipeline)

# ---- console-native render backends, host-model verification --------------
# Compiled directly (front end + the chosen backend, no phx_render) so both backends can coexist
# in one build tree without colliding on phx_make_render_backend.
foreach(spec ppu:gba/gba_ppu gu:gu/gu_backend)
  string(REPLACE ":" ";" parts ${spec})
  list(GET parts 0 tname)
  list(GET parts 1 bsrc)
  add_executable(it_${tname}
    ${CMAKE_SOURCE_DIR}/engine/core/src/assert.cpp
    ${CMAKE_SOURCE_DIR}/engine/core/src/fixed.cpp
    ${CMAKE_SOURCE_DIR}/engine/core/src/log.cpp
    ${CMAKE_SOURCE_DIR}/engine/memory/src/memory_root.cpp
    ${CMAKE_SOURCE_DIR}/engine/platform/src/null/null_platform.cpp
    ${_render_src}/renderer.cpp
    ${_render_src}/${bsrc}.cpp
    suites/${tname}_test.cpp)
  target_include_directories(it_${tname} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine/core/include
    ${CMAKE_SOURCE_DIR}/engine/memory/include
    ${CMAKE_SOURCE_DIR}/engine/platform/include
    ${CMAKE_SOURCE_DIR}/engine/render/include
    ${_render_src} ${_render_src}/gba ${_render_src}/gu)   # ppu_model.h / gu_model.h
  add_test(NAME ${tname} COMMAND it_${tname} WORKING_DIRECTORY ${_test_wd})
endforeach()

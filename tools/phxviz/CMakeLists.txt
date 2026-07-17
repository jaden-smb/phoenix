# tools/phxviz/CMakeLists.txt — host-only converter CLI (docs/08, sibling of phxsnd). Analyses a
# WAV into a per-video-frame visualization track (VizHeader + VizFrame records) and emits an
# intermediate .phxviz bundle that phxpack merges. Shares the one bake path (builders.h) and the
# runtime POD format (examples/miracle-player/src/viz.h).
add_executable(phxviz main.cpp)
target_include_directories(phxviz PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/tools/phxpack
  ${CMAKE_SOURCE_DIR}/examples/miracle-player/src
  ${CMAKE_SOURCE_DIR}/engine/core/include
  ${CMAKE_SOURCE_DIR}/engine/resource/include)
target_compile_features(phxviz PRIVATE cxx_std_17)

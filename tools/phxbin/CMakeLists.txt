# tools/phxbin/CMakeLists.txt — host-only converter CLI (docs/08). A thin front-end over the shared
# bake logic in tools/phxpack/builders.h, so it shares the engine's on-disk asset format and the
# one bake path. Emits an intermediate .phx* file that phxpack merges into the bundle.
add_executable(phxbin main.cpp)
target_include_directories(phxbin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/tools/phxpack
  ${CMAKE_SOURCE_DIR}/engine/core/include
  ${CMAKE_SOURCE_DIR}/engine/resource/include)
target_compile_features(phxbin PRIVATE cxx_std_17)

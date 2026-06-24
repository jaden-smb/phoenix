# tools/phxpack/CMakeLists.txt — host-only asset packer CLI (never ships to a console).
# Bundles png/ppm/tmcsv/sprdef/tmj/wav -> assets.phxp; ships dependency-free DEFLATE/PNG/
# JSON/Tiled/WAV importers as headers (STL allowed in host tools). Built for the host even
# when cross-compiling the engine.
add_executable(phxpack main.cpp)
# Its writer/codec headers reference the engine's bundle/pixel/lz POD layouts (header-only,
# no engine library link — the tool just shares the on-disk format definitions).
target_include_directories(phxpack PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/engine/core/include
  ${CMAKE_SOURCE_DIR}/engine/resource/include)
target_compile_features(phxpack PRIVATE cxx_std_17)

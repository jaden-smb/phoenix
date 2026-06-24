# engine/input/CMakeLists.txt — L2 semantic input. Header-only (raw->Button/edge/axis lives
# in the public header), so it is an INTERFACE library: it contributes include paths + deps
# but compiles no translation unit of its own.
add_library(phx_input INTERFACE)
target_include_directories(phx_input INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(phx_input INTERFACE phx_core phx_platform)

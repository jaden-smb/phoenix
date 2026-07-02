# cmake/mingw.toolchain.cmake — MinGW-w64 cross toolchain for Windows (x86_64).
# Works with either GCC MinGW (apt install g++-mingw-w64-x86-64) or llvm-mingw; any
# toolchain exporting the x86_64-w64-mingw32-* triple binaries on PATH (or set
# PHX_MINGW_PREFIX to a toolchain root).
#   cmake -S . -B build/windows -DPHX_TARGET=windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw.toolchain.cmake
set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{PHX_MINGW_PREFIX})
  set(_bin "$ENV{PHX_MINGW_PREFIX}/bin/")
else()
  set(_bin "")
endif()

set(CMAKE_C_COMPILER   ${_bin}x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER ${_bin}x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  ${_bin}x86_64-w64-mingw32-windres)

# don't try to run host test binaries during compiler checks
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

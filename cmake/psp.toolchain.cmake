# cmake/psp.toolchain.cmake — pspsdk cross toolchain for the Sony PSP (MIPS Allegrex).
# Requires pspsdk installed and $PSPDEV exported.
#   cmake -S . -B build/psp -DPHX_TARGET=psp -DCMAKE_TOOLCHAIN_FILE=cmake/psp.toolchain.cmake
set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR mips)

if(NOT DEFINED ENV{PSPDEV})
  message(FATAL_ERROR "PSPDEV not set. Install pspdev/pspsdk and export PSPDEV.")
endif()
set(PSPDEV $ENV{PSPDEV})
set(PSPSDK ${PSPDEV}/psp/sdk)

set(CMAKE_C_COMPILER   ${PSPDEV}/bin/psp-gcc)
set(CMAKE_CXX_COMPILER ${PSPDEV}/bin/psp-g++)

set(_arch "-G0 -march=allegrex -mno-abicalls -fno-pic")
set(CMAKE_C_FLAGS_INIT   "${_arch} -I${PSPSDK}/include")
set(CMAKE_CXX_FLAGS_INIT "${_arch} -I${PSPSDK}/include -fno-exceptions -fno-rtti")
set(CMAKE_C_FLAGS_RELEASE_INIT   "-O2")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-L${PSPSDK}/lib -specs=${PSPSDK}/lib/prxspecs -Wl,-q,-T${PSPSDK}/lib/linkfile.prx")

# don't try to run/link host test binaries during compiler checks
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH ${PSPDEV} ${PSPSDK})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Helper invoked by examples/platformer to package the relocatable ELF into a runnable EBOOT.PBP.
# Mirrors the Makefile's proven PRX flow (the prxspecs/linkfile above keep relocations -Wl,-q):
#   ELF --fixup-imports--> --prxgen--> .prx ; mksfo --> PARAM.SFO ; pack-pbp --> EBOOT.PBP
function(phx_psp_eboot target title)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${PSPDEV}/bin/psp-fixup-imports $<TARGET_FILE:${target}>
    COMMAND ${PSPDEV}/bin/psp-prxgen $<TARGET_FILE:${target}> ${target}.prx
    COMMAND ${PSPDEV}/bin/mksfo "${title}" PARAM.SFO
    COMMAND ${PSPDEV}/bin/pack-pbp EBOOT.PBP PARAM.SFO NULL NULL NULL NULL NULL ${target}.prx NULL
    COMMENT "Building PSP EBOOT.PBP: ${title}")
endfunction()

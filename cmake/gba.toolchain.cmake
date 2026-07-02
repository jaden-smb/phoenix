# cmake/gba.toolchain.cmake — devkitARM cross toolchain for the Game Boy Advance.
# Requires devkitPro installed and $DEVKITARM exported.
#   cmake -S . -B build/gba -DPHX_TARGET=gba -DCMAKE_TOOLCHAIN_FILE=cmake/gba.toolchain.cmake
set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED ENV{DEVKITARM})
  message(FATAL_ERROR "DEVKITARM not set. Install devkitPro and export DEVKITARM.")
endif()
set(DEVKITARM $ENV{DEVKITARM})

set(CMAKE_C_COMPILER   ${DEVKITARM}/bin/arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${DEVKITARM}/bin/arm-none-eabi-g++)
set(CMAKE_OBJCOPY      ${DEVKITARM}/bin/arm-none-eabi-objcopy CACHE FILEPATH "")

# PHX_GBA_HW marks REAL hardware (MMIO/VRAM/OAM paths) on top of the PHX_TARGET_GBA tier
# define from caps_select.cmake — the host TIER=gba_sim build gets the tier but not this.
set(_arch "-mthumb -mthumb-interwork -mcpu=arm7tdmi -DPHX_GBA_HW=1")
set(CMAKE_C_FLAGS_INIT   "${_arch}")
set(CMAKE_CXX_FLAGS_INIT "${_arch} -fno-exceptions -fno-rtti")
set(CMAKE_C_FLAGS_RELEASE_INIT   "-Os -ffast-math")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Os -ffast-math")
set(CMAKE_EXE_LINKER_FLAGS_INIT  "-specs=gba.specs")

# don't try to run/link host test binaries during compiler checks
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Helper invoked by examples/platformer to turn the ELF into a runnable .gba ROM:
#   ELF --objcopy--> .gba binary --gbafix--> cartridge header/checksum
function(phx_gba_rom target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}> ${target}.gba
    COMMAND ${DEVKITARM}/bin/gbafix ${target}.gba
    COMMENT "Building GBA ROM: ${target}.gba")
endfunction()

# Phoenix Engine — Build System

> One CMake tree → four runtime binaries + host tools. The build system is itself a
> portability mechanism: a new platform is *a toolchain file + a backend folder*, with
> zero changes to the engine or game CMake targets.

## 1. Layout

```
fenix/
├── CMakeLists.txt              # root: options, target registration, depcheck
├── cmake/
│   ├── phx_module.cmake        # helper: phx_add_module() / phx_add_backend()
│   ├── gba.toolchain.cmake     # devkitARM
│   ├── psp.toolchain.cmake     # pspsdk / devkitPSP
│   ├── caps_select.cmake       # maps target -> phx_caps tier header
│   └── warnings.cmake          # shared strict flags
├── engine/<module>/CMakeLists.txt    # each module is a static lib target
├── tools/<tool>/CMakeLists.txt       # host-only executables
└── examples/platformer/CMakeLists.txt
```

## 2. The four configure invocations

```bash
# Linux  (host)
cmake -S . -B build/linux  -DPHX_TARGET=linux  -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux

# Windows (host MSVC, or cross with mingw)
cmake -S . -B build/win    -DPHX_TARGET=windows -G "Visual Studio 17 2022"
cmake --build build/win --config Release

# GBA    (cross, devkitARM)
cmake -S . -B build/gba    -DPHX_TARGET=gba \
      -DCMAKE_TOOLCHAIN_FILE=cmake/gba.toolchain.cmake
cmake --build build/gba     # -> platformer.gba (ROM)

# PSP    (cross, pspsdk)
cmake -S . -B build/psp    -DPHX_TARGET=psp \
      -DCMAKE_TOOLCHAIN_FILE=cmake/psp.toolchain.cmake
cmake --build build/psp     # -> EBOOT.PBP
```

`PHX_TARGET` drives three things via `cmake/caps_select.cmake`:
1. which `platform/src/<backend>` and `render/src/<backend>` sources compile,
2. which `phx_caps` tier header is active (`-DPHX_CAPS_HEADER=...`),
3. platform compile flags (no-exceptions/no-rtti on console, etc.).

## 3. `phx_add_module` helper (keeps every module identical)

```cmake
# cmake/phx_module.cmake
function(phx_add_module name)
  cmake_parse_arguments(M "" "" "SRC;DEPS;BACKENDS" ${ARGN})
  add_library(phx_${name} STATIC ${M_SRC})
  target_include_directories(phx_${name} PUBLIC include)
  target_link_libraries(phx_${name} PUBLIC ${M_DEPS})
  target_compile_features(phx_${name} PUBLIC cxx_std_17)
  # link only the backend matching PHX_TARGET:
  foreach(b ${M_BACKENDS})
    if(b STREQUAL PHX_TARGET OR b STREQUAL PHX_RENDER_TIER)
      target_sources(phx_${name} PRIVATE ${b}/...)
    endif()
  endforeach()
endfunction()
```

Result: `engine/platform/CMakeLists.txt` is ~5 lines; the linker gets exactly one
backend; no `#ifdef` soup, no dead backend code in the binary.

## 4. Console toolchain notes

### `cmake/gba.toolchain.cmake`

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(DEVKITARM $ENV{DEVKITARM})
set(CMAKE_C_COMPILER   ${DEVKITARM}/bin/arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER ${DEVKITARM}/bin/arm-none-eabi-g++)
set(ARCH "-mthumb -mthumb-interwork -mcpu=arm7tdmi")
set(CMAKE_CXX_FLAGS_INIT "${ARCH} -fno-exceptions -fno-rtti -ffast-math -Os")
# link: specs=gba_cart, then objcopy ELF -> .gba, then gbafix (header/checksum)
```

Post-build custom command: `arm-none-eabi-objcopy -O binary` → `gbafix` writes the
cartridge header. Output: `platformer.gba`, runnable in mGBA or on hardware.

### `cmake/psp.toolchain.cmake`

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER   psp-gcc)
set(CMAKE_CXX_COMPILER psp-g++)
set(CMAKE_CXX_FLAGS_INIT "-G0 -fno-exceptions -fno-rtti -O2")
# link against pspsdk libs; PSP_MODULE_INFO via build_prx
```

Post-build: `psp-fixup-imports` → `mksfo` (param.sfo) → `pack-pbp` → `EBOOT.PBP`,
runnable in PPSSPP or on a CFW PSP.

## 5. Asset build integration

Assets are a CMake target too — bundles are built by host tools before the runtime
binary that consumes them. Cross-target builds still build tools for the **host**:

```cmake
# tools always build for host, even in a GBA/PSP configure (via ExternalProject/host build)
add_custom_command(
  OUTPUT  ${CMAKE_BINARY_DIR}/assets.phxp
  COMMAND phxpack --target ${PHX_TARGET}
          --in  ${CMAKE_SOURCE_DIR}/examples/platformer/assets
          --out ${CMAKE_BINARY_DIR}/assets.phxp
  DEPENDS phxpack ${ASSET_SOURCES})
add_custom_target(assets DEPENDS ${CMAKE_BINARY_DIR}/assets.phxp)
add_dependencies(platformer assets)
# GBA: assets.phxp is bin2o'd into the ROM; PSP/PC: shipped alongside EBOOT/exe.
```

Incremental: only changed source assets re-encode (the `.phxp.lock` tracks hashes).

## 6. Dependency enforcement in CI

```cmake
add_custom_target(depcheck ALL
  COMMAND ${Python3_EXECUTABLE} tools/common/depcheck.py engine/
  COMMENT "Verifying acyclic module dependency graph")
```

`depcheck.py` parses `#include "phx/<mod>/..."` edges and fails if any back-edge or
cycle exists (the law in `docs/00` §3). A violation breaks the build like a compile
error — the architecture is *mechanically* enforced, not just documented.

## 7. CI matrix

| Job        | Toolchain   | Output            | Tests run                          |
|------------|-------------|-------------------|------------------------------------|
| linux-gcc  | gcc         | ELF               | all (unit + conformance + golden)  |
| linux-clang| clang       | ELF + sanitizers  | all + ASan/UBSan                   |
| windows    | MSVC        | exe               | unit + conformance                 |
| gba        | devkitARM   | .gba              | builds + mGBA headless smoke + size|
| psp        | pspsdk      | EBOOT.PBP         | builds + PPSSPP headless smoke     |
| tools      | host        | phxpack etc.      | pack round-trip                    |

Console jobs assert a **size budget** (`.gba` ROM/IWRAM, PSP RAM) — a regression that
blows the GBA budget fails CI, keeping pillar #2 honest.

## 8. Single-command developer ergonomics

A top-level `Makefile`/`justfile` wraps the four configures:

```
just build linux     # or: gba | psp | windows | all
just run  linux       # launches the example
just run  gba         # boots mGBA with the freshly built ROM
just pack             # rebuild asset bundles only
just test             # full host test suite
```

This is sugar over CMake; CMake remains the source of truth so IDEs and CI work
unmodified.

# Phoenix Engine — Folder Structure (annotated)

Every engine module follows the identical shape: a public `include/phx/<mod>/` (the
only thing siblings may include) and a private `src/` with optional per-backend
subfolders. The build links exactly one backend per module.

```
fenix/
├── README.md                     Engine identity + navigation
├── STRUCTURE.md                  (this file)
├── CMakeLists.txt                Root build: target select, module registration, depcheck gate
│
├── cmake/                        Build machinery (a new platform = a file here + a backend folder)
│   ├── phx_module.cmake          phx_add_module(): one-backend-per-module helper
│   ├── caps_select.cmake         PHX_TARGET -> compile defines + render tier
│   ├── gba.toolchain.cmake       devkitARM cross toolchain (+ phx_gba_rom)
│   └── psp.toolchain.cmake       pspsdk cross toolchain  (+ EBOOT.PBP packaging)
│
├── docs/                         The technical design documentation (start at 00)
│   ├── 00-architecture.md        Master architecture, seams, risk register
│   ├── 01-core.md … 10-gameplay-systems.md
│   └── diagrams/                 Standalone UML / module / sequence diagrams
│
├── engine/                       The engine. Acyclic dependency graph (enforced).
│   ├── core/                     Closed foundation: types, assert, fixed, math, caps, log, config, time
│   │   ├── include/phx/core/     types.h fixed.h math.h caps.h app.h log.h config.h time.h profile.h
│   │   └── src/                  app.cpp fixed.cpp log.cpp profile.cpp ...
│   ├── memory/                   Arena / Stack / Pool / Object allocators + MemoryRoot
│   │   └── include/phx/memory/   allocators.h memory_root.h
│   ├── platform/                 The C-ABI seam (the ONLY place OS/SDK headers appear)
│   │   ├── include/phx/platform/ platform.h (C)  platform.hpp (thin C++ adapter)
│   │   └── src/{null,sdl,gba,psp}/ one linked per build — null=headless, sdl=window+audio,
│   │                                gba=Mode3+keypad (real ROM), psp=sceDisplay/Ctrl (real EBOOT)
│   ├── runtime/                  Composition root: the App fixed-step main loop (top layer)
│   │   ├── include/phx/runtime/  app.h (App, Game hooks)
│   │   └── src/                  app.cpp (boot -> loop -> teardown)
│   ├── render/                   One 2D-intent API; backends per render tier; LRU texture cache
│   │   ├── include/phx/render/   renderer.h tilemap.h texture.h texture_cache.h
│   │   └── src/{gl,gu,gba,soft}/  GL(tier2) · PSP GU(tier1: gu_backend.cpp + gu_model.h compositor) · GBA PPU(tier0: gba_ppu.cpp + ppu_model.h compositor) · software(ref/CI)
│   ├── ecs/                      Sparse-set World, components, systems
│   ├── input/                    phx_input_raw -> semantic Button/edge/axis state
│   ├── audio/                    Software mixer (mixer.h) + SPSC ring streaming (stream.h) + lock-free command queue (command_queue.h)
│   ├── resource/                 .phxp bundle mount, mmap/zero-copy views, LZSS codec, LRU cache
│   ├── scene/                    Scene stack, transitions, persistent blackboard
│   ├── physics/                  AABB + swept tile collision; overlap queries
│   ├── anim/                     Sprite-sheet frames + animation state machine
│   └── ui/                       Immediate-mode menus/text/HUD/dialogue (-> sprite path)
│
├── tools/                        Host-only (C++17, STL allowed; never ships to console)
│   ├── phxpack/                  Bundle assembler: png/ppm/tmcsv/sprdef/tmj/wav -> assets.phxp; DEFLATE/PNG/JSON/Tiled/WAV importers
│   ├── phxsprite/                PNG (+slices) -> .phxspr (atlas + animation tables)
│   ├── phxtile/                  Tiled .tmj/.tmx -> .phxtmap (layers + collision + spawns)
│   ├── phxsnd/                   WAV -> .phxsnd (PCM/ADPCM/8-bit per target)
│   ├── phxbin/                   JSON/XML -> .phxbin (+ generated accessor header)
│   ├── phxtmap/                  GUI tilemap editor (ImGui + engine GL backend)
│   ├── phxentity/                GUI entity/prefab editor (reflection-driven)
│   └── common/                   depcheck.py + shared tool utilities
│
├── examples/
│   └── platformer/               The MVP gate: full slice using ONLY engine systems ✅
│       ├── src/                  components.h (EngineCtx) · systems.cpp (logic+scenes) ·
│       │                         main.cpp (entry) · bake.h (host demo-asset baker)
│       └── assets/               hero.png tiles.png level1.tmj music.wav items.json ...
│
└── tests/                        Host test suites (the architecture's safety net)
    ├── core/ memory/ ecs/        unit tests
    ├── platform_conformance/     every backend must pass this to be "done"
    ├── render_conformance/       golden-image diff via the software backend
    ├── pack/                     bake -> load round-trip for every asset type/target
    └── determinism/              fixed vs float scalar divergence bounds (release gate)
```

## The two rules that keep this clean

1. **Include only `include/phx/<mod>/`** of another module — never its `src/`.
   Dependencies point *downward* only (core never includes ecs). A back-edge is a
   build break, caught by `tools/common/depcheck.py`.
2. **One backend per module per build.** Platform/render backends live in `src/<name>/`
   subfolders; `phx_add_module(... BACKENDS ...)` links exactly the matching one. No
   `#ifdef PLATFORM` in gameplay or systems code — ever.

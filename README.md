<div align="center">

# Phoenix Engine (`phx`)

**A lightweight, modular, retro-inspired 2D/2.5D game engine.**
One codebase → **Game Boy Advance · PSP · Windows · Linux**.

*Built for independent development, homebrew, low-resource environments, and
long-term maintainability — prioritizing code quality, portability, and performance
over feature count.*

</div>

---

## What it is

Phoenix is a from-scratch C++17 engine in the spirit of the engines that shipped
real games in the 1990s and early 2000s — Doom/Quake's disciplined data layouts,
RenderWare's portability, Allegro/SDL/raylib's pragmatism, and the classic Nintendo
SDK design ethos. It runs the **same gameplay code** on a 256 KB Game Boy Advance and
a multi-gigabyte PC by designing around a **17,000× RAM gap** instead of ignoring it.

```
        GAME  ─────────────────────────────────────────────────────┐
        ├─ ecs · scene · physics · anim · ui · resource (systems)   │
        ├─ core (loop · memory · log · config · time · profile)     │  portable C++17
        ╞═════════════════════ C-ABI seam ═════════════════════════╡
        ├─ platform: sdl (win/linux) │ gba (mmio) │ psp (sceXxx)     │  one backend linked
        └────────────────────────────────────────────────────────────┘
```

## Design pillars (in priority order)

1. **Portability** — gameplay never includes a platform header.
2. **Performance under constraint** — predictable frame time, zero hidden allocations.
3. **Simplicity** — any module readable in one sitting.
4. **Extensibility** — new platforms/systems plug in without core edits.
5. **Feature count** — explicitly *last*.

## Highlights

| | |
|---|---|
| **Memory** | One root arena at boot; arena/stack/pool/object allocators; **zero hot-path heap**; fragmentation structurally impossible. |
| **Math** | `phx::scalar` = `float` on PC/PSP, `fixed16` (Q16.16) on GBA — same source, no FPU required. |
| **ECS** | Sparse-set, bounded, cache-friendly; 512 entities on GBA → 65,536 on PC. |
| **Render** | One 2D-intent API (sprites/tilemaps/parallax/zoom/shake/palettes) → GBA PPU, PSP GU, PC GL (Vulkan: future). |
| **Assets** | Baked offline to `.phxp` bundles; loaded by mmap/zero-copy (or LZSS-decompressed once); per-target encoded. |
| **Tools** | CLI converters + `phxpack` assembler + two GUI editors built on the engine itself; editors emit open author formats (Tiled `.tmj`, JSON). |
| **Build** | One CMake tree → ELF, `.exe`, `.gba` ROM, PSP `EBOOT.PBP`. |
| **Verification** | Byte-exact float/fixed determinism gate · ASan+UBSan gate · GBA size gate · golden-image diffs vs the software reference · Windows suite run under Wine — all in CI. |

## Repository layout

```
phoenix/
├── docs/            ← the technical design documentation (start at 00)
├── engine/          ← the engine modules (core, memory, platform, render, ecs, ...)
├── tools/           ← host-only asset pipeline + GUI editors (instructions.md in each folder)
├── examples/        ← the platformer that proves the architecture (MVP gate)
├── tests/           ← unit + conformance + golden-image + determinism suites
└── cmake/           ← toolchain files (gba, psp, mingw) + module helpers
```

## Documentation — read in this order

| Doc | Title | |
|-----|-------|---|
| [00](docs/00-architecture.md) | **Master Architecture** | whole-engine style, seams, cross-cutting decisions, risk register |
| [01](docs/01-core.md) | Core | app loop, memory root, log, config, time, math/fixed-point |
| [02](docs/02-platform-layer.md) | Platform Layer | the C seam + per-platform backends |
| [03](docs/03-rendering.md) | Rendering | unified API, GL/GU/PPU backends, sprite/tile model |
| [04](docs/04-ecs.md) | ECS | sparse-set entity/component/system + tradeoffs |
| [05](docs/05-memory.md) | Memory | arena/stack/pool/object allocators + diagrams |
| [06](docs/06-resources.md) | Resources | `.phxp` pack format, caching, streaming, versioning |
| [07](docs/07-build-system.md) | Build System | CMake, toolchains, four-target strategy |
| [08](docs/08-tooling.md) | Tooling | phxpack + tilemap/entity editors + converters |
| [09](docs/09-roadmap.md) | Roadmap | MVP, milestones, time estimates, future platforms |
| [10](docs/10-gameplay-systems.md) | Gameplay Systems | input, audio, scene, physics, animation, UI |
| — | [STRUCTURE.md](STRUCTURE.md) | annotated folder tree |

## Development status

> **MVP COMPLETE (roadmap M0–M5), M6 editors delivered, M7 gates standing. Every roadmap item
> that can be verified without physical hardware is done — on all four targets, at runtime.**
> The example platformer is a real game (enemies with stomp/damage/i-frames, death/respawn,
> save/load, a parallax cloud backdrop) driven **entirely by the baked asset pipeline**: level +
> spawns + per-layer parallax from a Tiled map, hero animation from a Sprite asset, SFX from
> WAV-baked Sounds — nothing gameplay-relevant is hardcoded. It runs identically on the PC tier
> (`scalar = float`) and the GBA tier (`scalar = fixed16`) — enforced by a **byte-exact
> determinism gate** — and the same codebase ships as a Linux ELF, a **Windows exe (full suite +
> game verified under Wine)**, a GBA ROM (software *and* native PPU, verified on mGBA), and a PSP
> EBOOT (software *and* native GU, verified on PPSSPP). The full check suite also runs clean
> under **ASan+UBSan**. Full history in **[STATUS.md](STATUS.md)**.

| Built & verified ✅ (every backend — render *and* audio — runs on a real target) | Remaining ⬜ |
|---|---|
| core (types, assert, fixed, math, caps, pixel, log, config, time, **profile**) · memory (4 allocators + MemoryRoot) · ecs (sparse-set World) · input (semantic buttons + edges + **pointer**) · physics (AABB-vs-tilemap + overlap pass) · anim (sprite-sheet + state machine) · scene (LIFO stack + Blackboard + scene arena) · ui (immediate-mode text/bar/menu/focus ring + **typewriter dialogue** + **frame-profiler overlay**) · audio (software mixer: voices, pan, resample, music + SPSC-ring streaming + lock-free command queue) · resource (`.phxp` + zero-copy `ResourceCache` + LZSS + texture/tilemap/sprite/spawns/sound views + **per-layer parallax factors**) · runtime (App loop owns World+Renderer+Input; per-phase **frame profiling**; `PHX_MAX_FRAMES` bounded-run cap) · **render**: front end (sort/batch + camera pan/**zoom**/**shake** + **per-layer parallax**, Q16 tier-exact) + software golden + **OpenGL (pixel-verified on a real GPU)** + **GBA PPU (VRAM/OAM verified on mGBA)** + **PSP GU (pixel-verified on PPSSPP)** + LRU texture cache · **platform**: `null` · `sdl` (window + audio device + mouse) · `gba` (ROM + Direct Sound + **battery-SRAM save**) · `psp` (EBOOT + sceAudio + **sceIo save, persistence verified**) · **tools**: the full two-stage pipeline (`phxsprite`/`phxtile`/`phxsnd` **with per-target encode**/`phxbin` → `phxpack`) **+ both GUI editors** (`phxtmap` tilemap, `phxentity` tables — built on the engine's own window/renderer/UI) · **example platformer** (data-driven, save/load, 2 enemy types, parallax backdrop) · **gates**: `check` · `determinism` · `sanitize` · GBA size gate · Windows-under-Wine — all in CI | physical-hardware validation (real GBA cart incl. SRAM persistence across power cycles; a physical PSP) · license (TBD below) · post-1.0 scope: 2.5D, tracker music/ADPCM, particles, job system, scripting, DS/Vita/Android ports (docs/09 §3) |

### Build & test right now (host, no cmake needed)

Requires only `g++` (C++17) + `make`:

```bash
make check                # THE gate: unit + 17 integration suites + pipeline + tools CLI + dep gate
make test                 # just the unit suite (PC tier, scalar=float)
make test TIER=gba_sim    # same suite under the GBA fixed-point path (scalar=fixed16)
make smoke                # boot the engine + run the fixed-step loop headlessly
make render               # software-rasterize a frame -> build/render_out.ppm
make ppu                  # GBA-native PPU backend: RGBA8 -> 4bpp tiles + OAM -> compose (headless)
make gu                   # PSP-native GU backend: textured sprite quads -> compose (bit-identical to soft)
make playable             # spawn an entity, drive it with input, verify it moved
make physics              # drop a body onto a tile floor; verify it lands (gravity+collision)
make anim                 # advance a sprite-sheet animator; verify the on-screen frame
make scene                # push a menu over gameplay; verify the overlay composites
make ui                   # draw text + HUD bar + focus-ring menu; verify pixels & nav
make platformer           # the M1 capstone: the full example game, played headlessly
make audio                # mix/stream PCM + decode a WAV -> bake Sound -> mount -> mix it
make texcache             # drive a Renderer through the budget-bounded LRU texture cache
make png                  # decode a real PNG (inflate) -> bake -> mount -> render from it
make sprite               # slice a sheet PNG + clips -> bake -> mount -> build Animator -> render
make tiled                # parse a Tiled .tmj (JSON) -> bake tilemap + spawns -> mount -> render
make resource             # bake a .phxp bundle (raw + LZSS) -> mount -> decompress -> render from it
# ...and, with SDL2 (+ libGL), play the example in a real window — or verify the desktop backends:
make sdl                  # software renderer -> SDL window (arrows/WASD, Z=jump, Enter=start)
make gl                   # OpenGL renderer  -> SDL window (same game, GPU-drawn quads)
make sdl-verify           # render a known scene through the SDL window, read back + match the soft golden
make gl-verify            # render through the OpenGL GPU, glReadPixels + match the soft golden (pixel-exact)
make audio-verify         # open a real audio device, mix a pushed SFX live, confirm non-silent output
# ...and, with devkitPro / pspsdk installed, cross-compile the SAME engine to real hardware:
make gba                  # devkitARM -> build/gba/phx-smoke.gba       (render smoke; boots on a GBA)
make gba-ppu              # devkitARM -> build/gba/phx-ppu.gba             (PPU hardware smoke: Mode-0 tiles+OBJ)
make gba-platformer       # devkitARM -> build/gba/phx-platformer.gba      (the FULL example game ROM)
make gba-platformer-ppu   # devkitARM -> build/gba/phx-platformer-ppu.gba  (the FULL game on the PPU hardware)
make psp                  # pspsdk    -> build/psp/EBOOT.PBP           (runs on a PSP / PPSSPP, software render)
make psp-gu               # pspsdk    -> build/psp/gu/EBOOT.PBP       (PSP GU hardware: sceGu display list)
make psp-audio            # pspsdk    -> build/psp/audio/EBOOT.PBP   (PSP sceAudio output device)
make gba-audio            # devkitARM -> build/gba/phx-audio.gba       (GBA Direct Sound: DMA1+Timer0->FIFO A)
make phxpack              # build the asset CLI + bake a demo bundle end-to-end
make depcheck             # enforce the acyclic module dependency law
make determinism          # M7 gate: 9 suites + the rendered frame byte-identical across BOTH scalar tiers
make sanitize             # M7 gate: the full check suite under ASan+UBSan (no recover)
# ...and, with MinGW-w64 (g++-mingw-w64-x86-64 or llvm-mingw), cross-compile to Windows:
make win                  # ALL of the above binaries as statically-linked PE32+ .exe (build/win/)
make win-verify           # run the Windows unit-suite exe under Wine (native or flatpak)
make gba-save             # devkitARM -> battery-SRAM save smoke ROM (verify via mGBA GDB stub)
make psp-save             # pspsdk   -> sceIo save smoke EBOOT (PPSSPP log: SAVE_DEVICE/PERSIST_PASS)
# ...and, with SDL2, the GUI editors (docs/08) — built on the engine's own window/renderer/UI:
make tmap                 # phxtmap: mouse tilemap editor over the open Tiled .tmj (+ entity mode)
make entity               # phxentity: entity/prefab table editor over the phxbin author JSON
make clean
```

Expected: `PASS 120336 checks across 101 cases`, `SMOKE/RENDER/PPU/GU/PLAYABLE/PHYSICS/ANIM/SCENE/UI/PLATFORMER/AUDIO/TEXCACHE/PNG/SPRITE/TILED/RESOURCE/PHXPACK/PIPELINE/TOOLS PASS`, `depcheck: OK` — and, for the gates, `DETERMINISM PASS` / `SANITIZE PASS`.

## Cross-compiling to real hardware

The host `Makefile` already cross-compiles the **same engine** to a Game Boy Advance ROM, a
PSP EBOOT, and Windows executables — no CMake needed:

```bash
make gba                 # devkitARM  -> build/gba/phx-smoke.gba           (render smoke; load in mGBA)
make gba-ppu             # devkitARM  -> build/gba/phx-ppu.gba             (native PPU smoke: Mode-0 tiles + OBJ)
make gba-platformer      # devkitARM  -> build/gba/phx-platformer.gba      (the full example game, software render)
make gba-platformer-ppu  # devkitARM  -> build/gba/phx-platformer-ppu.gba  (the full example game on the PPU hardware)
make psp                 # pspsdk     -> build/psp/EBOOT.PBP               (run in PPSSPP / on a PSP)
make psp-platformer      # pspsdk     -> build/psp/platformer/EBOOT.PBP    (the full example game)
make psp-gu              # pspsdk     -> build/psp/gu/EBOOT.PBP            (PSP GU hardware: sceGu display list)
make win                 # MinGW-w64  -> build/win/*.exe                   (every host binary, static PE32+)
```

`make gba-platformer` bakes the `.phxp` bundle on the host, links it into the ROM with `bin2s`
(there is no filesystem on a GBA), and runs the **same `examples/platformer` game and engine
systems** as the host build — only the entry point's budgets differ (120×80 framebuffer 2×-scaled
to 240×160; ~160 KB EWRAM arena; 256 entities). Load `build/gba/phx-platformer.gba` in mGBA:
arrows move, A/Z jumps, Start opens the menu.

Every portable engine module compiles unchanged for ARM7TDMI (GBA, fixed-point tier) and MIPS
Allegrex (PSP); only the platform backend differs (`engine/platform/src/{gba,psp}/`). The CMake
tree (below) remains the documented path for full multi-target packaging:

```bash
cmake -S . -B build/linux   -DPHX_TARGET=linux -DCMAKE_BUILD_TYPE=Release   # PC (+ SDL2)
cmake -S . -B build/gba     -DPHX_TARGET=gba     -DCMAKE_TOOLCHAIN_FILE=cmake/gba.toolchain.cmake
cmake -S . -B build/psp     -DPHX_TARGET=psp     -DCMAKE_TOOLCHAIN_FILE=cmake/psp.toolchain.cmake
cmake -S . -B build/windows -DPHX_TARGET=windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw.toolchain.cmake
```

> **Status: every seam of the design is implemented and verified at runtime on a real or
> emulated target.** Render (software golden · OpenGL, pixel-verified on a real GPU · GBA PPU,
> VRAM/OAM-verified on mGBA · PSP GU, pixel-verified on PPSSPP), audio output (SDL device ·
> GBA Direct Sound · PSP sceAudio), persistent saves (host file · GBA battery SRAM · PSP sceIo,
> persistence across boots verified), and the **Windows build runs its complete test suite and
> the whole game under Wine**. Both scalar tiers are held byte-identical by the `determinism`
> gate, the full suite is ASan+UBSan-clean (`sanitize`), the GBA ROM sits behind a CI size gate,
> and the GUI editors author content the pipeline bakes. Remaining before a 0.1 tag: validation
> on physical hardware (a real GBA cart — including SRAM persistence across power cycles, which
> emulators can't prove — and a physical PSP) and the license. The verification recipes and the
> full engineering history live in **[STATUS.md](STATUS.md)**; future scope is in the
> [roadmap](docs/09-roadmap.md).

## License

TBD (intended: permissive — MIT/Zlib — to suit homebrew and indie use).

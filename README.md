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
        ├─ platform: win32 │ posix │ gba (mmio) │ psp (sceXxx)       │  one backend linked
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
| **Render** | One 2D-intent API (sprites/tilemaps/parallax/palettes) → GBA PPU, PSP GU, PC GL/Vulkan. |
| **Assets** | Baked offline to `.phxp` bundles; loaded by mmap/zero-copy (or LZSS-decompressed once); per-target encoded. |
| **Build** | One CMake tree → ELF, `.exe`, `.gba` ROM, PSP `EBOOT.PBP`. |

## Repository layout

```
fenix/
├── docs/            ← the technical design documentation (start at 00)
├── engine/          ← the engine modules (core, memory, platform, render, ecs, ...)
├── tools/           ← host-only asset pipeline + GUI editors
├── examples/        ← the platformer that proves the architecture (MVP gate)
├── tests/           ← unit + conformance + golden-image + determinism suites
└── cmake/           ← toolchain files (gba, psp) + module helpers
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

> **M1 COMPLETE: a full headless platformer runs on every engine system — now driven entirely by
> the baked asset pipeline.** The example mounts a `phxpack`-baked `.phxp` bundle and loads its
> **level + entity spawns from a Tiled map, its hero animation from a Sprite asset, and its SFX
> from WAV-baked Sound assets** (nothing gameplay-relevant is hardcoded). It runs a **title →
> level → pause** scene stack, drives a player with input + tile physics (gravity, jump,
> AABB-vs-tile collision), animates it with a sprite-sheet state machine, collects coins via the
> physics overlap pass, plays SFX through the mixer, and draws a HUD (bitmap text + health bar) —
> all under the fixed-step loop. A scripted controller plays it headlessly and asserts the result
> from the ECS + framebuffer (and that the mixer produced non-silent output).
> Every module builds clean (zero warnings) and the entire game produces **identical results**
> under the PC tier (`scalar = float`) and the GBA tier (`scalar = fixed16`) — **and the same
> engine cross-compiles to a real GBA ROM and PSP EBOOT.** Full breakdown in **[STATUS.md](STATUS.md)**.

| Built & tested ✅ | Authored, cross-compiles (not run here) 🟡 | Designed (docs) ⬜ |
|---|---|---|
| core (types, assert, fixed, math, caps, pixel, log, config, time) · memory (4 allocators + MemoryRoot) · ecs (sparse-set World) · input (semantic buttons + edges) · physics (AABB-vs-tilemap + overlap pass) · anim (sprite-sheet + state machine) · scene (LIFO stack + Blackboard + scene arena) · ui (immediate-mode text/bar/menu + focus ring) · audio (software mixer: voices, pan, resample, music + SPSC-ring streaming + lock-free command queue) · resource (`.phxp` + zero-copy `ResourceCache` + LZSS compression + texture/tilemap/sprite/spawns/sound views) · runtime (App loop owns World+Renderer+Input) · platform `null` backend · render front end + software backend (sprites, tilemaps, tint + scale, texture load/unload) · **GBA-native PPU render backend (4bpp tiles + OAM + `ppu_compose`; ARM7TDMI)** · **PSP-native GU render backend (textured sprite quads + `gu_compose`, bit-identical to soft; MIPS Allegrex)** · budget-bounded LRU texture cache · tools (`phxpack` CLI + DEFLATE/PNG + JSON/Tiled + WAV importers) · **example platformer (data-driven by the baked pipeline, headless)** | **GBA platform backend → real `.gba` ROM (devkitARM)** · **PSP platform backend → real `EBOOT.PBP` (pspsdk)** · **`sdl` platform backend (window + audio device) + `gl` render backend** (need SDL2/a display) | GBA PPU hardware DMA path (VRAM/OAM) · PSP GU `sceGu` display-list path · GBA/PSP audio (DMA/sceAudio) |

### Build & test right now (host, no cmake needed)

Requires only `g++` (C++17) + `make`:

```bash
make check                # unit + loop + render + playable + resource + phxpack + dep gate
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
# ...and, where SDL2 (+ libGL) is installed, play the example in a real window:
make sdl                  # software renderer -> SDL window (arrows/WASD, Z=jump, Enter=start)
make gl                   # OpenGL renderer  -> SDL window (same game, GPU-drawn quads)
# ...and, with devkitPro / pspsdk installed, cross-compile the SAME engine to real hardware:
make gba                  # devkitARM -> build/gba/phx-smoke.gba       (render smoke; boots on a GBA)
make gba-ppu              # devkitARM -> build/gba/phx-ppu.gba             (PPU hardware smoke: Mode-0 tiles+OBJ)
make gba-platformer       # devkitARM -> build/gba/phx-platformer.gba      (the FULL example game ROM)
make gba-platformer-ppu   # devkitARM -> build/gba/phx-platformer-ppu.gba  (the FULL game on the PPU hardware)
make psp                  # pspsdk    -> build/psp/EBOOT.PBP           (runs on a PSP / PPSSPP, software render)
make psp-gu               # pspsdk    -> build/psp/gu/EBOOT.PBP       (PSP GU hardware: sceGu display list)
make phxpack              # build the asset CLI + bake a demo bundle end-to-end
make depcheck             # enforce the acyclic module dependency law
make clean
```

Expected: `PASS 120336 checks across 101 cases`, `SMOKE/RENDER/PPU/GU/PLAYABLE/PHYSICS/ANIM/SCENE/UI/PLATFORMER/AUDIO/TEXCACHE/PNG/SPRITE/TILED/RESOURCE/PHXPACK PASS`, `depcheck: OK`.

## Cross-compiling to real hardware

The host `Makefile` already cross-compiles the **same engine** to a Game Boy Advance ROM and a
PSP EBOOT — no CMake needed:

```bash
make gba             # devkitARM  -> build/gba/phx-smoke.gba       (render smoke; load in mGBA)
make gba-ppu             # devkitARM  -> build/gba/phx-ppu.gba             (native PPU smoke: Mode-0 tiles + OBJ)
make gba-platformer      # devkitARM  -> build/gba/phx-platformer.gba      (the full example game, software render)
make gba-platformer-ppu  # devkitARM  -> build/gba/phx-platformer-ppu.gba  (the full example game on the PPU hardware)
make psp             # pspsdk     -> build/psp/EBOOT.PBP           (run in PPSSPP / on a PSP)
make psp-gu          # pspsdk     -> build/psp/gu/EBOOT.PBP       (PSP GU hardware: sceGu display list)
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
cmake -S . -B build/linux -DPHX_TARGET=linux -DCMAKE_BUILD_TYPE=Release   # PC (+ SDL2)
cmake -S . -B build/gba   -DPHX_TARGET=gba -DCMAKE_TOOLCHAIN_FILE=cmake/gba.toolchain.cmake
cmake -S . -B build/psp   -DPHX_TARGET=psp -DCMAKE_TOOLCHAIN_FILE=cmake/psp.toolchain.cmake
```

> **Status:** the one C++17 codebase runs the full host test suite (both scalar tiers) **and
> cross-compiles to real GBA and PSP binaries** — three architectures, one source. The GBA
> platformer ROM is now **verified actually running** on emulated ARM7TDMI (headlessly, via
> `mgba-qt` offscreen + its GDB stub + a VRAM→PNG dump): title screen, fixed-step loop, input
> polling, and the **Start→level transition all confirmed on hardware** (this also caught + fixed
> a GBA-only `__cxa_guard` deadlock — see STATUS.md #28). The **GBA PPU hardware path** is now
> implemented too — and the **full platformer now runs on the GBA PPU hardware**
> (`make gba-platformer-ppu`): tilemap → BG screenblock, sprites → OAM, Mode-0 DISPCNT, the whole
> game verified booting + transitioning + rendering its level through real VRAM/OAM via the GDB
> stub (STATUS.md #29–30). The **PSP GU `sceGu` display-list path is also done** (`make psp-gu`),
> pixel-verified on PPSSPP by an on-PSP eDRAM readback against the `gu_compose` model (#31–32). So
> **all four render backends now run on their native targets** (soft, GBA PPU on ARM7TDMI, PSP GU
> on Allegrec). See the [roadmap](docs/09-roadmap.md) and **[STATUS.md](STATUS.md)**.

## License

TBD (intended: permissive — MIT/Zlib — to suit homebrew and indie use).

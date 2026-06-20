# Phoenix Engine — Roadmap, MVP & Scalability

> Estimates assume **2 engineers** (one engine/runtime, one tools/platform) plus
> part-time art for the example. Durations are calendar weeks; "eng-weeks" in tables is
> effort. Dates anchored to a hypothetical start of **2026-07-01**.

---

## 1. MVP definition (the gate that proves the architecture)

**MVP = a playable 2D platformer vertical slice that builds and runs on all four
targets from one codebase, using only engine systems.**

Concretely, the MVP ships when:

- [ ] One CMake tree produces: Linux ELF, Windows exe, `.gba` ROM, PSP `EBOOT.PBP`.
- [ ] Memory: root arena + arena/stack/pool/object allocators; **zero hot-path heap**.
- [ ] Platform seam implemented for SDL(PC), GBA, PSP; passes `platform_conformance`.
- [ ] Renderer: sprites + tilemap + parallax + bitmap text on GL, GU, and GBA PPU.
- [ ] ECS: sparse-set world, fixed-step systems, deferred structural changes.
- [ ] Resource: `.phxp` bundle baked by `phxpack`; mmap/zero-copy load; per-target encode.
- [ ] Input/Audio/Scene/Physics/Anim/UI at the depth in `docs/10`.
- [ ] Example game: player movement, jump, 2 enemy types, camera, HUD, save/load.
- [ ] Fits GBA budget (ROM + IWRAM/EWRAM) — proven by the CI size gate.

**Explicitly NOT in MVP:** Vulkan, job/threading system, particle system, lighting,
networking, scripting, the GUI editors' full feature set (CLI converters suffice for
MVP; editors land in M5).

---

## 2. Milestones & estimates

| M | Milestone                          | Deliverable                                                    | Eng-weeks | Cum. weeks |
|---|------------------------------------|---------------------------------------------------------------|-----------|------------|
| M0| Foundations                        | repo, CMake, toolchains (4), `core/types`, `fixed16`, log, math, **memory allocators** + tests | 4 | 4 |
| M1| Platform seam + windows up         | `phx_platform` C seam; SDL + GBA + PSP backends; clock/input/file; conformance suite; a window that clears to a color on all four | 5 | 9 |
| M2| Rendering                          | unified API; GL + GU + GBA PPU backends; sprite batcher, tilemap+parallax, bitmap text; golden-image tests | 6 | 15 |
| M3| ECS + resources                    | sparse-set World; `.phxp` format; `phxpack` + `phxsprite`/`phxtile`/`phxsnd`/`phxbin` CLIs; cache/mmap; round-trip tests | 5 | 20 |
| M4| Gameplay systems                   | input semantics, audio mixer, scene stack, AABB/tile physics, animation SM, UI (menu/HUD/dialogue) | 6 | 26 |
| M5| **Example game (MVP gate)**        | full platformer slice; save system; runs+fits on all 4; size gate green | 4 | 30 |
| M6| Tools GUI + polish                 | `phxtmap` + `phxentity` editors; profiler overlay; docs pass; perf tuning | 5 | 35 |
| M7| Hardening                          | determinism suite, sanitizer pass, hardware testing (real GBA cart / PSP), 0.1 release | 3 | 38 |

**MVP at end of M5 (~30 eng-weeks ≈ ~15 calendar weeks with 2 engineers).**
**v0.1 public at end of M7 (~38 eng-weeks).**

```
 M0 ████ memory/core/build
 M1     █████ platform seam
 M2          ██████ rendering
 M3                █████ ecs + resources
 M4                     ██████ gameplay systems
 M5                           ████ EXAMPLE = MVP ◄── gate
 M6                               █████ editors + polish
 M7                                    ███ harden → v0.1
```

### Critical path & parallelism
- Engine engineer drives M0→M2→M4; tools/platform engineer drives M1 backends + M3
  pipeline + M6 editors in parallel where the seam is stable.
- **Biggest risk to the schedule:** M1/M2 console backends (GBA PPU + PSP GU) — budget
  slack here; they gate everything visual. Mitigation: stand up the **software backend
  first** so the front end and tests proceed while console backends mature.

---

## 3. Post-MVP version roadmap

| Version | Theme                  | Headline features                                                       |
|---------|------------------------|-------------------------------------------------------------------------|
| 0.1     | MVP (above)            | 4 platforms, platformer slice, CLI pipeline                              |
| 0.2     | Authoring & 2.5D       | full `phxtmap`/`phxentity`; PSP/PC 2.5D (depth, billboards, affine GBA)  |
| 0.3     | Audio & FX             | tracker music, ADPCM streaming, particle system, palette/blend FX        |
| 0.4     | Performance & jobs     | optional PC/PSP job system; render instancing 2.0; profiler GUI          |
| 0.5     | Scripting (optional)   | tiny embeddable VM (Lua-subset or bytecode) — **off on GBA**             |
| 0.6     | New platforms (tier 2) | Nintendo DS, PS Vita backends (see §4)                                   |
| 1.0     | Stability & docs       | API freeze, semver guarantees, full manual, two shipped sample games     |

---

## 4. Scalability — adding the next five platforms

The seam (`docs/02`) makes a new platform a **backend folder + toolchain + caps tier**.
Effort estimates assume the engine is at v0.2+.

| Platform     | What's reused (≈90%)             | New work                                                              | Caps tier   | Est. |
|--------------|----------------------------------|----------------------------------------------------------------------|-------------|------|
| **Nintendo DS** | core, ecs, resource, gameplay  | dual-screen PPU backend (2 screens, 3D core for one), touch input, devkitARM toolchain | tier 0.5 (PPU+3D) | 6 eng-weeks |
| **Nintendo 3DS**| everything + DS lessons        | `citro3d`/`citro2d` render backend, 3D-slider/parallax option, ctrulib platform, stereoscopic | tier 1.5    | 6 eng-weeks |
| **PS Vita**     | PSP backend is the template     | `libgpu`/Vita SDK render backend (programmable!), touch/dual-stick, vitasdk toolchain | tier 2 | 5 eng-weeks |
| **Android**     | PC GL backend (GL ES!)          | JNI/NativeActivity platform shim, GL ES 2/3 render path, touch + accel, OpenSL audio, gradle/ndk build | tier 2 | 6 eng-weeks |
| **Steam Deck**  | **Linux build runs as-is**      | controller glyphs, Proton/native packaging, gamepad-first UI defaults | tier 2 (=Linux) | 1 eng-week |

Why these are cheap relative to the engine: each only re-implements the C seam and one
render backend against an *existing* tier (PPU / fixed-function / programmable). The
17,000× RAM-gap design (capability tiers, fixed budgets, baked assets, no hidden
allocations) already spans the hard range; new machines land *inside* it.

### Render-tier reuse map

```
 tier 0  (tile/sprite PPU) : GBA ──► Nintendo DS (×2 screens)
 tier 1  (fixed-function)  : PSP ──► PS Vita (early path) , 3DS citro2d
 tier 2  (programmable)    : PC GL ──► Android GL ES , Steam Deck , Vita (full)
```

A new platform picks the nearest tier's render backend as its starting point — most of
the porting cost is the **platform seam + toolchain**, not graphics.

---

## 5. Standing risks beyond MVP

| Risk                              | Mitigation                                                        |
|-----------------------------------|------------------------------------------------------------------|
| Homebrew SDK rot (vitasdk, etc.)  | pin versions in `cmake/`; CI builds every supported target nightly|
| 2.5D scope balloons into a 3D engine | hard line: 2.5D = sprites/billboards/affine + ortho-ish depth; no full scene graph, no skeletal 3D in 1.x |
| Editor maintenance burden         | editors share the engine's GL backend (dogfood); minimal bespoke UI via reflection |
| Determinism breaks across new CPUs | the determinism suite is a release gate, not a nicety            |
| Community fragmentation of the seam| seam is small + conformance-tested; new backends must pass it before merge |

---

## 6. Definition of done for v1.0

- API frozen, semver from here; the `phx_platform` seam and public headers are stable.
- All listed platforms build in CI and pass conformance + golden + determinism suites.
- Two complete sample games (the platformer + one 2.5D demo) ship in `examples/`.
- A written manual (these `docs/` + per-API reference generated from headers).
- A new contributor can port to a new tier-2 platform in **< 2 weeks** using only the
  porting checklist (`docs/02` §6) — the ultimate test that the architecture delivered
  on its portability pillar.

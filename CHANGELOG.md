# Changelog

All notable changes to Phoenix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[semantic versioning](https://semver.org) (pre-1.0: MINOR may break, PATCH never does — see
[RELEASING.md](RELEASING.md)).

## [Unreleased]

### Added
- `make docs`: Doxygen API reference generated from the public headers (`engine/*/include`
  only — the same boundary depcheck enforces) plus the `docs/` manual, output in
  `build/docs/html`. `tools/common/doxyfilter.py` promotes the house `//` header comments
  to doc comments so the existing prose is the reference; the version is injected from
  `phx/core/version.h` (first slice of the roadmap's v1.0 "written manual" item).

### Fixed
- Docs/comments corrected to match the implementation: desktop `map()` is a load-once heap
  buffer (there is no OS mmap), PSP bundles are linked into the EBOOT (not read via `sceIo` at
  mount), platform backends allocate init-time state outside the root arena
  (`phx_platform_desc.root_arena` is reserved/unused today), and the PC bundle image counts
  against RAM — it is not OS-backed as `docs/06-resources.md` previously claimed.

## [0.1.0] - 2026-07-15

First tagged release: the complete engine slice proven on all four targets.

### Added
- **Engine core** — fixed-step App loop; one-arena memory model (arena/stack/pool/object
  allocators, zero hot-path heap); sparse-set ECS; scene, physics (tile collision incl.
  per-tile flags), animation, and UI systems; deterministic `phx::scalar` dual-tier math
  (`float` on PC/PSP, Q16.16 `fixed16` on GBA) held byte-identical by a determinism gate.
- **Rendering** — one 2D-intent API (sprites, tilemaps, per-layer parallax, palettes, zoom,
  shake) with four backends: software golden reference, OpenGL, PSP GU, GBA PPU (Mode-0 tiles +
  OAM, streamed backgrounds).
- **Audio** — block mixer + streaming, per-target sample-rate baking, SDL/PSP/GBA output paths.
- **Asset pipeline** — `.phxp` bundles, zero-copy/LZSS loading, per-target encoding; converters
  (`phxsprite`, `phxtile`, `phxsnd`, `phxbin`) + the `phxpack` assembler; save/load on every
  target including GBA SRAM and PSP savedata.
- **Editors** — `phxtmap` (tilemap, incl. collision-flag painting and prefab palettes) and
  `phxentity` (record tables), both built on the engine itself.
- **Games** — `examples/platformer` (the MVP gate) and **Emberwing — Cinder Hollow**, a
  complete level shipping as GBA ROM, PSP EBOOT, Windows exe, and Linux binary from one tree.
- **Targets** — Linux, Windows (MinGW-w64, static), GBA (devkitARM, ROM size gates), PSP
  (pspsdk); CI gates: full suite, cross-tier determinism, ASan+UBSan, release config, Wine-run
  Windows suite, console cross builds.
- **Release plumbing** — single-source version header (`phx/core/version.h`), `make dist*`
  packaging, CMake install/`find_package(phoenix)`/CPack SDK, tag-driven release workflow.

[Unreleased]: https://github.com/jaden-smb/phoenix/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/jaden-smb/phoenix/releases/tag/v0.1.0
